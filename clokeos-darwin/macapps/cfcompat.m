/* Ergänzungen für Darlings CoreFoundation auf ClokeOS: Literal-Objekte, die Apples clang erzeugt.
 *
 * Apples clang übersetzt Objective-C-Literale ab macOS 11 als Ziel in feste Objekte im Programm:
 *   @[]          -> ___NSArray0__struct          (Klasse __NSArray0)
 *   @{}          -> ___NSDictionary0__struct     (Klasse __NSDictionary0)
 *   @[a, b]      -> { NSConstantArray, count, objects }
 *   @{k: v, …}   -> { NSConstantDictionary, options, count, keys, objects }
 * Apple legt diese Klassen in CoreFoundation, Darling hat sie nicht. Ohne sie lädt kein Programm,
 * das mit aktuellem Xcode für macOS 11+ gebaut ist und solche Literale benutzt. Die Layouts sind
 * aus clangs Ausgabe abgelesen (Abschnitte __objc_arrayobj, __objc_dictobj). Die Zahlen-Literale
 * (NSConstantIntegerNumber usw.) gehören zu Foundation, siehe fndcompat.m. */
#import <Foundation/NSArray.h>
#import <Foundation/NSDictionary.h>
#import <Foundation/NSEnumerator.h>
#import <Foundation/NSException.h>

/* Objekte ohne Referenzzähler: sie liegen im Programm und leben für immer. */
#define CLOKE_IMMORTAL                                        \
  -(id)retain { return self; }                                \
  -(oneway void)release {}                                    \
  -(id)autorelease { return self; }                           \
  -(NSUInteger)retainCount { return NSUIntegerMax; }          \
  -(id)copyWithZone:(NSZone *)zone { return self; }           \
  -(void)dealloc { if (0) [super dealloc]; }

@interface __NSArray0 : NSArray
@end
@interface __NSDictionary0 : NSDictionary
@end
@interface NSConstantArray : NSArray {
  NSUInteger _count;
  const id *_objects;
}
@end
@interface NSConstantDictionary : NSDictionary {
  NSUInteger _options;
  NSUInteger _count;
  const id *_keys;
  const id *_objects;
}
@end

extern char cloke_NSArray0_class __asm("_OBJC_CLASS_$___NSArray0");
extern char cloke_NSDictionary0_class __asm("_OBJC_CLASS_$___NSDictionary0");
__attribute__((used)) struct { void *isa; } __NSArray0__struct = { &cloke_NSArray0_class };
__attribute__((used)) struct { void *isa; } __NSDictionary0__struct = { &cloke_NSDictionary0_class };

static NSUInteger enumerate(NSFastEnumerationState *state, const id *items, NSUInteger count, void *owner) {
  if (state->state != 0) return 0;
  state->state = 1;
  state->itemsPtr = (__unsafe_unretained id *)(void *)items;
  state->mutationsPtr = (unsigned long *)owner; /* unveränderlich: zeigt auf etwas Festes */
  return count;
}

@implementation __NSArray0
CLOKE_IMMORTAL
- (NSUInteger)count { return 0; }
- (id)objectAtIndex:(NSUInteger)index {
  [NSException raise:NSRangeException format:@"*** -[__NSArray0 objectAtIndex:]: index %lu beyond bounds for empty array",
                                              (unsigned long)index];
  return nil;
}
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state objects:(id *)buffer count:(NSUInteger)len {
  return 0;
}
@end

@implementation __NSDictionary0
CLOKE_IMMORTAL
- (NSUInteger)count { return 0; }
- (id)objectForKey:(id)key { return nil; }
- (NSEnumerator *)keyEnumerator { return [(NSArray *)(void *)&__NSArray0__struct objectEnumerator]; }
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state objects:(id *)buffer count:(NSUInteger)len {
  return 0;
}
@end

@implementation NSConstantArray
CLOKE_IMMORTAL
- (NSUInteger)count { return _count; }
- (id)objectAtIndex:(NSUInteger)index {
  if (index >= _count)
    [NSException raise:NSRangeException format:@"*** -[NSConstantArray objectAtIndex:]: index %lu beyond bounds [0 .. %lu]",
                                                (unsigned long)index, (unsigned long)_count - 1];
  return _objects[index];
}
- (void)getObjects:(id *)objects range:(NSRange)range {
  for (NSUInteger i = 0; i < range.length; i++) objects[i] = [self objectAtIndex:range.location + i];
}
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state objects:(id *)buffer count:(NSUInteger)len {
  return enumerate(state, _objects, _count, &_count);
}
@end

