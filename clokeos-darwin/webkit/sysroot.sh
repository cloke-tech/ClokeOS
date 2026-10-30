#!/bin/bash
# Holt für den WebKitGTK-Cross-Build alle Bibliotheken, Header und pkg-config-Dateien (ohne LLVM) aus dem
# arm64-Desktop-Image nach $WORK/sysroot/webkit-$CLOKE_ARCH/ (debugfs, ohne das Image zu booten).
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS"
sr="$WORK/sysroot/webkit-$CLOKE_ARCH"
read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"
mkdir -p "$sr/lib" "$sr/usr"
"$DEBUGFS" -R "rdump /usr/include $sr/usr" "$fs" >/dev/null 2>&1 || true
"$DEBUGFS" -R "rdump /include $sr" "$fs" >/dev/null 2>&1 || true
for n in $("$DEBUGFS" -R "ls /lib" "$fs" 2>/dev/null | tr -s ' ' '\n' | grep -v '^[0-9(]' | grep -v -e '^\.\.\?$' -e LLVM -e clang -e '^python' -e '^cmake' -e wlroots); do
  [ -e "$sr/lib/$n" ] || "$DEBUGFS" -R "rdump /lib/$n $sr/lib" "$fs" >/dev/null 2>&1 || true
done
for n in $("$DEBUGFS" -R "ls /usr/lib" "$fs" 2>/dev/null | tr -s ' ' '\n' | grep -v '^[0-9(]' | grep -v -e '^\.\.\?$' -e LLVM -e clang -e '^python'); do
  [ -e "$sr/usr/lib/$n" ] || "$DEBUGFS" -R "rdump /usr/lib/$n $sr/usr/lib" "$fs" >/dev/null 2>&1 || true
done
cp -f "$sr/usr/lib/libSystem.B.dylib" "$sr/usr/lib/libSystem.dylib" 2>/dev/null || true
mkdir -p "$sr/usr/lib/system"; "$DEBUGFS" -R "dump /usr/lib/system/libdyld.dylib $sr/usr/lib/system/libdyld.dylib" "$fs" >/dev/null 2>&1 || true
"$DEBUGFS" -R "dump /usr/lib/libSystem.B.dylib $sr/usr/lib/libSystem.B.dylib" "$fs" >/dev/null 2>&1 || true
touch "$sr/.fertig"; du -sh "$sr"
# Die .pc-Dateien nennen /nix/store/<hash>-<paket>; im Sysroot liegt alles flach unter include/ und lib/.
for f in "$sr"/lib/pkgconfig/*.pc; do [ -L "$f" ] && continue; sed -i '' -E 's#/nix/store/[^/ ]+##g' "$f"; done
# EGL/GL-Header: PDs usr/include selbst darf nicht in den Suchpfad (seine C-Header verdecken die des SDK), nur diese Unterordner
mkdir -p "$sr/glinc"; for d in EGL GL GLES2 GLES3 KHR; do ln -sfn "../usr/include/$d" "$sr/glinc/$d"; done
# Platzhalter-.pc für Pakete, die in den .pc-Dateien des Images verlangt werden, aber nicht im Image liegen (pixman-1, xproto, ...)
export PKG_CONFIG_LIBDIR="$sr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$sr"
for _ in 1 2 3 4 5; do
  miss=$(for f in "$sr"/lib/pkgconfig/*.pc; do pkg-config --cflags "$(basename "$f" .pc)" 2>&1 | grep -o "Package '[^']*', required" | sed "s/Package '//;s/', required//"; done | sort -u) || true
  [ -z "$miss" ] && break
  for m in $miss; do printf 'Name: %s\nDescription: Platzhalter (Paket nicht im Image)\nVersion: 99.0\n' "$m" > "$sr/lib/pkgconfig/$m.pc"; done
done
# libgpg-error fehlt im Image (libgcrypt braucht es): selbst bauen, siehe README
# EGL: eglplatform.h kennt Darwin nicht (EGLNativeDisplayType wäre int); PD verhält sich wie Unix
sed -i "" "s/^#elif defined(__unix__)\$/#elif defined(__unix__) || defined(__APPLE__)/" "$sr/usr/include/EGL/eglplatform.h"
# libintl.h: glibs gi18n-lib.h definiert gettext & Co. als Makros, bevor WebKit libintl.h einbindet; dann scheitern dessen Inline-Funktionen
[ -L "$sr/include/libintl.h" ] && { t=$(readlink "$sr/include/libintl.h"); rm "$sr/include/libintl.h"; cp "$sr/include/$t" "$sr/include/libintl.h" 2>/dev/null || cp "$t" "$sr/include/libintl.h"; }
sed -i "" 's/^#define _LIBINTL_H 1$/#define _LIBINTL_H 1\n#undef gettext\n#undef dgettext\n#undef dcgettext\n#undef ngettext\n#undef dngettext\n#undef dcngettext\n#undef textdomain\n#undef bindtextdomain\n#undef bind_textdomain_codeset/' "$sr/include/libintl.h"
