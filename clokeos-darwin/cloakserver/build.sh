#!/bin/bash
# Baut CloakServer, den Fensterserver von ClokeOS, auf dem Mac für PureDarwin.
#
#   cloakserver/build.sh [stage]   ohne stage: nur bauen nach ~/ClokeOS-work/out/CloakServer/
#
# Architektur wie überall mit CLOKEOS_ARCH (Standard: x86_64; arm64 baut gegen PureDarwins
# arm64-Desktop-Image). Das Programm kommt jeweils nach ~/ClokeOS-work/out/CloakServer[-arm64]/.
#
# Übersetzt wird mit Apples clang gegen Header und Bibliotheken aus PureDarwins Desktop-Image
# (Xlib, cairo, pango, librsvg). Die holt das Skript mit debugfs direkt aus dem Image, ohne es
# zu booten, nach ~/ClokeOS-work/sysroot/. Das Ergebnis ist ein Mach-O-Programm wie PureDarwins
# eigene (macOS 11, dyld-Info ohne chained fixups). Logo und Icons erzeugt resources.py aus
# clokeos.html. In <stage> landet es als /System/Library/CoreServices/CloakServer/.
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS" clang python3 osascript

src="$HERE/cloakserver"
out="$WORK/out/CloakServer"
[ "$CLOKE_ARCH" = x86_64 ] || out="$WORK/out/CloakServer-$CLOKE_ARCH"
sysroot="$WORK/sysroot/$CLOKE_ARCH-${PD_SHA256:0:12}"
target=x86_64-apple-macos11.0
[ "$CLOKE_ARCH" = arm64 ] && target=arm64-apple-macos11.0

if [ ! -f "$PD_IMG" ]; then
  echo "Fehlt: $PD_IMG – CloakServer braucht das Desktop-Image (CLOKEOS_FLAVOR=desktop ./fetch.sh)." >&2
  exit 1
fi

# --- Header und Bibliotheken aus dem Image ----------------------------------------------------
read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"
dfs() { "$DEBUGFS" -R "$1" "$fs" >/dev/null 2>&1 || true; }
if [ ! -f "$sysroot/.fertig" ]; then
  echo "Hole Header aus $(basename "$PD_IMG")"
  rm -rf "$sysroot"
  mkdir -p "$sysroot/usr/lib/system" "$sysroot/lib/glib-2.0" "$sysroot/libs"
  dfs "rdump /usr/include $sysroot/usr"
  dfs "rdump /include $sysroot"
  dfs "rdump /lib/glib-2.0/include $sysroot/lib/glib-2.0"
  dfs "dump /usr/lib/system/libdyld.dylib $sysroot/usr/lib/system/libdyld.dylib"
  for f in "$sysroot/usr/lib/system/libdyld.dylib" "$sysroot/include/cairo/cairo.h" "$sysroot/usr/include/stdio.h" \
           "$sysroot/lib/glib-2.0/include/glibconfig.h"; do
    [ -s "$f" ] || { echo "Nicht aus dem Image geholt: $f" >&2; exit 1; }
  done
  touch "$sysroot/.fertig"
fi
# Zum Linken genügen die Bibliotheken, die CloakServer direkt benutzt, unter ihrem -l-Namen.
# Ihre Installationsnamen (otool -D) landen im Programm. Xcomposite und Xdamage gibt es im Image
# nur als statische Archive.
#
# arm64: Dort gibt es Xlib und alle X-Erweiterungen nur als statische Archive (libXcursor.1.dylib
# ist versehentlich eine x86_64-Datei). cairo hat seine eigene Kopie von Xlib eingebaut und
# exportiert einen Teil davon, GTK (libgdk) ebenso. CloakServer bekommt deshalb wie PureDarwins
# Programme eine eigene statische Xlib; sie steht beim Linken vor cairo, damit kein X-Aufruf an
# cairos Kopie gebunden wird. Beide arbeiten auf derselben Display-Struktur, wie bei GTK.
libs=(System:/usr/lib/libSystem.B.dylib cairo:/lib/libcairo.2.dylib
      pango-1.0:/lib/libpango-1.0.0.dylib pangocairo-1.0:/lib/libpangocairo-1.0.0.dylib
      gobject-2.0:/lib/libgobject-2.0.0.dylib glib-2.0:/lib/libglib-2.0.0.dylib rsvg-2:/lib/librsvg-2.2.dylib
      Xcomposite.a:/lib/libXcomposite.a Xdamage.a:/lib/libXdamage.a)
