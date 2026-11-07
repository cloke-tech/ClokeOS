#!/bin/bash
# AppKit-Probe (Phase 3): baut Darlings Grafik-Frameworks aus darling-cocotron als Mac-Frameworks
# für ClokeOS, auf den CoreFoundation und Foundation aus foundation.sh.
#
#   macapps/appkit.sh        bauen nach ~/ClokeOS-work/out/macapps/root/
#   CLOKEOS_ARCH=arm64 macapps/appkit.sh      arm64, nach ~/ClokeOS-work/out/macapps-arm64/root/
#
# Gleiches Vorgehen wie foundation.sh: Apples clang und SDK-Header, gelinkt gegen die echten
# Bibliotheken aus PureDarwins Image (libSystem, libobjc, libX11, Xext, Xrandr, Xcursor,
# fontconfig, FreeType, libpng und libjpeg statisch), mit Apples Install-Namen, Versionen und
# Re-Exporten. Es entstehen unter /Library/ClokeOS/MacFrameworks:
#
#   Onyx2D            Software-Renderer (Darlings Fassung, privat)
#   CoreGraphics      + Resources/Backends/X11.backend (CGS über X11)
#   CoreText, CoreImage, QuartzCore (re-exportiert CoreImage), CoreData
#   ImageIO           CGImageSource/CGImageDestination über Onyx2D (appkit/imageio.m)
#   ApplicationServices  re-exportiert CoreGraphics, CoreText, ImageIO
#   AppKit            Versions/C, compat 45, re-exportiert Foundation und ApplicationServices,
#                     + Resources/Backends/X11.backend (X11Display, X11Window …)
#   Cocoa             re-exportiert AppKit und CoreData
#
# PureDarwins eigenes Onyx2D und CoreGraphics bleiben im System. Programme bekommen diese
# Fassungen nur über DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks.
#
# AppKit zeichnet ohne OpenGL: X11Window kopiert das Onyx2D-Bild per XShmPutImage oder XPutImage
# ins Fenster (appkit/x11present.m, eingebunden über patches/darling-cocotron/…/X11Window.*.sed).
# Das Hauptmenü steht in CloakServers Menüleiste statt im Fenster (appkit/clokemenu.m).
# libxkbfile fehlt in PureDarwin; die eine Funktion daraus steht in appkit/xkbrules.c. Gegen
# OpenGL und IOKit wird nicht gelinkt (Mesa mit LLVM beim Start, zweite CoreFoundation); die
# wenigen Aufrufe bekommen Platzhalter ohne Export (appkit/sysstubs.c).
#
# arm64: Im arm64-Image gibt es Xlib und die X-Erweiterungen nur als statische Archive. Die beiden
# X11.backend-Bundles bekommen deshalb je eine eigene statische Xlib (wie CloakServer); jedes
# öffnet ohnehin seine eigene X-Verbindung.
#
# Quelle (Download nach Rückfrage am 20.10.2026, git clone --depth 1, Commit d89d7aa):
#   ~/ClokeOS-work/download/darling/darling-cocotron   github.com/darlinghq/darling-cocotron
set -euo pipefail
source "$(dirname "$0")/../config.sh"
CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS" clang xcrun

M="$HERE/macapps"
COC="$WORK/download/darling/darling-cocotron"
CF="$WORK/download/darling/darling-corefoundation"
FND="$WORK/download/darling/darling-foundation"
[ -d "$COC" ] || { echo "Fehlt: $COC (git clone --depth 1 https://github.com/darlinghq/darling-cocotron)" >&2; exit 1; }
SDK="$(xcrun --show-sdk-path)"
# Ausgabe, Sysroot und Schalter je Architektur wie in foundation.sh
TARGET="$CLOKE_ARCH-apple-macos11.0"
out="$WORK/out/macapps"
ARCHCC=() ARCHLD=()
if [ "$CLOKE_ARCH" = arm64 ]; then
  out="$WORK/out/macapps-arm64"
  ARCHCC=(-fno-stack-check) ARCHLD=(-Wl,-no_fixup_chains)
