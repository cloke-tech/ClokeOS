# Gemeinsame Einstellungen für die ClokeOS-Skripte (wird per "source" geladen).

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Große Dateien (Downloads, gebaute Images, VM-Zustand) liegen außerhalb des Projektordners.
WORK="${CLOKEOS_WORK:-$HOME/ClokeOS-work}"

# Zielarchitektur: x86_64 (Standard) oder arm64 (QEMU virt), z. B. CLOKEOS_ARCH=arm64 ./brand.sh
CLOKE_ARCH="${CLOKEOS_ARCH:-x86_64}"

# Variante: minimal (Standard: nur die Shell) oder desktop (PureDarwins Vorschau-Image mit Xorg,
# Wayland und Xfce, 6,1 GiB für x86_64 und 3,1 GiB für arm64; darauf startet CloakServer, der
# ClokeOS-Desktop), z. B. CLOKEOS_FLAVOR=desktop ./brand.sh.
CLOKE_FLAVOR="${CLOKEOS_FLAVOR:-minimal}"
case "$CLOKE_FLAVOR" in
  minimal|desktop) ;;
  *)
    echo "Unbekannte Variante: $CLOKE_FLAVOR (minimal oder desktop)" >&2
    exit 1
    ;;
esac

# Basis: PureDarwin 20.5. Alle Images haben UEFI, xnu-loader und eine ext4-Wurzel.
PD_VERSION="20.5"
case "$CLOKE_ARCH" in
  x86_64)
    if [ "$CLOKE_FLAVOR" = desktop ]; then
      # Kein Release, sondern das Vorschau-Image von puredarwin.org (Stand 23.08.2026, wie 20.5):
      # dieselbe Basis wie das Minimal-Image plus Xorg, Wayland/sway, Xfce, Wine und Compiler.
      PD_FILE="puredarwin.img"
      PD_URL="https://internal.lab004.dev/private/images/$PD_FILE"
      PD_SHA256="5970582305ef495f046b55720711ee8d7065e00d8a56dbc55bd765887502a50b"
    else
      PD_FILE="puredarwin-minimal.img"
      PD_URL="https://github.com/PureDarwin/PureDarwin/releases/download/$PD_VERSION/$PD_FILE"
      PD_SHA256="ef65a816395680ef9210f401cebe3783c131badca1a942cb80e56bec97e717ac"
    fi
    EFI_BINARY="BOOTX64.EFI"
    ;;
  arm64)
    # PureDarwin hat für arm64 kein Release, nur Vorschau-Images auf puredarwin.org (Stand
    # 23.08.2026, wie das x86_64-Image von 20.5).
    if [ "$CLOKE_FLAVOR" = desktop ]; then
      # Wie puredarwin.img für x86_64: Xorg, Wayland/sway, Xfce und Compiler, Bild über ramfb.
      PD_FILE="puredarwin-arm64-virt.img"
      PD_SHA256="584d9f2601f58079d7bafa307499662a48dfc558cbac7f0fc04c0c143892fb2c"
    else
      # Trotz des Namens die Variante "arm64-virt-minimal-release": Release-Kernel, ext4-Wurzel
      # auf der Partition, keine RAM-Disk.
      PD_FILE="puredarwin-arm64-virt-minimal.img"
      PD_SHA256="351de0f7611f34912653f5fc9e685eba7d5b1724fb502993a59b8f4eebbcc218"
    fi
    PD_URL="https://internal.lab004.dev/private/images/$PD_FILE"
    EFI_BINARY="BOOTAA64.EFI"
    ;;
  *)
    echo "Unbekannte Architektur: $CLOKE_ARCH (x86_64 oder arm64)" >&2
    exit 1
    ;;
esac
PD_IMG="$WORK/download/pd-$PD_VERSION/$PD_FILE"

