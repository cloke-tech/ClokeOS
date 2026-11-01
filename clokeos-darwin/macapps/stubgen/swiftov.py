#!/usr/bin/env python3
"""Fängt Swift-Symbole ab, die Mac-Programme aus Apple-Bibliotheken importieren und die es hier nicht gibt.

    macapps/stubgen/swiftov.py IMPORTS.json ROOT OUT --arch arm64|x86_64

IMPORTS.json: Ausgabe von collect.py (Bibliothek -> Symbole). ROOT: Ordner wie das Gastsystem
(…/Library/ClokeOS/MacFrameworks, …/usr/lib/swift). Für jede Bibliothek dort, aus der das Programm Swift-Symbole
(`_$s…`) importiert, die sie nicht selbst exportiert, wird OUT/usr/lib/sov/<Nr>.dylib (kurze Namen: im Mach-O-Kopf der Bibliotheken ist nur wenig Platz) gebaut: jedes fehlende Symbol ist
eine Marke mit einem Trap-Befehl. Die Bibliothek selbst bekommt einen LC_REEXPORT_DYLIB auf diese Datei (die Zwei-Ebenen-
Namensräume des Programms finden die Symbole dann "in" der Originalbibliothek). Das ist kein Ersatz für das Swift-Overlay,
sondern lässt dyld und die Programme weiterkommen, bis wirklich ein Swift-Symbol aufgerufen wird (dann Trap).
Die Datei OUT-swiftov-tmp/swiftov.txt zählt je Bibliothek die Symbole.
"""
import argparse, glob, json, os, struct, subprocess, sys
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stubgen   # emit() und build() für die C-/ObjC-Symbole

LC_REEXPORT_DYLIB = 0x8000001F
MH64 = 0xFEEDFACF


def run(*cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)


def exports(path, arch):
    r = run("nm", "-gU", "-arch", arch, path)
    return {l.split()[-1] for l in r.stdout.splitlines() if l.strip()}


def thin_offset(f):
    """Offset der Scheibe (fat -> arm64/x86_64 gesucht, hier nur dünne Dateien und fat mit passender Arch)."""
    return 0


def add_reexport(path, install, arch):
    """Hängt ein LC_REEXPORT_DYLIB an die Ladebefehle an, wenn zwischen den Befehlen und dem ersten Abschnitt Platz ist."""
    data = bytearray(open(path, "rb").read())
    base = 0
    magic = struct.unpack("<I", data[:4])[0]
    if magic in (0xBEBAFECA, 0xCAFEBABE):                       # fat (Big Endian)
        n = struct.unpack(">I", data[4:8])[0]
        cpu = {"arm64": 0x0100000C, "x86_64": 0x01000007}[arch]
        for i in range(n):
            c, _, off, size, _ = struct.unpack(">IIIII", data[8 + 20 * i:28 + 20 * i])
            if c == cpu:
                base = off
        magic = struct.unpack("<I", data[base:base + 4])[0]
    if magic != MH64:
        return "keine 64-Bit-Mach-O"
    ncmds, sizeofcmds = struct.unpack("<II", data[base + 16:base + 24])
    off, first = base + 32, 1 << 60
    for _ in range(ncmds):
        cmd, cs = struct.unpack("<II", data[off:off + 8])
        if cmd == 0x19:                                         # LC_SEGMENT_64
            nsects = struct.unpack("<I", data[off + 64:off + 68])[0]
            for s in range(nsects):
                so = off + 72 + s * 80
                o = struct.unpack("<I", data[so + 48:so + 52])[0]
                if o:
                    first = min(first, base + o)
        if cmd == LC_REEXPORT_DYLIB and install.encode() in data[off:off + cs]:
            return "schon vorhanden"
        off += cs
    p = install.encode() + b"\0"
    cs = (24 + len(p) + 7) & ~7
    end = base + 32 + sizeofcmds
    if end + cs > first:
        return f"zu wenig Platz ({first - end} Byte frei, {cs} gebraucht)"
    data[end:end + cs] = struct.pack("<IIIIII", LC_REEXPORT_DYLIB, cs, 24, 0, 0x10000, 0x10000) + p + b"\0" * (cs - 24 - len(p))
    struct.pack_into("<II", data, base + 16, ncmds + 1, sizeofcmds + cs)
    open(path, "wb").write(data)
    return "ok"