@implementation NSConstantDictionary
CLOKE_IMMORTAL
- (NSUInteger)count { return _count; }
- (id)objectForKey:(id)key {
  if (!key) return nil;
  for (NSUInteger i = 0; i < _count; i++)
    if (_keys[i] == key || [_keys[i] isEqual:key]) return _objects[i];
  return nil;
}
- (NSEnumerator *)keyEnumerator {
  return [[NSArray arrayWithObjects:(id *)_keys count:_count] objectEnumerator];
}
- (NSEnumerator *)objectEnumerator {
  return [[NSArray arrayWithObjects:(id *)_objects count:_count] objectEnumerator];
}
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state objects:(id *)buffer count:(NSUInteger)len {
  return enumerate(state, _keys, _count, &_count);
}
@end

/* Lesezeichen (Bookmarks) im Format von Apple: "book", Länge, Version, Kopflänge; danach Datenbereich mit Einträgen (Länge, Typ, Daten) und
 * Inhaltsverzeichnis (TOC). Für den Pfad zählt Schlüssel 0x1004: ein Feld (Typ 0x0601) mit Verweisen auf die Pfadteile (Typ 0x0101, UTF-8).
 * Gelesen werden auch Lesezeichen vom Mac (z. B. Logic Pro Library.bookmark); geschrieben wird nur der Pfad. Stale gibt es nicht. */
#include <CoreFoundation/CFError.h>
#include <CoreFoundation/CFURL.h>
#include <CoreFoundation/CFString.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static uint32_t clk_rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void clk_wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void clk_bm_err(CFErrorRef *e, CFIndex code) { if (e) *e = CFErrorCreate(NULL, kCFErrorDomainCocoa, code, NULL); }

static CFStringRef clk_bookmark_path(CFDataRef data) {
    if (data == NULL) return NULL;
    const uint8_t *b = CFDataGetBytePtr(data); CFIndex n = CFDataGetLength(data);
    if (n < 0x30 || memcmp(b, "book", 4) != 0) return NULL;
    uint32_t D = clk_rd32(b + 0xC);
    if ((CFIndex)D + 4 > n) return NULL;
    uint32_t toc = D + clk_rd32(b + D);
    for (int guard = 0; guard < 16 && (CFIndex)toc + 20 <= n; guard++) {
        uint32_t next = clk_rd32(b + toc + 12), cnt = clk_rd32(b + toc + 16);
        for (uint32_t i = 0; i < cnt && (CFIndex)(toc + 20 + 12 * i + 12) <= n; i++) {
            const uint8_t *e = b + toc + 20 + 12 * i;
            if (clk_rd32(e) != 0x1004) continue;
            uint32_t it = D + clk_rd32(e + 4);
            if ((CFIndex)it + 8 > n || (clk_rd32(b + it + 4) & 0xff00) != 0x0600) return NULL;
            uint32_t len = clk_rd32(b + it);
            CFMutableStringRef s = CFStringCreateMutable(NULL, 0);
            for (uint32_t k = 0; k < len / 4 && (CFIndex)(it + 8 + 4 * k + 4) <= n; k++) {
                uint32_t so = D + clk_rd32(b + it + 8 + 4 * k);
                if ((CFIndex)so + 8 > n) break;
                uint32_t sl = clk_rd32(b + so);
                if ((CFIndex)(so + 8 + sl) > n) break;
                CFStringRef c = CFStringCreateWithBytes(NULL, b + so + 8, sl, kCFStringEncodingUTF8, false);
                CFStringAppend(s, CFSTR("/"));
                if (c) { CFStringAppend(s, c); CFRelease(c); }
            }
            if (CFStringGetLength(s) == 0) CFStringAppend(s, CFSTR("/"));
            return s;
        }
        if (next == 0) break;
        toc = D + next;
    }
    return NULL;
}

