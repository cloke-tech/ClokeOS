#!/usr/bin/env python3
"""Welche libSystem-Symbole importiert ein Mac-Programm, die ClokeOS' libSystem nicht exportiert?

  libcompat/missing.py APP_VERZEICHNIS ARCH PROVIDER.dylib [PROVIDER.dylib ...]

Liest von jeder Mach-O-Datei im Verzeichnis die Importe aus libSystem (dyld_info -imports; Re-Exporte
von libSystem, etwa libdyld, gehören zu den PROVIDERn, die man selbst angibt) und zieht alles ab, was die
PROVIDER exportieren (dyld_info -exports). Ausgabe je Zeile: Symbol, Zahl der importierenden Dateien
und drei Beispiele. Schreibt nichts ins Verzeichnis.
"""
import collections
import os
import re
import subprocess
import sys

MAGIC = {b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace").stdout


def macho_files(root):
    for d, _, files in os.walk(root):
        for n in files:
            p = os.path.join(d, n)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            try:
                with open(p, "rb") as f:
                    if f.read(4) in MAGIC:
                        yield p
            except OSError:
                pass


def main():
    root, arch, providers = sys.argv[1], sys.argv[2], sys.argv[3:]
    have = set()
    for p in providers:
        for line in run("dyld_info", "-arch", arch, "-exports", p).splitlines():
            m = re.match(r"\s+0x[0-9A-F]+\s+(\S+)", line)
            if m:
                have.add(m.group(1))
    users = collections.defaultdict(set)
    for path in macho_files(root):
        if arch not in run("lipo", "-archs", path).split():
            continue
        for line in run("dyld_info", "-arch", arch, "-imports", path).splitlines():
            m = re.match(r"\s+0x[0-9A-F]+\s+(\S+)\s+\(from (libSystem[^)]*)\)", line)
            if m and m.group(1) not in have:
                users[m.group(1)].add(os.path.relpath(path, root))
    for sym in sorted(users):
        u = sorted(users[sym])
        print(f"{sym}\t{len(u)}\t{', '.join(os.path.basename(x) for x in u[:3])}")


main()
