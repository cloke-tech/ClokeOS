#!/bin/sh
# Installiert WebKit (Search auf WebKitGTK) in einer laufenden ClokeOS-arm64-VM, als root:
#   sh install.sh <URL-des-Pakets.tgz>      Paket = Ausgabe von stage.sh (opt/webkit/{bin,lib}) plus gio-modules/ und runmb.sh
# Danach startet Search (bundles.py) clokesearch statt NetSurf. Rückgängig: rm -rf /opt/webkit
set -e
url="$1"
mkdir -p /opt /usr/local/libexec/webkit2gtk-4.1
curl -fsSL "$url" | tar xzf - -C /
for p in WebKitNetworkProcess WebKitWebProcess; do ln -sf /opt/webkit/bin/$p /usr/local/libexec/webkit2gtk-4.1/$p; done
# AF_UNIX-Datagramme bis 4096 Byte (WebKit-IPC nutzt auf Darwin SOCK_DGRAM)
sysctl -w net.local.dgram.maxdgram=16384 net.local.dgram.recvspace=262144 >/dev/null
grep -q maxdgram /etc/sysctl.conf 2>/dev/null || printf 'net.local.dgram.maxdgram=16384\nnet.local.dgram.recvspace=262144\n' >> /etc/sysctl.conf
echo "WebKit installiert: /opt/webkit (Search neu starten)"
