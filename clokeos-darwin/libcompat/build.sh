#!/bin/bash
# Baut libclokeos_compat (Symbole, die Mac-Programme an libSystem binden und PureDarwins libSystem
# 20.5 nicht hat: compat.c, chkstk.S, system.c, wide.c, misc.c, mathx.c, dispatch.c, workgroup.c, version.c;
# dazu libffi.dylib aus PureDarwins libffi.a) und legt sie samt einer libSystem.B.dylib, die sie
# zusätzlich re-exportiert (und deren Export-Tabelle die OS_*-Klassen bekommt), und einer libdyld.dylib
# ohne die zwei Versionsfunktionen in <stage>. Was für ein Programm noch fehlt, zeigt libcompat/missing.py. Zweistufig gebundene Programme (Homebrews Ruby, Bottles)
# finden die Symbole so über libSystem.
#
#   libcompat/build.sh <stage>     Architektur wie überall mit CLOKEOS_ARCH
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS" clang nm codesign

stage="$1"
read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"
out="$WORK/tmp/libcompat-$CLOKE_ARCH"
rm -rf "$out"
mkdir -p "$out" "$stage/usr/lib/system"

"$DEBUGFS" -R "dump /usr/lib/libSystem.B.dylib $out/libSystem.B.dylib" "$fs" >/dev/null 2>&1
[ -s "$out/libSystem.B.dylib" ] || { echo "libSystem.B.dylib nicht aus dem Image geholt" >&2; exit 1; }

target=x86_64-apple-macos11.0
[ "$CLOKE_ARCH" = arm64 ] && target=arm64-apple-macos11.0
inst=/usr/lib/system/libclokeos_compat.dylib
# -nostdlib, gegen die PureDarwin-libSystem: system.c ruft chmod, lgamma, setreuid usw. auf
# libdispatch-Erweiterung (dispatch.c): Abstand der internen _dispatch_barrier_async_detached_f zu
# dispatch_barrier_sync in genau dieser libSystem; fehlt das Symbol, bleibt dispatch_async als Ersatz
dflags=()
nmarch=x86_64; [ "$CLOKE_ARCH" = arm64 ] && nmarch=arm64
off="$(python3 -I - "$out/libSystem.B.dylib" "$CLOKE_ARCH" <<'PY'
import subprocess, sys
arch = "arm64" if sys.argv[2] == "arm64" else "x86_64"
a = b = None
for line in subprocess.run(["nm", "-arch", arch, sys.argv[1]], capture_output=True, text=True).stdout.splitlines():
    f = line.split()
    if len(f) == 3 and f[2] == "__dispatch_barrier_async_detached_f": a = int(f[0], 16)
    if len(f) == 3 and f[2] == "_dispatch_barrier_sync": b = int(f[0], 16)
if a and b: print(a - b)
PY
)"
[ -n "$off" ] && dflags=("-DBARRIER_DETACHED_OFFSET=$off")
[ "$CLOKE_ARCH" = x86_64 ] && dflags+=(-mcx16)
srcs=("$HERE/libcompat/compat.c" "$HERE/libcompat/chkstk.S" "$HERE/libcompat/system.c" "$HERE/libcompat/wide.c" "$HERE/libcompat/misc.c" "$HERE/libcompat/mathx.c" "$HERE/libcompat/dispatch.c" "$HERE/libcompat/workgroup.c" "$HERE/libcompat/version.c")
clang -target "$target" -dynamiclib -nostdlib -O2 -fno-stack-check -D_FORTIFY_SOURCE=0 -std=gnu11 -Wall \
  -Wno-unused-function ${dflags[@]+"${dflags[@]}"} -Wl,-no_fixup_chains -Wl,-platform_version,macos,11.0,11.5 \
  -install_name /usr/lib/libclokeos_compat.dylib -o "$out/libclokeos_compat.dylib" \
  "${srcs[@]}" "$out/libSystem.B.dylib"
# Der Linker des Macs (x86_64) sucht bei einem Install-Namen unter /usr/lib/system die Teilbibliotheken von
# libSystem ("library 'system_pthread' not found"); deshalb erst ein anderer Name, dann umsetzen
install_name_tool -id "$inst" "$out/libclokeos_compat.dylib"
# Die Klassen der Dispatch-Objekte (OS_dispatch_queue usw.) stehen in libSystem, sind aber lokal;
# Programme binden sie als _OBJC_CLASS_$_OS_dispatch_queue. OS_os_log gibt es in PureDarwin nicht
# (libsystem_trace ist nicht ObjC); die Klasse zeigt auf OS_object, damit die Bindung gelingt.
python3 -I "$HERE/libcompat/exporttrie.py" "$out/libSystem.B.dylib" "$nmarch" 'glob:_OBJC_CLASS_$_OS_*' '_OBJC_CLASS_$_OS_os_log=_OBJC_CLASS_$_OS_object'
python3 -I "$HERE/libcompat/reexport.py" "$out/libSystem.B.dylib" "$inst"
# libdyld antwortet auf _availability_version_check/__isPlatformVersionAtLeast immer mit 0: aus den
# Export-Tabellen von libSystem.B und libdyld ausblenden, version.c liefert die Funktionen
"$DEBUGFS" -R "dump /usr/lib/system/libdyld.dylib $out/libdyld.dylib" "$fs" >/dev/null 2>&1
[ -s "$out/libdyld.dylib" ] || { echo "libdyld.dylib nicht aus dem Image geholt" >&2; exit 1; }
for f in libSystem.B libdyld; do
  python3 -I "$HERE/libcompat/hidesym.py" "$out/$f.dylib" __availability_version_check ___isPlatformVersionAtLeast
done
codesign --force -s - "$out/libSystem.B.dylib" "$out/libdyld.dylib" "$out/libclokeos_compat.dylib" 2>/dev/null || true

# libffi: PureDarwin hat nur libffi.a; Rubys fiddle (Homebrew) lädt /usr/lib/libffi.dylib
"$DEBUGFS" -R "dump /lib/libffi.a $out/libffi.a" "$fs" >/dev/null 2>&1
[ -s "$out/libffi.a" ] || { echo "libffi.a nicht aus dem Image geholt" >&2; exit 1; }
clang -target "$target" -dynamiclib -nostdlib -Wl,-no_fixup_chains -Wl,-platform_version,macos,11.0,11.5 \
  -compatibility_version 27.0.0 -current_version 27.0.0 -install_name /usr/lib/libffi.dylib \
  -Wl,-force_load,"$out/libffi.a" -o "$out/libffi.dylib" "$out/libSystem.B.dylib"
codesign --force -s - "$out/libffi.dylib" 2>/dev/null || true
cp "$out/libffi.dylib" "$stage/usr/lib/libffi.dylib"
cp "$out/libclokeos_compat.dylib" "$stage$inst"
cp "$out/libSystem.B.dylib" "$stage/usr/lib/libSystem.B.dylib"
cp "$out/libdyld.dylib" "$stage/usr/lib/system/libdyld.dylib"
chmod 755 "$stage$inst" "$stage/usr/lib/libSystem.B.dylib" "$stage/usr/lib/system/libdyld.dylib"
echo "Fertig: libclokeos_compat für $CLOKE_ARCH"
