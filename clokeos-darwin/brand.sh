#!/bin/bash
# Baut aus dem PureDarwin-Image ein ClokeOS-Image (~/ClokeOS-work/out/clokeos-<version>-<arch>.img).
# Architektur wie bei run.sh mit CLOKEOS_ARCH=arm64 (Standard: x86_64), die Desktop-Variante mit
# CLOKEOS_FLAVOR=desktop (Ergebnis: clokeos-<version>-<arch>-desktop.img).
#
# Das Basisimage bleibt unverändert. Das Wurzeldateisystem (ext4) wird mit debugfs bearbeitet,
# dafür braucht es weder Linux noch Root-Rechte. Was geändert wird:
#   - overlay/ wird ins Wurzeldateisystem kopiert (sw_vers, clokefetch, Rechnername, Anmeldetext)
#   - SystemVersion.plist und /etc/clokeos-release werden aus config.sh erzeugt
#   - /etc/zshrc bekommt die ClokeOS-Eingabeaufforderung und zeigt beim Start /etc/motd
#   - PureDarwins Rechnername-Dienst wird entfernt, das Volume heißt "Cloke HD"
#   - liegt eine RAM-Disk in der EFI-Partition (ramdisk.img, arm64-Minimal), bekommt sie dasselbe
#   - x86_64: die EFI-Partition bekommt den Loader mit Bootlogo (loader/build.sh) und die
#     Boot-Argumente aus config.sh, beides mit mtools
#   - desktop: dazu overlay-desktop/ und CloakServer (cloakserver/build.sh), der beim Start
#     Xorg mit dem ClokeOS-Desktop öffnet, die Programme als .app-Bundles wie bei macOS
#     (cloakserver/bundles.py) und das Benutzerkonto aus config.sh (CLOKE_USER); sind die
#     Darling-Quellen geladen, auch die Mac-Frameworks samt Hallo.app (macapps/install.sh);
#     dazu die globale Menüleiste (menubar/build.sh: GTK-Modul, cloakmenu, GSettings-Schema)
set -euo pipefail
source "$(dirname "$0")/config.sh"
need "$DEBUGFS" "$E2FSCK" "$TUNE2FS" python3 mcopy mtype mdir
export MTOOLS_SKIP_CHECK=1

"$HERE/fetch.sh"
[ -n "$LOADER_EFI" ] && "$HERE/loader/build.sh" "$LOADER_EFI"

tmp="$WORK/tmp/brand"
rm -rf "$tmp"
mkdir -p "$tmp/stage" "$(dirname "$CLOKE_IMG")"
# Auf APFS ist -c ein Klon: kostet bei den 6 GiB des Desktop-Images weder Zeit noch Platz.
cp -c "$PD_IMG" "$tmp/image.img" 2>/dev/null || cp "$PD_IMG" "$tmp/image.img"
chmod u+w "$tmp/image.img"   # auch wenn der Download schreibgeschützt liegt

read -r root_start root_count < <(gpt_part "$tmp/image.img" "$GUID_LINUX_FS")
root="$tmp/root.ext4"
dd if="$tmp/image.img" of="$root" bs=512 skip="$root_start" count="$root_count" status=none

