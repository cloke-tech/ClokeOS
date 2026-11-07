#!/bin/bash
# Spiegelt einen Ordner des Macs ins vorhandene Desktop-Image (Ordner anlegen, Dateien schreiben), ohne es neu zu bauen. Die VM muss aus sein.
#   CLOKEOS_WORK=<Ordner> macapps/deploy-tree.sh <Quellordner> <Zielpfad im Image>
#   z. B. macapps/deploy-tree.sh "$WORK/tmp/appimages/Logic Pro.app" "/Applications/Logic Pro.app"
set -euo pipefail
export CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
src="${1%/}" dst="${2%/}"
if pgrep -f "qemu.*$CLOKE_IMG" > /dev/null; then echo "VM auf $CLOKE_IMG läuft noch" >&2; exit 1; fi
cmds="$WORK/tmp/deploy-tree.cmds"; mkdir -p "$WORK/tmp"; : > "$cmds"
# Zielordner samt fehlender Elternordner anlegen (debugfs mkdir legt nur eine Ebene an; vorhandene Ordner melden nur einen harmlosen Fehler)
p=""; IFS=/ read -ra parts <<< "${dst#/}"; for part in "${parts[@]}"; do p="$p/$part"; printf 'mkdir "%s"\n' "$p" >> "$cmds"; done
(cd "$src" && find . -type d | sort) | while IFS= read -r d; do
  [ "$d" = . ] || printf 'mkdir "%s/%s"\n' "$dst" "${d#./}" >> "$cmds"
done
n=0
while IFS= read -r f; do
  printf 'rm "%s/%s"\nwrite "%s/%s" "%s/%s"\n' "$dst" "${f#./}" "$src" "${f#./}" "$dst" "${f#./}" >> "$cmds"; n=$((n + 1))
done < <(cd "$src" && find . -type f | sort)
read -r s c < <(gpt_part "$CLOKE_IMG" "$GUID_LINUX_FS")
"$DEBUGFS" -w -f "$cmds" "$CLOKE_IMG?offset=$((s * 512))" > "$WORK/tmp/deploy-tree.log" 2>&1
# Echte Fehler melden (nicht: Ordner gibt es schon, Datei zum Ersetzen gab es noch nicht)
grep -i -E "error|not found|no space|could not" "$WORK/tmp/deploy-tree.log" | grep -v -E "^rm |File not found by ext2_lookup while (unlinking|removing)|exists|already" | head -5 >&2 || true
echo "geschrieben: $n Datei(en) aus $src nach $dst"
