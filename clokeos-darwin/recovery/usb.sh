#!/bin/bash
# USB-Installationsmedien für echte Rechner (x86_64): ClokeOS vom USB-Stick auf eine SATA- oder
# NVMe-SSD installieren.
#
#   recovery/usb.sh online                 usb-online.img: nur die schlanke Wiederherstellung, die das
#                                          System aus dem Internet lädt (per LAN-Kabel)
#   recovery/usb.sh offline <ordner>       usb-offline.img: bringt das System mit, aus <ordner> mit
#                                          clokeos-<arch>.txt und den .xz-Abbildern (recovery/release.sh)
#   recovery/usb.sh live <pubkey> <host>…  usb-live.img: wie online, aber mit SSH (nur Schlüssel, der
#                                          öffentliche Schlüssel aus <pubkey> für root) und clokeos-livecall,
#                                          das sich bei <host> (Adresse oder Name, Port 8222) meldet; für
#                                          die Treiberarbeit an einem fremden Rechner (recovery/live-listen.py)
#
# Der Stick hat eine FAT32-Partition mit Loader, Kernel und Boot-Argumenten; die Wiederherstellung
# lädt der Loader (loader/xnu-loader.patch, clokeos_load_ramdisk) als RAM-Disk in den Arbeitsspeicher,
# der Kernel startet mit rd=md0 daraus. XNU erreicht eine RAM-Disk nur unterhalb von 0x3E800000 (knapp
# 1 GB), und dort ist je nach Firmware wenig am Stück frei (ein HP-Notebook mit 8 GB: 381 MB). Der
# Loader nimmt deshalb die erste, die passt:
#   \ramdisk.img        nur offline: Wiederherstellung und unter /Library/ClokeOS/Install die Abbilder
#                       von EFI-Partition und System, gut 700 MB: installiert aus dem Arbeitsspeicher
#   \ramdisk-lite.img   nur die Wiederherstellung, ohne SSH und Entwicklerwerkzeuge, gut 250 MB
# Startet auf dem Offline-Stick die schlanke, liest der Installer die Abbilder unter \ClokeOS\Install
# vom Stick selbst ("stick:", recovery/installer.c), über PureDarwins Treiber für USB-Massenspeicher
# (RavynXHCIMassStorageDisk), mit Wiederholungen bei Lesefehlern. Auf dem Online-Stick lädt sie aus
# dem Internet. GPT-Name "ClokeOS Installer": der Installer bietet den Stick nicht als Ziel an.
#
# Ergebnis: ~/ClokeOS-work/out/recovery-<arch>/usb-<art>.img (vorher recovery/build.sh). Auf den Stick:
#   sudo dd if=usb-offline.img of=/dev/rdiskN bs=4m    (macOS; oder balenaEtcher)
# Braucht einen Rechner mit UEFI (ohne CSM, Secure Boot aus).
set -euo pipefail
export CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
RESIZE2FS="$BREW/opt/e2fsprogs/sbin/resize2fs"
need "$DEBUGFS" "$E2FSCK" "$TUNE2FS" "$RESIZE2FS" "$MKE2FS" python3 mcopy mformat mtype mdir mmd uuidgen
export MTOOLS_SKIP_CHECK=1
kind="${1:-}"
payload="${2:-}"
livekey=""; livehosts=()
case "$kind" in
  online) ;;
  live) livekey="$payload"; shift 2 || true; livehosts=("$@")
        [ -f "$livekey" ] || { echo "Öffentlicher SSH-Schlüssel fehlt: ${livekey:-<datei>}" >&2; exit 1; } ;;
  offline) [ -f "$payload/clokeos-$CLOKE_ARCH.txt" ] || { echo "Keine Beschreibung clokeos-$CLOKE_ARCH.txt in ${payload:-<ordner>}" >&2; exit 1; } ;;
  *) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
esac

out="$WORK/out/recovery-$CLOKE_ARCH"
tmp="$WORK/tmp/usb-$CLOKE_ARCH"
[ -f "$out/recovery.ext4" ] && [ -f "$out/esp.img" ] || { echo "Fehlt: $out – zuerst recovery/build.sh" >&2; exit 1; }
rm -rf "$tmp"
mkdir -p "$tmp"

# Abbilder aus der Beschreibung: Datei zum Schlüssel (esp, system, recovery)
asset() { awk -F'[= ]' -v k="$1" '$1 == k {print $2}' "$payload/clokeos-$CLOKE_ARCH.txt"; }

