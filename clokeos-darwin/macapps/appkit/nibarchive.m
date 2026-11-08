/* NIBArchive-Leser für Cocotrons NSNib: Neuere Xcode-Versionen kompilieren Nibs in Apples Binärformat "NIBArchive" (alle 514 Nibs von
 * Logic Pro), das Cocotron nicht kennt. ClkNibArchiveToKeyed() setzt eine solche Datei in ein NSKeyedArchiver-Archiv (Binär-Plist) um,
 * das Darlings NSKeyedUnarchiver wie gewohnt liest; NSNib.m ruft es vor dem Entpacken auf (patches/darling-cocotron/AppKit/nib.subproj).
 * Andere Daten gibt die Funktion unverändert zurück.
 *
 * Aufbau des Formats: macapps/nibarchive.py.  Umsetzung:
 *   Objekt i            -> $objects[i+1] ($objects[0] = "$null"), Objekt 0 (NSObject mit IB.objectdata) wird $top
 *   NSString/NSLocalizableString (NS.bytes, UTF-8) -> CFString,  NSMutableString -> {NS.string},  NSNumber -> CFNumber/CFBoolean,
 *   NSData/NSMutableData -> CFData,  NSArray/NSSet/... (Folge von UINibEncoderEmptyKey) -> {NS.objects},
 *   NSDictionary (Schlüssel und Werte abwechselnd) -> {NS.keys, NS.objects},  NSDecimalNumberPlaceholder -> NSDecimalNumber
 *   Werte: Ganzzahlen/Gleitkomma -> CFNumber, wahr/falsch -> CFBoolean, Daten -> CFData, nil -> UID 0, Verweis -> UID.
 * Klassen, die es im Prozess nicht gibt (NSClassFromString == nil), ersetzt ein Ersatz nach dem Namen (…View -> NSView, …Cell -> NSCell,
 * …Button -> NSButton, …Control -> NSControl, …Connector -> NSNibConnector, sonst ClkNibUnknownObject); jede wird einmal auf stderr gemeldet: [clokeos-nib] ... */
#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef const struct __CFKeyedArchiverUID *CFKeyedArchiverUIDRef;
extern CFKeyedArchiverUIDRef _CFKeyedArchiverUIDCreate(CFAllocatorRef allocator, uint32_t value);

/* Platzhalter für Objekte unbekannter Klassen: nimmt alles an, damit der Rest der Nib geladen wird */
@interface ClkNibUnknownObject : NSObject <NSCoding>
@end
@implementation ClkNibUnknownObject
- (id)initWithCoder:(NSCoder *)c { return [super init]; }
- (void)encodeWithCoder:(NSCoder *)c {}
@end

typedef struct { uint32_t cls, first, count; } nobj;
typedef struct { uint32_t key; uint8_t type; int64_t i; double d; const uint8_t *p; uint32_t len; } nval;

static int rdvar(const uint8_t *d, size_t n, size_t *o, uint32_t *out) {
    uint32_t v = 0; int s = 0;
    while (*o < n && s < 35) {
        uint8_t b = d[(*o)++];
        v |= (uint32_t)(b & 0x7f) << s; s += 7;
        if (b & 0x80) { *out = v; return 1; }
    }
    return 0;
}
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static CFTypeRef uid(uint32_t v) { return (CFTypeRef)_CFKeyedArchiverUIDCreate(kCFAllocatorDefault, v); }