# Bootloader: PureDarwins xnu-loader (der Stand aus PureDarwin 20.5) mit ClokeOS-Bootlogo,
# gebaut mit mingw-w64 gegen gnu-efi (die Version aus PureDarwins Nix-Build). Siehe loader/.
XNU_LOADER_REV="8295cd89c184785896d007d0554b398d22d8e32f"
XNU_LOADER_URL="https://codeload.github.com/PureDarwin/xnu-loader/tar.gz/$XNU_LOADER_REV"
XNU_LOADER_SHA256="625c13d7302de58519b8c9e25ae10e46cca816ca3a42abe00f47f4ab1668a92a"
XNU_LOADER_TGZ="$WORK/download/xnu-loader/xnu-loader-$XNU_LOADER_REV.tar.gz"
# PureDarwin selbst: Quelle der ACPI-Plattform (kext/pdacpi baut sie mit eigenen Änderungen neu)
PDSRC_REV="1bbaeb2ee6aa41e6f792a0b1e2943cc295b3b290"
PDSRC_URL="https://codeload.github.com/PureDarwin/PureDarwin/tar.gz/$PDSRC_REV"
PDSRC_SHA256="022d0ce38f40acd212ce535e6bc85529b31c62298df603483cdf8b3864cbe795"
PDSRC_TGZ="$WORK/download/puredarwin-src/PureDarwin-${PDSRC_REV:0:7}.tar.gz"
# Firmware des WLAN-Chips RTL8821CE (kext/ClokeRtw88) aus linux-firmware; weitergeben erlaubt
# (kext/ClokeRtw88/LICENCE.rtlwifi_firmware.txt)
RTW8821C_FW_URL="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/rtw88/rtw8821c_fw.bin"
RTW8821C_FW_SHA256="2ef409bc418549fcf294061dd0cae1fc22fd9da79b60524950b25de18732f3f0"
RTW8821C_FW="$WORK/download/rtw88/rtw8821c_fw.bin"
GNU_EFI_VERSION="4.0.2"
GNU_EFI_URL="https://codeload.github.com/ncroxon/gnu-efi/tar.gz/refs/tags/$GNU_EFI_VERSION"
GNU_EFI_SHA256="f31488c3fc7d64257ec8fcad470ec75a936a4db77350fabaf043c52a7acbae72"
GNU_EFI_TGZ="$WORK/download/gnu-efi/gnu-efi-$GNU_EFI_VERSION.tar.gz"
# Globale Menüleiste (Desktop): appmenu-gtk-module aus vala-panel-appmenu (LGPL-3) gibt die Menüs
# von GTK-Programmen an CloakServer. Siehe menubar/.
APPMENU_VERSION="25.04"
APPMENU_URL="https://gitlab.com/vala-panel-project/vala-panel-appmenu/-/archive/$APPMENU_VERSION/vala-panel-appmenu-$APPMENU_VERSION.tar.gz"
APPMENU_SHA256="48d0be87b260056a65a9056a9be7e190b67a508540ebcb1fc18a67349cca0177"
APPMENU_TGZ="$WORK/download/appmenu/vala-panel-appmenu-$APPMENU_VERSION.tar.gz"
# Den eigenen Loader gibt es bisher nur für x86_64; arm64 behält PureDarwins Loader ohne Logo.
LOADER_EFI=""
[ "$CLOKE_ARCH" = x86_64 ] && LOADER_EFI="$WORK/out/BOOTX64.EFI"

# Kernel-Argumente (EFI/BOOT/boot-args.txt). Ohne -v zeigt der Loader das Bootlogo und gibt dem
# Kernel den Framebuffer so (24 Bit), dass PureDarwins Kernel kein Log darauf malt; Kernel-Log und
# Shell laufen über die serielle Konsole (serial=3), auf dem Bildschirm folgt der Desktop.
# Mit "-v gopconsole=1" vorn dran sieht der Bildschirm wieder aus wie bei PureDarwin (Taste V beim
# Start hängt beides einmalig an). PureDarwins eigene Vorgabe hat gopconsole=1 immer dabei.
# Leer = die Boot-Argumente des Basisimages bleiben (arm64-Minimal, solange es dort keinen eigenen
# Loader gibt).
# arm64-Desktop: PureDarwins Vorgabe ohne -v, gopconsole=1, serial_video_mirror=1, pdtrace=1 und
# vgpu_debug=1. Mit virtio-gpu bekommt der Kernel vom Loader keinen Framebuffer (die Firmware bietet
# dort nur Blt an), und mit -v oder serial_video_mirror=1 malt er seine Konsole trotzdem und hängt
# kurz nach dem Start in einer Endlosschleife.
CLOKE_BOOT_ARGS=""
[ "$CLOKE_ARCH" = x86_64 ] && CLOKE_BOOT_ARGS="debug=0x218 -nogzalloc_mode keepsyms=1 serial=3 gen9_debug=1"
[ "$CLOKE_ARCH/$CLOKE_FLAVOR" = arm64/desktop ] &&
  CLOKE_BOOT_ARGS="debug=0x218 -nogzalloc_mode keepsyms=1 serial=3 no_interrupt_masked_debug=1"