# --- große RAM-Disk (offline): Wiederherstellung mit den Abbildern von EFI-Partition und System -----
# Die Wiederherstellungspartition passt nicht mehr mit hinein; der Installer nimmt sie vom Stick.
root="$tmp/root.ext4"
if [ "$kind" = offline ]; then
  cp -c "$out/recovery.ext4" "$root" 2>/dev/null || cp "$out/recovery.ext4" "$root"
  files=("$payload/clokeos-$CLOKE_ARCH.txt")
  for k in esp system; do
    f=$(asset $k)
    [ -f "$payload/$f" ] || { echo "Fehlt: $payload/$f" >&2; exit 1; }
    files+=("$payload/$f")
  done
  extra=$(( 16 + $(du -cm "${files[@]}" | tail -1 | cut -f1) ))
  # erst auf das Minimum, dann um die Abbilder und Platz für Protokolle und /tmp vergrößern
  "$E2FSCK" -fy "$root" >/dev/null 2>&1 || true
  "$RESIZE2FS" -M "$root" >/dev/null 2>&1
  size_mb=$(( $(stat -f %z "$root") / 1048576 ))
  "$RESIZE2FS" "$root" "$(( (size_mb + extra + 15) / 16 * 16 ))M" >/dev/null 2>&1
  "$TUNE2FS" -U random -L "Installer" "$root" >/dev/null
  {
    echo "mkdir /Library/ClokeOS"
    echo "mkdir /Library/ClokeOS/Install"
    for f in "${files[@]}"; do echo "write $f /Library/ClokeOS/Install/$(basename "$f")"; done
  } > "$tmp/payload.cmds"
  "$DEBUGFS" -w -f "$tmp/payload.cmds" "$root" > "$tmp/payload.log" 2>&1
  "$DEBUGFS" -R "stat /Library/ClokeOS/Install/clokeos-$CLOKE_ARCH.txt" "$root" 2>/dev/null | grep -q '^Inode' ||
    { cat "$tmp/payload.log" >&2; exit 1; }
  "$E2FSCK" -fy "$root" >/dev/null 2>&1 || true
  "$E2FSCK" -fn "$root" > "$tmp/e2fsck.log" 2>&1 || { cat "$tmp/e2fsck.log" >&2; exit 1; }
  rd_mb=$(( $(stat -f %z "$root") / 1048576 ))
  [ "$rd_mb" -le 880 ] || { echo "RAM-Disk zu groß: $rd_mb MB (höchstens 880, siehe loader/xnu-loader.patch)" >&2; exit 1; }
fi

# --- schlanke RAM-Disk: nur die Wiederherstellung, ohne alles, was sie zum Installieren aus dem
# Internet nicht braucht. Auf einem Notebook mit 1 GB RAM war das größte freie Stück unter der Grenze
# des Loaders 381 MB groß; die RAM-Disk soll deutlich darunter bleiben.
# Weg: SSH (Server und Client, eigenes OpenSSL), Werkzeuge zum Bauen (cctools, Autotools, Compiler,
# Wine-Skripte), andere Fenstermanager und Xfce-Sitzung, X-Demos, Animationen der Warte-Zeiger.
# Was die Wiederherstellung startet, bleibt: Xorg, xinit, xrdb, xkbcomp, dbus, CloakServer,
# xfce4-terminal, netsurf-gtk3, clokeos-installer, die Shell-Werkzeuge.
LITE_DROP_SSH='bin/ssh bin/ssh-keygen sbin/sshd libexec/sshd-auth libexec/sshd-session libexec/sftp-server'
LITE_DROP='
  bin/i3 bin/i3-* bin/i3bar bin/i3status bin/dmenu bin/dmenu_* bin/puredarwin-sway bin/startxfce4
  bin/xfce4-session bin/xfce4-session-* bin/xfdesktop bin/xfdesktop-settings bin/xfce4-panel
  bin/xfce4-popup-* bin/xfce4-appfinder bin/xfce4-*-settings bin/xfce4-settings-* bin/xfsettingsd
  bin/xfwm4 bin/xfwm4-* bin/thunar-settings bin/xflock4 bin/xfrun4 bin/xfce4-about bin/xfce4-find-cursor
  lib/xfce4/panel lib/xfce4/xfwm4 lib/xfce4/session lib/xfce4/settings
  bin/xcalc bin/xclock bin/xeyes bin/xmessage bin/resize bin/uxterm bin/koi8rxterm bin/xkeystone
  bin/sqlite3 bin/mpicalc bin/tcc bin/bmake bin/cjpeg bin/djpeg bin/jpegtran bin/rdjpgcom bin/wrjpgcom
  bin/pcre2test bin/pcre2grep bin/xmllint bin/xmlcatalog bin/gdbus-codegen bin/glib-compile-*
  bin/glib-genmarshal bin/glib-gettextize bin/glib-mkenums bin/gtester bin/gtester-report
  bin/gtk-builder-tool bin/gtk-encode-symbolic-svg bin/gdk-pixbuf-csource bin/gdk-pixbuf-pixdata
  bin/dbus-test-tool bin/psl-make-dafsa bin/pango-view bin/pango-segmentation bin/rsvg-convert
  bin/csslint-0.6 bin/pcmplay bin/mousemon
  usr/share/autoconf usr/share/automake-* usr/share/aclocal* usr/share/bison usr/share/wine-build
  usr/share/vim usr/share/bash-completion
  share/icons/Adwaita/cursors/progress share/icons/Adwaita/cursors/wait
