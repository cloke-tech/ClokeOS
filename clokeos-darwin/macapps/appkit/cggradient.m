/* CGGradient und die Zeichenfunktionen, die in Cocotrons CoreGraphics fehlten (CGGradientCreateWithColorComponents gab NULL zurück,
 * CGContextDrawLinearGradient/DrawRadialGradient/DrawTiledImage/ShowGlyphsAtPositions waren STUBs ohne Wirkung). Logic Pro zeichnet damit
 * Knopfflächen, Spurköpfe und Anzeigen.
 *
 *   CGGradient: Farben als RGBA (Grau, RGB und CMYK werden umgerechnet), Positionen 0…1 (ohne Angabe gleichmäßig verteilt).
 *   Linear: Streifen senkrecht zur Achse über das Hüllrechteck des Clips, eine Farbe je Streifen (etwa ein Streifen je Punkt der
 *           Achsenlänge, höchstens 1024); vor/nach den Enden nur mit kCGGradientDrawsBefore/AfterStartLocation, wie bei Apple.
 *   Radial: Kreise vom Ende zum Anfang (Maler-Reihenfolge), mit Fortsetzung über die Enden wie oben.
 *   Kachelbild: das Bild im Raster von rect über das Hüllrechteck des Clips.
 *   Glyphen an Positionen: je Glyphe Textposition setzen und zeichnen. */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <math.h>

@interface ClkCGGradient : NSObject {
@public
    size_t count;
    CGFloat *rgba;
    CGFloat *loc;
}
@end
@implementation ClkCGGradient
- (void) dealloc { free(rgba); free(loc); [super dealloc]; }
@end

static void to_rgba(const CGFloat *c, size_t n, BOOL hasAlpha, CGFloat out[4]) {
    size_t cn = hasAlpha ? n - 1 : n;
    CGFloat a = hasAlpha ? c[n - 1] : 1;
    if (cn == 1) { out[0] = out[1] = out[2] = c[0]; }
    else if (cn == 3) { out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; }
    else if (cn == 4) { out[0] = (1 - c[0]) * (1 - c[3]); out[1] = (1 - c[1]) * (1 - c[3]); out[2] = (1 - c[2]) * (1 - c[3]); }
    else { out[0] = out[1] = out[2] = 0; }
    out[3] = a;
}

static ClkCGGradient *make(size_t count, const CGFloat *locations) {
    ClkCGGradient *g = [ClkCGGradient new];
    g->count = count;
    g->rgba = calloc(count * 4 + 4, sizeof(CGFloat));
    g->loc = calloc(count + 1, sizeof(CGFloat));
    for (size_t i = 0; i < count; i++)
        g->loc[i] = locations ? locations[i] : (count > 1 ? (CGFloat) i / (count - 1) : 0);
    return g;
}

CGGradientRef CGGradientCreateWithColorComponents(CGColorSpaceRef space, const CGFloat components[], const CGFloat locations[], size_t count) {
    if (!components || count == 0) return NULL;
    size_t n = space ? CGColorSpaceGetNumberOfComponents(space) : 3;
    ClkCGGradient *g = make(count, locations);
    for (size_t i = 0; i < count; i++) to_rgba(components + i * (n + 1), n + 1, YES, g->rgba + i * 4);
    return (CGGradientRef) g;
}

CGGradientRef CGGradientCreateWithColors(CGColorSpaceRef space, CFArrayRef colors, const CGFloat locations[]) {
    CFIndex count = colors ? CFArrayGetCount(colors) : 0;
    if (count == 0) return NULL;
    ClkCGGradient *g = make(count, locations);
    for (CFIndex i = 0; i < count; i++) {
        CGColorRef c = (CGColorRef) CFArrayGetValueAtIndex(colors, i);
        size_t n = c ? CGColorGetNumberOfComponents(c) : 0;
        if (n > 0) to_rgba(CGColorGetComponents(c), n, YES, g->rgba + i * 4);
    }
    return (CGGradientRef) g;
}

void CGGradientRelease(CGGradientRef self) { if (self) CFRelease(self); }
CGGradientRef CGGradientRetain(CGGradientRef self) { return self ? (CGGradientRef) CFRetain(self) : NULL; }
CFTypeID CGGradientGetTypeID(void) { return 0; }

