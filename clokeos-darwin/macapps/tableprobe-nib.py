#!/usr/bin/env python3
"""Schreibt tableprobe.nib für macapps/tableprobe.m: eine ansichtsbasierte Tabelle im Binärformat NIBArchive (Aufbau: nibarchive.py),
wie Xcode sie ablegt, ohne Xcode (ibtool fehlt).  Inhalt: oberste Ebene eine NSTableView (Steckdose "table" des Besitzers), Spalten
"check" (20 pt) und "name" (204 pt), Zeilenhöhe 20, Abstand 8 x 2; im Schlüssel NSTableViewArchivedReusableViewsKey zwei eingebettete
Nibs: "check" = Ankreuzfeld (Aktion toggle: an den Besitzer), "name" = NSTableCellView mit Beschriftung (Steckdose textField), die
über drei Bedingungen gehalten wird (vorn +2, hinten +2, senkrecht mittig).  Auf dem Mac lädt AppKit die Datei unverändert.

    macapps/tableprobe-nib.py tableprobe.nib
"""
import struct, sys


class Ref:
    def __init__(self, i): self.i = i


class Nib:
    def __init__(self):
        self.objs = []          # (Klasse, [(Schlüssel, Wert)])

    def add(self, cls, *kv):
        self.objs.append((cls, list(kv)))
        return Ref(len(self.objs) - 1)

    def set(self, ref, *kv):
        self.objs[ref.i][1].extend(kv)

    def s(self, text): return self.add('NSString', ('NS.bytes', text.encode()))
    def num(self, n): return self.add('NSNumber', ('NS.intval', n))
    def array(self, cls, items): return self.add(cls, ('NSInlinedValue', True), *[('UINibEncoderEmptyKey', x) for x in items])
    def dict(self, pairs):
        kv = []
        for k, v in pairs: kv += [('UINibEncoderEmptyKey', k), ('UINibEncoderEmptyKey', v)]
        return self.add('NSMutableDictionary', ('NSInlinedValue', True), *kv)

    def data(self):
        def varint(n):
            out = bytearray()
            while True:
                b = n & 0x7f; n >>= 7
                if n == 0: out.append(b | 0x80); return bytes(out)
                out.append(b)
        keys, classes, vals, objs = [], [], bytearray(), bytearray()
        nval = 0
        def kidx(k):
            if k not in keys: keys.append(k)
            return keys.index(k)
        for cls, kv in self.objs:
            if cls not in classes: classes.append(cls)
            objs += varint(classes.index(cls)) + varint(nval) + varint(len(kv))
            for k, v in kv:
                vals.extend(varint(kidx(k)))
                if v is True: vals.append(5)
                elif v is False: vals.append(4)
                elif v is None: vals.append(9)
                elif isinstance(v, Ref): vals.append(10); vals += struct.pack('<I', v.i)
                elif isinstance(v, float): vals.append(7); vals += struct.pack('<d', v)
                elif isinstance(v, bytes): vals.append(8); vals += varint(len(v)) + v
                elif isinstance(v, int):
                    if -128 <= v < 128: vals.append(0); vals += struct.pack('<b', v)
                    elif -32768 <= v < 32768: vals.append(1); vals += struct.pack('<h', v)
                    elif -2**31 <= v < 2**31: vals.append(2); vals += struct.pack('<i', v)
                    else: vals.append(3); vals += struct.pack('<q', v)
                else: raise TypeError(v)
                nval += 1
        kb = b''.join(varint(len(k.encode())) + k.encode() for k in keys)
        cb = b''.join(varint(len(c) + 1) + varint(0) + c.encode() + b'\0' for c in classes)
        off = 50
        o_objs = off; o_keys = o_objs + len(objs); o_vals = o_keys + len(kb); o_cls = o_vals + len(vals)
        head = b'NIBArchive' + struct.pack('<10I', 1, 10, len(self.objs), o_objs, len(keys), o_keys, nval, o_vals, len(classes), o_cls)
        return head + bytes(objs) + kb + bytes(vals) + cb