'
LITE_KEEP_USR_BIN=' X Xorg xkbcomp launchctl env iconv clokefetch gtf '
lite="$tmp/lite.ext4"
if true; then
  lroot="$tmp/lite-root"
  mkdir -p "$lroot" # rdump legt den Baum in einen vorhandenen Ordner
  "$DEBUGFS" -R "rdump / $lroot" "$out/recovery.ext4" >/dev/null 2>&1 || true
  [ -f "$lroot/usr/libexec/clokeos-installer" ] || { echo "Wiederherstellung ließ sich nicht auspacken" >&2; exit 1; }
  (
    cd "$lroot"
    for p in $LITE_DROP; do rm -rf -- $p; done
    [ "$kind" = live ] || for p in $LITE_DROP_SSH; do rm -rf -- $p; done
    for f in usr/bin/*; do
      case "$LITE_KEEP_USR_BIN" in *" ${f#usr/bin/} "*) ;; *) rm -rf -- "$f" ;; esac
    done
  )
  if [ "$kind" = live ]; then
    # SSH nur mit Schlüssel, root; StrictModes aus, weil die hier angelegten Dateien nicht root gehören
    echo on > "$lroot/etc/clokeos-remote-login"
    mkdir -p "$lroot/var/root/.ssh"
    cp "$livekey" "$lroot/var/root/.ssh/authorized_keys"
    chmod 700 "$lroot/var/root/.ssh"; chmod 600 "$lroot/var/root/.ssh/authorized_keys"
    sed -i '' -e '/^PasswordAuthentication/d' -e '/^PermitRootLogin/d' -e '/^KbdInteractiveAuthentication/d' \
      -e '/^StrictModes/d' "$lroot/etc/ssh/sshd_config"
    printf 'PasswordAuthentication no\nKbdInteractiveAuthentication no\nPermitRootLogin prohibit-password\nStrictModes no\n' \
      >> "$lroot/etc/ssh/sshd_config"
    clang -O2 -target x86_64-apple-macos11.0 "$HERE/recovery/livecall.c" -o "$lroot/usr/libexec/clokeos-livecall"
    printf '%s\n' "${livehosts[@]}" > "$lroot/etc/clokeos-livecall.conf"
    cat > "$lroot/System/Library/LaunchDaemons/com.clokeos.livecall.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>com.clokeos.livecall</string>
	<key>ProgramArguments</key>
	<array>
		<string>/usr/libexec/clokeos-livecall</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>StandardErrorPath</key>
	<string>/var/log/livecall.log</string>
</dict>
</plist>
PLIST
  else
    echo off > "$lroot/etc/clokeos-remote-login" # ohne sshd: clokeos-sshd schläft
  fi
  xattr -rc "$lroot" 2>/dev/null || true
  DEBUGFS="$DEBUGFS" python3 -I "$HERE/recovery/imagetool.py" owners "$out/recovery.ext4" "$lroot" "$tmp/lite-owners.cmds"
  if [ "$kind" = live ]; then # neue Dateien gehören root (launchd mag keine Plists anderer Besitzer)
    for f in /var/root/.ssh /var/root/.ssh/authorized_keys /usr/libexec/clokeos-livecall /etc/clokeos-livecall.conf \
             /System/Library/LaunchDaemons/com.clokeos.livecall.plist; do
      printf 'set_inode_field %s uid 0\nset_inode_field %s gid 0\n' "$f" "$f"
    done >> "$tmp/lite-owners.cmds"
  fi
  lused=$(du -sm "$lroot" | cut -f1)
  lfiles=$(find "$lroot" | wc -l | tr -d ' ')
  lmb=$(( ((lused + 24) * 11 / 10 + 7) / 8 * 8 ))
  "$MKE2FS" -q -t ext4 -O ^orphan_file,metadata_csum_seed -N $(( lfiles * 3 / 2 + 2048 )) -m 0 -J size=16 \
    -L Installer -d "$lroot" "$lite" "${lmb}M"
  "$DEBUGFS" -w -f "$tmp/lite-owners.cmds" "$lite" > "$tmp/lite-owners.log" 2>&1
  "$E2FSCK" -fy "$lite" >/dev/null 2>&1 || true
  "$E2FSCK" -fn "$lite" > "$tmp/e2fsck-lite.log" 2>&1 || { cat "$tmp/e2fsck-lite.log" >&2; exit 1; }
  "$DEBUGFS" -R 'stat /usr/libexec/clokeos-installer' "$lite" 2>/dev/null | grep -q 'User: *0 ' ||
    { echo "Besitzer in der schlanken RAM-Disk nicht gesetzt" >&2; exit 1; }
  rm -rf "$lroot"
fi

# --- EFI-Partition des Sticks ----------------------------------------------------------------
# Dateien einzeln aus EFI/BOOT (mcopy -s benennt einen ausgepackten Ordner EFI in EFI_ um)
mkdir -p "$tmp/boot"
for f in $(mdir -b -i "$out/esp.img@@0" ::/EFI/BOOT | sed 's|^::/EFI/BOOT/||'); do
  case "$f" in boot-args-*|.|..) continue ;; esac
  mcopy -n -i "$out/esp.img@@0" "::/EFI/BOOT/$f" "$tmp/boot/$f"
done
[ -f "$tmp/boot/BOOTX64.EFI" ] && [ -f "$tmp/boot/kernel" ] || { echo "Loader oder Kernel fehlt in esp.img" >&2; exit 1; }
base=$(tr -d '\r\n' < "$tmp/boot/boot-args.txt" | sed -E 's/ *boot-uuid=[^ ]*//g; s/ *rd=[^ ]*//g')
printf '%s rd=md0\n' "$base" > "$tmp/boot/boot-args.txt"
stick_files=()
[ "$kind" = offline ] && for k in esp system recovery; do stick_files+=("$payload/$(asset $k)"); done
bytes=$(( $(stat -f %z "$lite") + $(du -sk "$tmp/boot" | cut -f1) * 1024 ))
[ "$kind" = offline ] && bytes=$(( bytes + $(stat -f %z "$root") + $(du -ck "${stick_files[@]}" | tail -1 | cut -f1) * 1024 ))
esize=$(( (bytes + 96 * 1048576) / 1048576 * 1048576 ))
esp="$tmp/usb-esp.img"
truncate -s "$esize" "$esp"
mformat -i "$esp" -F -v CLOKEOS -T $(( esize / 512 )) -h 64 -s 32 ::
python3 -I "$HERE/recovery/imagetool.py" fixlabel "$esp" >/dev/null
mmd -i "$esp" ::/EFI ::/EFI/BOOT
mcopy -i "$esp" "$tmp/boot/"* ::/EFI/BOOT/
mtype -i "$esp" ::/EFI/BOOT/boot-args.txt | grep -q 'rd=md0' || { echo "boot-args.txt fehlt im USB-Abbild" >&2; exit 1; }
if [ "$kind" = offline ]; then
  mcopy -i "$esp" "$root" ::/ramdisk.img
  mmd -i "$esp" ::/ClokeOS ::/ClokeOS/Install
  mcopy -i "$esp" "$payload/clokeos-$CLOKE_ARCH.txt" "${stick_files[@]}" ::/ClokeOS/Install/
fi
mcopy -i "$esp" "$lite" ::/ramdisk-lite.img
# Platzhalter für die Diagnose: die Wiederherstellung überschreibt ihn an Ort und Stelle
# (clokeos-installer stick-write, cloakserver-session), lesbar am Mac
mmd -i "$esp" ::/ClokeOS 2>/dev/null || true
{ printf 'ClokeOS diagnostics: nothing written yet. The recovery on the stick writes here every 30 seconds.\n'
  head -c 131072 /dev/zero | tr '\0' ' '; } > "$tmp/diag.txt"
mcopy -i "$esp" "$tmp/diag.txt" ::/ClokeOS/diag.txt
img="$out/usb-$kind.img"
rm -f "$img"
python3 -I "$HERE/recovery/imagetool.py" gpt "$img" "$esp" - "ClokeOS Installer" "$(uuidgen)" >/dev/null
if [ "$kind" = offline ]; then
  echo "USB-Stick offline: $img ($(( $(stat -f %z "$img") / 1048576 )) MB; RAM-Disk $(( $(stat -f %z "$root") / 1048576 )) MB mit den Abbildern, schlanke $(( $(stat -f %z "$lite") / 1048576 )) MB, Abbilder auch auf dem Stick)"
else
  echo "USB-Stick $kind: $img ($(( $(stat -f %z "$img") / 1048576 )) MB; schlanke RAM-Disk $(( $(stat -f %z "$lite") / 1048576 )) MB)"
fi
rm -rf "$tmp"
