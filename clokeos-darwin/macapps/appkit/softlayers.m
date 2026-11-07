/* Ebenen (CALayer) ohne OpenGL: ClokeOS schaltet Cocotrons Ebenen-Darstellung aus (sie braucht OpenGL). Ebenengestützte Ansichten
 * zeichneten damit nur, was in drawRect: steht; alles, was ein Programm über die Ebene macht, fehlte (Logic Pro: dunkle Hintergründe aus
 * layer.backgroundColor, Ansichten mit updateLayer, eigene Unterebenen). Hier wird die Ebene beim Zeichnen der Ansicht in deren Kontext
 * gemalt, in Software, wie Core Animation es täte:
 *
 *   Hintergrundfarbe (mit cornerRadius), Inhalt (contents: CGImage/NSImage, contentsGravity), bei updateLayer-Ansichten statt drawRect:
 *   updateLayer, sonst drawRect:, dann die Unterebenen (nicht die anderer Ansichten) und zuletzt der Rand (borderWidth/borderColor).
 *   Unterebenen: position/anchorPoint/bounds, affine transform, geometryFlipped, masksToBounds, hidden, opacity (als Alpha);
 *   Inhalt mit drawInContext: (Delegat drawLayer:inContext:), CATextLayer (string, font, fontSize, foregroundColor, alignmentMode),
 *   CAShapeLayer (path, fillColor, strokeColor, lineWidth; Eigenschaften aus modern.m), CAGradientLayer (colors, locations, start/endPoint).
 *
 * Fehlende CALayer-Eigenschaften (Cocotrons CALayer kennt nur Geometrie, contents und opacity) werden hier mit class_addMethod ergänzt
 * und in einem Wörterbuch an der Ebene gespeichert; Setzen markiert die Ansicht, zu der die Ebene gehört, zum Neuzeichnen.
 * Nicht: Animationen, Schatten, Filter, Masken-Ebenen, 3D-Transformationen. NSView.m.sed ruft -_clkDrawLayerBackedRect: auf. */
#import <AppKit/AppKit.h>
#import <QuartzCore/CALayer.h>
#import <QuartzCore/CATextLayer.h>
#import <QuartzCore/CAGradientLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <QuartzCore/CATransform3D.h>
#import <CoreText/CTLine.h>
#import <objc/runtime.h>
#import <objc/message.h>

static char k_props;

