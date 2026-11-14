#!/bin/bash
# Baut die kleinen Programme des Docks (Notizen, Rechner, Kalender, Aktivitätsanzeige) als GTK-Programm:
#
#   apps/build.sh <stage>     Architektur wie überall mit CLOKEOS_ARCH (Standard: x86_64)
#
#   /System/Library/CoreServices/ClokeApps/clokeapps
#
# Header und Bibliotheken kommen wie bei der Menüleiste aus dem Desktop-Image (cloakserver/build.sh und
# menubar/build.sh legen sie nach ~/ClokeOS-work/sysroot/).
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need clang
stage="$1"
sysroot="$WORK/sysroot/$CLOKE_ARCH-${PD_SHA256:0:12}"
[ -f "$sysroot/.fertig" ] || { echo "Fehlt: $sysroot – zuerst cloakserver/build.sh" >&2; exit 1; }
read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"
for spec in gtk-3:/lib/libgtk-3.0.dylib gdk-3:/lib/libgdk-3.0.dylib gio-2.0:/lib/libgio-2.0.0.dylib \
            gobject-2.0:/lib/libgobject-2.0.0.dylib glib-2.0:/lib/libglib-2.0.0.dylib \
            gdk_pixbuf-2.0:/lib/libgdk_pixbuf-2.0.0.dylib pango-1.0:/lib/libpango-1.0.0.dylib \
            cairo:/lib/libcairo.2.dylib; do
  f="$sysroot/libs/lib${spec%%:*}.dylib"
  [ -s "$f" ] || "$DEBUGFS" -R "dump ${spec#*:} $f" "$fs" >/dev/null 2>&1 || true
  [ -s "$f" ] || { echo "Nicht aus dem Image geholt: ${spec#*:}" >&2; exit 1; }
done
target=x86_64-apple-macos11.0
[ "$CLOKE_ARCH" = arm64 ] && target=arm64-apple-macos11.0
inc=(-I"$sysroot/include" -I"$sysroot/include/gtk-3.0" -I"$sysroot/include/glib-2.0" -I"$sysroot/lib/glib-2.0/include"
     -I"$sysroot/include/gio-unix-2.0" -I"$sysroot/include/pango-1.0" -I"$sysroot/include/harfbuzz"
     -I"$sysroot/include/cairo" -I"$sysroot/include/gdk-pixbuf-2.0" -I"$sysroot/include/atk-1.0"
     -I"$sysroot/include/freetype2" -I"$sysroot/include/fribidi")
d="$stage/System/Library/CoreServices/ClokeApps"
mkdir -p "$d"
clang -target "$target" -isysroot "$sysroot" -DPLATFORM_MacOSX=1 -fno-stack-check -std=gnu11 -O2 -D_FORTIFY_SOURCE=0 -Wall -Wextra \
  -Wno-unused-parameter -ffile-prefix-map="$HERE/"= -nostdlib -L"$sysroot/libs" \
  -Wl,-platform_version,macos,11.0,11.5 -Wl,-no_fixup_chains "${inc[@]}" -I"$HERE/macapps/coremidi" \
  -o "$d/clokeapps" "$HERE/apps/clokeapps.c" \
  -lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lpango-1.0 -lcairo -lSystem
echo "Fertig: clokeapps für $CLOKE_ARCH"