/* Ersatzklasse für Namen, die es hier nicht gibt (im Prozess nachgesehen) */
static NSString *substitute(NSString *name) {
    if (NSClassFromString(name)) return name;
    if ([name isEqualToString:@"_NSCornerView"] || [name isEqualToString:@"NSIBHelpConnector"]) return name;   /* setzt NSNib selbst um (setClass:forClassName:) */
    NSString *sub = @"ClkNibUnknownObject";
    static const char *suffix[][2] = { {"Connector", "NSNibConnector"}, {"Cell", "NSCell"}, {"Button", "NSButton"}, {"Control", "NSControl"}, {"View", "NSView"},
                                       {"Window", "NSWindow"}, {"Menu", "NSMenu"} };
    for (size_t i = 0; i < sizeof suffix / sizeof *suffix; i++)
        if ([name hasSuffix:[NSString stringWithUTF8String:suffix[i][0]]] && NSClassFromString([NSString stringWithUTF8String:suffix[i][1]])) {
            sub = [NSString stringWithUTF8String:suffix[i][1]]; break;
        }
    static NSMutableSet *seen;
    @synchronized([ClkNibUnknownObject class]) {
        if (!seen) seen = [NSMutableSet new];
        if (![seen containsObject:name]) {
            [seen addObject:name];
            if (getenv("CLOKE_NIB_LOG")) fprintf(stderr, "[clokeos-nib] Klasse %s fehlt, ersetzt durch %s\n", [name UTF8String], [sub UTF8String]);
        }
    }
    return sub;
}

static CFStringRef strFromBytes(const uint8_t *p, uint32_t len) {
    CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault, p, len, kCFStringEncodingUTF8, false);
    if (!s) s = CFStringCreateWithBytes(kCFAllocatorDefault, p, len, kCFStringEncodingMacRoman, false);
    return s ? s : CFSTR("");
}

static CFTypeRef valueObject(const nval *v) {
    switch (v->type) {
    case 0: case 1: case 2: case 3: { long long x = v->i; return CFNumberCreate(kCFAllocatorDefault, kCFNumberLongLongType, &x); }
    case 4: return CFRetain(kCFBooleanFalse);   /* 4 = falsch, 5 = wahr (auf dem Mac geprüft: NSControlContinuous 4 -> NO, NSEnabled 5 -> YES) */
    case 5: return CFRetain(kCFBooleanTrue);
    case 6: case 7: { double x = v->d; return CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &x); }
    case 8: return CFDataCreate(kCFAllocatorDefault, v->p, v->len);
    case 9: return uid(0);
    case 10: return uid((uint32_t)v->i + 1);
    }
    return NULL;
}