fi
obj="$out/obj/appkit"
root="$out/root"
fwdir="/Library/ClokeOS/MacFrameworks"
fw="$root$fwdir"
sysroot="$WORK/sysroot/$CLOKE_ARCH-${PD_SHA256:0:12}"
pd="$out/pdlibs"
JOBS="$(sysctl -n hw.ncpu)"
for f in CoreFoundation.framework/Versions/A/CoreFoundation Foundation.framework/Versions/C/Foundation; do
  [ -f "$fw/$f" ] || { echo "Fehlt: $fw/$f (zuerst macapps/foundation.sh)" >&2; exit 1; }
done
[ -f "$sysroot/include/X11/Xlib.h" ] || { echo "Fehlt: $sysroot (zuerst cloakserver/build.sh)" >&2; exit 1; }

# --- Bibliotheken aus dem Image ----------------------------------------------------------------
read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"
mkdir -p "$pd/usr/lib/system" "$pd/lib" "$obj"
xlibs=(/lib/libX11.6.dylib /lib/libXext.6.dylib /lib/libXrandr.2.dylib /lib/libXcursor.1.dylib)
[ "$CLOKE_ARCH" = arm64 ] && xlibs=(/lib/libXcursor.a /lib/libXrandr.a /lib/libXrender.a /lib/libXfixes.a /lib/libXext.a
                                    /lib/libX11.a /lib/libxcb.a /lib/libXau.a /lib/libXdmcp.a)
for p in /usr/lib/libSystem.B.dylib /usr/lib/system/libdyld.dylib /usr/lib/libobjc.A.dylib /usr/lib/libz.1.dylib \
         "${xlibs[@]}" \
         /lib/libfontconfig.1.dylib /lib/libfreetype.6.20.6.dylib /lib/libpng16.a /lib/libjpeg.a; do  # libfreetype.6.dylib ist ein Symlink
  [ -s "$pd$p" ] || "$DEBUGFS" -R "dump $p $pd$p" "$fs" >/dev/null 2>&1 || true
  [ -s "$pd$p" ] || { echo "Nicht aus dem Image geholt: $p" >&2; exit 1; }
done
sys=("$pd/usr/lib/libSystem.B.dylib" "$pd/usr/lib/system/libdyld.dylib" "$pd/usr/lib/libobjc.A.dylib")
cf="$fw/CoreFoundation.framework/Versions/A/CoreFoundation"
fnd="$fw/Foundation.framework/Versions/C/Foundation"
x11=()
for p in "${xlibs[@]}"; do x11+=("$pd$p"); done
x11+=("$pd/lib/libfontconfig.1.dylib")

# --- Quellen: APFS-Klon von darling-cocotron mit den Korrekturen aus patches/ -------------------
# Ein Klon statt einzelner Kopien, weil auch Header korrigiert werden (X11Window.h), die andere
# Dateien mit "…" relativ zu sich selbst einbinden. Die heruntergeladenen Quellen bleiben
# unverändert. Korrigierte Dateien werden nur neu geschrieben, wenn sich der Inhalt ändert.
# Korrekturen aus patches/darling-cocotron/ und patches/arm64/darling-cocotron/ (gibt es für eine
# Datei beide, gelten beide).
src="$obj/src"
[ -d "$src" ] || cp -cR "$COC" "$src"
touch "$obj/patched.list"
P1="$M/patches/darling-cocotron" P2="$M/patches/arm64/darling-cocotron"
while read -r f; do   # Korrekturen, die es nicht mehr gibt, zurücknehmen
  [ -f "$P1/$f.sed" ] || [ -f "$P2/$f.sed" ] || cp -p "$COC/$f" "$src/$f"
done < "$obj/patched.list"
for d in "$P1" "$P2"; do
  [ -d "$d" ] && (cd "$d" && find . -name '*.sed' | sed -e 's|^\./||' -e 's|\.sed$||')
