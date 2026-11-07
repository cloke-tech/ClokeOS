#!/bin/bash
# SF Symbols für Mac-Programme in ClokeOS: rendert die Symbole einer Namensliste auf dem Mac (macapps/sfsymbols.m, Apples AppKit) und
# schreibt sie nach /Library/ClokeOS/Symbols ins Desktop-Image (VM aus). Nichts davon kommt ins Repository (Teil von macOS).
#   CLOKEOS_WORK=<Ordner> macapps/sfsymbols.sh <Namensliste>
# Namen findet CLOKE_TRACE ("[clokeos-trace] Symbol fehlt: <Name>") beim Lauf eines Programms.
set -euo pipefail
[ $# -ge 1 ] && [ -s "$1" ] || { echo "Aufruf: $0 <Namensliste>" >&2; exit 2; }
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp:?}"' EXIT
clang -fobjc-arc -framework AppKit macapps/sfsymbols.m -o "$tmp/sfsymbols"
"$tmp/sfsymbols" "$1" "$tmp/Symbols"
macapps/deploy-tree.sh "$tmp/Symbols" /Library/ClokeOS/Symbols