# ClokeOS-Version, passend zur Oberfläche in clokeos.html (OS-Objekt).
CLOKE_NAME="ClokeOS"
CLOKE_VERSION="2.8"
# ProductVersion in SystemVersion.plist: die macOS-Version, die Darwin 20.5 entspricht (Big Sur). Homebrew und
# Mac-Programme lesen sie dort; ClokeOS' eigene Version steht in ProductUserVisibleVersion.
CLOKE_MACOS_COMPAT="${CLOKEOS_MACOS_COMPAT:-11.7}"
CLOKE_CODENAME="Presidio"
CLOKE_BUILD="2E3"
CLOKE_HOST="clokeos"
# Zeitzone als POSIX-TZ-Regel: PureDarwin hat keine Zeitzonen-Datenbank, die Uhr der VM läuft in UTC.
# Gilt für Shell und Desktop (Uhr in der Menüleiste). Mitteleuropa mit Sommerzeit:
CLOKE_TZ="CET-1CEST,M3.5.0,M10.5.0/3"
# Desktop: Anmeldebildschirm wie in clokeos.html vor dem Desktop ("ja") oder direkt zum Desktop ("nein")
CLOKE_LOGIN="ja"
# Desktop: Benutzerkonto wie im Prototyp, ohne Passwort, Ordner /Users/<name>; leer = nur root
# Sprache von ClokeOS: en (Standard, wie bei macOS) oder de; landet in /etc/clokeos-language.
# Installierte Systeme fragen im Einrichtungsassistenten danach.
CLOKE_LANGUAGE="${CLOKEOS_LANGUAGE:-en}"
# Anmelde-Shell des Kontos und von root auf der Konsole: zsh (Standard) oder bash (CLOKEOS_SHELL=bash).
# Beide liegen immer im Image (/bin/zsh von PureDarwin, /bin/bash von tools/bash-build.sh).
CLOKE_SHELL="${CLOKEOS_SHELL:-zsh}"
case "$CLOKE_SHELL" in
  zsh|bash) ;;
  *) echo "Unbekannte Shell: $CLOKE_SHELL (zsh oder bash)" >&2; exit 1 ;;
esac
CLOKE_USER="guest"
CLOKE_USER_NAME="Guest"   # CloakServer zeigt ihn auf Deutsch als „Gast“
CLOKE_USER_UID=501
# Online-Releases für Installer und Wiederherstellung (öffentliches Repo, recovery/release.sh):
# die Wiederherstellung lädt von dort clokeos-<arch>.txt und die Partitionsabbilder.
CLOKE_RELEASES_REPO="cloke-tech/ClokeOS"
# Zum Testen ohne GitHub: CLOKEOS_RELEASES_URL=http://10.0.2.2:8099 (Host aus Sicht der VM)
CLOKE_RELEASES_URL="${CLOKEOS_RELEASES_URL:-https://github.com/$CLOKE_RELEASES_REPO/releases/latest/download}"
CLOKE_IMG="$WORK/out/clokeos-$CLOKE_VERSION-$CLOKE_ARCH.img"
[ "$CLOKE_FLAVOR" = desktop ] && CLOKE_IMG="$WORK/out/clokeos-$CLOKE_VERSION-$CLOKE_ARCH-desktop.img"

# Laufzeitdateien der VM (UEFI-Variablen, Sockets, Logs).
VM_STATE="$WORK/vm/$CLOKE_ARCH"

# Homebrew-Werkzeuge
BREW="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
DEBUGFS="$BREW/opt/e2fsprogs/sbin/debugfs"
E2FSCK="$BREW/opt/e2fsprogs/sbin/e2fsck"
TUNE2FS="$BREW/opt/e2fsprogs/sbin/tune2fs"
MKE2FS="$BREW/opt/e2fsprogs/sbin/mke2fs"
if [ "$CLOKE_ARCH" = arm64 ]; then
  QEMU="$BREW/bin/qemu-system-aarch64"
  FW_CODE="$BREW/share/qemu/edk2-aarch64-code.fd"
  FW_VARS="$BREW/share/qemu/edk2-arm-vars.fd"
else
  QEMU="$BREW/bin/qemu-system-x86_64"
  FW_CODE="$BREW/share/qemu/edk2-x86_64-code.fd"
  FW_VARS="$BREW/share/qemu/edk2-i386-vars.fd"
