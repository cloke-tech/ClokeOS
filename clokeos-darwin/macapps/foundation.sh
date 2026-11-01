#!/bin/bash
# Foundation-Probe: baut Darlings CoreFoundation und Foundation als Mac-Frameworks für ClokeOS.
#
#   macapps/foundation.sh        bauen nach ~/ClokeOS-work/out/macapps/root/
#   CLOKEOS_ARCH=arm64 macapps/foundation.sh      arm64, nach ~/ClokeOS-work/out/macapps-arm64/root/
#
# Übersetzt wird auf dem Mac mit Apples clang und Apples SDK-Headern, wie ein Mac-Programm für
# macOS 11. Gelinkt wird gegen die echten Bibliotheken aus PureDarwins Image (libSystem, libdyld,
# libobjc, ICU), damit nur Symbole benutzt werden, die es auf ClokeOS gibt. Die Frameworks bekommen
# Apples Install-Namen und Re-Exporte (Foundation re-exportiert CoreFoundation und libobjc).
#
# Das Ergebnis ersetzt nichts im System. Es liegt im Gast unter /Library/ClokeOS/MacFrameworks
# und wird mit DYLD_FRAMEWORK_PATH benutzt; launchd, configd usw. behalten PureDarwins
# CoreFoundation (/usr/lib/libCoreFoundation.dylib). Zwei CFs in einem Prozess darf es nicht geben.
#
# arm64: Darlings CoreFoundation hat Weiterleitung (_CF_forwarding_prep_*) und NSInvocation
# (__invoke__) nur in x86- und arm32-Assembler und rechnet NSMethodSignature nach der x86_64-
# Aufrufkonvention. Für arm64 ersetzen cfarm64.S und cfarm64.m die beiden .S-Dateien, und
# patches/arm64/darling-corefoundation/NSMethodSignature.m.sed und NSInvocation.m.sed hängen die
# arm64-Fassung (Apples Konvention: Stack-Packing, HFA, Strukturen per Verweis, x8) dort ein.
#
# Quellen (Download nach Rückfrage am 20.10.2026, je git clone --depth 1):
#   ~/ClokeOS-work/download/darling/darling-corefoundation   github.com/darlinghq/darling-corefoundation
#   ~/ClokeOS-work/download/darling/darling-foundation       github.com/darlinghq/darling-foundation
# Was dafür an Apple-Headern und libSystem fehlt, ergänzen macapps/include, syscompat.c,
# cfcompat.m und fndcompat.m.
set -euo pipefail
source "$(dirname "$0")/../config.sh"
CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS" clang xcrun

M="$HERE/macapps"
CF="$WORK/download/darling/darling-corefoundation"
FND="$WORK/download/darling/darling-foundation"
for d in "$CF" "$FND"; do
  [ -d "$d" ] || { echo "Fehlt: $d (git clone --depth 1 https://github.com/darlinghq/$(basename "$d"))" >&2; exit 1; }
done
SDK="$(xcrun --show-sdk-path)"
# Je Architektur eigener Ausgabeordner (x86_64 behält out/macapps, wie probe-image.sh es erwartet)
# und Sysroot aus dem passenden Desktop-Image (cloakserver/build.sh legt es an).
# arm64: PureDarwins libSystem hat kein ___chkstk_darwin (-fno-stack-check), dyld ohne chained
# fixups (-no_fixup_chains).
TARGET="$CLOKE_ARCH-apple-macos11.0"
out="$WORK/out/macapps"
ARCHCC=() ARCHLD=()
if [ "$CLOKE_ARCH" = arm64 ]; then
  out="$WORK/out/macapps-arm64"
  ARCHCC=(-fno-stack-check) ARCHLD=(-Wl,-no_fixup_chains)
fi
obj="$out/obj"
root="$out/root"
fwdir="/Library/ClokeOS/MacFrameworks"
sysroot="$WORK/sysroot/$CLOKE_ARCH-${PD_SHA256:0:12}"
pd="$out/pdlibs"
JOBS="$(sysctl -n hw.ncpu)"

