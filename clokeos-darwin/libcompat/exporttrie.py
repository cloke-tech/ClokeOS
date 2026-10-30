#!/usr/bin/env python3
"""Fügt der Export-Tabelle (Trie) eines Mach-O-Dylib weitere Symbole hinzu, die auf Adressen im
Image selbst zeigen. PureDarwins libSystem enthält die Objective-C-Klassen der Dispatch-Objekte
(OS_dispatch_queue, OS_dispatch_source, ...), exportiert sie aber nicht; Programme aus Apples SDK
binden sie als _OBJC_CLASS_$_OS_dispatch_queue.

  exporttrie.py <dylib> <nm-arch> NAME[=ZIEL] [NAME[=ZIEL] ...]
      ZIEL ist ein Symbol der lokalen Symboltabelle (Standard: NAME), dessen Adresse NAME bekommt.
      glob:MUSTER exportiert jedes noch nicht exportierte lokale Symbol, das auf MUSTER passt.
      Die Datei wird an Ort und Stelle geändert: der Trie wird neu geschrieben, alles dahinter im
      __LINKEDIT rückt nach hinten, die Code-Signatur fällt weg (danach codesign -s - aufrufen).
"""
import struct
import subprocess
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB, LC_CODE_SIGNATURE = 0x19, 0x2, 0xB, 0x1D
LINKEDIT_DATA = {0x1D, 0x1E, 0x26, 0x29, 0x2B, 0x80000033, 0x80000034, 0x8000001E, 0x80000035}
LC_DYLD_EXPORTS_TRIE = 0x80000033


def uleb_read(d, o):
    r, s = 0, 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7F) << s; s += 7
        if not b & 0x80:
            return r, o


def uleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def read_trie(d, start, size):
    """alle Symbole mit ihren Roh-Bytes der Terminal-Information"""
    out = {}
    def walk(node, prefix):
        p = start + node
        n, p = uleb_read(d, p)
        if n:
            out[prefix] = bytes(d[p:p + n])
        p += n
        cnt = d[p]
        p += 1
        for _ in range(cnt):
            e = d.index(b'\0', p)
            label = bytes(d[p:e])
            off, p = uleb_read(d, e + 1)
            walk(off, prefix + label)
    walk(0, b'')
    return out


def build_trie(entries):
    """entries: {name(bytes): terminal-Bytes}; Knoten als verschachtelte Wörterbücher"""
    class N:
        def __init__(self): self.kids = {}; self.term = None; self.off = 0
    root = N()
    for name in sorted(entries):
        n = root
        rest = name
        while True:
            for label in list(n.kids):
                c = 0
                while c < len(label) and c < len(rest) and label[c] == rest[c]: c += 1
                if c == 0: continue
                if c < len(label):   # Kante teilen
                    mid = N()
                    mid.kids[label[c:]] = n.kids.pop(label)
                    n.kids[label[:c]] = mid
                n = n.kids[label[:c]]
                rest = rest[c:]
                break
            else:
                if rest:
                    k = N(); n.kids[rest] = k; n = k
                    rest = b''
                break
            if not rest: break
        n.term = entries[name]
    nodes = []
    def collect(n):
        nodes.append(n)
        for k in sorted(n.kids): collect(n.kids[k])
    collect(root)
    def encode(n):
        out = bytearray()
        t = n.term or b''
        out += uleb(len(t)) + t if n.term is not None else uleb(0)
        out.append(len(n.kids))
        for k in sorted(n.kids):
            out += k + b'\0' + uleb(n.kids[k].off)
        return bytes(out)
    for _ in range(20):   # Offsets (ULEB) hängen von der Größe ab: bis zum Fixpunkt
        pos, changed = 0, False
        for n in nodes:
            if n.off != pos: n.off = pos; changed = True
            pos += len(encode(n))
        if not changed: break
    return b''.join(encode(n) for n in nodes)