xlibs=(-lX11 -lXext -lXcursor -lXfixes -lXcomposite -lXdamage)
if [ "$CLOKE_ARCH" = arm64 ]; then
  libs+=(X11.a:/lib/libX11.a Xext.a:/lib/libXext.a Xcursor.a:/lib/libXcursor.a Xfixes.a:/lib/libXfixes.a
         Xrender.a:/lib/libXrender.a xcb.a:/lib/libxcb.a Xau.a:/lib/libXau.a Xdmcp.a:/lib/libXdmcp.a)
  xlibs+=(-lXrender -lxcb -lXau -lXdmcp)
else
  libs+=(X11:/lib/libX11.6.dylib Xext:/lib/libXext.6.dylib Xcursor:/lib/libXcursor.1.dylib
         Xfixes:/lib/libXfixes.3.dylib)
fi
for spec in "${libs[@]}"; do
  name="${spec%%:*}"
  case "$name" in *.a) f="$sysroot/libs/lib$name" ;; *) f="$sysroot/libs/lib$name.dylib" ;; esac
  [ -s "$f" ] || dfs "dump ${spec#*:} $f"
  [ -s "$f" ] || { echo "Nicht aus dem Image geholt: ${spec#*:}" >&2; exit 1; }
done
[ -s "$sysroot/usr/lib/libSystem.B.dylib" ] || cp "$sysroot/libs/libSystem.dylib" "$sysroot/usr/lib/libSystem.B.dylib"

# --- übersetzen ------------------------------------------------------------------------------
rm -rf "$out"
mkdir -p "$out/Resources"
python3 -I "$src/resources.py" "$HERE/../clokeos.html" "$out/Resources"

inc=(-I"$sysroot/include" -I"$sysroot/include/cairo" -I"$sysroot/include/pango-1.0" -I"$sysroot/include/glib-2.0"
     -I"$sysroot/lib/glib-2.0/include" -I"$sysroot/include/harfbuzz" -I"$sysroot/include/librsvg-2.0"
     -I"$sysroot/include/gdk-pixbuf-2.0" -I"$sysroot/include/freetype2")
# PLATFORM_MacOSX: PureDarwins sys/cdefs.h ist die unbearbeitete Fassung aus Apples Libc und
# wählt Symbolnamen (fopen statt fopen$UNIX2003) erst mit diesem Makro wie für macOS.
# -fno-stack-check: PureDarwins libSystem hat kein ___chkstk_darwin, das clang auf arm64 für
# Funktionen mit mehr als 4 KiB Stack aufruft. Chained fixups kennt PureDarwins dyld nicht.
clang -target "$target" -isysroot "$sysroot" -DPLATFORM_MacOSX=1 -fno-stack-check -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
  -Wno-nullability-completeness -Wno-missing-field-initializers "${inc[@]}" \
  -nostdlib -L"$sysroot/libs" -Wl,-platform_version,macos,11.0,11.5 -Wl,-no_fixup_chains -Wl,-dead_strip \
  -o "$out/CloakServer" "$src/cloakserver.c" "$src/comp.c" "$src/wall.c" "$src/apps.c" "$src/l10n.c" \
  -lSystem "${xlibs[@]}" -lcairo -lpango-1.0 -lpangocairo-1.0 -lgobject-2.0 -lglib-2.0 -lrsvg-2
echo "Fertig: $out/CloakServer"
# Testwerkzeug: echte Maus- und Tastenereignisse über XTest (vmctl.py click/key/type), siehe xtest.c
clang -target "$target" -isysroot "$sysroot" -DPLATFORM_MacOSX=1 -fno-stack-check -std=gnu11 -O2 -Wall -Wextra \
  -Wno-nullability-completeness -I"$sysroot/include" -nostdlib -L"$sysroot/libs" -Wl,-platform_version,macos,11.0,11.5 \
  -Wl,-no_fixup_chains -Wl,-dead_strip -o "$out/clokeos-xtest" "$src/xtest.c" -lSystem "${xlibs[@]}"