# --- Bibliotheken und Header aus dem Image -----------------------------------------------------
read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"
mkdir -p "$pd/usr/lib/system" "$pd/lib" "$obj"
for p in /usr/lib/libSystem.B.dylib /usr/lib/system/libdyld.dylib /usr/lib/libobjc.A.dylib /usr/lib/libicucore.A.dylib \
         /usr/lib/libicuuc.76.1.dylib /usr/lib/libicui18n.76.1.dylib /usr/lib/libicudata.76.1.dylib \
         /usr/lib/libz.1.dylib /lib/libxml2.a; do
  [ -s "$pd$p" ] || "$DEBUGFS" -R "dump $p $pd$p" "$fs" >/dev/null 2>&1 || true
  [ -s "$pd$p" ] || { echo "Nicht aus dem Image geholt: $p" >&2; exit 1; }
done
[ -d "$sysroot/include/unicode" ] || { echo "Fehlt: $sysroot (zuerst cloakserver/build.sh)" >&2; exit 1; }
# ICU-Header aus dem Image (ICU 76 ohne Versions-Suffix, wie libicucore sie exportiert)
mkdir -p "$obj/icu" "$obj/cfhdr"
ln -sfn "$sysroot/include/unicode" "$obj/icu/unicode"
# Darlings öffentliche CF-Header sind Symlinks in ein Untermodul (swift-corelibs-foundation), das
# nicht gebraucht wird; an ihre Stelle treten Apples Header aus dem SDK.
for f in "$CF"/include/CoreFoundation/*; do
  [ -L "$f" ] && [ ! -e "$f" ] && ln -sfn "$SDK/System/Library/Frameworks/CoreFoundation.framework/Headers/$(basename "$f")" "$obj/cfhdr/"
done

# -ffile-prefix-map: keine Pfade dieses Macs (__FILE__ in Assertions) in den Frameworks
CC=(clang -target "$TARGET" -isysroot "$SDK" -O2 -g0 -w -ffile-prefix-map="$WORK/"= -ffile-prefix-map="$HERE/"= -fblocks -fexceptions -fobjc-exceptions
    -fconstant-cfstrings -Xclang -fno-constant-nsarray-literals -Xclang -fno-constant-nsdictionary-literals
    -Xclang -fno-constant-nsnumber-literals
    -include "$M/include/cloke_cf_compat.h" -include "$CF/CoreFoundation_Prefix.h" -include "$CF/macros.h"
    -I"$obj/cfhdr" -I"$CF" -I"$CF/include" -I"$M/include" -I"$FND/include" -I"$FND/internal_include" -I"$FND/src"
    -I"$obj/icu" -iframework "$SDK/System/Library/Frameworks/CoreServices.framework/Frameworks"
    -idirafter "$sysroot/usr/include" -idirafter "$sysroot/include/libxml2"
    -DDARLING=1 -DINCLUDE_OBJC -DDEPLOYMENT_TARGET_MACOSX=1 -D__CONSTANT_CFSTRINGS__=1 -D__CONSTANT_STRINGS__=1
    -DOBJC_OLD_DISPATCH_PROTOTYPES=1 ${ARCHCC[@]+"${ARCHCC[@]}"})
LD=(clang -target "$TARGET" -dynamiclib -nostdlib -Wl,-platform_version,macos,11.0,11.5
    -Wl,-syslibroot,"$pd" -Wl,-dead_strip_dylibs -Wl,-unexported_symbols_list,"$M/syscompat.unexp"
    ${ARCHLD[@]+"${ARCHLD[@]}"})

# compile_all <objdir> <quellordner> <liste> <flags…>: übersetzt alle Dateien der Liste parallel,
# .c als Objective-C (wie Darling), .S ohne die vorab geladenen Header. Neu übersetzt wird nur,
# was fehlt, sich geändert hat oder andere Flags bekommt.
compile_all() {
  local objdir="$1" base="$2" list="$3"; shift 3
  mkdir -p "$objdir"
  local flags; flags="$(printf '%q ' "${CC[@]}" "$@")"
  [ -f "$objdir/.flags" ] && [ "$(cat "$objdir/.flags")" = "$flags" ] || printf '%s' "$flags" > "$objdir/.flags"
  OBJDIR="$objdir" BASE="$base" M="$M" xargs -P "$JOBS" -n 1 bash -c '
    s="$0"; src="$s"; case "$s" in /*) ;; *) src="$BASE/$s" ;; esac
    o="$OBJDIR/$(basename "$s").o"
    [ -f "$o" ] && [ "$o" -nt "$src" ] && [ "$o" -nt "$OBJDIR/.flags" ] && [ -z "$(find "$M/include" -newer "$o" -print -quit)" ] && exit 0
    eval "flags=($(cat "$OBJDIR/.flags"))"
    case "$s" in
      *.c) flags+=(-x objective-c) ;;
      *.S) f2=(); skip=0
           for a in "${flags[@]}"; do if [ $skip = 1 ]; then skip=0; continue; fi; [ "$a" = -include ] && { skip=1; continue; }; f2+=("$a"); done
           flags=("${f2[@]}") ;;
    esac
    "${flags[@]}" -c "$src" -o "$o" 2> "$o.log" || { echo "FEHLER: $s" >&2; head -25 "$o.log" >&2; exit 255; }
  ' < "$list"
}

# bundle <Name> <Version> <dylib> <Info.plist>: Framework-Ordner wie bei Apple
bundle() {
  local name="$1" ver="$2" bin="$3" plist="$4" d="$root$fwdir/$1.framework"
  rm -rf "$d"
  mkdir -p "$d/Versions/$ver/Resources"
  cp "$bin" "$d/Versions/$ver/$name"
  cp "$plist" "$d/Versions/$ver/Resources/Info.plist"
  ln -s "$ver" "$d/Versions/Current"
  ln -s "Versions/Current/$name" "$d/$name"
  ln -s "Versions/Current/Resources" "$d/Resources"
}

# patch_list <repo> <liste>: Einträge mit Korrektur in patches/<repo>/ oder patches/arm64/<repo>/
# durch eine korrigierte Kopie ersetzen (siehe patches/README; gibt es beide, gelten beide). Die
# Kopie wird nur neu geschrieben, wenn sich etwas ändert.
patch_list() {
  local repo="$1" list="$2" base="$WORK/download/darling/$1" s p dst
  while read -r s; do
    p=()
    [ -f "$M/patches/$repo/$s.sed" ] && p+=(-f "$M/patches/$repo/$s.sed")
    [ -f "$M/patches/arm64/$repo/$s.sed" ] && p+=(-f "$M/patches/arm64/$repo/$s.sed")
    if [ ${#p[@]} -gt 0 ]; then
      dst="$obj/patched/$repo/$s"
      mkdir -p "$(dirname "$dst")"
      sed -E "${p[@]}" "$base/$s" > "$dst.neu"
      if cmp -s "$dst.neu" "$dst"; then rm "$dst.neu"; else mv "$dst.neu" "$dst"; fi
      # #include "…" sucht zuerst neben der Datei: die Header des Originalordners dorthin verlinken
      # (Foundations src/CFInternal.h verdeckt sonst nicht mehr die von CoreFoundation).
      for h in "$(dirname "$base/$s")"/*.h; do
        [ -e "$(dirname "$dst")/${h##*/}" ] || [ -L "$(dirname "$dst")/${h##*/}" ] || ln -s "$h" "$(dirname "$dst")/"
      done
      echo "$dst"
    else
      echo "$s"
    fi
  done < "$list" > "$list.p"
  mv "$list.p" "$list"
}

