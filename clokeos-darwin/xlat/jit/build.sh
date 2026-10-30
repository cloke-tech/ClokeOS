#!/bin/bash
# Baut die Werkzeuge des Block-JIT (Studie Abschnitt 16) für den Host-Mac (arm64):
#   jit/build.sh <Ausgabeordner> <Unicorn-Bauordner>      (z. B. ein cmake-Build von $WORK/download/unicorn/src mit -DUNICORN_ARCH=x86)
# Ergebnis: jtest (Differenztest gegen Unicorn), ucrun (Host-Build mit JIT), bench / t-sys / t-jit (x86_64, statisch).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?Ausgabeordner}"; ub="${2:?Unicorn-Bauordner}"; src="${UNICORN_SRC:-$HOME/ClokeOS-work/download/unicorn/src}"
mkdir -p "$out"; cd "$here"
libs=("$ub/libunicorn-static.a" "$ub/libx86_64-softmmu.a" "$ub/libunicorn-common.a")
clang -O1 -g -Wno-deprecated-declarations -I"$src/include" jit/jtest.c jit/jit.c "${libs[@]}" -lpthread -o "$out/jtest"
clang -O1 -g -Wno-deprecated-declarations -I"$src/include" ucrun.c jit/jit.c "${libs[@]}" -lpthread -o "$out/ucrun"
x="-target x86_64-apple-macos11.0 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static -Wl,-e,_start"
clang $x -O2 -fno-vectorize -fno-slp-vectorize -o "$out/bench" jit/bench.c
clang $x -O2 -o "$out/t-sys" t-sys.c
clang $x -O1 -o "$out/t-jit" jit/t-jit.c
echo "fertig: $out  (jtest [Anzahl] [Seed]; UCRUN_JIT=1 ucrun bench)"