done | sort -u > "$obj/patched.list"
while read -r f; do
  p=()
  [ -f "$P1/$f.sed" ] && p+=(-f "$P1/$f.sed")
  [ -f "$P2/$f.sed" ] && p+=(-f "$P2/$f.sed")
  sed -E "${p[@]}" "$COC/$f" > "$src/$f.neu"
  if cmp -s "$src/$f.neu" "$src/$f"; then rm "$src/$f.neu"; else mv "$src/$f.neu" "$src/$f"; fi
done < "$obj/patched.list"

# --- Übersetzen ---------------------------------------------------------------------------------
# Darlings Framework-Header (-I) gehen Apples SDK-Frameworks vor. appkit/include ersetzt Darlings
# private Header (OpenGL/CGLInternal.h, X11/extensions/XKBrules.h), nimmt aus Apples CFCGTypes.h
# und ApplicationServices.h heraus, was mit Darling oder X11 kollidiert, und ergänzt OpenGL.h,
# ImageIO.h und Onyx2Ds Schalter für libtiff.
# -ffile-prefix-map: keine Pfade dieses Macs (__FILE__ in Assertions) in den Frameworks
CC=(clang -target "$TARGET" -isysroot "$SDK" -O2 -g0 -w -ffile-prefix-map="$WORK/"= -ffile-prefix-map="$HERE/"= -fblocks -fexceptions -fobjc-exceptions
    -fconstant-cfstrings -Xclang -fno-constant-nsarray-literals -Xclang -fno-constant-nsdictionary-literals
    -Xclang -fno-constant-nsnumber-literals
    -include math.h -include stdlib.h -include CoreFoundation/CoreFoundation.h -include Foundation/Foundation.h
    -I"$M/appkit/include"
    -I"$src/Onyx2D/include" -I"$src/CoreGraphics/include" -I"$src/CoreText/include" -I"$src/QuartzCore/include"
    -I"$src/AppKit/include" -I"$src/AppKit/private-include" -I"$src/Cocoa/include" -I"$src/CoreData/include"
    -I"$M/include" -I"$FND/include" -I"$CF/include" -I"$src"
    -iframework "$SDK/System/Library/Frameworks/CoreServices.framework/Frameworks"
    -iframework "$SDK/System/Library/Frameworks/ApplicationServices.framework/Frameworks"
    -iframework "$SDK/System/Library/Frameworks/Carbon.framework/Frameworks"
    -idirafter "$sysroot/include/freetype2" -idirafter "$sysroot/include" -idirafter "$sysroot/include/libpng16"
    -DDARLING=1 -DOBJC_OLD_DISPATCH_PROTOTYPES=1 -D__CONSTANT_CFSTRINGS__=1 ${ARCHCC[@]+"${ARCHCC[@]}"})
# Ohne -dead_strip_dylibs: ld-1230 wirft damit Re-Exporte weg, deren Symbole das Framework selbst
# nicht benutzt (Cocoa verlöre AppKit und CoreData).
LD=(clang -target "$TARGET" -dynamiclib -nostdlib -Wl,-platform_version,macos,11.0,11.5
    -Wl,-syslibroot,"$pd" -Wl,-unexported_symbols_list,"$M/appkit/sysstubs.unexp" ${ARCHLD[@]+"${ARCHLD[@]}"})

