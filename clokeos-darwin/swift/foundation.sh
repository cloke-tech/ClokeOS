#!/bin/bash
# Baut apple/swift-foundation (FoundationEssentials) mit dem ABI-Namen "Foundation" für das Foundation-Swift-Overlay von Logic Pro
# (docs/logic-launch.md, Runde 18). Quellen: swift-foundation (swift-6.4.0-RELEASE) und per SwiftPM swift-collections 1.1.6, swift-foundation-icu
# und swift-syntax (release/6.4.x): zusammen rund 490 MB auf der Platte mit Git-Verlauf. Dauer: etwa 4 Minuten für FoundationEssentials.
#
#   swift/foundation.sh [Arbeitsordner]      Ergebnis: <Arbeitsordner>/swift-foundation/.build/release/FoundationEssentials.o (+ Collections-Objekte)
#
# DATA_LEGACY_ABI ist im Paket für Apple-Plattformen schon gesetzt (Apples ältere Data-Anordnung, die Logics übersetzter Code erwartet);
# -module-abi-name Foundation lässt die Symbole $s10Foundation… heißen statt $s20FoundationEssentials….
set -euo pipefail
w="${1:-${CLOKEOS_WORK:-$HOME/ClokeOS-work}/swift-foundation-build}"
mkdir -p "$w"; cd "$w"
[ -d swift-foundation ] || git clone -q --depth 1 --branch swift-6.4.0-RELEASE https://github.com/swiftlang/swift-foundation.git
cd swift-foundation
python3 -I - <<'PY'
p = "Package.swift"
t = open(p).read()
if "module-abi-name" not in t:
    abi = '.unsafeFlags(["-Xfrontend", "-module-abi-name", "-Xfrontend", "Foundation"])'
    a = '''            .define("DATA_LEGACY_ABI", .when(platforms: [.macOS, .iOS, .tvOS, .watchOS, .visionOS]))
          ] + availabilityMacros + featureSettings,
          linkerSettings'''
    assert a in t
    t = t.replace(a, a.replace("featureSettings,", "featureSettings + [" + abi + "],"), 1)
    open(p, "w").write(t)
PY
swift package resolve
swift build -c release --product FoundationEssentials
echo "$w/swift-foundation/.build/release/FoundationEssentials.o"