static void color_at(ClkCGGradient *g, CGFloat t, CGFloat out[4]) {
    size_t n = g->count;
    if (n == 1 || t <= g->loc[0]) { memcpy(out, g->rgba, 4 * sizeof(CGFloat)); return; }
    if (t >= g->loc[n - 1]) { memcpy(out, g->rgba + (n - 1) * 4, 4 * sizeof(CGFloat)); return; }
    for (size_t i = 0; i + 1 < n; i++) {
        if (t <= g->loc[i + 1]) {
            CGFloat span = g->loc[i + 1] - g->loc[i], f = span > 0 ? (t - g->loc[i]) / span : 0;
            for (int k = 0; k < 4; k++) out[k] = g->rgba[i * 4 + k] + (g->rgba[(i + 1) * 4 + k] - g->rgba[i * 4 + k]) * f;
            return;
        }
    }
    memcpy(out, g->rgba + (n - 1) * 4, 4 * sizeof(CGFloat));
}

static void fill_quad(CGContextRef c, CGPoint a, CGPoint b, CGPoint d, CGPoint e, const CGFloat col[4]) {
    CGContextSetRGBFillColor(c, col[0], col[1], col[2], col[3]);
    CGContextBeginPath(c);
    CGContextMoveToPoint(c, a.x, a.y);
    CGContextAddLineToPoint(c, b.x, b.y);
    CGContextAddLineToPoint(c, d.x, d.y);
    CGContextAddLineToPoint(c, e.x, e.y);
    CGContextClosePath(c);
    CGContextFillPath(c);
}

void CGContextDrawLinearGradient(CGContextRef c, CGGradientRef gradient, CGPoint start, CGPoint end, CGGradientDrawingOptions options) {
    ClkCGGradient *g = (ClkCGGradient *) gradient;
    if (!c || !g) return;
    CGFloat dx = end.x - start.x, dy = end.y - start.y, len = sqrt(dx * dx + dy * dy);
    if (len <= 0) return;
    CGRect clip = CGContextGetClipBoundingBox(c);
    if (CGRectIsEmpty(clip)) return;
    CGPoint u = CGPointMake(dx / len, dy / len), nrm = CGPointMake(-u.y, u.x);
    /* Ausdehnung des Clip-Rechtecks entlang der Achse (t in Längen) und quer dazu (s) */
    CGFloat tmin = INFINITY, tmax = -INFINITY, smin = INFINITY, smax = -INFINITY;
    CGPoint corners[4] = { {CGRectGetMinX(clip), CGRectGetMinY(clip)}, {CGRectGetMaxX(clip), CGRectGetMinY(clip)},
                           {CGRectGetMinX(clip), CGRectGetMaxY(clip)}, {CGRectGetMaxX(clip), CGRectGetMaxY(clip)} };
    for (int i = 0; i < 4; i++) {
        CGFloat px = corners[i].x - start.x, py = corners[i].y - start.y;
        CGFloat t = (px * u.x + py * u.y) / len, s = px * nrm.x + py * nrm.y;
        tmin = MIN(tmin, t); tmax = MAX(tmax, t); smin = MIN(smin, s); smax = MAX(smax, s);
    }
    smin -= 1; smax += 1;
    CGFloat t0 = MAX(tmin, 0), t1 = MIN(tmax, 1);
    #define AT(t, s) CGPointMake(start.x + u.x * len * (t) + nrm.x * (s), start.y + u.y * len * (t) + nrm.y * (s))
    CGContextSaveGState(c);
    CGContextSetShouldAntialias(c, false);
    CGFloat col[4];
    if ((options & kCGGradientDrawsBeforeStartLocation) && tmin < 0) {
        color_at(g, 0, col);
        fill_quad(c, AT(tmin, smin), AT(tmin, smax), AT(0, smax), AT(0, smin), col);
    }
    if ((options & kCGGradientDrawsAfterEndLocation) && tmax > 1) {
        color_at(g, 1, col);
        fill_quad(c, AT(1, smin), AT(1, smax), AT(tmax, smax), AT(tmax, smin), col);
    }
    if (t1 > t0) {
        int steps = (int) ceil((t1 - t0) * len);
        if (steps < 1) steps = 1;
        if (steps > 1024) steps = 1024;
        for (int i = 0; i < steps; i++) {
            CGFloat a = t0 + (t1 - t0) * i / steps, b = t0 + (t1 - t0) * (i + 1) / steps;
            color_at(g, (a + b) / 2, col);
            /* etwas Überlappung gegen Lücken zwischen den Streifen */
            CGFloat bb = i + 1 < steps ? b + 0.5 / len : b;
            fill_quad(c, AT(a, smin), AT(a, smax), AT(bb, smax), AT(bb, smin), col);
        }
    }
    #undef AT
    CGContextRestoreGState(c);
}

