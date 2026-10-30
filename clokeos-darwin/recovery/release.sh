#!/bin/bash
# Online-Release für Installer und Wiederherstellung, wie macOS es aus dem Internet lädt.
#
#   recovery/release.sh [--upload] [x86_64] [arm64]     ohne Architektur: beide
#
# Baut je Architektur die Desktop-Variante (brand.sh) und daraus Wiederherstellung und
# Partitionsabbilder (recovery/build.sh) frisch, packt sie mit xz und legt alles unter festen Namen
# nach ~/ClokeOS-work/dist/online-<version>/. Die Wiederherstellung lädt sie über
# https://github.com/<CLOKE_RELEASES_REPO>/releases/latest/download/<name>:
#   clokeos-<arch>.txt                Beschreibung: Version, je Abbild Name, Größe, gepackte Größe, SHA-256
#   clokeos-<arch>-esp.img.xz         EFI-Partition
#   clokeos-<arch>-system.ext4.xz     System
#   clokeos-<arch>-recovery.ext4.xz   Wiederherstellung
#   clokeos-<arch>-installer.img.xz   startfähiges Installationsmedium für VMs (QEMU)
#   clokeos-x86_64-usb-offline.img.xz USB-Stick für echte PCs, bringt das System mit (recovery/usb.sh)
#   clokeos-x86_64-usb-online.img.xz  kleiner USB-Stick, lädt das System aus dem Internet
#   clokeos-x86_64-disk.img.xz        fertig installierte SSD zum Draufschreiben (recovery/disk.sh)
#   clokeos-<version>-source.tar.gz   Quellcode dieses Commits und der mitgebauten Fremdquellen (GPL)
#   SHA256SUMS, RELEASE-NOTES.md
#
# --upload legt das Release v<version> im öffentlichen Repo an und veröffentlicht es: Nur ein
# veröffentlichtes Release ist "latest", nur dann findet die Wiederherstellung es. Wie release.sh
# nur mit sauberem, gepushtem Stand; parallel arbeitende Sitzungen lassen offene Änderungen liegen,
# dann aus einem eigenen git worktree mit eigenem CLOKEOS_WORK bauen (siehe README).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
upload=0
arches=()
for a in "$@"; do
  case "$a" in
    --upload) upload=1 ;;
    x86_64|arm64) arches+=("$a") ;;
    *) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
  esac
