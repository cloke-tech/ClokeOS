#!/bin/bash
# Vergleicht zwei Verzeichnisbäume: Pfade, Art, Modus, Größe, Symlink-Ziele und SHA-256 jeder Datei.
#   macapps/pkgverify.sh <baum-a> <baum-b>
# Ausgabe: leer + "IDENTISCH", sonst die ersten Unterschiede. Nichts davon landet im Repository.
set -euo pipefail
[ $# -eq 2 ] || { echo "Aufruf: pkgverify.sh <a> <b>" >&2; exit 2; }
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
list() {
  (cd "$1" && find . -print0 | sort -z | while IFS= read -r -d '' f; do
    if [ -L "$f" ]; then printf 'l %s -> %s\n' "$f" "$(readlink "$f")"
    elif [ -d "$f" ]; then printf 'd %s %s\n' "$(stat -f %Lp "$f")" "$f"
    else printf 'f %s %s %s %s\n' "$(stat -f '%Lp %z' "$f")" "$f" "$(shasum -a 256 < "$f" | cut -c1-64)" ""; fi
  done) > "$2"
}
list "$1" "$t/a" & list "$2" "$t/b" & wait
if cmp -s "$t/a" "$t/b"; then echo "IDENTISCH: $(wc -l < "$t/a") Einträge"; else
  diff "$t/a" "$t/b" | head -40; echo "UNTERSCHIEDE: $(diff "$t/a" "$t/b" | grep -c '^[<>]')"; exit 1; fi