static CFDataRef clk_bookmark_make(const char *path) {
    CFMutableDataRef d = CFDataCreateMutable(NULL, 0);
    uint8_t hdr[0x34] = { 'b', 'o', 'o', 'k' };
    clk_wr32(hdr + 8, 0x10040000); clk_wr32(hdr + 12, 0x30);
    CFDataAppendBytes(d, hdr, sizeof hdr);   /* 0x30 Kopf + 4 Bytes TOC-Versatz am Datenanfang */
    uint32_t offs[256]; int n = 0;
    for (const char *p = path; *p && n < 256; ) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *q = strchr(p, '/'); size_t l = q ? (size_t)(q - p) : strlen(p);
        offs[n++] = (uint32_t)(CFDataGetLength(d) - 0x30);
        uint8_t h[8]; clk_wr32(h, (uint32_t)l); clk_wr32(h + 4, 0x0101);
        CFDataAppendBytes(d, h, 8); CFDataAppendBytes(d, (const uint8_t *)p, (CFIndex)l);
        static const uint8_t z[4]; if (l & 3) CFDataAppendBytes(d, z, (CFIndex)(4 - (l & 3)));
        p += l;
    }
    uint32_t arr = (uint32_t)(CFDataGetLength(d) - 0x30);
    uint8_t h[8]; clk_wr32(h, 4 * n); clk_wr32(h + 4, 0x0601); CFDataAppendBytes(d, h, 8);
    for (int i = 0; i < n; i++) { uint8_t o[4]; clk_wr32(o, offs[i]); CFDataAppendBytes(d, o, 4); }
    uint32_t toc = (uint32_t)(CFDataGetLength(d) - 0x30);
    uint8_t t[32]; clk_wr32(t, 12 + 12); clk_wr32(t + 4, 0xfffffffe); clk_wr32(t + 8, 1); clk_wr32(t + 12, 0); clk_wr32(t + 16, 1);
    clk_wr32(t + 20, 0x1004); clk_wr32(t + 24, arr); clk_wr32(t + 28, 0);
    CFDataAppendBytes(d, t, sizeof t);
    uint8_t *b = CFDataGetMutableBytePtr(d);
    clk_wr32(b + 4, (uint32_t)CFDataGetLength(d)); clk_wr32(b + 0x30, toc);
    return d;
}

CFDataRef CFURLCreateBookmarkData(CFAllocatorRef a, CFURLRef url, CFURLBookmarkCreationOptions o, CFArrayRef keys, CFURLRef rel, CFErrorRef *err) {
    (void)a; (void)o; (void)keys; (void)rel;
    char path[4096];
    if (url == NULL || !CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path)) { clk_bm_err(err, 4); return NULL; }
    struct stat st;
    if (stat(path, &st) != 0) { clk_bm_err(err, 4); return NULL; }   /* wie bei Apple: nur für vorhandene Ziele */
    return clk_bookmark_make(path);
}
CFURLRef CFURLCreateByResolvingBookmarkData(CFAllocatorRef a, CFDataRef bm, CFURLBookmarkResolutionOptions o, CFURLRef rel, CFArrayRef keys, Boolean *stale, CFErrorRef *err) {
    (void)a; (void)o; (void)rel; (void)keys;
    if (stale) *stale = false;
    CFStringRef p = clk_bookmark_path(bm);
    if (p == NULL) { clk_bm_err(err, 259); return NULL; }   /* NSFileReadCorruptFileError */
    char cp[4096]; struct stat st;
    if (!CFStringGetFileSystemRepresentation(p, cp, sizeof cp) || stat(cp, &st) != 0) { CFRelease(p); clk_bm_err(err, 4); return NULL; }
    CFURLRef u = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle, S_ISDIR(st.st_mode));
    CFRelease(p);
    return u;
}
CFDataRef CFURLCreateBookmarkDataFromFile(CFAllocatorRef a, CFURLRef file, CFErrorRef *err) {
    (void)a; char path[4096];
    if (file == NULL || !CFURLGetFileSystemRepresentation(file, true, (UInt8 *)path, sizeof path)) { clk_bm_err(err, 4); return NULL; }
    int fd = open(path, O_RDONLY); if (fd < 0) { clk_bm_err(err, 260); return NULL; }   /* NSFileReadNoSuchFileError */
    CFMutableDataRef d = CFDataCreateMutable(NULL, 0); uint8_t buf[8192]; ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) CFDataAppendBytes(d, buf, r);
    close(fd);
    return d;
}
Boolean CFURLWriteBookmarkDataToFile(CFDataRef bm, CFURLRef file, CFURLBookmarkFileCreationOptions o, CFErrorRef *err) {
    (void)o; char path[4096];
    if (bm == NULL || file == NULL || !CFURLGetFileSystemRepresentation(file, true, (UInt8 *)path, sizeof path)) { clk_bm_err(err, 4); return false; }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) { clk_bm_err(err, 513); return false; }
    Boolean ok = write(fd, CFDataGetBytePtr(bm), CFDataGetLength(bm)) == CFDataGetLength(bm);
    close(fd);
    return ok;
}
Boolean CFURLResourceIsReachable(CFURLRef url, CFErrorRef *err) {
    char path[4096]; struct stat st;
    if (url && CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path) && stat(path, &st) == 0) return true;
    clk_bm_err(err, 260);
    return false;
}
