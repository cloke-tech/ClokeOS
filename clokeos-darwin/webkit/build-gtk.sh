#!/bin/bash
# Konfiguriert und baut den GTK-Port (MiniBrowser) für CLOKEOS_ARCH (arm64 oder x86_64).
#   CLOKEOS_ARCH=x86_64 CLOKEOS_FLAVOR=desktop webkit/build-gtk.sh <webkit-quellbaum> <builddir>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/gtk-options.sh"
"$here/configure.sh" GTK "$1" "$2" $GTKOPTS > "$2.cfg.log" 2>&1 || { rm -rf "$2/.x"; tail -20 "$2.cfg.log"; exit 1; }
ninja -C "$2" MiniBrowser