static NSMutableDictionary *props(id l) {
    NSMutableDictionary *d = objc_getAssociatedObject(l, &k_props);
    if (d == nil) {
        d = [NSMutableDictionary dictionary];
        objc_setAssociatedObject(l, &k_props, d, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return d;
}

/* Ansicht, zu der eine Ebene gehört: die Ebene selbst oder eine darüber hat eine NSView als Delegaten */
static NSView *owner_view(CALayer *l) {
    for (int i = 0; l != nil && i < 64; i++, l = [l superlayer]) {
        id d = [l delegate];
        if ([d isKindOfClass: [NSView class]]) return d;
    }
    return nil;
}
static int g_painting;   /* während des Malens (updateLayer setzt Eigenschaften) nicht neu anstoßen: sonst Endlosschleife */
static void touched(CALayer *l) {
    if (g_painting) return;
    NSView *v = owner_view(l);
    if (v != nil) [v setNeedsDisplay: YES];
}

/* Schlüssel aus dem Selektor: setFooBar: -> fooBar, isFoo -> foo */
static NSString *key_for(SEL s) {
    const char *n = sel_getName(s);
    if (strncmp(n, "set", 3) == 0 && n[strlen(n) - 1] == ':') {
        NSString *k = [[NSString alloc] initWithBytes: n + 3 length: strlen(n) - 4 encoding: NSUTF8StringEncoding];
        NSString *r = [[[k substringToIndex: 1] lowercaseString] stringByAppendingString: [k substringFromIndex: 1]];
        [k release];
        return r;
    }
    if (strncmp(n, "is", 2) == 0 && n[2] >= 'A' && n[2] <= 'Z')
        return [[[NSString stringWithUTF8String: n + 2] substringToIndex: 1].lowercaseString
                   stringByAppendingString: [[NSString stringWithUTF8String: n + 2] substringFromIndex: 1]];
    return [NSString stringWithUTF8String: n];
}

static NSDictionary *g_defaults;
static id dflt(id l, NSString *k) {
    if ([l isKindOfClass: [CATextLayer class]]) {
        if ([k isEqualToString: @"fontSize"]) return @36.0;
        if ([k isEqualToString: @"foregroundColor"]) return (id) CGColorGetConstantColor(kCGColorWhite);
    }
    if ([l isKindOfClass: [CAGradientLayer class]]) {
        if ([k isEqualToString: @"startPoint"]) return [NSValue valueWithPoint: NSMakePoint(0.5, 0)];
        if ([k isEqualToString: @"endPoint"]) return [NSValue valueWithPoint: NSMakePoint(0.5, 1)];
    }
    return g_defaults[k];
}
static id get(id l, NSString *k) { id v = props(l)[k]; return v ? v : dflt(l, k); }
static void put(id l, NSString *k, id v) {
    if (v) props(l)[k] = v; else [props(l) removeObjectForKey: k];
    touched(l);
}

static id g_obj(id s, SEL c) { return get(s, key_for(c)); }
static void s_obj(id s, SEL c, id v) { put(s, key_for(c), v); }
static CGFloat g_f(id s, SEL c) { return [get(s, key_for(c)) doubleValue]; }
static void s_f(id s, SEL c, CGFloat v) { put(s, key_for(c), @(v)); }
static BOOL g_b(id s, SEL c) { return [get(s, key_for(c)) boolValue]; }
static void s_b(id s, SEL c, BOOL v) { put(s, key_for(c), @(v)); }
static NSUInteger g_u(id s, SEL c) { return [get(s, key_for(c)) unsignedIntegerValue]; }
static void s_u(id s, SEL c, NSUInteger v) { put(s, key_for(c), @(v)); }
static CGPoint g_p(id s, SEL c) { id v = get(s, key_for(c)); return v ? NSPointToCGPoint([v pointValue]) : CGPointZero; }
static void s_p(id s, SEL c, CGPoint v) { put(s, key_for(c), [NSValue valueWithPoint: NSPointFromCGPoint(v)]); }
static CGRect g_r(id s, SEL c) { id v = get(s, key_for(c)); return v ? NSRectToCGRect([v rectValue]) : CGRectMake(0, 0, 1, 1); }
static void s_r(id s, SEL c, CGRect v) { put(s, key_for(c), [NSValue valueWithRect: NSRectFromCGRect(v)]); }

static void add_prop(Class cls, const char *name, char type, BOOL isGetter) {
    char setter[96];
    snprintf(setter, sizeof setter, "set%c%s:", toupper((unsigned char) name[0]), name + 1);
    IMP g = NULL, s = NULL;
    const char *gt = NULL, *st = NULL;
    switch (type) {
    case 'o': g = (IMP) g_obj; s = (IMP) s_obj; gt = "@@:"; st = "v@:@"; break;
    case 'f': g = (IMP) g_f; s = (IMP) s_f; gt = "d@:"; st = "v@:d"; break;
    case 'b': g = (IMP) g_b; s = (IMP) s_b; gt = "c@:"; st = "v@:c"; break;
    case 'u': g = (IMP) g_u; s = (IMP) s_u; gt = "Q@:"; st = "v@:Q"; break;
    case 'p': g = (IMP) g_p; s = (IMP) s_p; gt = "{CGPoint=dd}@:"; st = "v@:{CGPoint=dd}"; break;
    case 'r': g = (IMP) g_r; s = (IMP) s_r; gt = "{CGRect={CGPoint=dd}{CGSize=dd}}@:"; st = "v@:{CGRect={CGPoint=dd}{CGSize=dd}}"; break;
    }
    class_addMethod(cls, sel_registerName(name), g, gt);
    if (type == 'b') {
        char is[96];
        snprintf(is, sizeof is, "is%c%s", toupper((unsigned char) name[0]), name + 1);
        class_addMethod(cls, sel_registerName(is), g, gt);
    }
    class_addMethod(cls, sel_registerName(setter), s, st);
}

/* --- Malen ------------------------------------------------------------------------------------------------------------------------ */

static CGPathRef rounded(CGRect r, CGFloat rad) {
    CGMutablePathRef p = CGPathCreateMutable();
    rad = MIN(rad, MIN(r.size.width, r.size.height) / 2);
    if (rad <= 0) { CGPathAddRect(p, NULL, r); return p; }
    CGPathMoveToPoint(p, NULL, CGRectGetMinX(r) + rad, CGRectGetMinY(r));
    CGPathAddArcToPoint(p, NULL, CGRectGetMaxX(r), CGRectGetMinY(r), CGRectGetMaxX(r), CGRectGetMaxY(r), rad);
    CGPathAddArcToPoint(p, NULL, CGRectGetMaxX(r), CGRectGetMaxY(r), CGRectGetMinX(r), CGRectGetMaxY(r), rad);
    CGPathAddArcToPoint(p, NULL, CGRectGetMinX(r), CGRectGetMaxY(r), CGRectGetMinX(r), CGRectGetMinY(r), rad);
    CGPathAddArcToPoint(p, NULL, CGRectGetMinX(r), CGRectGetMinY(r), CGRectGetMaxX(r), CGRectGetMinY(r), rad);
    CGPathCloseSubpath(p);
    return p;
}

static CGColorRef as_cgcolor(id c) {
    if (c == nil) return NULL;
    if ([c isKindOfClass: [NSColor class]]) return [(NSColor *) c CGColor];
    return (CGColorRef) c;
}

static void paint_background(CALayer *l, CGContextRef c) {
    CGColorRef bg = as_cgcolor(get(l, @"backgroundColor"));
    static int trace = -1;
    if (trace < 0) trace = getenv("CLOKE_LAYER_TRACE") != NULL;
    if (trace && bg) {   /* je Ebene einmal: Farbe des Hintergrunds und wem sie gehört */
        static NSMutableSet *seen;
        if (!seen) seen = [NSMutableSet new];
        NSValue *k = [NSValue valueWithPointer: l];
        if (![seen containsObject: k]) {
            [seen addObject: k];
            const CGFloat *q = CGColorGetComponents(bg);
            size_t n = CGColorGetNumberOfComponents(bg);
            NSView *v = owner_view(l);
            fprintf(stderr, "[clokeos-layer] %s/%s bg n=%zu %.2f %.2f %.2f %.2f (%s)\n", v ? class_getName(object_getClass(v)) : "-",
                    class_getName(object_getClass(l)), n, n > 0 ? q[0] : -1, n > 1 ? q[1] : -1, n > 2 ? q[2] : -1, n > 3 ? q[3] : -1,
                    class_getName(object_getClass(get(l, @"backgroundColor"))));
        }
    }
    if (bg == NULL || CGColorGetAlpha(bg) <= 0) return;
    CGPathRef p = rounded([l bounds], g_f(l, @selector(cornerRadius)));
    CGContextSaveGState(c);
    CGContextSetFillColorWithColor(c, bg);
    CGContextAddPath(c, p);
    CGContextFillPath(c);
    CGContextRestoreGState(c);
    CGPathRelease(p);
}

static void paint_border(CALayer *l, CGContextRef c) {
    CGFloat w = g_f(l, @selector(borderWidth));
    CGColorRef bc = as_cgcolor(get(l, @"borderColor"));
    if (w <= 0) return;
    CGRect b = CGRectInset([l bounds], w / 2, w / 2);
    CGPathRef p = rounded(b, MAX(0, g_f(l, @selector(cornerRadius)) - w / 2));
    CGContextSaveGState(c);
    if (bc) CGContextSetStrokeColorWithColor(c, bc); else CGContextSetRGBStrokeColor(c, 0, 0, 0, 1);
    CGContextSetLineWidth(c, w);
    CGContextAddPath(c, p);
    CGContextStrokePath(c);
    CGContextRestoreGState(c);
    CGPathRelease(p);
}

/* Inhalt aufrecht zeichnen: im gekippten System innerhalb der Ebene zurückkippen */
static void upright_begin(CGContextRef c, CGRect b, BOOL flipped) {
    CGContextSaveGState(c);
    if (flipped) {
        CGContextTranslateCTM(c, 0, CGRectGetMinY(b) + CGRectGetMaxY(b));
        CGContextScaleCTM(c, 1, -1);
    }
}

static CGRect gravity_rect(NSString *g, CGRect b, CGSize s, BOOL flipped) {
    if (g == nil || [g isEqualToString: kCAGravityResize] || s.width <= 0 || s.height <= 0) return b;
    if ([g isEqualToString: kCAGravityResizeAspect] || [g isEqualToString: kCAGravityResizeAspectFill]) {
        CGFloat f = [g isEqualToString: kCAGravityResizeAspect] ? MIN(b.size.width / s.width, b.size.height / s.height)
                                                                 : MAX(b.size.width / s.width, b.size.height / s.height);
        s = CGSizeMake(s.width * f, s.height * f);
    }
    CGFloat x = CGRectGetMidX(b) - s.width / 2, y = CGRectGetMidY(b) - s.height / 2;
    /* oben/unten im aufrechten System: oben = MaxY */
    if ([g rangeOfString: @"left" options: NSCaseInsensitiveSearch].location != NSNotFound) x = CGRectGetMinX(b);
    if ([g rangeOfString: @"right" options: NSCaseInsensitiveSearch].location != NSNotFound) x = CGRectGetMaxX(b) - s.width;
    if ([g hasPrefix: @"top"]) y = CGRectGetMaxY(b) - s.height;
    if ([g hasPrefix: @"bottom"]) y = CGRectGetMinY(b);
    return CGRectMake(x, y, s.width, s.height);
}

static void paint_contents(CALayer *l, CGContextRef c, BOOL flipped) {
    id contents = [l contents];
    if (contents == nil) return;
    CGRect b = [l bounds];
    if ([contents isKindOfClass: [NSImage class]]) {
        /* über NSImage selbst zeichnen: auch Bilder ohne Bitmap (imageWithSize:flipped:drawingHandler:, PDF, Zeichenblock) */
        NSImage *ni = contents;
        upright_begin(c, b, flipped);
        CGRect r = gravity_rect(get(l, @"contentsGravity"), b, NSSizeToCGSize([ni size]), flipped);
        [ni drawInRect: NSRectFromCGRect(r) fromRect: NSZeroRect operation: NSCompositeSourceOver fraction: 1.0];
        CGContextRestoreGState(c);
        return;
    }
    CGImageRef img = NULL;
    if ([contents isKindOfClass: NSClassFromString(@"O2Image")]) img = (CGImageRef) contents;
    if (img == NULL) return;
    upright_begin(c, b, flipped);
    CGContextDrawImage(c, gravity_rect(get(l, @"contentsGravity"), b, CGSizeMake(CGImageGetWidth(img), CGImageGetHeight(img)), flipped), img);
    CGContextRestoreGState(c);
}

static void paint_text(CATextLayer *l, CGContextRef c, BOOL flipped) {
    id str = get(l, @"string");
    if (str == nil) return;
    NSAttributedString *as = nil;
    if ([str isKindOfClass: [NSAttributedString class]]) as = [[str retain] autorelease];
    else {
        id font = get(l, @"font");
        CGFloat size = g_f(l, @selector(fontSize));
        NSFont *f = nil;
        if ([font isKindOfClass: [NSFont class]]) f = [NSFont fontWithName: [font fontName] size: size];
        else if ([font isKindOfClass: [NSString class]]) f = [NSFont fontWithName: font size: size];
        if (f == nil) f = [NSFont systemFontOfSize: size > 0 ? size : 12];
        NSMutableDictionary *a = [NSMutableDictionary dictionaryWithObject: f forKey: NSFontAttributeName];
        CGColorRef fg = as_cgcolor(get(l, @"foregroundColor"));
        if (fg) a[@"CTForegroundColor"] = (id) fg;
        as = [[[NSAttributedString alloc] initWithString: [str description] attributes: a] autorelease];
    }
    CTLineRef line = CTLineCreateWithAttributedString((CFAttributedStringRef) as);
    if (line == NULL) return;
    CGFloat asc = 0, desc = 0;
    double w = CTLineGetTypographicBounds(line, &asc, &desc, NULL);
    CGRect b = [l bounds];
    NSString *al = get(l, @"alignmentMode");
    CGFloat x = CGRectGetMinX(b);
    if ([al isEqualToString: kCAAlignmentCenter]) x = CGRectGetMidX(b) - w / 2;
    else if ([al isEqualToString: kCAAlignmentRight]) x = CGRectGetMaxX(b) - w;
    upright_begin(c, b, flipped);
    CGContextSetTextMatrix(c, CGAffineTransformIdentity);
    CGContextSetTextPosition(c, x, CGRectGetMaxY(b) - asc);   /* CATextLayer: Text oben in der Ebene */
    CTLineDraw(line, c);
    CGContextRestoreGState(c);
    CFRelease(line);
}

static void paint_shape(CALayer *l, CGContextRef c) {
    id (*o)(id, SEL) = (id (*)(id, SEL)) objc_msgSend;
    CGFloat (*f)(id, SEL) = (CGFloat (*)(id, SEL)) objc_msgSend;
    CGPathRef path = (CGPathRef) o(l, sel_registerName("path"));
    if (path == NULL) return;
    CGColorRef fill = as_cgcolor(o(l, sel_registerName("fillColor"))), stroke = as_cgcolor(o(l, sel_registerName("strokeColor")));
    CGContextSaveGState(c);
    if (fill) {
        CGContextSetFillColorWithColor(c, fill);
        CGContextAddPath(c, path);
        if ([o(l, sel_registerName("fillRule")) isEqual: @"even-odd"]) CGContextEOFillPath(c); else CGContextFillPath(c);
    }
    if (stroke) {
        CGContextSetStrokeColorWithColor(c, stroke);
        CGContextSetLineWidth(c, f(l, sel_registerName("lineWidth")));
        CGContextAddPath(c, path);
        CGContextStrokePath(c);
    }
    CGContextRestoreGState(c);
}

static void paint_gradient(CALayer *l, CGContextRef c, BOOL flipped) {
    NSArray *colors = get(l, @"colors");
    if ([colors count] == 0) return;
    NSArray *locs = get(l, @"locations");
    CGFloat lc[[colors count] + 1];
    BOOL haveLocs = [locs count] == [colors count];
    for (NSUInteger i = 0; haveLocs && i < [locs count]; i++) lc[i] = [locs[i] doubleValue];
    NSMutableArray *cg = [NSMutableArray array];
    for (id col in colors) { CGColorRef cc = as_cgcolor(col); if (cc) [cg addObject: (id) cc]; }
    if ([cg count] != [colors count]) return;
    CGGradientRef g = CGGradientCreateWithColors(NULL, (CFArrayRef) cg, haveLocs ? lc : NULL);
    if (g == NULL) return;
    CGRect b = [l bounds];
    CGPoint sp = g_p(l, @selector(startPoint)), ep = g_p(l, @selector(endPoint));
    /* Einheitskoordinaten der Ebene; Apple: (0,0) unten links (aufrecht) */
    upright_begin(c, b, flipped);
    CGContextClipToRect(c, b);
    CGContextDrawLinearGradient(c, g, CGPointMake(b.origin.x + sp.x * b.size.width, b.origin.y + sp.y * b.size.height),
                                CGPointMake(b.origin.x + ep.x * b.size.width, b.origin.y + ep.y * b.size.height),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    CGContextRestoreGState(c);
    CGGradientRelease(g);
}

static BOOL overrides(Class cls, SEL sel, Class base) {
    IMP mine = class_getMethodImplementation(cls, sel), b = class_getMethodImplementation(base, sel);
    return mine && mine != b;
}

static void paint_own_content(CALayer *l, CGContextRef c, BOOL flipped) {
    if ([l contents] != nil) { paint_contents(l, c, flipped); return; }
    if ([l isKindOfClass: [CATextLayer class]]) { paint_text((CATextLayer *) l, c, flipped); return; }
    if ([l isKindOfClass: [CAGradientLayer class]]) { paint_gradient(l, c, flipped); return; }
    if ([l isKindOfClass: [CAShapeLayer class]]) { paint_shape(l, c); return; }
    id d = [l delegate];
    BOOL custom = overrides([l class], @selector(drawInContext:), [CALayer class]);
    if (custom || (d && ![d isKindOfClass: [NSView class]] && [d respondsToSelector: @selector(drawLayer:inContext:)])) {
        upright_begin(c, [l bounds], flipped);
        @try { [l drawInContext: c]; } @catch (id e) {}
        CGContextRestoreGState(c);
    }
}

static void paint_sublayers(CALayer *l, CGContextRef c, BOOL flipped, int depth);

static void paint_layer(CALayer *l, CGContextRef c, BOOL parentFlipped, int depth) {
    if (depth > 32 || g_b(l, @selector(hidden))) return;
    if ([[l delegate] isKindOfClass: [NSView class]]) return;   /* Ebene einer anderen Ansicht: zeichnet die Ansicht selbst */
    CGFloat op = [l opacity];
    if (op <= 0.001f) return;
    CGRect b = [l bounds];
    CGPoint pos = [l position], ap = [l anchorPoint];
    CGContextSaveGState(c);
    CGContextTranslateCTM(c, pos.x, pos.y);
    CATransform3D t = ((CATransform3D (*)(id, SEL)) objc_msgSend)(l, @selector(transform));
    if (!(t.m11 == 1 && t.m12 == 0 && t.m21 == 0 && t.m22 == 1 && t.m41 == 0 && t.m42 == 0))
        CGContextConcatCTM(c, CGAffineTransformMake(t.m11, t.m12, t.m21, t.m22, t.m41, t.m42));
    CGContextTranslateCTM(c, -ap.x * b.size.width - b.origin.x, -ap.y * b.size.height - b.origin.y);
    if (op < 0.999f) CGContextSetAlpha(c, op);
    if (g_b(l, @selector(masksToBounds))) {
        CGPathRef p = rounded(b, g_f(l, @selector(cornerRadius)));
        CGContextAddPath(c, p);
        CGContextClip(c);
        CGPathRelease(p);
    }
    paint_background(l, c);
    paint_own_content(l, c, parentFlipped);
    paint_sublayers(l, c, parentFlipped, depth);
    paint_border(l, c);
    CGContextRestoreGState(c);
}

static void paint_sublayers(CALayer *l, CGContextRef c, BOOL flipped, int depth) {
    NSArray *subs = [l sublayers];
    if ([subs count] == 0) return;
    CGContextSaveGState(c);
    if (g_b(l, @selector(geometryFlipped))) {
        CGRect b = [l bounds];
        CGContextTranslateCTM(c, 0, CGRectGetMinY(b) + CGRectGetMaxY(b));
        CGContextScaleCTM(c, 1, -1);
        flipped = !flipped;
    }
    /* nach zPosition, sonst in Reihenfolge */
    NSArray *sorted = [subs sortedArrayWithOptions: NSSortStable usingComparator: ^NSComparisonResult(id a, id b2) {
        CGFloat za = g_f(a, @selector(zPosition)), zb = g_f(b2, @selector(zPosition));
        return za < zb ? NSOrderedAscending : za > zb ? NSOrderedDescending : NSOrderedSame;
    }];
    for (CALayer *s in sorted) paint_layer(s, c, flipped, depth + 1);
    CGContextRestoreGState(c);
}

/* --- NSView ----------------------------------------------------------------------------------------------------------------------- */

static BOOL v_wants_update(id s, SEL c) { return NO; }

/* Zeichnet eine Ansicht mit Ebene: Hintergrund, updateLayer-Inhalt oder drawRect:, Unterebenen, Rand. Kontext und CTM sind die der
 * Ansicht (lockFocus); die Geometrie der Ebene ist die der Ansicht (bounds), gekippt wie die Ansicht. */
static void v_draw_layer_backed(NSView *self, SEL cmd, NSRect rect) {
    CALayer *l = [self layer];
    CGContextRef c = [[NSGraphicsContext currentContext] graphicsPort];
    if (l == nil || c == NULL) { [self drawRect: rect]; return; }
    g_painting++;
    BOOL upd = [self wantsUpdateLayer] && overrides([self class], @selector(updateLayer), [NSView class]);
    if (upd) {
        @try { [self updateLayer]; } @catch (id e) {}
    }
    /* Die Ebene der Ansicht deckt deren bounds (wie bei Apple nachgeführt) */
    CGRect vb = NSRectToCGRect([self bounds]);
    if (!CGRectEqualToRect([l bounds], vb)) [l setBounds: vb];
    CGContextSaveGState(c);
    BOOL flipped = [self isFlipped];
    if (!g_b(l, @selector(hidden))) {
        paint_background(l, c);
        if (upd) paint_contents(l, c, flipped);
    }
    CGContextRestoreGState(c);
    /* updateLayer, das weder Inhalt noch Hintergrund setzt (oder nur über Wege, die es hier nicht gibt): wie vorher drawRect: */
    if (upd && [l contents] == nil && as_cgcolor(get(l, @"backgroundColor")) == NULL && [[l sublayers] count] == 0)
        upd = NO;
    if (!upd) {
        @try { [self drawRect: rect]; }
        @catch (id e) { g_painting--; @throw; }
    }
    if (!g_b(l, @selector(hidden))) {
        CGContextSaveGState(c);
        paint_sublayers(l, c, flipped, 0);
        paint_border(l, c);
        CGContextRestoreGState(c);
    }
    g_painting--;
}

static IMP o_addsub, o_remove, o_setneeds, o_setcontents;
static void l_addsub(CALayer *s, SEL c, CALayer *sub) { ((void (*)(id, SEL, id)) o_addsub)(s, c, sub); touched(s); }
static void l_remove(CALayer *s, SEL c) { CALayer *sup = [s superlayer]; ((void (*)(id, SEL)) o_remove)(s, c); if (sup) touched(sup); }
static void l_setneeds(CALayer *s, SEL c) { ((void (*)(id, SEL)) o_setneeds)(s, c); touched(s); }
static void l_setcontents(CALayer *s, SEL c, id v) { ((void (*)(id, SEL, id)) o_setcontents)(s, c, v); touched(s); }

/* CALayer: insertSublayer:atIndex:, affineTransform/setAffineTransform: (über transform), isOpaque (Cocotron nennt den Getter opaque) */
static void l_insert(CALayer *self, SEL c, CALayer *sub, unsigned idx) {
    if (sub == nil) return;
    NSMutableArray *a = [NSMutableArray arrayWithArray: [self sublayers] ?: @[]];
    [a removeObjectIdenticalTo: sub];
    if (idx > [a count]) idx = (unsigned) [a count];
    [sub removeFromSuperlayer];
    [a insertObject: sub atIndex: idx];
    [self setSublayers: a];
    touched(self);
}
static CGAffineTransform l_affine(CALayer *self, SEL c) {
    CATransform3D t = ((CATransform3D (*)(id, SEL)) objc_msgSend)(self, @selector(transform));
    return CGAffineTransformMake(t.m11, t.m12, t.m21, t.m22, t.m41, t.m42);
}
static void l_set_affine(CALayer *self, SEL c, CGAffineTransform a) {
    CATransform3D t = CATransform3DIdentity;
    t.m11 = a.a; t.m12 = a.b; t.m21 = a.c; t.m22 = a.d; t.m41 = a.tx; t.m42 = a.ty;
    ((void (*)(id, SEL, CATransform3D)) objc_msgSend)(self, sel_registerName("setTransform:"), t);
    touched(self);
}
static BOOL l_is_opaque(CALayer *self, SEL c) { return [self opaque]; }

__attribute__((constructor)) static void clk_softlayers_init(void) {
    Class L = objc_getClass("CALayer");
    if (L == Nil) return;
    g_defaults = [@{ @"opacity": @1.0, @"contentsGravity": kCAGravityResize } retain];
    struct { const char *name; char type; } p[] = {
        {"backgroundColor", 'o'}, {"borderColor", 'o'}, {"borderWidth", 'f'}, {"cornerRadius", 'f'}, {"masksToBounds", 'b'},
        {"hidden", 'b'}, {"zPosition", 'f'}, {"contentsGravity", 'o'}, {"geometryFlipped", 'b'}, {"name", 'o'}, {"mask", 'o'},
        {"maskedCorners", 'u'}, {"cornerCurve", 'o'}, {"shadowColor", 'o'}, {"shadowOpacity", 'f'}, {"shadowRadius", 'f'},
        {"shadowPath", 'o'}, {"filters", 'o'}, {"backgroundFilters", 'o'}, {"compositingFilter", 'o'}, {"actions", 'o'},
        {"style", 'o'}, {"contentsRect", 'r'}, {"contentsCenter", 'r'}, {"needsDisplayOnBoundsChange", 'b'},
        {"allowsEdgeAntialiasing", 'b'}, {"edgeAntialiasingMask", 'u'}, {"drawsAsynchronously", 'b'}, {"doubleSided", 'b'},
        {"rasterizationScale", 'f'}, {"shouldRasterize", 'b'}, {"contentsFormat", 'o'}, {"layoutManager", 'o'},
        {"autoresizingMask", 'u'},
    };
    for (size_t i = 0; i < sizeof p / sizeof *p; i++) add_prop(L, p[i].name, p[i].type, YES);
    Class T = objc_getClass("CATextLayer");
    struct { const char *name; char type; } tp[] = {
        {"string", 'o'}, {"font", 'o'}, {"fontSize", 'f'}, {"foregroundColor", 'o'}, {"alignmentMode", 'o'}, {"truncationMode", 'o'},
        {"wrapped", 'b'}, {"allowsFontSubpixelQuantization", 'b'},
    };
    for (size_t i = 0; T && i < sizeof tp / sizeof *tp; i++) add_prop(T, tp[i].name, tp[i].type, YES);
    Class G = objc_getClass("CAGradientLayer");
    struct { const char *name; char type; } gp[] = { {"colors", 'o'}, {"locations", 'o'}, {"startPoint", 'p'}, {"endPoint", 'p'}, {"type", 'o'} };
    for (size_t i = 0; G && i < sizeof gp / sizeof *gp; i++) add_prop(G, gp[i].name, gp[i].type, YES);
    o_addsub = method_setImplementation(class_getInstanceMethod(L, @selector(addSublayer:)), (IMP) l_addsub);
    o_remove = method_setImplementation(class_getInstanceMethod(L, @selector(removeFromSuperlayer)), (IMP) l_remove);
    o_setneeds = method_setImplementation(class_getInstanceMethod(L, @selector(setNeedsDisplay)), (IMP) l_setneeds);
    o_setcontents = method_setImplementation(class_getInstanceMethod(L, @selector(setContents:)), (IMP) l_setcontents);
    class_addMethod(L, sel_registerName("insertSublayer:atIndex:"), (IMP) l_insert, "v@:@I");
    class_addMethod(L, sel_registerName("affineTransform"), (IMP) l_affine, "{CGAffineTransform=dddddd}@:");
    class_addMethod(L, sel_registerName("setAffineTransform:"), (IMP) l_set_affine, "v@:{CGAffineTransform=dddddd}");
    class_addMethod(L, sel_registerName("isOpaque"), (IMP) l_is_opaque, "c@:");
    class_addMethod([NSView class], @selector(wantsUpdateLayer), (IMP) v_wants_update, "c@:");
    class_addMethod([NSView class], sel_registerName("_clkDrawLayerBackedRect:"), (IMP) v_draw_layer_backed,
                    "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
}