# --- CoreFoundation ----------------------------------------------------------------------------
echo "Baue CoreFoundation"
{ sed -n '/^set(cf_c_sources/,/^)/p; /^set(cf_sources/,/^)/p' "$CF/CMakeLists.txt" |
    grep -oE '^[[:space:]]+[A-Za-z0-9_+-]+\.(c|m|S)' | sed -E 's/^[[:space:]]+//' | sort -u |
    { if [ "$CLOKE_ARCH" = arm64 ]; then grep -vxE 'CFForwardingPrep\.S|NSInvoke-x86\.S'; else cat; fi; }
  echo "$M/cfcompat.m"; echo "$M/syscompat.c"
  [ "$CLOKE_ARCH" != arm64 ] || { echo "$M/cfarm64.S"; echo "$M/cfarm64.m"; }; } > "$obj/cf.list"
patch_list darling-corefoundation "$obj/cf.list"
compile_all "$obj/cf" "$CF" "$obj/cf.list" -DCF_BUILDING_CF -DU_SHOW_DRAFT_API=1 -DU_SHOW_CPLUSPLUS_API=0 -DDISPATCH_SUPPORT=1
"${LD[@]}" -o "$obj/CoreFoundation" "$obj"/cf/*.o \
  -install_name /System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation \
  -compatibility_version 150 -current_version 1153.18 -Wl,-init,___CFInitialize -Wl,-alias_list,"$CF/SymbolAliases" \
  -Wl,-sectcreate,__UNICODE,__csbitmaps,"$CF/CFCharacterSetBitmaps.bitmap" \
  -Wl,-sectcreate,__UNICODE,__properties,"$CF/CFUniCharPropertyDatabase.data" \
  -Wl,-sectcreate,__UNICODE,__data,"$CF/CFUnicodeData-L.mapping" -Wl,-segprot,__UNICODE,r,r \
  -Wl,-reexported_symbols_list,"$CF/reexport_x86_64.exp" \
  -Wl,-alias,'_OBJC_CLASS_$___NSCFConstantString',___CFConstantStringClassReference \
  "$pd/usr/lib/libSystem.B.dylib" "$pd/usr/lib/system/libdyld.dylib" "$pd/usr/lib/libobjc.A.dylib" \
  "$pd/usr/lib/libicucore.A.dylib" 2>&1 | grep -v '^ld: warning' || true
[ -f "$obj/CoreFoundation" ] || exit 1
bundle CoreFoundation A "$obj/CoreFoundation" "$CF/Info.plist"

# --- Foundation --------------------------------------------------------------------------------
# NSNetServices (Bonjour) braucht CFNetwork und fehlt in der Probe.
echo "Baue Foundation"
{ sed -n '/^set(foundation_sources/,/^)/p' "$FND/CMakeLists.txt" |
    grep -oE '^[[:space:]]+[A-Za-z0-9_/+.-]+\.(c|m)' | sed -E 's/^[[:space:]]+//' | grep -v NSNetServices | sort -u
  echo "$M/fndcompat.m"; echo "$M/fndurlcomponents.m"; echo "$M/syscompat.c"; } > "$obj/fnd.list"
patch_list darling-foundation "$obj/fnd.list"
compile_all "$obj/fnd" "$FND" "$obj/fnd.list" -include "$M/include/cloke_fnd_compat.h" \
  -I"$FND/include/Foundation" -DNSBUILDINGFOUNDATION=1
"${LD[@]}" -o "$obj/Foundation" "$obj"/fnd/*.o \
  -install_name /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation \
  -compatibility_version 300 -current_version 1349 \
  -Wl,-reexport_library,"$obj/CoreFoundation" -Wl,-reexport_library,"$pd/usr/lib/libobjc.A.dylib" \
  "$pd/usr/lib/libSystem.B.dylib" "$pd/usr/lib/system/libdyld.dylib" "$pd/lib/libxml2.a" "$pd/usr/lib/libz.1.dylib" \
  "$pd/usr/lib/libicucore.A.dylib" 2>&1 | grep -v '^ld: warning' || true
[ -f "$obj/Foundation" ] || exit 1
bundle Foundation C "$obj/Foundation" "$FND/Info.plist"

echo "Fertig: $root$fwdir"
ls -l "$root$fwdir"/*.framework/Versions/*/{CoreFoundation,Foundation} 2>/dev/null | awk '{print $5, $NF}'
