#!/bin/bash
# Baut das Darwin-Overlay (libswiftDarwin.dylib, Modul "Darwin") für arm64 aus den offenen Platform-Quellen der Swift-Quellen
# (stdlib/public/Platform: Platform.swift, POSIXError.swift, TiocConstants.swift). Apples eigenes Darwin-Overlay ist nicht offen, deshalb:
#   * Platform.swift ohne den semaphore.h-Teil (auf Darwin gibt es sem_t im Modul nicht) und mit einem leeren _UnsupportedPlatformError,
#   * POSIXErrorCode (die offenen Quellen haben ihn nur für Linux/BSD/Windows/WASI) wird aus <sys/errno.h> des SDK erzeugt,
#   * swift/darwin/extras.swift: MAP_FAILED, noErr, POSIXErrorCode.EWOULDBLOCK,
#   * ohne tgmath (die Mathematik-Wrapper übersetzen hier nicht; Logic importiert aus Darwin nur noErr und die Konstanten).
# Deckt alle $s6Darwin…-Symbole ab, die Logic Pro und FoundationEssentials (swift/foundation.sh) brauchen.
#
#   swift/darwin.sh [Arbeitsordner]     Standard: $CLOKEOS_WORK/swiftsrc; Ergebnis <Arbeitsordner>/run-arm64/libswiftDarwin.dylib
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
w="${1:-${CLOKEOS_WORK:-$HOME/ClokeOS-work}/swiftsrc}"
P="$w/swift/stdlib/public/Platform"
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
python3 -I - "$P/Platform.swift" "$t/Platform.swift" <<'PY'
import sys
t = open(sys.argv[1]).read()
a = t.index("// semaphore.h"); a = t.rfind("//===", 0, a)
b = t.index("\n#endif\n", t.index("public func sem_open", a)) + len("\n#endif\n")
t = t[:a] + t[b:]
t = t.replace("internal var _ignore = _UnsupportedPlatformError()", "")
open(sys.argv[2], "w").write("internal struct _UnsupportedPlatformError { init() {} }\n" + t)
PY
python3 -I - "$(xcrun --show-sdk-path)" "$t/errcodes.swift" <<'PY'
import re, sys
text = open(sys.argv[1] + "/usr/include/sys/errno.h").read()
cases, seen = [], {}
for m in re.finditer(r'^#define\s+(E[A-Z0-9_]+)\s+(\d+)\b', text, re.M):
    n, v = m.group(1), int(m.group(2))
    if n == "ELAST" or v in seen: continue
    seen[v] = n; cases.append((n, v))
out = ["// aus <sys/errno.h> erzeugt (swift/darwin.sh)", "public enum POSIXErrorCode: Int32, Sendable {"]
out += ["  case %s = %d" % c for c in cases] + ["}"]
open(sys.argv[2], "w").write("\n".join(out) + "\n")
PY
cp "$P/POSIXError.swift" "$P/TiocConstants.swift" "$here/darwin/extras.swift" "$t/"
clang -arch arm64 -mmacosx-version-min=11.0 -c "$here/overlay-stubs.c" -o "$t/stubs.o"
swiftc -emit-library -module-name Darwin -target arm64-apple-macos11.0 -parse-as-library -O \
  -Xfrontend -disable-implicit-concurrency-module-import -Xfrontend -disable-implicit-string-processing-module-import \
  -Xlinker -ignore_auto_link -Xlinker -not_for_dyld_shared_cache -Xlinker -install_name -Xlinker /usr/lib/swift/libswiftDarwin.dylib \
  -Xlinker -rpath -Xlinker /usr/lib/swift -o "$w/run-arm64/libswiftDarwin.dylib" \
  "$t/Platform.swift" "$t/POSIXError.swift" "$t/TiocConstants.swift" "$t/extras.swift" "$t/errcodes.swift" "$t/stubs.o" \
  "$w/run-arm64/libswiftCore.dylib" -lc++ -lobjc
echo "$w/run-arm64/libswiftDarwin.dylib"