fi
MINGW_CC="$BREW/bin/x86_64-w64-mingw32-gcc"

need() {
  for tool in "$@"; do
    if [ ! -x "$tool" ] && ! command -v "$tool" >/dev/null 2>&1; then
      echo "Fehlt: $tool – installieren mit: brew install qemu e2fsprogs mtools mingw-w64" >&2
      exit 1
    fi
  done
}

# Lädt eine Datei, falls sie fehlt oder nicht stimmt, und prüft die SHA-256-Prüfsumme.
fetch_file() {
  local url="$1" sha="$2" dest="$3"
  if [ -f "$dest" ] && [ "$(shasum -a 256 "$dest" | cut -d' ' -f1)" = "$sha" ]; then
    echo "Schon da: $dest"
    return 0
  fi
  mkdir -p "$(dirname "$dest")"
  echo "Lade $url"
  curl -fL --retry 5 -o "$dest.part" "$url"
  local sum
  sum="$(shasum -a 256 "$dest.part" | cut -d' ' -f1)"
  if [ "$sum" != "$sha" ]; then
    echo "Prüfsumme stimmt nicht für $dest: $sum (erwartet $sha)" >&2
    rm -f "$dest.part"
    exit 1
  fi
  mv "$dest.part" "$dest"
  echo "OK: $dest"
}

# Startsektor und Länge (in 512-Byte-Sektoren) einer GPT-Partition nach Typ-GUID.
# Liest die GPT direkt, damit kein sgdisk/sfdisk nötig ist.
gpt_part() {
  python3 -I - "$1" "$2" <<'PY'
import struct, sys, uuid
img, want = sys.argv[1], uuid.UUID(sys.argv[2])
with open(img, 'rb') as f:
    f.seek(512)
    hdr = f.read(92)
    if hdr[:8] != b'EFI PART':
        sys.exit('keine GPT in ' + img)
    lba, count, size = struct.unpack_from('<QII', hdr, 72)
    f.seek(lba * 512)
    for _ in range(count):
        e = f.read(size)
        if uuid.UUID(bytes_le=e[:16]) == want:
            first, last = struct.unpack_from('<QQ', e, 32)
            print(first, last - first + 1)
            sys.exit(0)
sys.exit('Partition %s nicht gefunden' % want)
PY
}
GUID_ESP="C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
GUID_LINUX_FS="0FC63DAF-8483-4772-8E79-3D69D8477DE4"

# Schrift für Chinesisch, Japanisch, Koreanisch: Noto Sans CJK (SIL Open Font License 1.1), alle Regionen in einer Sammlung
CJK_FONT_URL="https://github.com/notofonts/noto-cjk/raw/main/Sans/OTC/NotoSansCJK-Regular.ttc"
CJK_FONT_SHA256="b76b0433203017ca80401b2ee0dd69350349871c4b19d504c34dbdd80541690a"
CJK_FONT="$WORK/download/fonts/NotoSansCJK-Regular.ttc"
CJK_LICENCE_URL="https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/LICENSE"

# GNU bash für /bin/bash (tools/bash-build.sh)
BASH_VERSION_CLOKE="5.3"
BASH_URL="https://ftp.gnu.org/gnu/bash/bash-$BASH_VERSION_CLOKE.tar.gz"
BASH_SHA256="0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba"
BASH_TARBALL="$WORK/download/bash/bash-$BASH_VERSION_CLOKE.tar.gz"

# Alpine Linux minirootfs (musl, busybox; MIT und GPL) als Linux-Baum /TUX (siehe tux/)
ALPINE_VERSION="3.24.3"
case "$CLOKE_ARCH" in
  arm64) ALPINE_ARCH=aarch64; ALPINE_SHA256="4d3f80afaebd447c5591a63a73bde380a149fe8b2d85488cafabb2ff97551f50" ;;
  *) ALPINE_ARCH=x86_64; ALPINE_SHA256="f8e1cafba9b1b283b5d30d06f9885c8d2bc161ffe9083ade8e2e157813a53407" ;;
esac
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION%.*}/releases/$ALPINE_ARCH/alpine-minirootfs-$ALPINE_VERSION-$ALPINE_ARCH.tar.gz"
ALPINE_TGZ="$WORK/download/alpine/alpine-minirootfs-$ALPINE_VERSION-$ALPINE_ARCH.tar.gz"
