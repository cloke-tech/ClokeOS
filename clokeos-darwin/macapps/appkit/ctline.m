/* CTLine und CTRun für ClokeOS' CoreText (ersetzt Cocotrons CTLine.m/CTRun.m, die nur Platzhalter mit NULL-Ergebnissen hatten;
 * Logic Pros MAToolKit legt Texte so aus und stürzte bei CFArrayGetCount(NULL) ab).
 *
 * Einfach, aber echt: eine Zeile hält ihre Zeichenkette und teilt sie an Attributgrenzen in Läufe. Glyphen und Vorschübe kommen aus der
 * Schrift des Laufs (NSFont; CTFontCreateWithName liefert hier NSFonts), ohne Ligaturen, Kerning, bidirektionalen Text oder
 * Schriftersatz. Positionen sind die Summe der Vorschübe auf der Grundlinie. CTLineDraw zeichnet die Zeichenkette mit AppKit an der
 * Textposition des Kontexts, wenn er der aktuelle NSGraphicsContext ist; sonst nichts. AppKit wird zur Laufzeit gefunden. */
#import <Foundation/Foundation.h>
#import <CoreText/CTLine.h>
#import <CoreText/CTRun.h>
#import <CoreText/CTFont.h>
#import <CoreText/CTStringAttributes.h>
#import <objc/runtime.h>
#import <objc/message.h>

@interface ClkCTRun : NSObject {
@public
    NSDictionary *attrs;
    CFRange range;
    CFIndex count;
    CGGlyph *glyphs;
    CGSize *advances;
    CGPoint *positions;
    CFIndex *indices;
    CGFloat ascent, descent, leading, width;
}
@end
@implementation ClkCTRun
- (void) dealloc {
    [attrs release];
    free(glyphs); free(advances); free(positions); free(indices);
    [super dealloc];
}
@end

@interface ClkCTLine : NSObject {
@public
    NSAttributedString *string;
    NSArray *runs;
    CGFloat ascent, descent, leading, width;
}
@end
@implementation ClkCTLine
- (void) dealloc {
    [string release];
    [runs release];
    [super dealloc];
}
@end

static id msg(id o, const char *s) { return o ? ((id (*)(id, SEL)) objc_msgSend)(o, sel_registerName(s)) : nil; }
static CGFloat fmsg(id o, const char *s) { return o ? ((CGFloat (*)(id, SEL)) objc_msgSend)(o, sel_registerName(s)) : 0; }

static id run_font(NSDictionary *a) {
    id f = a[@"NSFont"];   /* kCTFontAttributeName und NSFontAttributeName heißen beide "NSFont" */
    if (f == nil) {
        Class nsfont = objc_getClass("NSFont");
        f = nsfont ? ((id (*)(id, SEL, CGFloat)) objc_msgSend)(nsfont, sel_registerName("systemFontOfSize:"), 12.0) : nil;
    }
    return f;
}

static ClkCTRun *make_run(NSString *text, NSDictionary *a, NSRange r, CGFloat x) {
    ClkCTRun *run = [ClkCTRun new];
    run->attrs = [a copy];
    run->range = CFRangeMake(r.location, r.length);
    run->count = r.length;
    run->glyphs = calloc(r.length + 1, sizeof(CGGlyph));
    run->advances = calloc(r.length + 1, sizeof(CGSize));
    run->positions = calloc(r.length + 1, sizeof(CGPoint));
    run->indices = calloc(r.length + 1, sizeof(CFIndex));
    unichar *chars = calloc(r.length + 1, sizeof(unichar));
    [text getCharacters: chars range: r];
    id font = run_font(a);
    NSUInteger *g32 = calloc(r.length + 1, sizeof(NSUInteger));
    BOOL haveGlyphs = font && [font respondsToSelector: sel_registerName("getGlyphs:forCharacters:length:")];
    if (haveGlyphs)
        ((NSUInteger (*)(id, SEL, NSUInteger *, unichar *, NSUInteger)) objc_msgSend)(font, sel_registerName("getGlyphs:forCharacters:length:"), g32, chars, r.length);
    CGFloat size = fmsg(font, "pointSize");
    if (size <= 0) size = 12;
    for (NSUInteger i = 0; i < r.length; i++) {
        run->glyphs[i] = haveGlyphs ? (CGGlyph) g32[i] : (CGGlyph) chars[i];
        CGSize adv = CGSizeMake(size * 0.55, 0);
        if (font && [font respondsToSelector: sel_registerName("advancementForGlyph:")]) {
            NSSize s = ((NSSize (*)(id, SEL, NSUInteger)) objc_msgSend)(font, sel_registerName("advancementForGlyph:"), (NSUInteger) run->glyphs[i]);
            if (s.width > 0 || chars[i] == ' ') adv = CGSizeMake(s.width, s.height);
        }
        run->advances[i] = adv;
        run->positions[i] = CGPointMake(x + run->width, 0);
        run->indices[i] = r.location + i;
        run->width += adv.width;
    }
    free(chars); free(g32);
    run->ascent = fmsg(font, "ascender");
    run->descent = -fmsg(font, "descender");
    if (run->ascent <= 0) run->ascent = size * 0.8;
    if (run->descent < 0) run->descent = 0;
    run->leading = fmsg(font, "leading");
    return run;
}

