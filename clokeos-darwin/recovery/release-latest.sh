#!/usr/bin/env bash
# Ein Befehl für das Release des neuesten Stands: holt origin/main, erhöht die Nebenversion
# (Argument überschreibt: release-latest.sh 2.9), setzt Versions- und Build-Nummer, schreibt die
# Release-Notizen aus recovery/release-bullets.md, pusht den Versions-Commit nach main (ohne
# Co-Author) und baut und veröffentlicht beide Architekturen mit Tag auf genau diesem Commit.
# Aufruf: recovery/release-latest.sh [version]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
repo="$(cd "$here/.." && pwd)"
work="${CLOKEOS_WORK:-$HOME/ClokeOS-work/wd}"
wt="${CLOKEOS_RELEASE_WT:-$HOME/ClokeOS-work/wt-release}"
rel_repo="${CLOKEOS_RELEASES_REPO:-cloke-tech/ClokeOS}"

# Platz: Bau und Pakete brauchen rund 30 GB; die eigenen Altlasten räumen wir zuerst weg.
rm -rf "$work"/dist/online-* "$work"/tmp
free_gb=$(df -g "$work" | awk 'NR==2{print $4}')
min="${CLOKEOS_MIN_GB:-30}"
[ "$free_gb" -ge "$min" ] || { echo "Nur $free_gb GB frei, es werden rund $min GB gebraucht." >&2; exit 1; }

git -C "$repo" fetch -q origin
[ -d "$wt" ] || git -C "$repo" worktree add -q --detach "$wt" origin/main
git -C "$wt" checkout -q --detach origin/main

ver="${1:-}"
if [ -z "$ver" ]; then
  last="$(gh release list -R "$rel_repo" --limit 1 --json tagName -q '.[0].tagName' | sed 's/^v//')"
  ver="${last%%.*}.$(( ${last#*.} + 1 ))"
fi
build="$(grep -o 'CLOKE_BUILD="[^"]*"' "$wt/clokeos-darwin/config.sh" | cut -d'"' -f2)"
sed -i '' "s/^CLOKE_VERSION=.*/CLOKE_VERSION=\"$ver\"/" "$wt/clokeos-darwin/config.sh"
echo "Release $ver (Build $build) aus $(git -C "$wt" rev-parse --short HEAD)"

git -C "$wt" commit -qam "ClokeOS $ver: Version, Release-Notizen"
git -C "$wt" push -q origin HEAD:main
CLOKEOS_RELEASE_PRUNE=1 CLOKEOS_WORK="$work" "$wt/clokeos-darwin/recovery/release.sh" --upload
