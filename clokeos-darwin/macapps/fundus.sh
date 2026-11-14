#!/bin/bash
# Fundus 2 (docs/fundus.md): baut FundusKit.framework und Fundus.app mit Apples SDK (universal x86_64 + arm64, wie Hallo)
# in eine Stage. Zur Laufzeit gelten ClokeOS' Mac-Frameworks (Darlings AppKit).
#   macapps/fundus.sh <stage>
#     <stage>/Library/ClokeOS/MacFrameworks/FundusKit.framework   Browser, Panel, Icons (PNG), Übersetzungen
#     <stage>/System/Library/CoreServices/Fundus.app              ersetzt die Thunar-Hülle aus cloakserver/bundles.py
# Icons: fundus/icons.py schreibt SVG, fundus/render.swift macht auf dem Mac PNG daraus (Darlings AppKit lädt kein SVG).
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need clang swift python3
stage="$1"
F="$HERE/macapps/fundus"
tmp="$WORK/tmp/fundus"; rm -rf "$tmp"; mkdir -p "$tmp"
CC=(clang -arch x86_64 -arch arm64 -mmacosx-version-min=11.0 -O2 -Wall -Wno-deprecated-declarations
    -ffile-prefix-map="$HERE/"=)

# FundusKit.framework
fk="$stage/Library/ClokeOS/MacFrameworks/FundusKit.framework"
rm -rf "$fk"; mkdir -p "$fk/Versions/A/Resources"
"${CC[@]}" -fobjc-arc -dynamiclib -framework Cocoa \
  -install_name /Library/ClokeOS/MacFrameworks/FundusKit.framework/Versions/A/FundusKit \
  -compatibility_version 1 -current_version 2 -I"$F" \
  -o "$fk/Versions/A/FundusKit" "$F/FundusKit.m" "$F/fcore.c"
python3 -I "$F/icons.py" "$tmp/svg" >/dev/null
swift "$F/render.swift" "$tmp/svg" "$fk/Versions/A/Resources" >/dev/null
python3 -I "$F/strings.py" "$fk/Versions/A/Resources" >/dev/null
cat > "$fk/Versions/A/Resources/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.clokeos.FundusKit</string>
<key>CFBundleName</key><string>FundusKit</string>
<key>CFBundleExecutable</key><string>FundusKit</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleVersion</key><string>2</string>
</dict></plist>
PL
ln -s A "$fk/Versions/Current"; ln -s Versions/Current/FundusKit "$fk/FundusKit"; ln -s Versions/Current/Resources "$fk/Resources"

# Fundus.app: Icon und deutscher Name aus der bisherigen Hülle übernehmen (bundles.py hat sie schon angelegt)
app="$stage/System/Library/CoreServices/Fundus.app"
old="$tmp/oldbundle"
[ -d "$app" ] && cp -R "$app/Contents/Resources" "$old" 2>/dev/null || mkdir -p "$old"
rm -rf "$app"; mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
"${CC[@]}" -fobjc-arc -framework Cocoa -I"$F" "$fk/Versions/A/FundusKit" -o "$app/Contents/MacOS/Fundus" "$F/main.m"
cp -R "$old/." "$app/Contents/Resources/" 2>/dev/null || true
printf 'APPL????' > "$app/Contents/PkgInfo"
cat > "$app/Contents/Info.plist" <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleDisplayName</key><string>Fundus</string>
	<key>CFBundleExecutable</key><string>Fundus</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundleIdentifier</key><string>com.clokeos.fundus</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>Fundus</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>2.0</string>
	<key>CFBundleVersion</key><string>2</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PL
# Programm-Icons auch als PNG (Darlings AppKit lädt kein SVG; FundusKit zeigt so die echten Icons)
swift "$F/render.swift" --apps "$stage" || true
echo "Fertig: Fundus.app und FundusKit.framework in $stage"
