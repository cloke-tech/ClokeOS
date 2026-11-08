/* CTLine und CTRun für ClokeOS' CoreText (ersetzt Cocotrons CTLine.m/CTRun.m, die nur Platzhalter mit NULL-Ergebnissen hatten;
 * Logic Pros MAToolKit legt Texte so aus und stürzte bei CFArrayGetCount(NULL) ab).
 *
 * Einfach, aber echt: eine Zeile hält ihre Zeichenkette und teilt sie an Attributgrenzen in Läufe. Glyphen und Vorschübe kommen aus der
 * Schrift des Laufs (NSFont; CTFontCreateWithName liefert hier NSFonts), ohne Ligaturen, Kerning, bidirektionalen Text oder
 * Schriftersatz. Positionen sind die Summe der Vorschübe auf der Grundlinie. CTLineDraw/CTRunDraw zeichnen die Glyphen über
 * CoreGraphics in jeden Kontext (auch CGLayer- und Bitmap-Kontexte) an der Textposition, mit Textmatrix und Füllfarbe aus den Attributen,
 * wie Apples CoreText. CTFramesetter/CTFrame: Zeilen an Zeilenumbrüchen und, wo nötig, an Leerzeichen umbrochen, von oben im
 * Hüllrechteck des Pfads. AppKit wird zur Laufzeit gefunden. */
#import <Foundation/Foundation.h>
#import <CoreText/CTLine.h>
#import <CoreText/CTRun.h>
#import <CoreText/CTFont.h>
#import <CoreText/CTStringAttributes.h>
#import <CoreText/CTFrame.h>
#import <CoreText/CTFramesetter.h>
#import <CoreGraphics/CoreGraphics.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <stdio.h>
#import <stdlib.h>
#import <execinfo.h>
#import <dlfcn.h>

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
    if (getenv("CLOKE_CT_TRACE"))   /* angelegte Zeilen (Fehlersuche: wird ein Text gebaut, aber nicht gezeichnet?) */
        fprintf(stderr, "[clokeos-ct] neue Zeile \"%.60s\"\n", s ? [[(NSAttributedString *) s string] UTF8String] ?: "" : "(NULL)");
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
/* Schrift eines Laufs als CGFont (NSFonts _cgFont), Farbe aus kCTForegroundColorAttributeName (CGColor) oder NSForegroundColor (NSColor);
 * kCTForegroundColorFromContextAttributeName lässt die Füllfarbe des Kontexts stehen. Ohne Angabe schwarz, wie bei Apple. */
static void draw_run(ClkCTRun *r, CGContextRef ctx, CGPoint origin, CFIndex from, CFIndex count) {
    if (count <= 0) return;
    id font = run_font(r->attrs);
    CGFontRef cg = NULL;
    if (font) object_getInstanceVariable(font, "_cgFont", (void **) &cg);
    static int trace = -1;
    if (trace < 0) trace = getenv("CLOKE_CT_TRACE") != NULL;
    if (trace) {
        CGAffineTransform m = CGContextGetCTM(ctx), t = CGContextGetTextMatrix(ctx);
        fprintf(stderr, "[clokeos-ct] n=%ld font=%s/%s cg=%p at %.1f,%.1f ctm=[%.2f %.2f %.2f %.2f %.1f %.1f] tm=[%.2f %.2f %.2f %.2f] keys=%s\n",
                (long) count, font ? class_getName(object_getClass(font)) : "nil",
                [font respondsToSelector: sel_registerName("fontName")] ? [[msg(font, "fontName") description] UTF8String] : "?", cg,
                origin.x, origin.y, m.a, m.b, m.c, m.d, m.tx, m.ty, t.a, t.b, t.c, t.d,
                [[[r->attrs allKeys] componentsJoinedByString: @","] UTF8String] ?: "");
    }
    if (cg == NULL) return;
    CGContextSaveGState(ctx);
    if (trace) {
        CGColorRef tc = (CGColorRef) r->attrs[@"CTForegroundColor"];
        const CGFloat *k = tc ? CGColorGetComponents(tc) : NULL;
        size_t kn = tc ? CGColorGetNumberOfComponents(tc) : 0;
        fprintf(stderr, "[clokeos-ct]   color n=%zu %.2f %.2f %.2f %.2f size=%.1f\n", kn, kn > 0 ? k[0] : -1, kn > 1 ? k[1] : -1,
                kn > 2 ? k[2] : -1, kn > 3 ? k[3] : -1, fmsg(font, "pointSize"));
        static int bts;
        if (kn == 4 && k[3] == 0 && bts++ < 3) {   /* durchsichtige Textfarbe: wer zeichnet (Bild + Versatz) */
            void *pcs[12];
            int n = backtrace(pcs, 12);
            for (int i = 1; i < n; i++) {
                Dl_info di;
                if (dladdr(pcs[i], &di) && di.dli_fname)
                    fprintf(stderr, "[clokeos-ct]     %s +%#lx %s\n", strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                            (unsigned long) ((char *) pcs[i] - (char *) di.dli_fbase), di.dli_sname ?: "");
            }
        }
    }
    id fromCtx = r->attrs[@"CTForegroundColorFromContext"];
    if (!(fromCtx && [fromCtx boolValue])) {
        CGColorRef c = (CGColorRef) r->attrs[@"CTForegroundColor"];
        id ns = r->attrs[@"NSColor"];
        if (c == NULL && ns && [ns respondsToSelector: sel_registerName("CGColor")]) c = (CGColorRef) msg(ns, "CGColor");
        if (c) CGContextSetFillColorWithColor(ctx, c);
        else CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    }
    CGFloat size = fmsg(font, "pointSize");
    CGContextSetFont(ctx, cg);
    CGContextSetFontSize(ctx, size > 0 ? size : 12);
    CGContextSetTextPosition(ctx, origin.x + r->positions[from].x, origin.y + r->positions[from].y);
    CGContextShowGlyphsWithAdvances(ctx, r->glyphs + from, r->advances + from, count);
    CGContextRestoreGState(ctx);
}