# --- Dateien zusammenstellen ---------------------------------------------------------------
cp -R "$HERE/overlay/." "$tmp/stage/"
[ -z "${CLOKE_EXTRA_STAGE:-}" ] || cp -R "$CLOKE_EXTRA_STAGE/." "$tmp/stage/"   # z. B. macapps/accelerate.sh-Ausgabe für einen Test in der VM
if [ "$CLOKE_FLAVOR" = desktop ]; then
  cp -R "$HERE/overlay-desktop/." "$tmp/stage/"
  # Noto Sans CJK für Schriftzeichen, die Cloke Sans nicht hat (fontconfig weicht automatisch darauf aus)
  fetch_file "$CJK_FONT_URL" "$CJK_FONT_SHA256" "$CJK_FONT"
  mkdir -p "$tmp/stage/Library/Fonts/NotoSansCJK"
  cp "$CJK_FONT" "$tmp/stage/Library/Fonts/NotoSansCJK/"
  cp "$HERE/overlay-desktop/Library/Fonts/NotoSansCJK-OFL.txt" "$tmp/stage/Library/Fonts/NotoSansCJK/OFL.txt"
  "$HERE/cloakserver/build.sh" "$tmp/stage"
  "$HERE/menubar/build.sh" "$tmp/stage"
  "$HERE/apps/build.sh" "$tmp/stage"
  "$HERE/recovery/installer-build.sh" "$tmp/stage"
  "$HERE/dmg/build.sh" "$tmp/stage"
  python3 -I "$HERE/cloakserver/bundles.py" "$tmp/stage" "$tmp/stage/System/Library/CoreServices/CloakServer/Resources/icons" \
    "$CLOKE_VERSION" "$CLOKE_BUILD"
  [ -n "${CLOKE_NO_MACAPPS:-}" ] || "$HERE/macapps/install.sh" "$tmp/stage"   # CLOKE_NO_MACAPPS=1: ohne Mac-Frameworks (schneller)
  # Tastentabelle in Xorgs Eingabetreiber ergänzen (Einfg, Pos1, Entf, Ziffernfeld, <>|, ...)
  pdi=/usr/lib/xorg/modules/input/puredarwininput_drv.so
  mkdir -p "$tmp/stage$(dirname "$pdi")"
  "$DEBUGFS" -R "dump $pdi $tmp/stage$pdi" "$root" >/dev/null 2>&1
  python3 -I "$HERE/inputfix.py" "$tmp/stage$pdi"
  # Eigene Kernel-Treiber (kext/build.sh; RavynHDAudio ersetzt PureDarwins Kext): ins System, damit pd-rebuild-kc sie mit einbaut; der
  # Kernel Collection selbst kommt unten in die EFI-Partition
  if [ "$CLOKE_ARCH" = x86_64 ]; then
    "$HERE/kext/build.sh" | tail -1
    mkdir -p "$tmp/stage/System/Library/Extensions" "$tmp/stage/usr/sbin"
    cp -R "$WORK"/out/kexts/*.kext "$tmp/stage/System/Library/Extensions/"
    "$DEBUGFS" -R "cat /usr/sbin/pd-rebuild-kc" "$root" 2>/dev/null |
      awk -v add="$(cd "$WORK/out/kexts" && for k in *.kext; do [ "$k" = RavynHDAudio.kext ] || printf 'set -- "$@" -kext "$KEXTS/%s"\\n' "$k"; done)" \
        '/^for p in "\$KEXTS"\/System.kext/ && !done { printf "%s", add; done = 1 } { print }' > "$tmp/stage/usr/sbin/pd-rebuild-kc"
    grep -q ClokePS2 "$tmp/stage/usr/sbin/pd-rebuild-kc" || { echo "pd-rebuild-kc nicht ergänzt" >&2; exit 1; }
    chmod 755 "$tmp/stage/usr/sbin/pd-rebuild-kc"
  fi
fi
# Netzwerk auf arm64: Netzwerkkarten selbst benennen und eine feste Adresse für QEMUs Benutzernetz
# setzen, danach die Uhr stellen (tools/). Das Minimal-Image hat kein configd. Im Desktop-Image
# benennt configd die Karte zwar, holt aber keine Adresse, und startet es nach clokeos-network,
# räumt es dessen Default-Route wieder ab (dann keine Uhrzeit, kein Internet). brand.sh nimmt
# configd dort deshalb heraus (brand_ext4). Eine Echtzeituhr liest PureDarwin auf arm64 nie.
# Auf x86_64 holt configd die Adresse per DHCP, die Uhr kommt aus der RTC.
if [ "$CLOKE_ARCH" = arm64 ] || ! "$DEBUGFS" -R 'stat /usr/libexec/configd' "$root" 2>/dev/null | grep -q '^Inode'; then
  mkdir -p "$tmp/stage/usr/libexec" "$tmp/stage/System/Library/LaunchDaemons"
  "$HERE/tools/build.sh" "$tmp/stage/usr/libexec"
  cp "$HERE/tools/clokeos-network" "$tmp/stage/usr/libexec/"
  cp "$HERE/tools/com.clokeos.network.plist" "$tmp/stage/System/Library/LaunchDaemons/"
fi
# PureDarwins diskarbitrationd ignoriert SIGTERM: Beim Herunterfahren wartet launchd sonst
# 20 Sekunden (ExitTimeOut), bevor es ihn hart beendet. Er hält nichts, was gesichert werden müsste.
da=/System/Library/LaunchDaemons/com.apple.diskarbitrationd.plist
if "$DEBUGFS" -R "cat $da" "$root" 2>/dev/null | grep -q '</dict>'; then
  mkdir -p "$tmp/stage$(dirname "$da")"
  "$DEBUGFS" -R "cat $da" "$root" 2>/dev/null |
    sed 's|^</dict>|	<key>ExitTimeOut</key>\
	<integer>2</integer>\
</dict>|' > "$tmp/stage$da"
  chmod 644 "$tmp/stage$da"
fi
find "$tmp/stage" -name .DS_Store -delete

mkdir -p "$tmp/stage/System/Library/CoreServices"
cat > "$tmp/stage/System/Library/CoreServices/SystemVersion.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>ProductBuildVersion</key>
	<string>$CLOKE_BUILD</string>
	<key>ProductCopyright</key>
	<string>$(date +%Y) ClokeOS. Darwin: Apple Inc. (APSL 2.0), PureDarwin $PD_VERSION.</string>
	<key>ProductName</key>
	<string>$CLOKE_NAME</string>
	<key>ProductUserVisibleVersion</key>
	<string>$CLOKE_VERSION</string>
	<key>ProductVersion</key>
	<string>$CLOKE_MACOS_COMPAT</string>
</dict>
</plist>
EOF

cat > "$tmp/stage/etc/clokeos-release" <<EOF
NAME="$CLOKE_NAME"
VERSION="$CLOKE_VERSION"
CODENAME="$CLOKE_CODENAME"
BUILD="$CLOKE_BUILD"
HOSTNAME="$CLOKE_HOST"
TIMEZONE="$CLOKE_TZ"
LOGIN="$CLOKE_LOGIN"
SHELL_DEFAULT="/bin/$CLOKE_SHELL"
BASE="PureDarwin $PD_VERSION"
EOF
printf '%s\n' "$CLOKE_LANGUAGE" > "$tmp/stage/etc/clokeos-language"

# bash: /bin/bash, die Shell-Liste und /etc/profile lädt /etc/bashrc für interaktive bash
"$HERE/tools/bash-build.sh" "$tmp/stage"
{ "$DEBUGFS" -R 'cat /etc/shells' "$root" 2>/dev/null; echo /bin/bash; } | awk '!seen[$0]++' > "$tmp/stage/etc/shells"
"$DEBUGFS" -R 'cat /etc/profile' "$root" 2>/dev/null > "$tmp/stage/etc/profile"
grep -q PATH "$tmp/stage/etc/profile" || { echo "/etc/profile im Basisimage nicht gefunden" >&2; exit 1; }
printf '\n# ClokeOS\n[ -n "$BASH_VERSION" ] && [ -r /etc/bashrc ] && . /etc/bashrc\n' >> "$tmp/stage/etc/profile"

# tux: Linux-Programme und der Linux-Baum /TUX (Desktop)
[ "$CLOKE_FLAVOR" = desktop ] && : > "$tmp/tux.cmds" && "$HERE/tux/image.sh" "$tmp/stage" "$tmp/tux.cmds"
# xlat: x86_64-Programme auf arm64 (Übersetzer ucrun + exec-Haken, siehe docs/translation-study.md)
[ "$CLOKE_FLAVOR" = desktop ] && "$HERE/xlat/image.sh" "$tmp/stage" "$tmp/tux.cmds" || true

# MIME-Datenbank (mime.cache), damit Fundus Programme anderer Systeme (ELF, x86_64-Mach-O) erkennt
[ "$CLOKE_FLAVOR" = desktop ] && "$HERE/mime/build.sh" "$tmp/stage" || true

# --- Desktop: Homebrew (siehe README, "Homebrew") ------------------------------------------
# libcompat: fehlende libSystem-Symbole und libffi.dylib; Absolute /usr/bin-Pfade, die Homebrew
# erwartet (PureDarwin legt die Werkzeuge nach /bin); Präfix für den Benutzer anlegen (kein sudo)
hb_cmds=
if [ "$CLOKE_FLAVOR" = desktop ]; then
  "$HERE/libcompat/build.sh" "$tmp/stage"
  "$HERE/macapps/carbon.sh" "$tmp/stage"   # Carbon-Hülle und Carbon-Funktionen in CoreServices (Logic Pros OMF)
  "$HERE/raven/build.sh" "$tmp/stage"   # Windows-Programme: Raven (nur x86_64), siehe docs/raven-port.md
  for n in curl date false find grep openssl printf readlink sed stat sw_vers tail true whoami xargs tar gzip \
           gunzip xz uname id ls mkdir rm cp mv ln chmod chown touch cat head sort uniq tr wc cut tee basename \
           dirname mktemp diff cmp awk file hostname which ps sysctl expr dd df du ssh ssh-keygen sleep kill \
           rmdir test bash zsh sh; do
    if "$DEBUGFS" -R "stat /bin/$n" "$root" 2>/dev/null | grep -q '^Inode' &&
       ! "$DEBUGFS" -R "stat /usr/bin/$n" "$root" 2>/dev/null | grep -q '^Inode' && [ ! -e "$tmp/stage/usr/bin/$n" ]; then
      ln -s "../../bin/$n" "$tmp/stage/usr/bin/$n"
    fi
  done
  if [ "$CLOKE_ARCH" = arm64 ]; then
    hb_prefix=/opt/homebrew
    mkdir -p "$tmp/stage$hb_prefix"
    hb_cmds="$(printf 'set_inode_field %s mode 040775\nset_inode_field %s uid %s\nset_inode_field %s gid 20\n' \
      "$hb_prefix" "$hb_prefix" "${CLOKE_USER_UID}" "$hb_prefix")"
  else
    hb_cmds="$(printf 'set_inode_field /usr/local mode 040775\nset_inode_field /usr/local gid 20\n')"
  fi
  # Homebrew im PATH, sobald es eingerichtet ist (clokeos-install-homebrew)
  cat >> "$tmp/stage/etc/profile" <<'EOF2'
if [ -x /opt/homebrew/bin/brew ]; then
  export HOMEBREW_PREFIX=/opt/homebrew HOMEBREW_CELLAR=/opt/homebrew/Cellar HOMEBREW_REPOSITORY=/opt/homebrew
  PATH=/opt/homebrew/bin:/opt/homebrew/sbin:$PATH
elif [ -x /usr/local/bin/brew ]; then
  export HOMEBREW_PREFIX=/usr/local HOMEBREW_CELLAR=/usr/local/Cellar HOMEBREW_REPOSITORY=/usr/local/Homebrew
  PATH=/usr/local/bin:/usr/local/sbin:$PATH
fi
export PATH
EOF2
fi

"$DEBUGFS" -R 'cat /etc/zshrc' "$root" 2>/dev/null > "$tmp/stage/etc/zshrc"
grep -q 'interactive' "$tmp/stage/etc/zshrc" || { echo "/etc/zshrc im Basisimage nicht gefunden" >&2; exit 1; }
cat >> "$tmp/stage/etc/zshrc" <<'EOF'

# ClokeOS
export PS1='%n@%m %1~ %# '
[[ -r /etc/clokeos-release ]] && export TZ=$(. /etc/clokeos-release; print -r -- $TIMEZONE)
# Die Konsole startet oft, bevor com.clokeos.hostname den Namen gesetzt hat.
_clokeos_host() { [[ $HOST == localhost* ]] && HOST=$(/bin/hostname) }
precmd_functions+=(_clokeos_host)
# Eine interaktive zsh ignoriert SIGTERM; beim Herunterfahren wartet launchd dann 20 Sekunden,
# bevor es die Konsole hart beendet. So beendet sie sich sofort.
TRAPTERM() { exit 143 }
# Vor dem Ausschalten alles auf die Platte schreiben: auf arm64 hängt PureDarwins Kernel am Ende
# des Herunterfahrens (Halt/Restart Timed Out), run.sh schaltet die VM dann aus.
poweroff() { sync; command poweroff "$@" }
reboot() { sync; command reboot "$@" }
# x86_64-Programme auf arm64 (xlat/image.sh): die Shell braucht den exec-Haken schon beim Start, deshalb einmal neu starten
[[ -r /etc/clokeos-xlat.zsh ]] && . /etc/clokeos-xlat.zsh
if [[ -z $CLOKEOS_GREETED && -r /etc/motd ]]; then
  export CLOKEOS_GREETED=1
  cat /etc/motd
fi
EOF

# --- Desktop: Benutzerkonto wie im Prototyp ------------------------------------------------
# Ohne Passwort (leeres Feld, die Eingabetaste meldet an), Gruppe staff, Ordner wie in clokeos.html.
# PureDarwin hat kein su und kein sudo; für Verwaltung gibt es auf der Anmeldung root.
: > "$tmp/extra.cmds"
[ ! -f "$tmp/tux.cmds" ] || cat "$tmp/tux.cmds" >> "$tmp/extra.cmds"
[ -z "$hb_cmds" ] || printf '%s\n' "$hb_cmds" >> "$tmp/extra.cmds"
if [ "$CLOKE_FLAVOR" = desktop ]; then
  # /Applications wie bei macOS für Administratoren beschreibbar; hier die Gruppe staff, damit
  # der Benutzer Programme durch Hineinziehen installieren kann
  printf 'set_inode_field /Applications mode 040775\nset_inode_field /Applications gid 20\n' >> "$tmp/extra.cmds"
  # /NT = Windows-Baum des Systems (NT-Oberfläche): Windows, Program Files usw. legt Raven (x86_64) beim
  # ersten Start an (/usr/bin/raven); Gruppe staff darf schreiben, Users hat das Sticky-Bit wie /tmp
  {
    echo "mkdir /NT"
    echo "mkdir /NT/Windows"
    echo "mkdir /NT/Users"
    echo "set_inode_field /NT mode 040775"
    echo "set_inode_field /NT gid 20"
    echo "set_inode_field /NT/Windows mode 040775"
    echo "set_inode_field /NT/Windows gid 20"
    echo "set_inode_field /NT/Users mode 041777"
  } >> "$tmp/extra.cmds"
fi
if [ "$CLOKE_FLAVOR" = desktop ] && [ -n "$CLOKE_USER" ]; then
  home="/Users/$CLOKE_USER"
  for f in master.passwd:root passwd:root group:staff; do
    "$DEBUGFS" -R "cat /etc/${f%:*}" "$root" 2>/dev/null > "$tmp/stage/etc/${f%:*}"
    grep -q "^${f#*:}:" "$tmp/stage/etc/${f%:*}" || { echo "/etc/${f%:*} im Basisimage nicht gefunden" >&2; exit 1; }
  done
  printf '%s::%s:20::0:0:%s:%s:/bin/%s\n' "$CLOKE_USER" "$CLOKE_USER_UID" "$CLOKE_USER_NAME" "$home" "$CLOKE_SHELL" \
    >> "$tmp/stage/etc/master.passwd"
  printf '%s:*:%s:20:%s:%s:/bin/%s\n' "$CLOKE_USER" "$CLOKE_USER_UID" "$CLOKE_USER_NAME" "$home" "$CLOKE_SHELL" \
    >> "$tmp/stage/etc/passwd"
  sed -i '' "s/^staff:\(.*\)$/staff:\1,$CLOKE_USER/" "$tmp/stage/etc/group"
  # wie bei macOS nur für root lesbar; die übrigen Programme lesen /etc/passwd
  chmod 600 "$tmp/stage/etc/master.passwd"

  h="$tmp/stage$home"
  # Ordner wie bei macOS mit englischen Namen
  mkdir -p "$h"/{Desktop,Documents,Downloads,Pictures,Music,Movies,.Trash,.config/gtk-3.0}
  cp "$HERE/overlay-desktop/var/root/.config/gtk-3.0/gtk.css" "$h/.config/gtk-3.0/"
  # Seitenleiste von Fundus (Thunar): Favoriten wie im Finder
  for d in Documents Downloads Pictures Music Movies; do printf 'file://%s/%s %s\n' "$home" "$d" "$d"; done > "$h/.config/gtk-3.0/bookmarks"
  echo 'file:///Volumes Volumes' >> "$h/.config/gtk-3.0/bookmarks"
  for f in profile zprofile zshrc; do printf 'test -r /etc/%s && . /etc/%s\n' $f $f > "$h/.$f"; done
  printf "alias ll='ls -la'\n" >> "$h/.zshrc"
  printf 'test -r /etc/profile && . /etc/profile\ntest -r ~/.bashrc && . ~/.bashrc\n' > "$h/.bash_profile"
  printf 'test -r /etc/bashrc && . /etc/bashrc\n' > "$h/.bashrc"
  cat > "$h/Desktop/Welcome.txt" <<EOF
Welcome to ClokeOS $CLOKE_VERSION "$CLOKE_CODENAME"

ClokeOS is a macOS-like desktop on Darwin: the XNU kernel with PureDarwin
$PD_VERSION underneath and CloakServer as its window server.
A few things to try:

• In Terminal: clokefetch, uname -a, sw_vers
• Drag windows, resize them at the corner, yellow = minimize to the Dock
• Appearance in the menu bar: auto, light or dark
• System Settings > Language & Region: English or Deutsch
• Log out in the ClokeOS menu

This account has no password. For administration, log in as
System Administrator; ClokeOS does not have su or sudo yet.
EOF
  # Besitzer und Rechte im Image: alles unter $home gehört dem Konto
  {
    echo "set_inode_field /Users mode 040755"
    (cd "$tmp/stage" && find ".$home" | sed 's|^\.||' | sort) | while read -r f; do
      [ -d "$tmp/stage$f" ] && echo "set_inode_field $f mode 040755"
      echo "set_inode_field $f uid $CLOKE_USER_UID"
      echo "set_inode_field $f gid 20"
    done
    echo "set_inode_field $home/.Trash mode 040700"
  } >> "$tmp/extra.cmds"
fi

# --- in ein ext4-Dateisystem schreiben ----------------------------------------------------
# brand_ext4 <ext4-datei> <volume-name|"">: Overlay hinein, prüfen, ob alles angekommen ist.
brand_ext4() {
  local fs="$1" label="$2" cmds="$tmp/debugfs.cmds" f
  {
    echo "rm /System/Library/LaunchDaemons/org.puredarwin.hostname.plist"
    echo "rm /usr/libexec/pd-set-hostname"
    [ "$CLOKE_ARCH" = arm64 ] && echo "rm /System/Library/LaunchDaemons/com.apple.configd.plist"
    (cd "$tmp/stage" && find . -mindepth 1 -type d | sed 's|^\.||' | sort) | while read -r d; do
      echo "mkdir $d"
    done
    (cd "$tmp/stage" && find . -type f | sed 's|^\.||' | sort) | while read -r f; do
      mode=$(stat -f '%Lp' "$tmp/stage$f")
      echo "rm $f"
      echo "write $tmp/stage$f $f"
      echo "set_inode_field $f mode 0100$mode"
      echo "set_inode_field $f uid 0"
      echo "set_inode_field $f gid 0"
    done
    # Symlinks (Framework-Bundles: Versions/Current, Resources, die Programmdatei)
    (cd "$tmp/stage" && find . -type l | sed 's|^\.||' | sort) | while read -r f; do
      echo "rm $f"
      echo "symlink $f $(readlink "$tmp/stage$f")"
    done
    cat "$tmp/extra.cmds"
  } > "$cmds"
  "$DEBUGFS" -w -f "$cmds" "$fs" > "$tmp/debugfs.log" 2>&1
  [ -n "$label" ] && "$TUNE2FS" -L "$label" "$fs" >/dev/null
  "$E2FSCK" -fn "$fs" > "$tmp/e2fsck.log" 2>&1 || { cat "$tmp/e2fsck.log" >&2; exit 1; }

  for f in $(cd "$tmp/stage" && find . -type f | sed 's|^\.||'); do
    if ! "$DEBUGFS" -R "cat $f" "$fs" 2>/dev/null | cmp -s - "$tmp/stage$f"; then
      echo "Nicht geschrieben: $f in $fs (siehe $tmp/debugfs.log)" >&2
      exit 1
    fi
  done
  for f in $(cd "$tmp/stage" && find . -type l | sed 's|^\.||'); do
    if ! "$DEBUGFS" -R "stat $f" "$fs" 2>/dev/null | grep -qF "Fast link dest: \"$(readlink "$tmp/stage$f")\""; then
      echo "Symlink nicht geschrieben: $f in $fs (siehe $tmp/debugfs.log)" >&2
      exit 1
    fi
  done
}

brand_ext4 "$root" "Cloke HD"
dd if="$root" of="$tmp/image.img" bs=512 seek="$root_start" conv=notrunc status=none

# --- EFI-Partition ------------------------------------------------------------------------
read -r esp_start _ < <(gpt_part "$tmp/image.img" "$GUID_ESP")
esp="$tmp/image.img@@$((esp_start * 512))"

# Manche PureDarwin-Images (arm64-Minimal) booten eine RAM-Disk statt der Wurzelpartition.
if mdir -i "$esp" ::/ramdisk.img >/dev/null 2>&1; then
  mcopy -n -i "$esp" ::/ramdisk.img "$tmp/ramdisk.ext4"
  brand_ext4 "$tmp/ramdisk.ext4" ""
  mcopy -o -i "$esp" "$tmp/ramdisk.ext4" ::/ramdisk.img
  mtype -i "$esp" ::/ramdisk.img | cmp -s - "$tmp/ramdisk.ext4" ||
    { echo "RAM-Disk nicht in der EFI-Partition angekommen" >&2; exit 1; }
fi

if [ -n "$LOADER_EFI" ]; then
  mcopy -o -i "$esp" "$LOADER_EFI" "::/EFI/BOOT/$EFI_BINARY"
  mtype -i "$esp" "::/EFI/BOOT/$EFI_BINARY" | cmp -s - "$LOADER_EFI" ||
    { echo "Loader nicht in der EFI-Partition angekommen" >&2; exit 1; }
fi
# Grafiktreiber im Kernel Collection korrigieren (kcfix.py): auf x86_64 legt IOGOPFramebuffer
# /dev/fb0 und Xorgs Bild ein Byte neben den Framebuffer (falsche Farben), auf arm64 gerät der
# Kernel in Panik, sobald Xorg über IOVirtIOGPU das erste Bild zeigt.
# Im x86_64-Desktop stattdessen der Kernel Collection von kext/build.sh: derselbe, neu gebaut mit den
# eigenen Treibern (ClokePS2) und ebenso korrigiert.
if [ "$CLOKE_ARCH" = x86_64 ] || [ "$CLOKE_FLAVOR" = desktop ]; then
  mcopy -n -i "$esp" ::/EFI/BOOT/kernel "$tmp/kernel"
  python3 -I "$HERE/kcfix.py" "$tmp/kernel"
  [ "$CLOKE_ARCH" = x86_64 ] && [ "$CLOKE_FLAVOR" = desktop ] && cp "$WORK/out/kernel" "$tmp/kernel"
  # Optional: eigener Kernel (CLOKEOS_KERNEL=Pfad, arm64), z. B. Darwin 20.6.0 laut docs/kernel-20.6.md
  if [ -n "${CLOKEOS_KERNEL:-}" ]; then
    [ "$CLOKE_ARCH" = arm64 ] && [ -f "$CLOKEOS_KERNEL" ] ||
      { echo "CLOKEOS_KERNEL: nur arm64, Datei muss existieren ($CLOKEOS_KERNEL)" >&2; exit 1; }
    cp "$CLOKEOS_KERNEL" "$tmp/kernel"
    python3 -I "$HERE/kcfix.py" "$tmp/kernel"
  fi
  if [ "$CLOKE_ARCH" = x86_64 ] && [ "$CLOKE_FLAVOR" = desktop ] && [ -f "$WORK/out/kernel-up" ]; then
    mcopy -o -i "$esp" "$WORK/out/kernel-up" ::/EFI/BOOT/kernel-up # U beim Einschalten: ein Kern
  fi
  mcopy -o -i "$esp" "$tmp/kernel" ::/EFI/BOOT/kernel
  mtype -i "$esp" ::/EFI/BOOT/kernel | cmp -s - "$tmp/kernel" ||
    { echo "Kernel nicht in der EFI-Partition angekommen" >&2; exit 1; }
fi
if [ -n "$CLOKE_BOOT_ARGS" ]; then
  printf '%s\n' "$CLOKE_BOOT_ARGS" > "$tmp/boot-args.txt"
  mcopy -o -i "$esp" "$tmp/boot-args.txt" ::/EFI/BOOT/boot-args.txt
  mtype -i "$esp" ::/EFI/BOOT/boot-args.txt | cmp -s - "$tmp/boot-args.txt" ||
    { echo "boot-args.txt nicht in der EFI-Partition angekommen" >&2; exit 1; }
fi

mv "$tmp/image.img" "$CLOKE_IMG"
rm -rf "$tmp"
echo "Fertig: $CLOKE_IMG"
env=""
[ "$CLOKE_ARCH" = x86_64 ] || env+="CLOKEOS_ARCH=$CLOKE_ARCH "
[ "$CLOKE_FLAVOR" = minimal ] || env+="CLOKEOS_FLAVOR=$CLOKE_FLAVOR "
echo "Starten mit: $env$HERE/run.sh"