done
[ ${#arches[@]} -gt 0 ] || arches=(x86_64 arm64)

export CLOKEOS_FLAVOR=desktop
source "$here/config.sh"
need xz shasum git
repo_dir="$(cd "$here/.." && pwd)"
commit="$(git -C "$repo_dir" rev-parse HEAD)"
tag="v$CLOKE_VERSION"
if [ "$upload" = 1 ]; then
  need gh
  [ -z "$(git -C "$repo_dir" status --porcelain)" ] || { echo "Nicht eingecheckte Änderungen im Repo." >&2; exit 1; }
  git -C "$repo_dir" fetch -q origin
  git -C "$repo_dir" branch -r --contains "$commit" | grep -q . || { echo "Commit $commit ist nicht gepusht." >&2; exit 1; }
  if gh release view "$tag" -R "$CLOKE_RELEASES_REPO" >/dev/null 2>&1; then
    echo "$tag gibt es in $CLOKE_RELEASES_REPO schon. CLOKE_VERSION in config.sh erhöhen." >&2
    exit 1
  fi
fi

dist="$WORK/dist/online-$CLOKE_VERSION"
rm -rf "$dist"
mkdir -p "$dist"
rows=""
for arch in "${arches[@]}"; do
  CLOKEOS_ARCH="$arch" "$here/brand.sh" | tail -1
  CLOKEOS_ARCH="$arch" "$here/recovery/build.sh" | tail -1
  src="$WORK/out/recovery-$arch"
  man="$dist/clokeos-$arch.txt"
  {
    echo "version=$CLOKE_VERSION"
    echo "build=$CLOKE_BUILD"
    echo "codename=$CLOKE_CODENAME"
    echo "arch=$arch"
  } > "$man"
  for part in esp:esp.img system:system.ext4 recovery:recovery.ext4 installer:installer.img; do
    key="${part%%:*}" file="${part#*:}"
    name="clokeos-$arch-$key.${file#*.}.xz"
    echo "Packe $name"
    xz -T0 -6 -c "$src/$file" > "$dist/$name"
    raw=$(stat -f %z "$src/$file")
    packed=$(stat -f %z "$dist/$name")
    sum=$(shasum -a 256 "$dist/$name" | cut -d' ' -f1)
    [ "$key" = installer ] || echo "$key=$name $raw $packed $sum" >> "$man"
    rows+="| \`$name\` | $arch | $(( packed / 1048576 )) MB | $(( raw / 1048576 )) MB |"$'\n'
  done
  if [ "$arch" = x86_64 ]; then # USB-Stick (mit den eben gepackten Abbildern) und SSD-Abbild für echte PCs
    CLOKEOS_ARCH="$arch" "$here/recovery/usb.sh" offline "$dist" | tail -1
    CLOKEOS_ARCH="$arch" "$here/recovery/usb.sh" online | tail -1
    CLOKEOS_ARCH="$arch" "$here/recovery/disk.sh" | tail -1
    for k in usb-offline usb-online disk; do
      name="clokeos-$arch-$k.img.xz"
      echo "Packe $name"
      xz -T0 -6 -c "$src/$k.img" > "$dist/$name"
      rows+="| \`$name\` | $arch | $(( $(stat -f %z "$dist/$name") / 1048576 )) MB | $(( $(stat -f %z "$src/$k.img") / 1048576 )) MB |"$'\n'
      [ "$k" = disk ] && rm -f "$src/$k.img" # gut 5 GB, gepackt reicht es
    done
  fi
  # CLOKEOS_RELEASE_PRUNE=1: große Zwischendateien nach dem Packen löschen (wenig Platz auf der Platte);
  # das Desktop-Image von arm64 bleibt für Bildschirmfotos
  if [ "${CLOKEOS_RELEASE_PRUNE:-0}" = 1 ] && [ "$arch" = x86_64 ]; then
    rm -f "$src/system.ext4" "$src/installer.img" "$src/usb-offline.img" "$src/usb-online.img" "$WORK"/out/clokeos-*-x86_64-desktop.img
  fi
done

# Quellcode: dieser Commit und die Fremdquellen, aus denen mitgebaut wurde
srcdir="$WORK/tmp/source-$CLOKE_VERSION"
rm -rf "$srcdir"
mkdir -p "$srcdir/third_party"
git -C "$repo_dir" archive --format=tar --prefix="ClokeOS-$CLOKE_VERSION/" "$commit" | tar -x -C "$srcdir"
for f in "$XNU_LOADER_TGZ" "$GNU_EFI_TGZ" "$APPMENU_TGZ"; do [ -f "$f" ] && cp "$f" "$srcdir/third_party/"; done
# Darlings Frameworks nur, wenn ihre Quellen geladen sind; nur dann baut brand.sh sie ein
# (macapps/install.sh), und nur dann gehören sie ins Quellarchiv
darling=0
for r in "$WORK"/download/darling/*/; do
  [ -d "$r/.git" ] || continue
  n=$(basename "$r")
  git -C "$r" archive --format=tar.gz --prefix="$n/" HEAD > "$srcdir/third_party/$n-$(git -C "$r" rev-parse --short=12 HEAD).tar.gz"
  darling=1
done
licenses="xnu-loader (BSD-3-Clause), gnu-efi and vala-panel-appmenu (LGPL-3)"
macnote="Darling's Mac frameworks (AppKit for Mac apps) are not part of this release."
if [ "$darling" = 1 ]; then
  licenses="xnu-loader (BSD-3-Clause), gnu-efi, vala-panel-appmenu (LGPL-3) and Darling's frameworks (GPL-3)"
  macnote="Mac apps run on Darling's frameworks (AppKit from darling-cocotron), still with many gaps."
fi
cat > "$srcdir/third_party/README" <<EOF
Fremdquellen, aus denen ClokeOS $CLOKE_VERSION mitgebaut wurde, so wie sie geladen wurden. Die
ClokeOS-Änderungen daran stehen im Quellbaum (loader/xnu-loader.patch, menubar/appmenu-clokeos.patch,
macapps/patches/). PureDarwin selbst, dessen Images die Grundlage sind, steht unter
https://github.com/PureDarwin/PureDarwin/tree/$PD_VERSION.
EOF
tar -czf "$dist/clokeos-$CLOKE_VERSION-source.tar.gz" -C "$srcdir" .
rm -rf "$srcdir"
(cd "$dist" && shasum -a 256 -- *.xz *.tar.gz > SHA256SUMS)

bullets="$(cat "$here/recovery/release-bullets.md")"
cat > "$dist/RELEASE-NOTES.md" <<EOF
ClokeOS $CLOKE_VERSION "$CLOKE_CODENAME" (build $CLOKE_BUILD) with the CloakServer desktop, installed as on macOS: from a USB stick onto a PC, as a ready-made SSD image, or from the internet in a VM.

## Install on a PC (x86_64, USB stick)

Two sticks:

- \`clokeos-x86_64-usb-offline.img.xz\` carries the whole system and installs without internet. The right choice for most PCs.
- \`clokeos-x86_64-usb-online.img.xz\` is small and downloads the system while installing; it needs a LAN cable and a supported network chip (see below).

1. Download, unpack (\`xz -dk\`) and write to a USB stick of at least 2 GB, for example with balenaEtcher, or on a Mac: \`diskutil list\`, \`diskutil unmountDisk /dev/diskN\`, \`sudo dd if=clokeos-x86_64-usb-offline.img of=/dev/rdiskN bs=4m\` (N is the stick; everything on it is erased).
2. Plug in a USB keyboard and mouse unless the PC has a built-in PS/2 keyboard and touchpad, then choose the stick in the firmware's boot menu (UEFI without CSM/legacy, Secure Boot off, SATA in AHCI mode, Intel VMD/RST off). Loading takes one to two minutes; holding V shows the verbose boot.
3. In Recovery choose "Install ClokeOS" and pick the SATA or NVMe SSD; it is erased. If recovery and system together do not fit into the memory below 1 GB (depending on the firmware only about 400 MB may be free there in one piece), the offline stick starts a slim variant that reads the system from the stick while installing.
4. Restart and remove the stick. On first start the setup assistant asks for language, region, your account and the appearance.

Requirements: UEFI, a SATA or NVMe SSD. Keyboard and mouse over USB (plugged in at power-on) or a built-in PS/2 keyboard and PS/2 touchpad; I²C touchpads (most notebooks from about 2015 on) do not work yet. Without keyboard or mouse ClokeOS shows a diagnostics window after 45 seconds. Network only over LAN with Realtek RTL8111/8168/8169 or Intel 82545EM; Wi-Fi and newer Intel network chips (I219, I225) are not supported. Display through the firmware's graphics (GOP).

## Without a USB stick: ready-made SSD image (x86_64)

\`clokeos-x86_64-disk.img.xz\` is an installed SSD with system and recovery, no internet needed: connect the PC's SSD to a Mac with a USB-SATA or USB-NVMe adapter, unpack and write it (\`sudo dd if=clokeos-x86_64-disk.img of=/dev/rdiskN bs=4m\`, or balenaEtcher), put it back and boot. The setup assistant runs on first start. ClokeOS uses a bit more than 5 GB, the rest of the SSD stays free.

## Install in a VM (QEMU)

Unpack \`clokeos-<arch>-installer.img.xz\` (x86_64, or arm64 for Macs with Apple silicon) and start it with an empty second disk, for example with \`clokeos-darwin/run.sh\` from the source: \`CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop ./run.sh --image clokeos-arm64-installer.img --disk disk.img\`. Recovery then downloads the system from this release.

Later, "Restart in Recovery…" in the ClokeOS menu starts the recovery on the disk, on x86_64 also the key R at power-on. From there ClokeOS can be reinstalled; the recovery stays.

## New in $CLOKE_VERSION

$bullets

## What is inside

- CloakServer, ClokeOS' window server: menu bar, Dock, Launchpad, login, light and dark mode, windows with shadows and glass
- Global menu bar as on macOS: apps' menus are at the top of the screen instead of in the window, for GTK apps like Fundus and Terminal as well as for Mac apps
- Wine for Windows programs (x86_64 desktop only; the recovery leaves it out)
- Recovery with installer, online help, Disk Utility and Terminal
- $macnote

Limits: no Wi-Fi yet, software graphics through the firmware, few drivers (see above). Tested in QEMU (x86_64 emulated, also with USB stick, NVMe and SATA SSD and PS/2 input; arm64 with Apple's hypervisor) and on an HP 15s-fq5655ng (boot from USB, recovery).

## Files

| File | Architecture | packed | unpacked |
|---|---|---|---|
$rows
Recovery reads \`clokeos-<arch>.txt\` and checks every image against the SHA-256 in it while loading. \`SHA256SUMS\` has the checksums of all files.

## Sources and licenses

\`clokeos-$CLOKE_VERSION-source.tar.gz\` contains this release's source (commit \`$commit\`) and the third-party sources built along: $licenses. The base is PureDarwin's images $PD_VERSION (Darwin and XNU under the APSL 2.0, plus GPL parts such as corecrypto); their source: https://github.com/PureDarwin/PureDarwin/tree/$PD_VERSION.
EOF
ls -l "$dist"

if [ "$upload" = 1 ]; then
  gh release create "$tag" -R "$CLOKE_RELEASES_REPO" --target "$commit" --title "ClokeOS $CLOKE_VERSION \"$CLOKE_CODENAME\"" \
    --notes-file "$dist/RELEASE-NOTES.md" --latest "$dist"/*.xz "$dist"/*.txt "$dist"/*.tar.gz "$dist/SHA256SUMS"
  echo "Veröffentlicht: $(gh release view "$tag" -R "$CLOKE_RELEASES_REPO" --json url -q .url)"
fi