def font(n):
    return n.add('NSFont', ('NSName', n.s('.AppleSystemUIFont')), ('NSSize', 13.0), ('NSfFlags', 1044))


def catalog_color(n, name, white):
    g = n.add('NSColor', ('NSColorSpace', 3), ('NSWhite', ('%d' % white).encode() + b'\0'))
    return n.add('NSColor', ('NSColorSpace', 6), ('NSCatalogName', n.s('System')), ('NSColorName', n.s(name)), ('NSColor', g))


def begin(owner_class):
    """Wurzel (Objekt 0 mit IB.objectdata), NSIBObjectData und der Besitzer (File's Owner)"""
    n = Nib()
    root = n.add('NSObject')
    od = n.add('NSIBObjectData')
    n.set(root, ('IB.objectdata', od), ('IB.systemFontUpdateVersion', 1))
    return n, od, n.add('NSCustomObject', ('NSClassName', n.s(owner_class)))


def finish(n, od, owner, objs, parents, conns):
    """Objekttabelle wie Xcode: Objekte mit Eltern (Besitzer = oberste Ebene), Verbindungen, Kennungen 1…n"""
    app = n.add('NSCustomObject', ('NSClassName', n.s('NSApplication')))
    objs = objs + [app]
    parents = parents + [owner]
    oids = [owner] + objs + conns
    n.set(od, ('NSRoot', owner), ('NSVisibleWindows', n.array('NSMutableSet', [])), ('NSConnections', n.array('NSMutableArray', conns)),
          ('NSObjectsKeys', n.array('NSArray', objs)), ('NSObjectsValues', n.array('NSArray', parents)),
          ('NSOidsKeys', n.array('NSArray', oids)), ('NSOidsValues', n.array('NSArray', [n.num(i + 1) for i in range(len(oids))])),
          ('NSAccessibilityConnectors', n.array('NSMutableArray', [])), ('NSAccessibilityOidsKeys', n.array('NSArray', [])),
          ('NSAccessibilityOidsValues', n.array('NSArray', [])))
    return n.data()


def constraint(n, a, aa, b, ba, c=None):
    kv = [('NSFirstItem', a), ('NSFirstAttribute', aa), ('NSFirstAttributeV2', aa), ('NSSecondItem', b), ('NSSecondAttribute', ba),
          ('NSSecondAttributeV2', ba), ('NSShouldBeArchived', True)]
    if c is not None: kv += [('NSConstant', c), ('NSConstantV2', c)]
    return n.add('NSLayoutConstraint', *kv)


def name_nib():
    n, od, owner = begin('NSObject')
    cv = n.add('NSTableCellView', ('NSNextResponder', None), ('NSvFlags', 274), ('NSFrame', n.s('{{0, 0}, {150, 20}}')),
               ('NSReuseIdentifierKey', n.s('name')))
    tf = n.add('NSTextField', ('NSNextResponder', cv), ('NSvFlags', 268), ('NSFrame', n.s('{{0, 2}, {146, 16}}')), ('NSSuperview', cv),
               ('NSDoNotTranslateAutoresizingMask', True), ('NSHuggingPriority', n.s('{251, 750}')), ('NSEnabled', True),
               ('NSControlSize', 0))
    cell = n.add('NSTextFieldCell', ('NSCellFlags', 67108928), ('NSCellFlags2', 272631808), ('NSContents', n.s('Table View Cell')),
                 ('NSSupport', font(n)), ('NSControlView', tf), ('NSBackgroundColor', catalog_color(n, 'textBackgroundColor', 1)),
                 ('NSTextColor', catalog_color(n, 'controlTextColor', 0)))
    n.set(tf, ('NSCell', cell))
    cs = [constraint(n, tf, 10, cv, 10), constraint(n, cv, 6, tf, 6, 2.0), constraint(n, tf, 5, cv, 5, 2.0)]
    n.set(cv, ('NSSubviews', n.array('NSMutableArray', [tf])), ('NSViewConstraints', n.array('NSArray', cs)))
    conn = n.add('NSNibOutletConnector', ('NSSource', cv), ('NSDestination', tf), ('NSLabel', n.s('textField')))
    return finish(n, od, owner, [cv, tf, cell] + cs, [owner, cv, tf] + [cv] * len(cs), [conn])


