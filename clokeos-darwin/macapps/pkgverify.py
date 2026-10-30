#!/usr/bin/env python3
"""Vergleicht zwei Verzeichnisbäume Eintrag für Eintrag: Pfad, Art, Modus, Größe, Symlink-Ziel, SHA-256.

    macapps/pkgverify.py BAUM_A BAUM_B [--list DATEI]

Gibt "IDENTISCH" aus oder die ersten Unterschiede (Rückgabewert 1). Mit --list schreibt es die
Zeilen von BAUM_A (Pfad, Modus, Größe, SHA-256), z. B. als Hash-Liste zum späteren Vergleich im Gast.
Nichts aus den Bäumen wird verändert oder in das Repository kopiert.
"""
import concurrent.futures, hashlib, os, stat, sys


def walk(root):
    out = {}
    for d, dirs, files in os.walk(root):
        for n in dirs + files:
            p = os.path.join(d, n)
            out[os.path.relpath(p, root)] = p
    out["."] = root
    return out


def info(p):
    st = os.lstat(p)
    m = st.st_mode
    if stat.S_ISLNK(m):
        return "l", "", os.readlink(p), ""
    if stat.S_ISDIR(m):
        return "d", "%o" % (m & 0o7777), "", ""
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return "f", "%o" % (m & 0o7777), str(st.st_size), h.hexdigest()


def tree(root):
    ents = walk(root)
    keys = sorted(ents)
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        res = list(ex.map(lambda k: info(ents[k]), keys))
    return dict(zip(keys, res))


def main():
    a, b = sys.argv[1], sys.argv[2]
    ta, tb = tree(a), tree(b)
    if "--list" in sys.argv:
        with open(sys.argv[sys.argv.index("--list") + 1], "w") as f:
            for k in sorted(ta):
                f.write("\t".join((k,) + ta[k]) + "\n")
    diffs = []
    for k in sorted(set(ta) | set(tb)):
        if ta.get(k) != tb.get(k):
            diffs.append((k, ta.get(k), tb.get(k)))
    nfiles = sum(1 for v in ta.values() if v[0] == "f")
    if not diffs:
        print("IDENTISCH: %d Einträge (%d Dateien, %d Links, %d Verzeichnisse), SHA-256 gleich" % (
            len(ta), nfiles, sum(1 for v in ta.values() if v[0] == "l"), sum(1 for v in ta.values() if v[0] == "d")))
        return 0
    for k, x, y in diffs[:40]:
        print(k, x, y)
    print("UNTERSCHIEDE: %d" % len(diffs))
    return 1


sys.exit(main())
