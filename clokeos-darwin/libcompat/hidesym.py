#!/usr/bin/env python3
"""Macht Symbole in der Export-Tabelle (Trie) eines Mach-O-Dylib unauffindbar, ohne die Datei zu
verschieben: das letzte Zeichen der letzten Kante des Symbols wird durch '~' ersetzt. dyld findet
das Symbol unter dem alten Namen dann nicht mehr in dieser Datei, sondern in der nächsten Bibliothek
der Re-Export-Kette (libclokeos_compat). Bei Re-Exporten (libSystem.B -> libdyld) gilt dasselbe.

  hidesym.py <dylib> <symbol> [<symbol> ...]      ändert die Datei an Ort und Stelle (alle Slices bei fat)
"""
import struct
import sys

LC_DYLD_INFO, LC_DYLD_INFO_ONLY, LC_DYLD_EXPORTS_TRIE = 0x22, 0x80000022, 0x80000033
MH_MAGIC_64 = 0xFEEDFACF


def uleb(d, o):
    r, s = 0, 0
    while True:
        b = d[o]; o += 1
        r |= (b & 0x7F) << s; s += 7
        if not b & 0x80:
            return r, o


def trie_region(d, base):
    magic, _, _, _, ncmds = struct.unpack_from('<5I', d, base)
    if magic != MH_MAGIC_64:
        sys.exit('kein 64-Bit-Mach-O')
    off = base + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == LC_DYLD_EXPORTS_TRIE:
            o, s = struct.unpack_from('<II', d, off + 8)
            return base + o, s
        if cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
            o, s = struct.unpack_from('<II', d, off + 40)
            return base + o, s
        off += size
    sys.exit('keine Export-Tabelle')


def children(d, start, node):
    p = start + node
    n, p = uleb(d, p)
    p += n
    cnt = d[p]; p += 1
    out = []
    for _ in range(cnt):
        e = d.index(b'\0', p)
        label = bytes(d[p:e])
        off, p = uleb(d, e + 1)
        out.append((label, off, p and e - len(label)))
    return out


def hide(d, start, sym):
    node, rest, last = 0, sym.encode(), None
    while rest:
        for label, off, lpos in children(d, start, node):
            if rest.startswith(label):
                node, rest, last = off, rest[len(label):], (lpos, len(label))
                break
        else:
            return False
    if last is None:
        return False
    lpos, ln = last
    if ln == 1:   # Ein-Zeichen-Kante: Nachbarn dürfen nicht mit '~' beginnen (nie der Fall bei Symbolen)
        return False
    d[lpos + ln - 1] = ord('~')
    return True


def main():
    path, syms = sys.argv[1], sys.argv[2:]
    d = bytearray(open(path, 'rb').read())
    bases = [0]
    if d[:4] in (b'\xca\xfe\xba\xbe',):
        n = struct.unpack_from('>I', d, 4)[0]
        bases = [struct.unpack_from('>I', d, 16 + 20 * i + 8)[0] for i in range(n)]
    for b in bases:
        start, size = trie_region(d, b)
        for s in syms:
            print(('ausgeblendet' if hide(d, start, s) else 'nicht gefunden') + ':', s)
    open(path, 'wb').write(d)


main()