void CTLineDraw(CTLineRef line, CGContextRef context) {
    if (!line || !context) return;
    CGPoint tp = CGContextGetTextPosition(context);
    if (getenv("CLOKE_CT_TRACE"))   /* der Text der Zeile (Fehlersuche: wer zeichnet was) */
        fprintf(stderr, "[clokeos-ct] Zeile \"%.60s\"\n", [[LINE(line)->string description] UTF8String] ?: "");
    for (ClkCTRun *r in LINE(line)->runs) draw_run(r, context, tp, 0, r->count);
    CGContextSetTextPosition(context, tp.x + LINE(line)->width, tp.y);
}

void CTRunDraw(CTRunRef run, CGContextRef context, CFRange range) {
    if (!run || !context) return;
    CFIndex n = RUN(run)->count, from = range.location, len = range.length;
    if (from < 0 || from >= n) return;
    if (len <= 0 || from + len > n) len = n - from;
    /* Positionen sind auf die Zeile bezogen; Apple zeichnet einen Lauf ebenso an Textposition + Laufposition */
    draw_run(RUN(run), context, CGContextGetTextPosition(context), from, len);
}

/* --- CTFramesetter / CTFrame ------------------------------------------------------------------------------------------------------- */
@interface ClkCTFramesetter : NSObject { @public NSAttributedString *string; } @end
@implementation ClkCTFramesetter
- (void) dealloc { [string release]; [super dealloc]; }
@end
@interface ClkCTFrame : NSObject {
@public
    NSArray *lines;
    CGPoint *origins;
    CFRange range, visible;
    CGPathRef path;
}
@end
@implementation ClkCTFrame
- (void) dealloc { [lines release]; free(origins); CGPathRelease(path); [super dealloc]; }
@end

/* Zeilen von [range] mit höchstens [width] Breite (<= 0: unbegrenzt): harte Umbrüche, sonst am letzten Leerzeichen, sonst mitten im Wort */
static NSArray *break_lines(NSAttributedString *s, CFRange range, CGFloat width, NSMutableArray *ranges) {
    NSMutableArray *out = [NSMutableArray array];
    NSString *t = [s string];
    NSUInteger end = range.length > 0 ? range.location + range.length : [t length];
    if (end > [t length]) end = [t length];
    NSUInteger pos = range.location;
    while (pos < end) {
        NSUInteger hard = pos;
        while (hard < end && [t characterAtIndex: hard] != '\n' && [t characterAtIndex: hard] != '\r' && [t characterAtIndex: hard] != 0x2028) hard++;
        NSUInteger stop = hard;
        if (width > 0) {
            ClkCTLine *probe = make_line([s attributedSubstringFromRange: NSMakeRange(pos, hard - pos)]);
            if (probe->width > width) {
                CFIndex fit = CTLineGetStringIndexForPosition((CTLineRef) probe, CGPointMake(width, 0));
                NSUInteger cut = pos + (fit > 0 ? (NSUInteger) fit : 1);
                NSUInteger sp = cut;
                while (sp > pos && [t characterAtIndex: sp - 1] != ' ') sp--;
                stop = sp > pos ? sp : cut;
            }
            [probe release];
        }
        NSRange lr = NSMakeRange(pos, stop - pos);
        ClkCTLine *line = make_line([s attributedSubstringFromRange: lr]);
        /* Indizes der Läufe auf die ganze Zeichenkette beziehen */
        for (ClkCTRun *r in line->runs) {
            r->range.location += pos;
            for (CFIndex k = 0; k < r->count; k++) r->indices[k] += pos;
        }
        [out addObject: line];
        [line release];
        [ranges addObject: [NSValue valueWithRange: lr]];
        pos = stop;
        if (pos == hard && pos < end) pos++;   /* Zeilenumbruch selbst überspringen */
    }
    return out;
}