static ClkCTLine *make_line(NSAttributedString *s) {
    ClkCTLine *line = [ClkCTLine new];
    line->string = [s copy];
    NSMutableArray *runs = [NSMutableArray array];
    NSString *text = [s string];
    NSUInteger pos = 0, n = [text length];
    while (pos < n) {
        NSRange eff;
        NSDictionary *a = [s attributesAtIndex: pos effectiveRange: &eff];
        if (eff.location + eff.length > n) eff.length = n - eff.location;
        NSRange r = NSMakeRange(pos, NSMaxRange(eff) - pos);
        if (r.length == 0) break;
        ClkCTRun *run = make_run(text, a ?: @{}, r, line->width);
        [runs addObject: run];
        [run release];
        line->width += run->width;
        if (run->ascent > line->ascent) line->ascent = run->ascent;
        if (run->descent > line->descent) line->descent = run->descent;
        if (run->leading > line->leading) line->leading = run->leading;
        pos = NSMaxRange(r);
    }
    line->runs = [runs copy];
    return line;
}

#define LINE(l) ((ClkCTLine *) (l))
#define RUN(r) ((ClkCTRun *) (r))

CFTypeID CTLineGetTypeID(void) { return 0; }
CFTypeID CTRunGetTypeID(void) { return 0; }

CTLineRef CTLineCreateWithAttributedString(CFAttributedStringRef s) {
    return s ? (CTLineRef) make_line((NSAttributedString *) s) : NULL;
}
CTLineRef CTLineCreateTruncatedLine(CTLineRef line, double width, CTLineTruncationType type, CTLineRef token) {
    return line ? (CTLineRef) [LINE(line) retain] : NULL;   /* nicht gekürzt */
}
CTLineRef CTLineCreateJustifiedLine(CTLineRef line, CGFloat factor, double width) {
    return line ? (CTLineRef) [LINE(line) retain] : NULL;
}
CFIndex CTLineGetGlyphCount(CTLineRef line) {
    CFIndex n = 0;
    for (ClkCTRun *r in LINE(line)->runs) n += r->count;
    return n;
}
CFArrayRef CTLineGetGlyphRuns(CTLineRef line) { return line ? (CFArrayRef) LINE(line)->runs : (CFArrayRef) [NSArray array]; }
CFRange CTLineGetStringRange(CTLineRef line) { return CFRangeMake(0, line ? [LINE(line)->string length] : 0); }
double CTLineGetPenOffsetForFlush(CTLineRef line, CGFloat flushFactor, double flushWidth) {
    double w = line ? LINE(line)->width : 0;
    return w < flushWidth ? (flushWidth - w) * flushFactor : 0;
}
double CTLineGetTypographicBounds(CTLineRef line, CGFloat *ascent, CGFloat *descent, CGFloat *leading) {
    if (!line) { if (ascent) *ascent = 0; if (descent) *descent = 0; if (leading) *leading = 0; return 0; }
    if (ascent) *ascent = LINE(line)->ascent;
    if (descent) *descent = LINE(line)->descent;
    if (leading) *leading = LINE(line)->leading;
    return LINE(line)->width;
}
CGRect CTLineGetBoundsWithOptions(CTLineRef line, CTLineBoundsOptions options) {
    if (!line) return CGRectZero;
    return CGRectMake(0, -LINE(line)->descent, LINE(line)->width, LINE(line)->ascent + LINE(line)->descent);
}
CGRect CTLineGetImageBounds(CTLineRef line, CGContextRef context) { return CTLineGetBoundsWithOptions(line, 0); }
double CTLineGetTrailingWhitespaceWidth(CTLineRef line) {
    if (!line) return 0;
    double w = 0;
    for (ClkCTRun *r in [LINE(line)->runs reverseObjectEnumerator]) {
        NSString *t = [LINE(line)->string string];
        for (CFIndex i = r->count - 1; i >= 0; i--) {
            if ([t characterAtIndex: r->range.location + i] != ' ') return w;
            w += r->advances[i].width;
        }
    }
    return w;
}
CFIndex CTLineGetStringIndexForPosition(CTLineRef line, CGPoint p) {
    if (!line) return kCFNotFound;
    for (ClkCTRun *r in LINE(line)->runs)
        for (CFIndex i = 0; i < r->count; i++)
            if (p.x < r->positions[i].x + r->advances[i].width / 2) return r->range.location + i;
    return [LINE(line)->string length];
}
CGFloat CTLineGetOffsetForStringIndex(CTLineRef line, CFIndex index, CGFloat *secondary) {
    CGFloat x = 0;
    if (line)
        for (ClkCTRun *r in LINE(line)->runs)
            for (CFIndex i = 0; i < r->count; i++)
                if (r->range.location + i < index) x = r->positions[i].x + r->advances[i].width;
    if (secondary) *secondary = x;
    return x;
}
void CTLineDraw(CTLineRef line, CGContextRef context) {
    if (!line || !context) return;
    Class gc = objc_getClass("NSGraphicsContext");
    id cur = gc ? msg((id) gc, "currentContext") : nil;
    if (cur == nil || (CGContextRef) msg(cur, "graphicsPort") != context) return;
    CGPoint tp = CGContextGetTextPosition(context);
    ((void (*)(id, SEL, NSPoint)) objc_msgSend)(LINE(line)->string, sel_registerName("drawAtPoint:"), NSMakePoint(tp.x, tp.y - LINE(line)->descent));
}