# compile_all <objdir> <quellordner> <liste> <flags…>: wie in foundation.sh. Übersetzt die Liste
# parallel (.c als Objective-C, wie Darling), neu nur, was fehlt, sich geändert hat oder andere
# Flags bekommt. Bricht beim ersten Fehler ab und zeigt dessen Meldungen.
compile_all() {
  local objdir="$1" base="$2" list="$3"; shift 3
  mkdir -p "$objdir"
  local flags; flags="$(printf '%q ' "${CC[@]}" "$@")"
  [ -f "$objdir/.flags" ] && [ "$(cat "$objdir/.flags")" = "$flags" ] || printf '%s' "$flags" > "$objdir/.flags"
  OBJDIR="$objdir" BASE="$base" M="$M" xargs -P "$JOBS" -n 1 bash -c '
    s="$0"; src="$s"; case "$s" in /*) ;; *) src="$BASE/$s" ;; esac
    o="$OBJDIR/$(basename "$s").o"
    [ -f "$o" ] && [ "$o" -nt "$src" ] && [ "$o" -nt "$OBJDIR/.flags" ] && [ -z "$(find "$M/appkit/include" -newer "$o" -print -quit)" ] && exit 0
    eval "flags=($(cat "$OBJDIR/.flags"))"
    case "$s" in *.c) flags+=(-x objective-c) ;; esac
    "${flags[@]}" -c "$src" -o "$o" 2> "$o.log" || { echo "FEHLER: $s" >&2; grep -m 8 -E "error" "$o.log" >&2; exit 255; }
  ' < "$list"
  # Objekte von Quellen, die nicht (mehr) in der Liste stehen, wegräumen: gelinkt wird alles in objdir, und ein
  # übrig gebliebenes Objekt einer früheren Fassung (z. B. carreader.c.o mit offenem inflate) bricht das Linken
  local keep f
  keep="$(sed -E 's#.*/##; s#$#.o#' "$list")"
  for f in "$objdir"/*.o; do
    [ -e "$f" ] || continue
    grep -qxF "$(basename "$f")" <<< "$keep" || rm -f "$f" "$f.log"
  done
}

# sources <Framework> <CMake-Liste> [ausgelassen…]: Quelldateien aus Darlings CMakeLists.txt
sources() {
  local name="$1" var="$2"; shift 2
  sed -n "/^set($var/,/^)/p" "$src/$name/CMakeLists.txt" | grep -vE '^[[:space:]]*#' |
    grep -oE '^[[:space:]]+[A-Za-z0-9_+/.-]+\.(c|m|mm)' | sed -E 's/^[[:space:]]+//' |
    { if [ $# -gt 0 ]; then grep -vxF -f <(printf '%s\n' "$@"); else cat; fi; } | sort -u
}

# plist <Name> <Bundle-ID> <Version> <Typ> <Datei> [Hauptklasse]
plist() {
  cat > "$5" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>English</string>
	<key>CFBundleExecutable</key><string>$1</string>
	<key>CFBundleIdentifier</key><string>$2</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>$1</string>
	<key>CFBundlePackageType</key><string>$4</string>
	<key>CFBundleShortVersionString</key><string>$3</string>
	<key>CFBundleVersion</key><string>$3</string>${6:+
	<key>NSPrincipalClass</key><string>$6</string>
	<key>NSPriority</key><integer>200</integer>}
</dict>
</plist>
EOF
}

# framework <Name> <Version> <compat> <current> <objdir> <ld-Argumente…>: linkt und legt das
# Framework-Bundle wie bei Apple an (Versions/<V>/<Name>, Resources, Symlinks).
framework() {
  local name="$1" ver="$2" compat="$3" cur="$4" o="$5"; shift 5
  local d="$fw/$name.framework" inst="/System/Library/Frameworks/$name.framework/Versions/$ver/$name"
  [ "$name" = Onyx2D ] && inst="/System/Library/PrivateFrameworks/$name.framework/Versions/$ver/$name"
  echo "Linke $name"
  rm -f "$obj/$name"
  "${LD[@]}" -o "$obj/$name" "$o"/*.o -install_name "$inst" -compatibility_version "$compat" -current_version "$cur" \
    "$@" 2>&1 | grep -v '^ld: warning' || true
  [ -f "$obj/$name" ] || exit 1
  rm -rf "$d"
  mkdir -p "$d/Versions/$ver/Resources"
  cp "$obj/$name" "$d/Versions/$ver/$name"
  plist "$name" "com.apple.$name" "$cur" FMWK "$d/Versions/$ver/Resources/Info.plist"
  ln -s "$ver" "$d/Versions/Current"
  ln -s "Versions/Current/$name" "$d/$name"
  ln -s "Versions/Current/Resources" "$d/Resources"
}

# backend <Framework> <Version> <objdir> <ld-Argumente…>: Resources/Backends/X11.backend, das
# Darlings NSDisplay (AppKit) und CGS (CoreGraphics) beim Start mit NSBundle laden.
backend() {
  local name="$1" ver="$2" o="$3"; shift 3
  local d="$fw/$name.framework/Versions/$ver/Resources/Backends/X11.backend/Contents"
  local principal=X11Display; [ "$name" = CoreGraphics ] && principal=CGSConnectionX11
  echo "Linke $name X11.backend"
  rm -f "$obj/X11-$name"
  "${LD[@]}" -o "$obj/X11-$name" "$o"/*.o \
    -install_name "/System/Library/Frameworks/$name.framework/Versions/$ver/Resources/Backends/X11.backend/Contents/MacOS/X11" \
    "$@" 2>&1 | grep -v '^ld: warning' || true
  [ -f "$obj/X11-$name" ] || exit 1
  mkdir -p "$d/MacOS"
  cp "$obj/X11-$name" "$d/MacOS/X11"
  plist X11 "org.darlinghq.backends.X11" 1.0 BNDL "$d/Info.plist" "$principal"
}

fwlib() { echo "$obj/$1"; }   # frisch gelinktes Framework zum Linken der nächsten

# --- Onyx2D ------------------------------------------------------------------------------------
# Ohne GIF (giflib fehlt in PureDarwin), TIFF mit Cocotrons eigenem Decoder statt libtiff.
echo "Baue Onyx2D"
sources Onyx2D Onyx2D_sources O2ImageSource_GIF.m > "$obj/o2.list"
compile_all "$obj/o2" "$src/Onyx2D" "$obj/o2.list" -I"$src/Onyx2D" -I"$src/CoreText"
framework Onyx2D A 1 1 "$obj/o2" "$cf" "$fnd" "${sys[@]}" "$pd/usr/lib/libz.1.dylib" "$pd/lib/libpng16.a" "$pd/lib/libjpeg.a" \
  "$pd/lib/libfreetype.6.20.6.dylib" "$pd/lib/libfontconfig.1.dylib"

# --- CoreGraphics ------------------------------------------------------------------------------
echo "Baue CoreGraphics"
# CGGradient.m ist ein Platzhalter (NULL); appkit/cggradient.m setzt CGGradient und die Verlaufs-/Kachelfunktionen um
sources CoreGraphics CoreGraphics_sources | grep -vE '(^|/)CGGradient\.m$' > "$obj/cg.list"
printf "%s\n" "$M/appkit/sysstubs.c" "$M/appkit/cgsprivate.c" "$M/appkit/cggradient.m" >> "$obj/cg.list"
compile_all "$obj/cg" "$src/CoreGraphics" "$obj/cg.list" -I"$src/CoreGraphics" -I"$src/CoreText"
framework CoreGraphics A 64 1070.22 "$obj/cg" "$(fwlib Onyx2D)" "$cf" "$fnd" "${sys[@]}"

# --- CoreText ----------------------------------------------------------------------------------
echo "Baue CoreText"
# CTLine.m/CTRun.m/CTFrame.m/CTFrameSetter.m sind nur Platzhalter (NULL-Ergebnisse); appkit/ctline.m setzt sie um
sources CoreText CoreText_sources | grep -vE '(^|/)CT(Line|Run|Frame|FrameSetter)\.m$' > "$obj/ct.list"
echo "$M/appkit/ctline.m" >> "$obj/ct.list"
compile_all "$obj/ct" "$src/CoreText" "$obj/ct.list" -I"$src/CoreText"
framework CoreText A 1 1 "$obj/ct" "$(fwlib CoreGraphics)" "$(fwlib Onyx2D)" "$cf" "$fnd" "${sys[@]}" \
  "$pd/lib/libfreetype.6.20.6.dylib"

# --- ImageIO (über Onyx2D) und ApplicationServices (nur Re-Exporte) ---------------------------
echo "Baue ImageIO"
echo "$M/appkit/imageio.m" > "$obj/iio.list"
compile_all "$obj/iio" / "$obj/iio.list"
framework ImageIO A 1 1 "$obj/iio" "$(fwlib Onyx2D)" "$(fwlib CoreGraphics)" "$cf" "$fnd" "${sys[@]}"
echo "Baue ApplicationServices"
mkdir -p "$obj/as"
printf 'static const char cloke_as[] __attribute__((used)) = "ClokeOS ApplicationServices";\n' > "$obj/as/as.c"
clang -target "$TARGET" -isysroot "$SDK" -c "$obj/as/as.c" -o "$obj/as/as.o"
framework ApplicationServices A 1 66 "$obj/as" -Wl,-reexport_library,"$(fwlib CoreGraphics)" \
  -Wl,-reexport_library,"$(fwlib CoreText)" -Wl,-reexport_library,"$(fwlib ImageIO)" "${sys[@]}"

# --- CoreImage (die CI-Dateien aus Darlings QuartzCore-Ordner) und QuartzCore -----------------
# Wie bei Apple liegen CIContext, CIImage usw. in CoreImage, und QuartzCore re-exportiert es.
echo "Baue CoreImage"
(cd "$src/QuartzCore" && ls CI*.m) > "$obj/ci.list"
compile_all "$obj/ci" "$src/QuartzCore" "$obj/ci.list" -I"$src/QuartzCore" -I"$src/QuartzCore/private-include"
framework CoreImage A 1 1 "$obj/ci" "$(fwlib CoreGraphics)" "$cf" "$fnd" "${sys[@]}"
# QuartzCore ohne CAMetalLayer/CAMetalDrawable (kein Metal). Apple re-exportiert auch libobjc.
echo "Baue QuartzCore"
sources QuartzCore QuartzCore_sources CAMetalLayer.mm CAMetalDrawable.mm > "$obj/qc.list"
echo "$M/appkit/sysstubs.c" >> "$obj/qc.list"
compile_all "$obj/qc" "$src/QuartzCore" "$obj/qc.list" -I"$src/QuartzCore" -I"$src/QuartzCore/private-include"
# libobjc darf dafür nicht zusätzlich als normale Abhängigkeit dastehen (${sys[@]}).
framework QuartzCore A 1.2 1.11 "$obj/qc" "$(fwlib CoreGraphics)" "$(fwlib Onyx2D)" "$cf" "$fnd" \
  -Wl,-reexport_library,"$(fwlib CoreImage)" -Wl,-reexport_library,"$pd/usr/lib/libobjc.A.dylib" \
  "$pd/usr/lib/libSystem.B.dylib" "$pd/usr/lib/system/libdyld.dylib"

# --- CoreData ----------------------------------------------------------------------------------
echo "Baue CoreData"
sources CoreData CoreData_sources > "$obj/cd.list"
compile_all "$obj/cd" "$src/CoreData" "$obj/cd.list" -I"$src/CoreData"
framework CoreData A 1 1 "$obj/cd" "$cf" "$fnd" "${sys[@]}"

# --- AppKit ------------------------------------------------------------------------------------
echo "Baue AppKit"
ak=(-I"$src/AppKit")
for d in nib.subproj NSColorPicker.subproj NSMenu.subproj NSTextView.subproj NSEvent.subproj NSColor.subproj \
         RTF.subproj NSToolbar.subproj NSDrawer.subproj X11.backend; do ak+=(-I"$src/AppKit/$d"); done
sources AppKit AppKit_sources > "$obj/ak.list"
printf '%s\n' "$M/appkit/sysstubs.c" "$M/appkit/nibarchive.m" "$M/appkit/modern.m" >> "$obj/ak.list"
compile_all "$obj/ak" "$src/AppKit" "$obj/ak.list" "${ak[@]}"
framework AppKit C 45 1499.11 "$obj/ak" -Wl,-reexport_library,"$fnd" -Wl,-reexport_library,"$(fwlib ApplicationServices)" \
  "$(fwlib Onyx2D)" "$(fwlib CoreGraphics)" "$(fwlib CoreText)" "$(fwlib ImageIO)" "$(fwlib QuartzCore)" \
  "$(fwlib CoreData)" "$(fwlib CoreImage)" "$cf" "${sys[@]}" "$pd/lib/libfreetype.6.20.6.dylib" "$pd/lib/libfontconfig.1.dylib"
# Bilder, Nibs und Tastenbelegung wie in Darlings CMakeLists.txt (Ziel Quelle)
sed -n '/^set(AppKit_resources/,/^)/p' "$src/AppKit/CMakeLists.txt" | grep -E '^[[:space:]]+[^[:space:]#]+[[:space:]]+[^[:space:]]+[[:space:]]*$' |
  while read -r dst s; do
    mkdir -p "$(dirname "$fw/AppKit.framework/Versions/C/Resources/$dst")"
    cp "$src/AppKit/$s" "$fw/AppKit.framework/Versions/C/Resources/$dst"
  done

# --- Cocoa -------------------------------------------------------------------------------------
echo "Baue Cocoa"
sources Cocoa Cocoa_sources > "$obj/cc.list"
compile_all "$obj/cc" "$src/Cocoa" "$obj/cc.list"
framework Cocoa A 1 22 "$obj/cc" -Wl,-reexport_library,"$(fwlib AppKit)" -Wl,-reexport_library,"$(fwlib CoreData)" \
  "$fnd" "${sys[@]}"

# --- X11-Backends ------------------------------------------------------------------------------
echo "Baue X11-Backends"
sed -n '/^add_backend(X11/,/INFO_PLIST/p' "$src/AppKit/CMakeLists.txt" | grep -oE 'X11\.backend/[A-Za-z0-9_]+\.m' > "$obj/akx.list"
printf '%s\n' "$M/appkit/x11present.m" "$M/appkit/clokemenu.m" "$M/appkit/nsbundle_images.m" "$M/appkit/lenient.m" "$M/appkit/layout.m" "$M/appkit/softlayers.m" "$M/appkit/appearance.m" "$M/appkit/titlebar.m" "$M/appkit/xkbrules.c" "$M/appkit/sysstubs.c" >> "$obj/akx.list"
compile_all "$obj/akx" "$src/AppKit" "$obj/akx.list" "${ak[@]}"
backend AppKit C "$obj/akx" "$(fwlib AppKit)" "$(fwlib Onyx2D)" "$(fwlib CoreGraphics)" "$(fwlib QuartzCore)" \
  "$fnd" "$cf" "${sys[@]}" "${x11[@]}"
sed -n '/^add_backend(X11/,/INFO_PLIST/p' "$src/CoreGraphics/X11.backend/CMakeLists.txt" | grep -oE '[A-Za-z0-9_]+\.m' |
  sed 's|^|X11.backend/|' > "$obj/cgx.list"
echo "$M/appkit/xkbrules.c" >> "$obj/cgx.list"
compile_all "$obj/cgx" "$src/CoreGraphics" "$obj/cgx.list" -I"$src/CoreGraphics" -I"$src/CoreGraphics/X11.backend"
backend CoreGraphics A "$obj/cgx" "$(fwlib CoreGraphics)" "$fnd" "$cf" "${sys[@]}" "${x11[@]}"

echo "Fertig: $fw"
for n in Onyx2D CoreGraphics CoreText ImageIO ApplicationServices CoreImage QuartzCore CoreData AppKit Cocoa; do
  printf '%9s  %s\n' "$(stat -f %z "$obj/$n")" "$n"
done
