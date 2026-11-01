#!/bin/bash
# Baut die ergänzenden Swift-Laufzeitmodule (Synchronization, Observation, _StringProcessing mit _RegexParser und RegexBuilder) für arm64
# und legt die Dylibs zu libswiftCore in <Arbeitsordner>/run-arm64 (docs/logic-launch.md, Runde 18/19). Voraussetzung: swift/build.sh war einmal mit
# demselben Arbeitsordner gelaufen (Quellklon "swift" mit dem Isa-Masken-Patch). Rechenzeit: Core neu ~15 Minuten, jedes Modul wenige Minuten.
#
#   swift/supplemental.sh [Arbeitsordner]    Standard: $CLOKEOS_WORK/swiftsrc
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
w="${1:-${CLOKEOS_WORK:-$HOME/ClokeOS-work}/swiftsrc}"
cd "$w"
sdk="$(xcrun --show-sdk-path)"; dep="${SWIFT_DEPLOY:-11.0}"; a=arm64
# die Ergänzungen verlangen SwiftOverlay (auf Darwin gibt es das offene Overlay-Paket nicht): optional machen
for m in Observation Synchronization StringProcessing; do
  sed -i '' 's/find_package(SwiftOverlay REQUIRED)/find_package(SwiftOverlay)/' "swift/Runtimes/Supplemental/$m/CMakeLists.txt"
done
common=(-G Ninja -DCMAKE_POLICY_DEFAULT_CMP0195=NEW -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_OSX_ARCHITECTURES=$a
  -DCMAKE_OSX_DEPLOYMENT_TARGET=$dep -DCMAKE_Swift_COMPILER_TARGET=$a-apple-macos$dep -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  -DCMAKE_Swift_COMPILER="$(xcrun -f swiftc)" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DBUILD_SHARED_LIBS=ON)
if [ ! -d "b-$a/cmake/SwiftCore" ]; then
  cmake -S swift/Runtimes/Core -B "b-$a" "${common[@]}" -DSwiftCore_ENABLE_BACKTRACING=OFF -DSwiftCore_ENABLE_CONCURRENCY=ON \
    -DCMAKE_CXX_FLAGS=-DCLOKEOS_OLD_OBJC_ISA=1 > /dev/null
  ninja -C "b-$a" swiftCore swift_Concurrency > /dev/null
fi
core="$PWD/b-$a/cmake/SwiftCore"
mkdir -p "run-$a"
for m in Synchronization Observation StringProcessing; do
  # Observation übersetzt C++ gegen die Laufzeit-Header und braucht das beim Core-Bau erzeugte CMakeConfig.h
  cmake -S "swift/Runtimes/Supplemental/$m" -B "b-$m-$a" "${common[@]}" -DSwiftCore_DIR="$core" -DCMAKE_CXX_FLAGS="-I$PWD/b-$a/include" > "b-$m-$a.cmake.log" 2>&1 || { tail -5 "b-$m-$a.cmake.log"; exit 1; }
  ninja -C "b-$m-$a" > "b-$m-$a.ninja.log" 2>&1 || { tail -15 "b-$m-$a.ninja.log"; exit 1; }
  find "b-$m-$a" -name 'libswift*.dylib' -exec cp {} "run-$a/" \;
done
ls -la "run-$a"/libswift*.dylib
