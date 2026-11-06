#!/bin/bash
# Schneller Weg für das Foundation-Overlay: fo.dylib neu linken (swift/foundation-overlay.sh), swiftov.py neu laufen lassen
# und fo.dylib samt usr/lib/sov direkt ins vorhandene Desktop-Image schreiben (die VM muss aus sein), ohne das Image neu zu bauen.
#   CLOKEOS_WORK=<Ordner mit out/…-arm64-desktop.img> swift/deploy-overlay.sh [Swift-Ordner mit run-arm64] [Ordner mit den FoundationEssentials-Objekten]
set -euo pipefail
export CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
sw="${1:-$HOME/ClokeOS-work-swiftb/swiftsrc}"
fb="${2:-$HOME/ClokeOS-work-sf/swift-foundation/.build/out/Products/Release}"
"$HERE/swift/foundation-overlay.sh" "$sw" "$fb" > /dev/null 2>&1
"$HERE/swift/foundation-objc.sh" "$sw" "$fb" > /dev/null 2>&1
mkdir -p "$WORK/swiftsrc/run-arm64"; cp "$sw/run-arm64/fo.dylib" "$sw/run-arm64/fo2.dylib" "$sw/run-arm64/combine.dylib" "$WORK/swiftsrc/run-arm64/"
fw="$WORK/out/macapps-arm64/root/Library/ClokeOS/MacFrameworks"; R="${fw%/Library/ClokeOS/MacFrameworks}"
rm -f "$R"/usr/lib/sov/*.dylib   # sonst zählen die alten Trap-Marken als vorhanden
python3 -I "$HERE/macapps/stubgen/swiftov.py" "$WORK/out/stubgen/imports.json" "$R" "$R" --arch arm64 \
  --build "${fw%/root/Library/ClokeOS/MacFrameworks}/pdlibs" "${fw%/root/Library/ClokeOS/MacFrameworks}/obj/CoreFoundation" \
  --overlay "Foundation=$WORK/swiftsrc/run-arm64/fo.dylib" --overlay "Foundation=$WORK/swiftsrc/run-arm64/fo2.dylib" --overlay "libswiftDispatch=$WORK/swiftsrc/run-arm64/fo2.dylib" --overlay "libswiftObjectiveC=$WORK/swiftsrc/run-arm64/fo2.dylib" --overlay "libswiftDarwin=$WORK/swiftsrc/run-arm64/fo2.dylib" --overlay "CoreGraphics=$WORK/swiftsrc/run-arm64/fo2.dylib" --overlay "libswiftos=$WORK/swiftsrc/run-arm64/fo.dylib" --overlay "Combine=$WORK/swiftsrc/run-arm64/combine.dylib" --overlay "Network=$WORK/swiftsrc/run-arm64/fo.dylib" --overlay "CoreData=$WORK/swiftsrc/run-arm64/fo2.dylib" --overlay "SwiftUI=$WORK/swiftsrc/run-arm64/fo2.dylib" --pd-image "$DEBUGFS" "$PD_IMG?offset=$(( $(gpt_part "$PD_IMG" "$GUID_LINUX_FS" | cut -d' ' -f1) * 512 ))" | grep -c "Re-Export" || true
cmds="$WORK/tmp/deploy.cmds"; : > "$cmds"
read -r s0 c0 < <(gpt_part "$CLOKE_IMG" "$GUID_LINUX_FS")
# veraltete Supplemente im Image entfernen (sonst re-exportieren die Stub-Bibliotheken weiter die alten Trap-Dateien)
"$DEBUGFS" -R "ls -p /usr/lib/sov" "$CLOKE_IMG?offset=$((s0 * 512))" 2>/dev/null | awk -F/ 'NF > 5 && $6 ~ /dylib$/ {print "rm /usr/lib/sov/" $6}' >> "$cmds"
for f in "$R"/usr/lib/sov/*.dylib "$WORK/swiftsrc/run-arm64/fo.dylib" "$WORK/swiftsrc/run-arm64/fo2.dylib" "$WORK/swiftsrc/run-arm64/combine.dylib"; do
  n="/usr/lib/sov/$(basename "$f")"
  echo "rm $n" >> "$cmds"; echo "write $f $n" >> "$cmds"
done
read -r s c < <(gpt_part "$CLOKE_IMG" "$GUID_LINUX_FS")
"$DEBUGFS" -w -f "$cmds" "$CLOKE_IMG?offset=$((s * 512))" > "$WORK/tmp/deploy.log" 2>&1
echo "deployt nach $CLOKE_IMG"