def plain_dylib(syms, dst, tmp, arch, idx):
    """Nur Assembler, keine Abhängigkeit außer libSystem: für PureDarwins eigene Bibliotheken (launchd lädt libIOKit; ein
    ObjC-/CoreFoundation-Bezug im Re-Export würde den Systemstart töten). Jede Marke gibt 0 zurück."""
    body = "mov x0, #0\n  ret" if arch == "arm64" else "xor %eax, %eax\n  ret"
    asm = os.path.join(tmp, f"{idx}.s")
    with open(asm, "w") as f:
        f.write(".text\n.p2align 2\n")
        for s in syms:
            f.write(f'.globl "{s}"\n"{s}":\n  {body}\n')
    obj = os.path.join(tmp, f"{idx}.o")
    tbd = os.path.join(tmp, "libSystemX.tbd")
    open(tbd, "w").write("--- !tapi-tbd\ntbd-version: 4\ntargets: [ x86_64-macos, arm64-macos ]\ninstall-name: '/usr/lib/libSystemX.B.dylib'\n"
                         "current-version: 1292\ncompatibility-version: 1\nexports:\n  - targets: [ x86_64-macos, arm64-macos ]\n    symbols: [ dummy_sym ]\n...\n")
    for cmd in (["clang", "-arch", arch, "-mmacosx-version-min=11.0", "-c", asm, "-o", obj],
                ["ld", "-arch", arch, "-dylib", "-o", dst, obj, "-install_name", f"/usr/lib/sov/{idx}.dylib",
                 "-platform_version", "macos", "11.0", "11.0", tbd],
                ["install_name_tool", "-change", "/usr/lib/libSystemX.B.dylib", "/usr/lib/libSystem.B.dylib", dst]):
        r = run(*cmd)
        if r.returncode:
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("imports"); ap.add_argument("root"); ap.add_argument("out")
    ap.add_argument("--arch", default="arm64", choices=["arm64", "x86_64"])
    ap.add_argument("--build", nargs=2, metavar=("PDLIBS", "CFOBJ"), required=True)
    ap.add_argument("--pd-image", nargs=2, metavar=("DEBUGFS", "IMAGE?offset=N"),
                    help="Bibliotheken des PureDarwin-Images (CoreServices, IOKit) herausholen und mit patchen")
    a = ap.parse_args()
    pd, cfobj = a.build
    imp = json.load(open(a.imports))
    if a.pd_image:
        for rel in ("usr/lib/libIOKitCF.dylib",):   # IOKit ist dort ein Symlink auf libIOKitCF (CoreServices/Carbon: macapps/carbon.sh)
            dst = os.path.join(a.root, rel)
            if not os.path.isfile(dst):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                run(a.pd_image[0], "-R", f"dump /{rel} {dst}", a.pd_image[1])
    sov = os.path.join(a.out, "usr/lib/sov")
    os.makedirs(sov, exist_ok=True)
    tmp = os.path.normpath(a.out) + "-swiftov-tmp"; os.makedirs(tmp, exist_ok=True)
    asmtrap = "brk #1" if a.arch == "arm64" else "ud2"
    ns = SimpleNamespace(arch=a.arch)
    hdr = stubgen.Headers(stubgen.sdk_path())
    report = []
    work = []
    for idx, (name, info) in enumerate(sorted(imp.items())):
        if name in ("CoreServices", "Carbon"):   # macapps/carbon.sh
            continue
        cand = glob.glob(os.path.join(a.root, "Library/ClokeOS/MacFrameworks", name + ".framework/Versions/*/" + name))
        cand += glob.glob(os.path.join(a.root, "usr/lib/swift", name + ".dylib"))
        cand += glob.glob(os.path.join(a.root, "System/Library/Frameworks", name + ".framework/Versions/*/" + name))
        if name == "IOKit":
            cand += glob.glob(os.path.join(a.root, "usr/lib/libIOKitCF.dylib"))
        cand = [c for c in cand if os.path.isfile(c) and not os.path.islink(c)]
        if not cand:
            continue
        lib = cand[0]
        have = exports(lib, a.arch)
        syms = [s for s in info["symbols"] if s not in have]
        if syms:
            work.append((idx, name, lib, syms))
    hdr.prefetch(sorted({s[1:] for _, _, _, syms in work for s in syms
                         if not s.startswith("_$") and s[1:2].isalpha() and s[1:].isidentifier() and not s.startswith("_OBJC")}))
    for idx, name, lib, syms in work:
        if name == "IOKit":
            dst = os.path.join(sov, f"{idx}.dylib")
            ok = plain_dylib(syms, dst, tmp, a.arch, idx)
            res = add_reexport(lib, f"/usr/lib/sov/{idx}.dylib", a.arch) if ok else "Bau fehlgeschlagen"
            report.append(f"{name}: {len(syms)} Symbole (nur Assembler, 0) -> sov/{idx}.dylib, Re-Export: {res}")
            continue
        swift = [s for s in syms if s.startswith("_$s") or s.startswith("_$S")]
        csyms = [s for s in syms if s not in swift]
        code, cnt, _, skipped = stubgen.emit(name, set(csyms), hdr, None) if csyms else ("", dict(func=0, str=0, data=0, cls=0, skip=0), [], [])
        if not csyms:
            code = stubgen.PRELUDE.format(fw=name)
        asm = ["__asm__(\".text\\n.p2align 2\\n\"\n"]
        for s in swift:
            asm.append(f'  ".globl {s}\\n{s}:\\n  {asmtrap}\\n"\n')
        asm.append(");\n")
        src = os.path.join(tmp, f"{idx}.c")
        open(src, "w").write(code + "\n" + "".join(asm))
        dst = stubgen.build(ns, name, dict(install=f"/usr/lib/sov/{idx}.dylib", compat="1.0.0", current="1.0.0"),
                            src, a.out, pd, cfobj, [])
        if not dst:
            report.append(f"{name}: Übersetzen fehlgeschlagen"); continue
        res = add_reexport(lib, f"/usr/lib/sov/{idx}.dylib", a.arch)
        report.append(f"{name}: {len(swift)} Swift (Trap) + {cnt['func']} Funktionen, {cnt['str']}+{cnt['data']} Konstanten, "
                      f"{cnt['cls']} Klassen -> sov/{idx}.dylib, Re-Export: {res}")
        if res not in ("ok", "schon vorhanden"):
            os.remove(dst)
    open(os.path.join(tmp, "swiftov.txt"), "w").write("\n".join(report) + "\n")
    print("\n".join(report))


main()