void CGContextDrawRadialGradient(CGContextRef c, CGGradientRef gradient, CGPoint startCenter, CGFloat startRadius,
                                 CGPoint endCenter, CGFloat endRadius, CGGradientDrawingOptions options) {
    ClkCGGradient *g = (ClkCGGradient *) gradient;
    if (!c || !g) return;
    CGRect clip = CGContextGetClipBoundingBox(c);
    if (CGRectIsEmpty(clip)) return;
    CGContextSaveGState(c);
    CGFloat col[4];
    if (options & kCGGradientDrawsAfterEndLocation) {
        color_at(g, 1, col);
        CGContextSetRGBFillColor(c, col[0], col[1], col[2], col[3]);
        CGContextFillRect(c, clip);
    }
    CGFloat span = MAX(fabs(endRadius - startRadius), hypot(endCenter.x - startCenter.x, endCenter.y - startCenter.y));
    int steps = (int) ceil(span);
    if (steps < 2) steps = 2;
    if (steps > 512) steps = 512;
    for (int i = steps; i >= 0; i--) {
        CGFloat t = (CGFloat) i / steps;
        CGFloat r = startRadius + (endRadius - startRadius) * t;
        if (r <= 0) continue;
        CGPoint p = CGPointMake(startCenter.x + (endCenter.x - startCenter.x) * t, startCenter.y + (endCenter.y - startCenter.y) * t);
        color_at(g, t, col);
        CGContextSetRGBFillColor(c, col[0], col[1], col[2], col[3]);
        CGContextFillEllipseInRect(c, CGRectMake(p.x - r, p.y - r, 2 * r, 2 * r));
    }
    if ((options & kCGGradientDrawsBeforeStartLocation) && startRadius > 0) {
        color_at(g, 0, col);
        CGContextSetRGBFillColor(c, col[0], col[1], col[2], col[3]);
        CGContextFillEllipseInRect(c, CGRectMake(startCenter.x - startRadius, startCenter.y - startRadius, 2 * startRadius, 2 * startRadius));
    }
    CGContextRestoreGState(c);
}

void CGContextDrawTiledImage(CGContextRef c, CGRect rect, CGImageRef image) {
    if (!c || !image || rect.size.width < 1 || rect.size.height < 1) return;
    CGRect clip = CGContextGetClipBoundingBox(c);
    if (CGRectIsEmpty(clip)) return;
    CGFloat x0 = rect.origin.x + floor((CGRectGetMinX(clip) - rect.origin.x) / rect.size.width) * rect.size.width;
    CGFloat y0 = rect.origin.y + floor((CGRectGetMinY(clip) - rect.origin.y) / rect.size.height) * rect.size.height;
    int n = 0;
    for (CGFloat y = y0; y < CGRectGetMaxY(clip) && n < 100000; y += rect.size.height)
        for (CGFloat x = x0; x < CGRectGetMaxX(clip) && n < 100000; x += rect.size.width, n++)
            CGContextDrawImage(c, CGRectMake(x, y, rect.size.width, rect.size.height), image);
}

void CGContextShowGlyphsAtPositions(CGContextRef c, const CGGlyph *glyphs, const CGPoint *positions, size_t count) {
    if (!c || !glyphs || !positions) return;
    for (size_t i = 0; i < count; i++) {
        CGContextSetTextPosition(c, positions[i].x, positions[i].y);
        CGContextShowGlyphs(c, glyphs + i, 1);
    }
}