CFTypeID CTFramesetterGetTypeID(void) { return 0; }
CFTypeID CTFrameGetTypeID(void) { return 0; }
CTFramesetterRef CTFramesetterCreateWithAttributedString(CFAttributedStringRef s) {
    if (!s) return NULL;
    ClkCTFramesetter *f = [ClkCTFramesetter new];
    f->string = [(NSAttributedString *) s copy];
    return (CTFramesetterRef) f;
}
CTFramesetterRef CTFramesetterCreateWithTypesetter(CTTypesetterRef typesetter) { return NULL; }
CTTypesetterRef CTFramesetterGetTypesetter(CTFramesetterRef framesetter) { return NULL; }

CTFrameRef CTFramesetterCreateFrame(CTFramesetterRef fs, CFRange range, CGPathRef path, CFDictionaryRef attrs) {
    if (!fs) return NULL;
    CGRect box = path ? CGPathGetBoundingBox(path) : CGRectMake(0, 0, 1e6, 1e6);
    NSMutableArray *ranges = [NSMutableArray array];
    NSArray *lines = break_lines(((ClkCTFramesetter *) fs)->string, range, box.size.width, ranges);
    ClkCTFrame *f = [ClkCTFrame new];
    f->origins = calloc([lines count] + 1, sizeof(CGPoint));
    CGFloat y = CGRectGetMaxY(box);
    NSUInteger shown = 0;
    for (ClkCTLine *l in lines) {
        y -= l->ascent;
        if (y - l->descent < CGRectGetMinY(box) - 0.5 && shown > 0) break;
        f->origins[shown++] = CGPointMake(box.origin.x, y);
        y -= l->descent + l->leading;
    }
    f->lines = [[lines subarrayWithRange: NSMakeRange(0, shown)] retain];
    f->path = path ? CGPathRetain(path) : NULL;
    NSUInteger n = [[((ClkCTFramesetter *) fs)->string string] length];
    f->range = CFRangeMake(range.location, range.length > 0 ? range.length : (CFIndex) (n - range.location));
    NSRange last = shown ? [ranges[shown - 1] rangeValue] : NSMakeRange(range.location, 0);
    f->visible = CFRangeMake(range.location, (CFIndex) (NSMaxRange(last) - range.location));
    return (CTFrameRef) f;
}

CGSize CTFramesetterSuggestFrameSizeWithConstraints(CTFramesetterRef fs, CFRange range, CFDictionaryRef attrs, CGSize constraints, CFRange *fitRange) {
    if (!fs) { if (fitRange) *fitRange = CFRangeMake(0, 0); return CGSizeZero; }
    NSMutableArray *ranges = [NSMutableArray array];
    NSArray *lines = break_lines(((ClkCTFramesetter *) fs)->string, range, constraints.width < 1e5 ? constraints.width : 0, ranges);
    CGFloat w = 0, h = 0;
    NSUInteger shown = 0;
    for (ClkCTLine *l in lines) {
        CGFloat lh = l->ascent + l->descent + l->leading;
        if (constraints.height > 0 && constraints.height < 1e5 && h + lh > constraints.height + 0.5 && shown > 0) break;
        h += lh; if (l->width > w) w = l->width; shown++;
    }
    if (fitRange) {
        NSRange last = shown ? [ranges[shown - 1] rangeValue] : NSMakeRange(range.location, 0);
        *fitRange = CFRangeMake(range.location, (CFIndex) (NSMaxRange(last) - range.location));
    }
    return CGSizeMake(ceil(w), ceil(h));
}

CFRange CTFrameGetStringRange(CTFrameRef f) { return f ? ((ClkCTFrame *) f)->range : CFRangeMake(0, 0); }
CFRange CTFrameGetVisibleStringRange(CTFrameRef f) { return f ? ((ClkCTFrame *) f)->visible : CFRangeMake(0, 0); }
CGPathRef CTFrameGetPath(CTFrameRef f) { return f ? ((ClkCTFrame *) f)->path : NULL; }
CFDictionaryRef CTFrameGetFrameAttributes(CTFrameRef f) { return NULL; }
CFArrayRef CTFrameGetLines(CTFrameRef f) { return f ? (CFArrayRef) ((ClkCTFrame *) f)->lines : (CFArrayRef) [NSArray array]; }
void CTFrameGetLineOrigins(CTFrameRef f, CFRange range, CGPoint origins[]) {
    if (!f || !origins) return;
    CFIndex n = [((ClkCTFrame *) f)->lines count];
    CFIndex len = range.length > 0 ? range.length : n - range.location;
    for (CFIndex i = 0; i < len && range.location + i < n; i++) origins[i] = ((ClkCTFrame *) f)->origins[range.location + i];
}
void CTFrameDraw(CTFrameRef f, CGContextRef ctx) {
    if (!f || !ctx) return;
    ClkCTFrame *fr = (ClkCTFrame *) f;
    CFIndex i = 0;
    for (ClkCTLine *l in fr->lines) {
        CGContextSetTextPosition(ctx, fr->origins[i].x, fr->origins[i].y);
        CTLineDraw((CTLineRef) l, ctx);
        i++;
    }
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
