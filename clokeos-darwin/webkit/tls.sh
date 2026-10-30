#!/bin/bash
# Baut das GIO-TLS-Backend (glib-networking mit GnuTLS aus dem Image) für PureDarwin arm64.
#   webkit/tls.sh <arbeitsordner>   Ergebnis: <arbeitsordner>/gn-build/tls/gnutls/libgiognutls.so
# In der VM nach /opt/webkit/gio-modules/ legen; runmb.sh setzt GIO_MODULE_DIR. Braucht meson (z. B. python3 -m venv + pip install meson).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../config.sh"
w="$1"; sr="$WORK/sysroot/webkit-$CLOKE_ARCH"; sdk="$(xcrun --show-sdk-path)"; ver=2.90.1
mkdir -p "$w"; cd "$w"
[ -d glib-networking-$ver ] || { curl -fsSLO https://download.gnome.org/sources/glib-networking/2.90/glib-networking-$ver.tar.xz; tar xf glib-networking-$ver.tar.xz; }
# gio-2.0.pc nennt gio-querymodules; es muss nur existieren (läuft auf dem Mac nie)
mkdir -p "$sr/bin"; printf '#!/bin/sh\nexit 0\n' > "$sr/bin/gio-querymodules"; chmod +x "$sr/bin/gio-querymodules"
cat > cross-pd.ini <<EOT
[binaries]
c = ['/usr/bin/clang', '-target', '${CLOKE_ARCH}-apple-macos11.0', '-isysroot', '$sdk', '-isystem', '$sr/include']
ar = '/usr/bin/ar'
strip = '/usr/bin/strip'
pkg-config = '$(command -v pkg-config)'
[built-in options]
c_link_args = ['-nostdlib', '-L$sr/usr/lib', '-L$sr/lib', '-Wl,-platform_version,macos,11.0,11.5', '-Wl,-no_fixup_chains', '-lSystem']
pkg_config_path = '$sr/lib/pkgconfig'
[properties]
needs_exe_wrapper = true
sys_root = '$sr'
pkg_config_libdir = '$sr/lib/pkgconfig'
[host_machine]
system = 'darwin'
cpu_family = '$([ "$CLOKE_ARCH" = arm64 ] && echo aarch64 || echo x86_64)'
cpu = '$([ "$CLOKE_ARCH" = arm64 ] && echo aarch64 || echo x86_64)'
endian = 'little'
EOT
export PKG_CONFIG_LIBDIR="$sr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$sr"
rm -rf gn-build
meson setup gn-build glib-networking-$ver --cross-file cross-pd.ini -Dlibproxy=disabled -Dgnome_proxy=disabled -Dopenssl=disabled -Dgnutls=enabled -Dtests=false --prefix=/opt/webkit
meson compile -C gn-build
codesign -s - -f gn-build/tls/gnutls/libgiognutls.so
