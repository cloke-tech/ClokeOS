#!/bin/bash
# Baut libclokeos_carbon (macapps/carbon.c) und eine leere Carbon.framework-Hülle, trägt die Bibliothek
# in PureDarwins CoreServices ein und legt alles in <stage>. Logic Pros OMF.framework lädt damit.
#
#   macapps/carbon.sh <stage>     Architektur wie überall mit CLOKEOS_ARCH
set -euo pipefail
source "$(dirname "$0")/../config.sh"
need "$DEBUGFS" clang codesign
stage="$1"
read -r root_start _ < <(gpt_part "$PD_IMG" "$GUID_LINUX_FS")
fs="$PD_IMG?offset=$((root_start * 512))"
out="$WORK/tmp/carbon-$CLOKE_ARCH"
rm -rf "$out"; mkdir -p "$out"
fw=System/Library/Frameworks
"$DEBUGFS" -R "dump /$fw/CoreServices.framework/Versions/A/CoreServices $out/CoreServices" "$fs" >/dev/null 2>&1
[ -s "$out/CoreServices" ] || { echo "CoreServices nicht aus dem Image geholt" >&2; exit 1; }
"$DEBUGFS" -R "dump /usr/lib/libSystem.B.dylib $out/libSystem.B.dylib" "$fs" >/dev/null 2>&1

target=x86_64-apple-macos11.0
[ "$CLOKE_ARCH" = arm64 ] && target=arm64-apple-macos11.0
flags=(-target "$target" -dynamiclib -nostdlib -O2 -Wl,-no_fixup_chains -Wl,-platform_version,macos,11.0,11.5)
# Was Mac-Programme aus CoreServices und Carbon importieren und weder PureDarwins CoreServices noch carbon.c haben (Importliste von
# macapps/stubs.sh, falls vorhanden): Marken, die 0 zurückgeben, damit dyld bindet (Logic Pro: Spotlight, Apple Events, HIToolbox …)
extra=()
if [ -f "$WORK/out/stubgen/imports.json" ]; then
  clang -target "$target" -O2 -c "$HERE/macapps/carbon.c" -o "$out/carbon.o" 2>/dev/null || true
  python3 -I - "$WORK/out/stubgen/imports.json" "$out/CoreServices" "$out/carbon.o" "$out/extra.s" "$CLOKE_ARCH" <<'PYX'
import json, subprocess, sys
imp, cs, obj, out, arch = sys.argv[1:6]
def names(f, *flags):
    r = subprocess.run(["nm", "-gU", "-arch", "arm64" if arch == "arm64" else "x86_64", f], capture_output=True, text=True)
    return {l.split()[-1] for l in r.stdout.splitlines() if l.strip()}
have = names(cs) | names(obj)
d = json.load(open(imp))
want = sorted({s for k in ("CoreServices", "Carbon") for s in d.get(k, {}).get("symbols", []) if not s.startswith("_$s")} - have)
body = "mov x0, #0\n  ret" if arch == "arm64" else "xor %eax, %eax\n  ret"
with open(out, "w") as f:
    f.write(".text\n.p2align 2\n")
    for s in want:
        if s.startswith("_k") and s[2:3].isupper():   # Konstanten (kTISProperty…, kMD…): 8 Nullbytes, kein Code, sonst liest das Programm Befehle als Zeiger
            f.write('.data\n.p2align 3\n.globl "%s"\n"%s":\n  .quad 0\n.text\n.p2align 2\n' % (s, s))
        else:
            f.write(f'.globl "{s}"\n"{s}":\n  {body}\n')
print(len(want), "Marken in libclokeos_carbon")
PYX
  [ -s "$out/extra.s" ] && extra=("$out/extra.s")
fi
clang "${flags[@]}" -install_name /usr/lib/libclokeos_carbon.dylib -o "$out/libclokeos_carbon.dylib" \
  "$HERE/macapps/carbon.c" ${extra[@]+"${extra[@]}"} "$out/libSystem.B.dylib"
# Carbon: leere Hülle mit Apples Versionsnummern (OMF verlangt Kompatibilität 2.0.0), die Carbon-Teile re-exportiert
echo 'static int carbon_stub;' > "$out/stub.c"
clang "${flags[@]}" -Wl,-headerpad,0x200 -compatibility_version 2.0.0 -current_version 170.0.0 \
  -install_name /System/Library/Frameworks/Carbon.framework/Versions/A/Carbon -o "$out/Carbon" "$out/stub.c" "$out/libSystem.B.dylib"
python3 -I "$HERE/libcompat/reexport.py" "$out/CoreServices" /usr/lib/libclokeos_carbon.dylib
python3 -I "$HERE/libcompat/reexport.py" "$out/Carbon" /System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices
codesign --force -s - "$out/libclokeos_carbon.dylib" "$out/CoreServices" "$out/Carbon" 2>/dev/null || true

mkdir -p "$stage/usr/lib" "$stage/$fw/CoreServices.framework/Versions/A" "$stage/$fw/Carbon.framework/Versions/A"
cp "$out/libclokeos_carbon.dylib" "$stage/usr/lib/"
cp "$out/CoreServices" "$stage/$fw/CoreServices.framework/Versions/A/CoreServices"
cp "$out/Carbon" "$stage/$fw/Carbon.framework/Versions/A/Carbon"
ln -sfn A "$stage/$fw/Carbon.framework/Versions/Current"
ln -sfn Versions/Current/Carbon "$stage/$fw/Carbon.framework/Carbon"
chmod 755 "$stage/usr/lib/libclokeos_carbon.dylib" "$stage/$fw"/*/Versions/A/*
echo "Fertig: libclokeos_carbon und Carbon für $CLOKE_ARCH"
