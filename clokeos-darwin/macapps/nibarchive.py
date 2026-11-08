#!/usr/bin/env python3
"""Liest Apples NIBArchive-Format (die kompilierten .nib-Dateien neuerer Xcode-Versionen, Dateikopf "NIBArchive"), das Cocotrons AppKit
nicht lädt, und gibt Klassen und Objekte lesbar aus.

    macapps/nibarchive.py MainMenu.nib [Anzahl Objekte]

Aufbau (aus den Dateien abgelesen, Little Endian; Variable Längen: 7 Bit je Byte, niederwertige zuerst, gesetztes Bit 7 = letztes Byte):
  "NIBArchive", dann 10 x uint32: Version 1, Version 10, Anzahl und Offset der Objekte, der Schlüssel, der Werte, der Klassen.
  Objekt: varint Klassenindex, varint erster Wert, varint Anzahl Werte.   Schlüssel: varint Länge + Bytes.
  Wert: varint Schlüsselindex, Byte Typ (0 int8, 1 int16, 2 int32, 3 int64, 4 falsch, 5 wahr, 6 float, 7 double, 8 Daten mit varint Länge,
        9 nil, 10 Objektverweis uint32), dann die Daten.   Klasse: varint Namenslänge (mit NUL), varint Zahl Zusatzwerte, uint32 je Zusatzwert, Name.
Ergebnis für Logic Pros MainMenu.nib (3359 Bytes, 95 Objekte): NSIBObjectData mit dem File's Owner (NSCustomObject, Klasse CLgApplication) und dem
Standardmenü (About/Hide/Quit); es enthält keinen Delegaten.  Ein Umsetzer nach NSKeyedArchiver-Plist (NS.objects für UINibEncoderEmptyKey-Folgen,
$classes aus der Klassentabelle) wäre der nächste Schritt, wenn eine Nib wirklich gebraucht wird. """
import struct,sys
def varint(d,o):
    v=0;s=0
    while True:
        b=d[o];o+=1
        v|=(b&0x7f)<<s;s+=7
        if b&0x80: return v,o
def parse(d):
    assert d[:10]==b'NIBArchive'
    v1,v2,oc,oo,kc,ko,vc,vo,cc,co=struct.unpack('<10I',d[10:50])
    objs=[];o=oo
    for _ in range(oc):
        c,o=varint(d,o);vi,o=varint(d,o);n,o=varint(d,o);objs.append((c,vi,n))
    keys=[];o=ko
    for _ in range(kc):
        n,o=varint(d,o);keys.append(d[o:o+n].decode());o+=n
    vals=[];o=vo
    for _ in range(vc):
        k,o=varint(d,o);t=d[o];o+=1
        if t==0: x=struct.unpack('<b',d[o:o+1])[0];o+=1
        elif t==1: x=struct.unpack('<h',d[o:o+2])[0];o+=2
        elif t==2: x=struct.unpack('<i',d[o:o+4])[0];o+=4
        elif t==3: x=struct.unpack('<q',d[o:o+8])[0];o+=8
        elif t==4: x=False
        elif t==5: x=True
        elif t==6: x=struct.unpack('<f',d[o:o+4])[0];o+=4
        elif t==7: x=struct.unpack('<d',d[o:o+8])[0];o+=8
        elif t==8:
            n,o=varint(d,o);x=bytes(d[o:o+n]);o+=n
        elif t==9: x=None
        elif t==10: x=('obj',struct.unpack('<I',d[o:o+4])[0]);o+=4
        else: raise Exception('type %d at %d'%(t,o))
        vals.append((k,t,x))
    cls=[];o=co
    for _ in range(cc):
        n,o=varint(d,o);e,o=varint(d,o)
        ex=struct.unpack('<%dI'%e,d[o:o+4*e]) if e else ();o+=4*e
        cls.append(d[o:o+n-1].decode());o+=n
    return objs,keys,vals,cls
if __name__=='__main__':
    objs,keys,vals,cls=parse(open(sys.argv[1],'rb').read())
    print('classes',cls)
    for i,(c,vi,n) in enumerate(objs[:int(sys.argv[2]) if len(sys.argv)>2 else 12]):
        print(i,cls[c],[(keys[k],t,x) for k,t,x in vals[vi:vi+n]])
