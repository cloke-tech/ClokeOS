#!/bin/bash
# Schreibt einzelne Dateien aus dem arm64-Bauordner (out/macapps-arm64/root) direkt ins vorhandene Desktop-Image, ohne es neu zu bauen
# (z. B. nach macapps/appkit.sh). Die VM auf diesem Image muss aus sein.
#   CLOKEOS_WORK=<Ordner> macapps/deploy-files.sh /Library/ClokeOS/MacFrameworks/AppKit.framework/Versions/C/AppKit [...]
set -euo pipefail
export CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
R="$WORK/out/macapps-arm64/root"
if pgrep -f "qemu.*$CLOKE_IMG" > /dev/null; then echo "VM auf $CLOKE_IMG läuft noch" >&2; exit 1; fi
cmds="$WORK/tmp/deploy-files.cmds"; mkdir -p "$WORK/tmp"; : > "$cmds"
for p in "$@"; do
  [ -f "$R$p" ] || { echo "fehlt: $R$p" >&2; exit 1; }
  echo "rm $p" >> "$cmds"; echo "write $R$p $p" >> "$cmds"
done
read -r s c < <(gpt_part "$CLOKE_IMG" "$GUID_LINUX_FS")
"$DEBUGFS" -w -f "$cmds" "$CLOKE_IMG?offset=$((s * 512))" > "$WORK/tmp/deploy-files.log" 2>&1
grep -i "error\|not found" "$WORK/tmp/deploy-files.log" | grep -v "^rm" || true
echo "geschrieben: $# Datei(en) nach $CLOKE_IMG"
