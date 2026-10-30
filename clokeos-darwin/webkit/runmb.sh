#!/bin/sh
# PureDarwin hat kein SysV-shm (shmget -> SIGSYS); ignoriert, scheitert der Aufruf mit ENOSYS und cairo weicht aus
trap '' SYS
# gettext fragt ohne LANG die CFPreferences und damit gethostuuid() - das hängt im PureDarwin-Kernel (Prozess in U-Zustand)
export LANG=C LC_ALL=C
export GIO_MODULE_DIR=/opt/webkit/gio-modules SSL_CERT_FILE=/etc/ssl/cert.pem
export DISPLAY=:0 WEBKIT_DISABLE_COMPOSITING_MODE=1 WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 HOME=/tmp GSETTINGS_BACKEND=memory DYLD_INSERT_LIBRARIES="${DYLD_INSERT_LIBRARIES:+$DYLD_INSERT_LIBRARIES:}/opt/webkit/lib/libcrashbt.dylib"
cd /opt/webkit/bin
exec ./MiniBrowser "${1:-https://example.com}"
