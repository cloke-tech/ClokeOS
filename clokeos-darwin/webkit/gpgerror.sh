#!/bin/bash
# Baut libgpg-error (fehlt im Image, libgcrypt braucht es) statisch für PureDarwin und legt es ins Sysroot.
#   webkit/gpgerror.sh <arbeitsordner>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../config.sh"
w="$1"; sr="$WORK/sysroot/webkit-$CLOKE_ARCH"; sdk="$(xcrun --show-sdk-path)"; ver=1.59
mkdir -p "$w"; cd "$w"
[ -d libgpg-error-$ver ] || { curl -fsSLO https://www.gnupg.org/ftp/gcrypt/libgpg-error/libgpg-error-$ver.tar.bz2; tar xf libgpg-error-$ver.tar.bz2; }
host=aarch64-apple-darwin; [ "$CLOKE_ARCH" = x86_64 ] && host=x86_64-apple-darwin   # Upstream kennt diese Darwin-Namen (syscfg/lock-obj-pub.*)
rm -rf "build-$CLOKE_ARCH"; mkdir "build-$CLOKE_ARCH"; cd "build-$CLOKE_ARCH"
../libgpg-error-$ver/configure --host=$host --build=aarch64-apple-darwin --prefix=/ --disable-shared --enable-static --disable-nls --disable-doc --disable-tests --disable-languages \
  CC="clang -target ${CLOKE_ARCH}-apple-macos11.0 -isysroot $sdk" CFLAGS="-O2 -fPIC -include sys/stat.h" LDFLAGS="-nostdlib -L$sr/usr/lib" LIBS="-lSystem" >/dev/null
make -j8 >/dev/null; make install DESTDIR="$PWD/inst" >/dev/null
cp inst/lib/libgpg-error.a "$sr/lib/"; cp inst/include/*.h "$sr/include/"; cp inst/lib/pkgconfig/gpg-error.pc "$sr/lib/pkgconfig/"
echo "libgpg-error im Sysroot"
