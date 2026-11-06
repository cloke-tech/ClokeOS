#!/bin/bash
# Baut <Swift-Ordner>/run-arm64/combine.dylib: ein Combine-Ersatz aus OpenCombine (MIT, github.com/OpenCombine/OpenCombine, rund 3,5 MB), übersetzt als Modul
# "Combine" mit Library Evolution, damit Logic Pros Aufrufe (Published, ObservableObject, Subjects, AnyCancellable …) auf die gleichen Symbole treffen. Apples Combine ist
# nicht offen. Die C++-Hilfsdatei von OpenCombine (libc++) ist durch swift/overlay/combine-helpers.c ersetzt. Installname /usr/lib/sov/combine.dylib.
#
#   swift/combine.sh [Swift-Ordner (swiftsrc)]       Standard: $CLOKEOS_WORK/swiftsrc
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
sw="${1:-${CLOKEOS_WORK:-$HOME/ClokeOS-work}/swiftsrc}"
run="$sw/run-arm64"; w="$sw/combine"; sdk="$(xcrun --show-sdk-path)"
mkdir -p "$w"
[ -d "$w/OpenCombine" ] || git clone -q --depth 1 https://github.com/OpenCombine/OpenCombine.git "$w/OpenCombine"
rm -rf "$w/src" "$w/out"; mkdir -p "$w/out"; cp -R "$w/OpenCombine/Sources/OpenCombine" "$w/src"
find "$w/src" -name "*.gyb" -delete                      # die erzeugten GENERATED-*.swift liegen im Repository
grep -rl 'OpenCombine\.' "$w/src" | xargs sed -i '' 's/OpenCombine\./Combine./g'    # Modulname im Quelltext (Combine.Subscriber …)
grep -rl '@inlinable' "$w/src" | xargs sed -i '' 's/@inlinable//g'                   # mit Library Evolution nicht nötig, und einige Initialisierer dürfen es nicht
find "$w/src" -name "*.swift" > "$w/files.txt"
inc="$w/OpenCombine/Sources/COpenCombineHelpers/include"
swiftc -c -O -wmo -module-name Combine -swift-version 5 -enable-library-evolution -parse-as-library -target arm64-apple-macos11.0 -sdk "$sdk" \
  -emit-module -emit-module-path "$w/out/Combine.swiftmodule" -I "$inc" -Xcc "-fmodule-map-file=$inc/module.modulemap" @"$w/files.txt" -o "$w/out/combine.o" 2>&1 | grep -E " error|Stack dump" || true
clang -arch arm64 -mmacosx-version-min=11.0 -O2 -c "$here/overlay/combine-helpers.c" -o "$w/out/helpers.o"
cat > "$w/out/sys.tbd" <<'T'
--- !tapi-tbd
tbd-version: 4
targets: [ arm64-macos ]
install-name: '/usr/lib/libSystemX.B.dylib'
current-version: 1292
compatibility-version: 1
exports:
  - targets: [ arm64-macos ]
    symbols: [ dummy_sym ]
...
T
ld -arch arm64 -dylib -o "$run/combine.dylib" "$w/out/combine.o" "$w/out/helpers.o" "$run/libswiftCore.dylib" "$run/libswift_Concurrency.dylib" "$run/libswiftDarwin.dylib" "$w/out/sys.tbd" \
  -platform_version macos 11.0 11.0 -install_name /usr/lib/sov/combine.dylib -rpath /usr/lib/swift -not_for_dyld_shared_cache -undefined dynamic_lookup \
  -L"$sdk/usr/lib" -lobjc 2>&1 | grep -v warning || true
install_name_tool -change /usr/lib/libSystemX.B.dylib /usr/lib/libSystem.B.dylib "$run/combine.dylib"
echo "$run/combine.dylib"
