#!/bin/bash
# Baut PureDarwins libobjc (objc4-781, src/Libraries/objc4 aus dem PDSRC-Tarball) mit libobjc/dataconst.patch neu:
#
#   libobjc/build.sh       Ergebnis: $WORK/out/libobjc-<arch>/libobjc.A.dylib   (Architektur wie überall mit CLOKEOS_ARCH)
#
# Warum: Programme, die mit einem neueren Xcode gebaut sind (Logic Pro), legen __objc_protorefs/__objc_classrefs nach __DATA_CONST,
# das dyld nach dem Binden schreibgeschützt macht; das alte libobjc schreibt diese Verweise in _read_images um und stürzte mit SIGBUS
# ab (docs/logic-launch.md, Runde 12). Die Rezeptur folgt nix/pkgs/apple/libobjc.nix des PDSRC-Baums; statt des Nix-Toolchains werkelt
# das Xcode-clang des Macs. Nur arm64 getestet; x86_64 baut dieselben Quellen, wurde aber nicht gestartet.
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need clang xcrun patch "$DEBUGFS"
fetch_file "$PDSRC_URL" "$PDSRC_SHA256" "$PDSRC_TGZ" >/dev/null
w="$WORK/tmp/libobjc-$CLOKE_ARCH"; out="$WORK/out/libobjc-$CLOKE_ARCH"
rm -rf "$w"; mkdir -p "$w/pd/system" "$out"
tar -xzf "$PDSRC_TGZ" -C "$w" --strip-components=1 '*/src/Libraries/objc4'
patch -s -p1 -d "$w" < "$HERE/libobjc/dataconst.patch"
# PureDarwins eigene libSystem, libdyld und libc++abi zum Binden
read -r rs _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((rs * 512))"
"$DEBUGFS" -R "dump /usr/lib/libSystem.B.dylib $w/pd/libSystem.B.dylib" "$fs" >/dev/null 2>&1
"$DEBUGFS" -R "dump /usr/lib/system/libdyld.dylib $w/pd/system/libdyld.dylib" "$fs" >/dev/null 2>&1
"$DEBUGFS" -R "dump /usr/lib/libc++abi.dylib $w/pd/libc++abi.dylib" "$fs" >/dev/null 2>&1
[ -s "$w/pd/libc++abi.dylib" ] || { echo "libc++abi nicht aus dem Image geholt" >&2; exit 1; }
sdk=$(xcrun --show-sdk-path)
target=arm64-apple-macos11.0; asm="Messengers.subproj/objc-msg-arm64 objc-blocktramps-arm64 objc-sel-table"
if [ "$CLOKE_ARCH" = x86_64 ]; then target=x86_64-apple-macos11.0; asm="Messengers.subproj/objc-msg-x86_64 objc-blocktramps-x86_64 objc-sel-table"; fi
O="$w/src/Libraries/objc4"
PRIV="$O/macosx.internal/System/Library/Frameworks/System.framework/Versions/B/PrivateHeaders"
mkdir -p "$w/incdir/objc" "$w/genhdr" "$w/o"
cp -a "$O"/runtime/*.h "$w/incdir/objc/"
cat > "$w/genhdr/objc-probes.h" <<'EOF'
#ifndef _OBJC_PROBES_H
#define _OBJC_PROBES_H
#define OBJC_RUNTIME_OBJC_EXCEPTION_THROW(arg0) do {} while (0)
#define OBJC_RUNTIME_OBJC_EXCEPTION_RETHROW() do {} while (0)
#define OBJC_RUNTIME_OBJC_EXCEPTION_THROW_ENABLED() (0)
#define OBJC_RUNTIME_OBJC_EXCEPTION_RETHROW_ENABLED() (0)
#endif
EOF
cxx=(-x objective-c++ -std=gnu++17 -fno-objc-arc -fexceptions -fno-delete-null-pointer-checks -fobjc-exceptions -fPIC -Os -DNDEBUG
     -DLIBC_NO_LIBCRASHREPORTERCLIENT -D__PUREDARWIN__=1 -Wno-undef-prefix -w -target "$target" -isysroot "$sdk"
     -I"$w/genhdr" -I"$w/incdir" -I"$O/runtime" -I"$PRIV")
srcs="hashtable2 maptable NSObject objc-accessors objc-auto objc-block-trampolines objc-cache objc-cache-old objc-class objc-class-old
 objc-errors objc-exception objc-file objc-file-old objc-initialize objc-layout objc-loadmethod objc-load objc-lockdebug objc-opt objc-os
 objc-references objc-runtime objc-runtime-new objc-runtime-old objc-sel objc-sel-old objc-sel-set objc-sync objc-typeencoding objc-weak
 objc-zalloc Object Protocol"
objs=()
for s in $srcs; do clang "${cxx[@]}" -c "$O/runtime/$s.mm" -o "$w/o/$s.o"; objs+=("$w/o/$s.o"); done
for s in $asm; do clang -target "$target" -isysroot "$sdk" -I"$O/runtime" -c "$O/runtime/$s.s" -o "$w/o/$(basename "$s").o"; objs+=("$w/o/$(basename "$s").o"); done
# ___chkstk_darwin liefert libcompat/chkstk.S (PureDarwins libSystem hat es nicht, und libobjc wird vor dem Patch von libSystem geladen)
clang -target "$target" -isysroot "$sdk" -c "$HERE/libcompat/chkstk.S" -o "$w/o/chkstk.o"; objs+=("$w/o/chkstk.o")
clang -target "$target" -isysroot "$sdk" -dynamiclib -nostdlib -Wl,-platform_version,macos,11.0,11.5 \
  -Wl,-install_name,/usr/lib/libobjc.A.dylib -Wl,-init,__objc_init -Wl,-sectalign,__DATA,__objc_data,0x1000 \
  -Wl,-order_file,"$O/libobjc.order" -Wl,-unexported_symbols_list,"$O/unexported_symbols" -Wl,-fixup_chains \
  -Wl,-dylib_file,/usr/lib/system/libdyld.dylib:"$w/pd/system/libdyld.dylib" -L"$w/pd" "$w/pd/libSystem.B.dylib" "$w/pd/libc++abi.dylib" \
  "${objs[@]}" -o "$out/libobjc.A.dylib" 2> >(grep -v warning >&2)
echo "$out/libobjc.A.dylib"