def main():
    path, arch, specs = sys.argv[1], sys.argv[2], sys.argv[3:]
    d = bytearray(open(path, 'rb').read())
    magic, _, _, _, ncmds, sizeofcmds = struct.unpack_from('<6I', d, 0)
    if magic != MH_MAGIC_64:
        sys.exit('kein dünnes 64-Bit-Mach-O')
    syms = {}
    for line in subprocess.run(['nm', '-arch', arch, path], capture_output=True, text=True).stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in 'tTsSdDbB':
            syms[f[2]] = int(f[0], 16)
    cmds, off, trie = [], 32, None
    le = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        cmds.append((cmd, off, size))
        if cmd == LC_DYLD_EXPORTS_TRIE: trie = off
        if cmd == LC_SEGMENT_64 and bytes(d[off + 8:off + 24]).rstrip(b'\0') == b'__LINKEDIT': le = off
        off += size
    toff, tsize = struct.unpack_from('<II', d, trie + 8)
    entries = read_trie(d, toff, tsize)
    base = None
    for cmd, o, size in cmds:   # Image-Basis: vmaddr des __TEXT
        if cmd == LC_SEGMENT_64 and bytes(d[o + 8:o + 24]).rstrip(b'\0') == b'__TEXT':
            base = struct.unpack_from('<Q', d, o + 24)[0]
    added = 0
    import fnmatch
    expanded = []
    for spec in specs:
        if spec.startswith('glob:'):
            expanded += sorted(n for n in syms if fnmatch.fnmatchcase(n, spec[5:]))
        else:
            expanded.append(spec)
    for spec in expanded:
        name, _, target = spec.partition('=')
        target = target or name
        if name.encode() in entries:
            print('schon exportiert:', name); continue
        if target not in syms:
            print('Ziel fehlt:', target, file=sys.stderr); continue
        entries[name.encode()] = uleb(0) + uleb(syms[target] - base)
        added += 1
    if not added:
        return
    new = build_trie(entries)
    new += b'\0' * (-len(new) % 8)
    delta = len(new) - tsize
    delta += -delta % 8 if delta > 0 else 0
    new += b'\0' * (delta + tsize - len(new))
    end = toff + tsize
    d[toff:end] = new                 # alter Trie wird durch den neuen (größeren) ersetzt, der Rest rückt nach
    d.extend(b'')
    # Dateioffsets hinter dem alten Trie um delta verschieben
    def fix(o):
        v = struct.unpack_from('<I', d, o)[0]
        if v >= end: struct.pack_into('<I', d, o, v + delta)
    for cmd, o, size in cmds:
        if cmd == LC_SYMTAB:
            fix(o + 8); fix(o + 16)
        elif cmd == LC_DYSYMTAB:
            for k in (32, 40, 48, 56, 64, 72):   # toc, modtab, extrefsym, indirectsym, extrel, locrel
                if struct.unpack_from('<I', d, o + k)[0]: fix(o + k)
        elif cmd in LINKEDIT_DATA and cmd != LC_DYLD_EXPORTS_TRIE:
            fix(o + 8)
        elif cmd == 0x80000022 or cmd == 0x22:   # LC_DYLD_INFO(_ONLY)
            for k in (8, 16, 24, 32, 40):
                if struct.unpack_from('<I', d, o + k)[0]: fix(o + k)
    struct.pack_into('<I', d, trie + 12, tsize + delta)
    fo, fs = struct.unpack_from('<QQ', d, le + 40)
    struct.pack_into('<Q', d, le + 48, fs + delta)
    vs = struct.unpack_from('<Q', d, le + 32)[0]
    struct.pack_into('<Q', d, le + 32, (fs + delta + 0x3FFF) & ~0x3FFF if (fs + delta) > vs else vs)
    # Code-Signatur entfernen (letztes Ladekommando), codesign legt sie neu an
    for cmd, o, size in cmds:
        if cmd == LC_CODE_SIGNATURE:
            so, ss = struct.unpack_from('<II', d, o + 8)
            assert o + size == 32 + sizeofcmds, 'LC_CODE_SIGNATURE nicht zuletzt'
            d[o:o + size] = b'\0' * size
            struct.pack_into('<II', d, 16, ncmds - 1, sizeofcmds - size)
            del d[so:so + ss]
            struct.pack_into('<Q', d, le + 48, struct.unpack_from('<Q', d, le + 48)[0] - ss)
    open(path, 'wb').write(d)
    print('Export-Tabelle erweitert um', added, 'Symbole, +', delta, 'Bytes')


main()