NSData *ClkNibArchiveToKeyed(NSData *data) {
    const uint8_t *d = [data bytes];
    size_t n = [data length];
    if (n < 50 || memcmp(d, "NIBArchive", 10)) return data;
    uint32_t h[10];
    for (int i = 0; i < 10; i++) h[i] = rd32(d + 10 + 4 * i);
    uint32_t oc = h[2], oo = h[3], kc = h[4], ko = h[5], vc = h[6], vo = h[7], cc = h[8], co = h[9];
    if (oo > n || ko > n || vo > n || co > n || oc == 0) return data;

    nobj *objs = calloc(oc, sizeof *objs);
    nval *vals = calloc(vc ? vc : 1, sizeof *vals);
    CFStringRef *keys = calloc(kc ? kc : 1, sizeof *keys);
    NSString **cls = calloc(cc ? cc : 1, sizeof *cls);
    CFMutableArrayRef objects = NULL;
    NSData *result = nil;
    size_t o;
    BOOL ok = YES;

    o = oo;
    for (uint32_t i = 0; i < oc && ok; i++)
        ok = rdvar(d, n, &o, &objs[i].cls) && rdvar(d, n, &o, &objs[i].first) && rdvar(d, n, &o, &objs[i].count);
    o = ko;
    for (uint32_t i = 0; i < kc && ok; i++) {
        uint32_t l;
        ok = rdvar(d, n, &o, &l) && o + l <= n;
        if (ok) { keys[i] = strFromBytes(d + o, l); o += l; }
    }
    o = vo;
    for (uint32_t i = 0; i < vc && ok; i++) {
        nval *v = &vals[i];
        ok = rdvar(d, n, &o, &v->key) && o < n && v->key < kc;
        if (!ok) break;
        v->type = d[o++];
        static const int sz[] = { 1, 2, 4, 8, 0, 0, 4, 8, -1, 0, 4 };
        if (v->type > 10) { ok = NO; break; }
        int s = sz[v->type];
        if (s < 0) { ok = rdvar(d, n, &o, &v->len) && o + v->len <= n; v->p = d + o; o += v->len; continue; }
        if (o + s > n) { ok = NO; break; }
        switch (v->type) {
        case 0: v->i = (int8_t)d[o]; break;
        case 1: { int16_t x; memcpy(&x, d + o, 2); v->i = x; break; }
        case 2: { int32_t x; memcpy(&x, d + o, 4); v->i = x; break; }
        case 3: memcpy(&v->i, d + o, 8); break;
        case 6: { float x; memcpy(&x, d + o, 4); v->d = x; break; }
        case 7: memcpy(&v->d, d + o, 8); break;
        case 10: v->i = rd32(d + o); if (v->i >= oc) ok = NO; break;
        }
        o += s;
    }
    o = co;
    for (uint32_t i = 0; i < cc && ok; i++) {
        uint32_t l, e;
        ok = rdvar(d, n, &o, &l) && rdvar(d, n, &o, &e) && o + 4 * (size_t)e + l <= n && l > 0;
        if (!ok) break;
        o += 4 * (size_t)e;
        cls[i] = [[NSString alloc] initWithBytes:d + o length:l - 1 encoding:NSUTF8StringEncoding];
        if (!cls[i]) cls[i] = @"NSObject";
        o += l;
    }
    for (uint32_t i = 0; i < oc && ok; i++)
        ok = objs[i].cls < cc && (uint64_t)objs[i].first + objs[i].count <= vc;
    if (!ok) { fprintf(stderr, "[clokeos-nib] NIBArchive beschädigt\n"); goto out; }

    objects = CFArrayCreateMutable(kCFAllocatorDefault, oc + cc + 1, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(objects, CFSTR("$null"));
    /* Klassen-Einträge hinter den Objekten, je Name einmal */
    NSMutableDictionary *classIndex = [NSMutableDictionary dictionary];
    NSMutableArray *classEntries = [NSMutableArray array];
    CFTypeRef (^classUID)(NSString *) = ^CFTypeRef(NSString *name) {
        NSNumber *ix = classIndex[name];
        if (!ix) {
            ix = @(oc + 1 + [classEntries count]);
            classIndex[name] = ix;
            [classEntries addObject:@{ @"$classname" : name, @"$classes" : @[ name, @"NSObject" ] }];
        }
        return uid([ix unsignedIntValue]);
    };
    CFStringRef kEmpty = CFSTR("UINibEncoderEmptyKey"), kInl = CFSTR("NSInlinedValue");

    for (uint32_t i = 0; i < oc; i++) {
        const nobj *ob = &objs[i];
        NSString *name = cls[ob->cls];
        const nval *vv = vals + ob->first;
        CFTypeRef entry = NULL;
        /* ein einzelner Wert mit Schlüssel k */
        const nval *(^one)(CFStringRef) = ^const nval *(CFStringRef k) {
            for (uint32_t j = 0; j < ob->count; j++)
                if (CFEqual(keys[vv[j].key], k)) return &vv[j];
            return (const nval *)NULL;
        };
        if ([name isEqualToString:@"NSString"] || [name isEqualToString:@"NSLocalizableString"] || [name isEqualToString:@"NSMutableString"]) {
            const nval *b = one(CFSTR("NS.bytes"));
            CFStringRef s = (b && b->type == 8) ? strFromBytes(b->p, b->len) : CFRetain(CFSTR(""));
            if ([name isEqualToString:@"NSMutableString"]) {
                CFMutableDictionaryRef m = CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFDictionarySetValue(m, CFSTR("NS.string"), s);
                CFTypeRef cu = classUID(name);
                CFDictionarySetValue(m, CFSTR("$class"), cu);
                CFRelease(cu); CFRelease(s);
                entry = m;
            } else
                entry = s;
        } else if ([name isEqualToString:@"NSNumber"] && ob->count == 1 && vv[0].type != 8 && vv[0].type != 9 && vv[0].type != 10) {
            entry = valueObject(&vv[0]);
        } else if (([name isEqualToString:@"NSData"] || [name isEqualToString:@"NSMutableData"]) && one(CFSTR("NS.bytes"))) {
            const nval *b = one(CFSTR("NS.bytes"));
            entry = CFDataCreate(kCFAllocatorDefault, b->p, b->type == 8 ? b->len : 0);
        } else {
            CFMutableDictionaryRef m = CFDictionaryCreateMutable(kCFAllocatorDefault, ob->count + 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFMutableArrayRef items = NULL;
            for (uint32_t j = 0; j < ob->count; j++) {
                CFStringRef k = keys[vv[j].key];
                if (CFEqual(k, kEmpty)) {
                    if (!items) items = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
                    CFTypeRef x = valueObject(&vv[j]);
                    if (x) { CFArrayAppendValue(items, x); CFRelease(x); }
                    continue;
                }
                if (CFEqual(k, kInl) || CFDictionaryContainsKey(m, k)) continue;
                CFTypeRef x = valueObject(&vv[j]);
                if (x) { CFDictionarySetValue(m, k, x); CFRelease(x); }
            }
            BOOL isDict = [name hasSuffix:@"Dictionary"];
            if (isDict) {
                CFMutableArrayRef ks = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
                CFMutableArrayRef os = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
                CFIndex c = items ? CFArrayGetCount(items) : 0;
                for (CFIndex j = 0; j + 1 < c; j += 2) {
                    CFArrayAppendValue(ks, CFArrayGetValueAtIndex(items, j));
                    CFArrayAppendValue(os, CFArrayGetValueAtIndex(items, j + 1));
                }
                CFDictionarySetValue(m, CFSTR("NS.keys"), ks);
                CFDictionarySetValue(m, CFSTR("NS.objects"), os);
                CFRelease(ks); CFRelease(os);
            } else if (items || [name hasSuffix:@"Array"] || [name hasSuffix:@"Set"]) {
                if (!items) items = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
                CFDictionarySetValue(m, CFSTR("NS.objects"), items);
            }
            if (items) CFRelease(items);
            NSString *cn = [name isEqualToString:@"NSDecimalNumberPlaceholder"] ? @"NSDecimalNumber" : name;
            if (i == 0 && CFDictionaryContainsKey(m, CFSTR("IB.objectdata"))) {
                entry = m; /* $top, wird unten auch als Objekt eingetragen */
            } else {
                CFTypeRef cu = classUID(substitute(cn));
                CFDictionarySetValue(m, CFSTR("$class"), cu);
                CFRelease(cu);
                entry = m;
            }
        }
        CFArrayAppendValue(objects, entry ? entry : CFSTR("$null"));
        if (entry) CFRelease(entry);
    }
    for (NSDictionary *e in classEntries) CFArrayAppendValue(objects, (CFTypeRef)e);

    {
        CFTypeRef top = CFArrayGetValueAtIndex(objects, 1);
        if (CFGetTypeID(top) != CFDictionaryGetTypeID()) { fprintf(stderr, "[clokeos-nib] NIBArchive ohne Wurzelobjekt\n"); goto out; }
        int ver = 100000;
        CFNumberRef verN = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ver);
        CFMutableDictionaryRef plist = CFDictionaryCreateMutable(kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(plist, CFSTR("$version"), verN);
        CFDictionarySetValue(plist, CFSTR("$archiver"), CFSTR("NSKeyedArchiver"));
        CFDictionarySetValue(plist, CFSTR("$top"), top);
        CFDictionarySetValue(plist, CFSTR("$objects"), objects);
        CFRelease(verN);
        CFErrorRef err = NULL;
        CFDataRef out = CFPropertyListCreateData(kCFAllocatorDefault, plist, kCFPropertyListBinaryFormat_v1_0, 0, &err);
        CFRelease(plist);
        if (!out) { fprintf(stderr, "[clokeos-nib] Plist-Ausgabe fehlgeschlagen\n"); if (err) CFRelease(err); goto out; }
        result = [(NSData *)out autorelease];
    }
out:
    if (objects) CFRelease(objects);
    for (uint32_t i = 0; i < kc; i++) if (keys[i]) CFRelease(keys[i]);
    for (uint32_t i = 0; i < cc; i++) [cls[i] release];
    free(objs); free(vals); free(keys); free(cls);
    return result ? result : data;
}