CFIndex CTRunGetGlyphCount(CTRunRef run) { return run ? RUN(run)->count : 0; }
CFDictionaryRef CTRunGetAttributes(CTRunRef run) { return run ? (CFDictionaryRef) RUN(run)->attrs : NULL; }
CTRunStatus CTRunGetStatus(CTRunRef run) { return 0; }
const CGGlyph *CTRunGetGlyphsPtr(CTRunRef run) { return run ? RUN(run)->glyphs : NULL; }
const CGPoint *CTRunGetPositionsPtr(CTRunRef run) { return run ? RUN(run)->positions : NULL; }
const CGSize *CTRunGetAdvancesPtr(CTRunRef run) { return run ? RUN(run)->advances : NULL; }
const CFIndex *CTRunGetStringIndicesPtr(CTRunRef run) { return run ? RUN(run)->indices : NULL; }
static CFRange clamp(CTRunRef run, CFRange r) {
    CFIndex n = run ? RUN(run)->count : 0;
    if (r.length == 0) r = CFRangeMake(r.location, n - r.location);   /* Länge 0 = bis zum Ende */
    if (r.location < 0) r.location = 0;
    if (r.location > n) r.location = n;
    if (r.location + r.length > n) r.length = n - r.location;
    return r;
}
void CTRunGetGlyphs(CTRunRef run, CFRange range, CGGlyph *buf) {
    range = clamp(run, range);
    if (buf && range.length > 0) memcpy(buf, RUN(run)->glyphs + range.location, range.length * sizeof *buf);
}
void CTRunGetPositions(CTRunRef run, CFRange range, CGPoint *buf) {
    range = clamp(run, range);
    if (buf && range.length > 0) memcpy(buf, RUN(run)->positions + range.location, range.length * sizeof *buf);
}
void CTRunGetAdvances(CTRunRef run, CFRange range, CGSize *buf) {
    range = clamp(run, range);
    if (buf && range.length > 0) memcpy(buf, RUN(run)->advances + range.location, range.length * sizeof *buf);
}
void CTRunGetStringIndices(CTRunRef run, CFRange range, CFIndex *buf) {
    range = clamp(run, range);
    if (buf && range.length > 0) memcpy(buf, RUN(run)->indices + range.location, range.length * sizeof *buf);
}
CFRange CTRunGetStringRange(CTRunRef run) { return run ? RUN(run)->range : CFRangeMake(0, 0); }
double CTRunGetTypographicBounds(CTRunRef run, CFRange range, CGFloat *ascent, CGFloat *descent, CGFloat *leading) {
    range = clamp(run, range);
    if (ascent) *ascent = run ? RUN(run)->ascent : 0;
    if (descent) *descent = run ? RUN(run)->descent : 0;
    if (leading) *leading = run ? RUN(run)->leading : 0;
    double w = 0;
    for (CFIndex i = 0; i < range.length; i++) w += RUN(run)->advances[range.location + i].width;
    return w;
}
CGRect CTRunGetImageBounds(CTRunRef run, CGContextRef context, CFRange range) {
    CGFloat a, d;
    double w = CTRunGetTypographicBounds(run, range, &a, &d, NULL);
    return CGRectMake(0, -d, w, a + d);
}
CGAffineTransform CTRunGetTextMatrix(CTRunRef run) { return CGAffineTransformIdentity; }
void CTRunGetBaseAdvancesAndOrigins(CTRunRef run, CFRange range, CGSize *adv, CGPoint *origins) {
    CTRunGetAdvances(run, range, adv);
    range = clamp(run, range);
    if (origins) for (CFIndex i = 0; i < range.length; i++) origins[i] = CGPointZero;
}
void CTRunDraw(CTRunRef run, CGContextRef context, CFRange range) {}
