#!/bin/bash
# Baut den Bootloader von ClokeOS: PureDarwins xnu-loader mit Bootlogo (BOOTX64.EFI).
#
#   loader/build.sh [ausgabe.efi]     Standard: ~/ClokeOS-work/out/BOOTX64.EFI
#
# Die Quellen von xnu-loader und gnu-efi lädt ../fetch.sh nach ~/ClokeOS-work/download. Dazu kommen:
#   xnu-loader.patch   ohne -v: Log nur seriell, Logo zeigen, Framebuffer mit 24 Bit Farbtiefe übergeben;
#                      Taste V beim Start hängt -v an (ausführlicher Start);
#                      außerdem jump.S/jump.h, damit mingw-w64 den Loader bauen kann
#   splash.c, splash.h zeichnen das Umhang-Logo über GOP
#   logo.py            macht aus LOGO_D in ../../clokeos.html den Header clokeos_logo.h
# mingw-w64 erzeugt direkt PE-Dateien mit der Aufrufkonvention von UEFI. Deshalb braucht der Build
# weder Linux noch den ELF-Umweg, den xnu-loader mit gnu-efi sonst nimmt.
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need "$MINGW_CC" python3 patch

out="${1:-$LOADER_EFI}"
src="$HERE/loader"
b="$WORK/tmp/loader"
efi="$b/gnu-efi-$GNU_EFI_VERSION"
xl="$b/xnu-loader-$XNU_LOADER_REV"

for f in "$XNU_LOADER_TGZ" "$GNU_EFI_TGZ"; do
  [ -f "$f" ] || { echo "Fehlt: $f – zuerst ./fetch.sh ausführen." >&2; exit 1; }
done

rm -rf "$b"
mkdir -p "$b/obj" "$b/inc"
tar -xzf "$GNU_EFI_TGZ" -C "$b"
tar -xzf "$XNU_LOADER_TGZ" -C "$b"
patch -s -d "$xl" -p1 < "$src/xnu-loader.patch"
cp "$src/splash.c" "$xl/src/"
cp "$src/splash.h" "$xl/include/"
cp "$src/gop.c" "$xl/src/"
cp "$src/gop.h" "$xl/include/"
python3 -I "$src/logo.py" "$HERE/../clokeos.html" "$xl/include/clokeos_logo.h" >/dev/null
ln -s "$efi/inc" "$b/inc/efi" # xnu-loader bindet auch <efi/efipxebc.h> ein

# Wie CMakeLists.txt von xnu-loader, nur mit der Aufrufkonvention von UEFI statt SysV
# (GNU_EFI_USE_MS_ABI) und ohne Stack-Proben, die mingw sonst bei großen Rahmen einfügt.
cflags=(-std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar
  -funsigned-char -mno-red-zone -mno-stack-arg-probe -maccumulate-outgoing-args -mno-avx
  -DGNU_EFI_USE_MS_ABI -DCONFIG_x86_64
  -I "$efi/inc" -I "$efi/inc/x86_64" -I "$efi/inc/protocol" -I "$b/inc")

objs=()
# libefi aus gnu-efi, ohne den Startcode (crt0, ctors, entry) und die SysV-Aufrufbrücken
for f in boxdraw smbios console crc data debug dpath error event exit guid hand hw init lock \
         misc pause sread str cmdline print va_print runtime/rtlock runtime/efirtlib \
         runtime/rtstr runtime/vm runtime/rtdata x86_64/initplat x86_64/math; do
  o="$b/obj/efi_${f//\//_}.o"
  "$MINGW_CC" "${cflags[@]}" -w -c "$efi/lib/$f.c" -o "$o"
  objs+=("$o")
done
for f in main app boot console devtree fileio lowmem macho serial splash gop; do
  o="$b/obj/$f.o"
  "$MINGW_CC" "${cflags[@]}" -Wall -Wno-pointer-sign -Wno-unused-function -Wno-unused-local-typedefs \
    -I "$xl/include" -c "$xl/src/$f.c" -o "$o"
  objs+=("$o")
done
"$MINGW_CC" "${cflags[@]}" -I "$xl/include" -c "$xl/src/jump.S" -o "$b/obj/jump.o"
objs+=("$b/obj/jump.o")

mkdir -p "$(dirname "$out")"
"$MINGW_CC" -nostdlib -shared -Wl,-dll -Wl,--subsystem,10 -Wl,--no-insert-timestamp \
  -Wl,--image-base,0x10000000 -Wl,--exclude-all-symbols -e efi_main -o "$out" "${objs[@]}" -lgcc
echo "Fertig: $out"
