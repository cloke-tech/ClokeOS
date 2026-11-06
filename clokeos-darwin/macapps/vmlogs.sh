#!/bin/bash
# Holt /tmp/logic.out und /tmp/crashpc.log aus der laufenden Desktop-VM: kopiert sie nach /var/root, sync, beendet die VM und liest
# sie mit debugfs aus dem Image (die serielle Konsole verstümmelt lange Ausgaben). Ziel: Ordner als Argument (Standard $WORK/tmp/vmlogs).
#   CLOKEOS_WORK=<Ordner> macapps/vmlogs.sh [Zielordner]
set -euo pipefail
export CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop
cd "$(dirname "$0")/.."
source config.sh
dst="${1:-$WORK/tmp/vmlogs}"; mkdir -p "$dst"
./vmctl.py send 'cp /tmp/crashpc.log /tmp/logic.out /var/root/ 2>/dev/null; sync; sync' 30 > /dev/null || true
sleep 2; ./vmctl.py quit > /dev/null 2>&1 || true; sleep 3; pkill -f "qemu.*$CLOKE_IMG" 2>/dev/null || true; sleep 1
read -r s c < <(gpt_part "$CLOKE_IMG" "$GUID_LINUX_FS")
for f in crashpc.log logic.out; do
  rm -f "$dst/$f"; "$DEBUGFS" -R "dump /private/var/root/$f $dst/$f" "$CLOKE_IMG?offset=$((s * 512))" > /dev/null 2>&1 || true
done
ls -la "$dst"