# open: Dateien, Ordner und Programme öffnen wie unter macOS (führt CloakServer aus, siehe open.c)
clang -target "$target" -isysroot "$sysroot" -DPLATFORM_MacOSX=1 -fno-stack-check -std=gnu11 -O2 -Wall -Wextra \
  -Wno-nullability-completeness -I"$sysroot/include" -nostdlib -L"$sysroot/libs" -Wl,-platform_version,macos,11.0,11.5 \
  -Wl,-no_fixup_chains -Wl,-dead_strip -o "$out/open" "$src/open.c" -lSystem "${xlibs[@]}"

# Automount für USB-Sticks und andere Datenträger (FAT lesen und schreiben, HFS+ lesen), siehe automount.c
clang -target "$target" -isysroot "$sysroot" -DPLATFORM_MacOSX=1 -fno-stack-check -std=gnu11 -O2 -Wall -Wextra \
  -Wno-nullability-completeness -I"$sysroot/include" -nostdlib -L"$sysroot/libs" -Wl,-platform_version,macos,11.0,11.5 \
  -Wl,-no_fixup_chains -Wl,-dead_strip -o "$out/clokeos-automount" "$src/automount.c" -lSystem

# arm64: PureDarwins libgdk hat Xlib und alle Erweiterungen statisch eingebaut, nur die 13
# Xcursor-Funktionen sucht es zur Laufzeit im flachen Namensraum. Die einzige libXcursor.1.dylib
# des Images ist aber für x86_64 gebaut, und so bricht jedes GTK-Programm ab, sobald es einen
# Mauszeiger setzt (dyld: Symbol not found: _XcursorLibraryLoadCursor). ClokeOS baut sie aus dem
# arm64-Archiv libXcursor.a neu, mit eigener statischer Xlib, Xrender und Xfixes (wie gdk und cairo
# arbeitet sie damit auf derselben Display-Struktur) und nur den Xcursor-Funktionen als Export.
# cloakserver-session lädt sie mit DYLD_INSERT_LIBRARIES in alle Programme der Sitzung.
libdir="$out-lib"
rm -rf "$libdir"
mkdir -p "$libdir/lib" "$libdir/usr/lib"
dylib=(clang -target "$target" -dynamiclib -nostdlib -L"$sysroot/libs" -Wl,-platform_version,macos,11.0,11.5
       -Wl,-no_fixup_chains -Wl,-dead_strip)
if [ "$CLOKE_ARCH" = arm64 ]; then
  printf '_Xcursor*\n' > "$libdir/xcursor.exp"
  "${dylib[@]}" -install_name /lib/libXcursor.1.dylib -compatibility_version 2.0.0 -current_version 2.0.2 \
    -Wl,-exported_symbols_list,"$libdir/xcursor.exp" -o "$libdir/lib/libXcursor.1.dylib" \
    -Wl,-force_load,"$sysroot/libs/libXcursor.a" -lXrender -lXfixes -lX11 -lxcb -lXau -lXdmcp -lSystem
  rm "$libdir/xcursor.exp"
fi

# Xorg ohne Dauerlast: PureDarwins HID-Geräte beantworten poll sofort mit POLLNVAL, Xorg kreist
# dann mit voller Last. cloakserver-xorg lädt xpoll.c in Xorg (siehe dort).
"${dylib[@]}" -isysroot "$sysroot" -DPLATFORM_MacOSX=1 -fno-stack-check -std=gnu11 -O2 -Wall -Wextra \
  -Wl,-not_for_dyld_shared_cache -install_name /usr/lib/libclokeos-xpoll.dylib -o "$libdir/usr/lib/libclokeos-xpoll.dylib" "$src/xpoll.c" -lSystem
echo "Fertig: $(cd "$libdir" && find . -name '*.dylib' | sed 's|^\.||' | tr '\n' ' ')"

if [ $# -ge 1 ]; then
  dest="$1/System/Library/CoreServices/CloakServer"
  mkdir -p "$dest"
  cp -R "$out/." "$dest/"
  cp -R "$libdir/." "$1/"
  mkdir -p "$1/usr/bin"
  ln -sf /System/Library/CoreServices/CloakServer/open "$1/usr/bin/open"   # wie macOS: /usr/bin/open
fi