def check_nib():
    n, od, owner = begin('NSObject')
    b = n.add('NSButton', ('NSNextResponder', None), ('NSvFlags', 268), ('NSFrame', n.s('{{0, 1}, {20, 18}}')),
              ('NSReuseIdentifierKey', n.s('check')), ('NSEnabled', True), ('NSControlSize', 0))
    empty = n.s('')
    cell = n.add('NSButtonCell', ('NSCellFlags', -2080374784), ('NSCellFlags2', 268435456), ('NSContents', empty), ('NSSupport', font(n)),
                 ('NSControlView', b), ('NSButtonFlags', 1211912448), ('NSButtonFlags2', 2), ('NSBezelStyle', 2),
                 ('NSAlternateImage', n.add('NSButtonImageSource', ('NSImageName', n.s('NSSwitch')))), ('NSAlternateContents', empty),
                 ('NSKeyEquivalent', empty), ('NSPeriodicDelay', 400), ('NSPeriodicInterval', 75), ('NSAuxButtonType', 3))
    n.set(b, ('NSCell', cell))
    conn = n.add('NSNibControlConnector', ('NSSource', b), ('NSDestination', owner), ('NSLabel', n.s('toggle:')))
    return finish(n, od, owner, [b, cell], [owner, b], [conn])


def table_nib():
    n, od, owner = begin('TableProbeOwner')
    t = n.add('NSTableView', ('NSNextResponder', None), ('NSvFlags', 274), ('NSFrameSize', n.s('{200, 100}')), ('NSEnabled', True),
              ('NSControlSize', 0))
    empty = n.s('')

    def column(ident, width, title):
        hc = n.add('NSTableHeaderCell', ('NSCellFlags', 75497536), ('NSCellFlags2', 2048), ('NSContents', n.s(title)))
        dc = n.add('NSTextFieldCell', ('NSCellFlags', 337641473), ('NSCellFlags2', 1073741888), ('NSContents', empty),
                   ('NSSupport', font(n)), ('NSControlView', t))
        return n.add('NSTableColumn', ('NSIdentifier', n.s(ident)), ('NSWidth', width), ('NSMinWidth', 20.0), ('NSMaxWidth', 1000.0),
                     ('NSHeaderCell', hc), ('NSDataCell', dc), ('NSIsEditable', True), ('NSTableView', t))
    c1 = column('check', 20.0, 'On')
    c2 = column('name', 204.0, 'Name')   # 20 + 8 + 204 + 8 = Breite der Clip-Ansicht der Probe

    def embedded(data):
        return n.add('NSNib', ('NSNibFileData', n.add('NSData', ('NS.bytes', data))), ('NSNibFileIsKeyed', True),
                     ('NSNibFileUseParentBundle', True), ('NSNibFileImages', None), ('NSNibFileSounds', None))
    reuse = n.dict([(n.s('check'), embedded(check_nib())), (n.s('name'), embedded(name_nib()))])
    n.set(t, ('NSTableColumns', n.array('NSMutableArray', [c1, c2])), ('NSIntercellSpacingWidth', 8.0), ('NSIntercellSpacingHeight', 2.0),
          ('NSBackgroundColor', catalog_color(n, 'controlBackgroundColor', 1)), ('NSGridColor', catalog_color(n, 'gridColor', 0)),
          ('NSRowHeight', 20.0), ('NSTvFlags', 48234496), ('NSDelegate', None), ('NSDataSource', None), ('NSColumnAutoresizingStyle', 0),
          ('NSAllowsTypeSelect', True), ('NSTableViewArchivedReusableViewsKey', reuse))
    conn = n.add('NSNibOutletConnector', ('NSSource', owner), ('NSDestination', t), ('NSLabel', n.s('table')))
    return finish(n, od, owner, [t, c1, c2], [owner, t, t], [conn])


if __name__ == '__main__':
    open(sys.argv[1] if len(sys.argv) > 1 else 'tableprobe.nib', 'wb').write(table_nib())
