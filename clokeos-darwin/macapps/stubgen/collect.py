#!/usr/bin/env python3
"""Sammelt aus einem Mac-Programm, welche Symbole es aus welcher Bibliothek importiert.

    macapps/stubgen/collect.py "/Applications/Logic Pro.app" AUS.json [--arch x86_64,arm64]

Liest jede Mach-O-Datei im Verzeichnis mit `dyld_info -imports` und `otool -L`. Ergebnis (JSON):
  { "Metal": { "install": "/System/Library/Frameworks/Metal.framework/Versions/A/Metal",
               "compat": "1.0.0", "current": "366.0.0", "symbols": ["_MTLCopyAllDevices", ...] }, ... }
Der Schlüssel ist der Kurzname, den dyld_info nach "from" ausgibt (Name der Bibliothek ohne Pfad).
Nichts wird in das Verzeichnis geschrieben.
"""
import argparse, collections, json, os, re, subprocess, sys

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


def vkey(v):
    return tuple(int(x) for x in v.split("."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("app")
    ap.add_argument("out")
    ap.add_argument("--arch", default="x86_64,arm64", help="Teile des Mach-O, deren Importe vereinigt werden")
    a = ap.parse_args()
    libs = collections.defaultdict(lambda: {"install": None, "compat": None, "current": None, "symbols": set()})
    for p, arch in ((p, arch) for p in macho_files(a.app) for arch in a.arch.split(",")):
        # Installationsnamen und Versionen aus otool -L (erste Zeile ist die Datei selbst)
        for m in re.finditer(r"^\s+(\S.*?) \(compatibility version ([\d.]+), current version ([\d.]+)\)", run("otool", "-arch", arch, "-L", p), re.M):
            path, comp, cur = m.groups()
            if not path.startswith(("/System/", "/usr/lib/")):
                continue
            n = re.sub(r"\.(framework|dylib)$", "", os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(path)))) if ".framework/" in path else os.path.basename(path))
            n = re.sub(r"\.[A-Z]$|\.\d.*$", "", n) if path.endswith(".dylib") else n
            if ".framework/" in path:
                n = path.split(".framework/")[0].rsplit("/", 1)[1]
            e = libs[n]
            e["install"] = e["install"] or path
            e["compat"] = min(filter(None, [e["compat"], comp]), key=vkey)
            e["current"] = max(filter(None, [e["current"], cur]), key=vkey)
        for line in run("dyld_info", "-arch", arch, "-imports", p).splitlines():
            m = re.match(r"\s+0x[0-9A-F]+\s+(\S+)(?:\s+\[[^\]]*\])?\s+\(from (.+?)\)", line)
            if m:
                libs[m.group(2)]["symbols"].add(m.group(1))
    out = {k: dict(v, symbols=sorted(v["symbols"])) for k, v in sorted(libs.items())}
    json.dump(out, open(a.out, "w"), indent=1)
    for k, v in out.items():
        print(f"{len(v['symbols']):6d} {k:32s} {v['install']}")


main()
