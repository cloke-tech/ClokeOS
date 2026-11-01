#!/bin/bash
# Baut ein Desktop-Image (arm64, mit CLOKEOS_ARCH=x86_64 für x86_64) und legt den Code von Logic Pro hinein (für Startversuche, docs/logic-launch.md).
#
#   CLOKEOS_WORK=<eigener Ordner> macapps/logicimage.sh ["/Applications/Logic Pro.app"] [MiB mehr]
#
# 1. brand.sh (CLOKE_EXTRA_STAGE=$WORK/extra wird mitkopiert, z. B. usr/lib/system/libdispatch.dylib)
# 2. das Wurzeldateisystem wird als eigene Datei vergrößert (resize2fs auf image?offset=… kürzt sonst die Image-Datei am
#    falschen Ende und schneidet das neue Dateisystem ab), die GPT samt Sicherungskopie angepasst, zurückgeschrieben
# 3. Contents/MacOS, Contents/Frameworks und Info.plist des Programms werden mit debugfs ins Image geschrieben
#    und Contents/Resources (1,7 GB: Nibs, Bilder, Inhalte; ohne die ganze Installation von clokeos-pkginstall)
# Nichts von Logic landet im Repository. Ergebnis: $WORK/out/clokeos-<version>-<arch>-desktop.img
set -euo pipefail
export CLOKEOS_ARCH="${CLOKEOS_ARCH:-arm64}" CLOKEOS_FLAVOR=desktop
source "$(dirname "$0")/../config.sh"
app="${1:-/Applications/Logic Pro.app}"
add="${2:-4200}"
R="$BREW/opt/e2fsprogs/sbin"
tmp="$WORK/tmp/logicimage"
rm -rf "$tmp"; mkdir -p "$tmp"

CLOKE_LIBOBJC=1 "$HERE/brand.sh" > "$tmp/brand.log" 2>&1 || { tail -5 "$tmp/brand.log" >&2; exit 1; }
I="$CLOKE_IMG"
read -r s c < <(gpt_part "$I" "$GUID_LINUX_FS")
mib=$((c * 512 / 1048576 + add))
F="$tmp/root.ext4"
dd if="$I" of="$F" bs=1m skip=$((s * 512 / 1048576)) count=$((c * 512 / 1048576)) 2>/dev/null
truncate -s ${mib}m "$F"
"$R/e2fsck" -fy "$F" > /dev/null 2>&1 || true
"$R/resize2fs" -f "$F" > /dev/null 2>&1

python3 -I - "$I" "$add" <<'PY'
import os, struct, sys, zlib
img, add = sys.argv[1], int(sys.argv[2]) * 2048          # MiB -> Sektoren
f = open(img, "r+b"); size = os.path.getsize(img)
f.seek(512); h = bytearray(f.read(92))
ent_lba = struct.unpack("<Q", h[72:80])[0]; n, esz = struct.unpack("<II", h[80:88])
f.seek(ent_lba * 512); ents = bytearray(f.read(n * esz))
best = None
for i in range(n):
    e = ents[i * esz:(i + 1) * esz]
    if e[:16] == b"\0" * 16: continue
    first = struct.unpack("<Q", e[32:40])[0]
    if best is None or first > best[1]: best = (i, first)
i = best[0]
total = size // 512 + add
f.truncate(total * 512)
last = total - 1 - 33
struct.pack_into("<Q", ents, i * esz + 40, last)
ecrc = zlib.crc32(bytes(ents)) & 0xffffffff
def mk(cur, other, entlba):
    x = bytearray(h)
    struct.pack_into("<Q", x, 24, cur); struct.pack_into("<Q", x, 32, other)
    struct.pack_into("<Q", x, 48, last); struct.pack_into("<Q", x, 72, entlba)
    struct.pack_into("<I", x, 88, ecrc); struct.pack_into("<I", x, 16, 0)
    struct.pack_into("<I", x, 16, zlib.crc32(bytes(x[:92])) & 0xffffffff)
    return x
f.seek(512); f.write(mk(1, total - 1, 2)); f.seek(1024); f.write(ents)
f.seek((total - 33) * 512); f.write(ents)
f.seek((total - 1) * 512); f.write(mk(total - 1, 1, total - 33))
PY
dd if="$F" of="$I" bs=1m seek=$((s * 512 / 1048576)) conv=notrunc 2>/dev/null
rm -f "$F"

python3 -I - "$app" "$tmp/logic.cmds" <<'PY'
import os, stat, sys
root, out = sys.argv[1], sys.argv[2]
dst = "/Applications/" + os.path.basename(root)
q = lambda s: '"%s"' % s
c, dirs = [], set()
def mk(d):
    if d in dirs: return
    p = os.path.dirname(d)
    if p and p != "/": mk(p)
    dirs.add(d); c.append("mkdir " + q(d))
mk(dst)
for sub in ("Contents/MacOS", "Contents/Frameworks", "Contents/Resources"):
    for dp, dn, fn in os.walk(os.path.join(root, sub)):
        rel = os.path.relpath(dp, root); mk(dst + "/" + rel)
        for n in dn + fn:
            sp = os.path.join(dp, n); d2 = dst + "/" + rel + "/" + n; st = os.lstat(sp)
            if stat.S_ISLNK(st.st_mode): c.append("symlink %s %s" % (q(d2), q(os.readlink(sp))))
            elif stat.S_ISREG(st.st_mode):
                c.append("write %s %s" % (q(sp), q(d2)))
                if st.st_mode & 0o111: c.append("sif %s mode 0100755" % q(d2))
c.append("write %s %s" % (q(root + "/Contents/Info.plist"), q(dst + "/Contents/Info.plist")))
# Info.plist verlangt NSMainNibFile = MainMenu im Hauptbundle, die Nib liegt aber in Logic.framework: Verweis für den Versuch
if os.path.isfile(root + "/Contents/Frameworks/Logic.framework/Versions/A/Resources/MainMenu.nib") and not os.path.exists(root + "/Contents/Resources/MainMenu.nib"):
    c.append("symlink %s %s" % (q(dst + "/Contents/Resources/MainMenu.nib"), q("../Frameworks/Logic.framework/Versions/A/Resources/MainMenu.nib")))
open(out, "w").write("\n".join(c) + "\n")
PY
read -r s c < <(gpt_part "$I" "$GUID_LINUX_FS")
"$DEBUGFS" -w -f "$tmp/logic.cmds" "$I?offset=$((s * 512))" > "$tmp/logic.log" 2>&1
echo "Image: $I"
