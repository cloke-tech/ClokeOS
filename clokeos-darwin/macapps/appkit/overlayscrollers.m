/* Rollbalken wie bei macOS ("Rollbalken einblenden: beim Scrollen", NSScrollerStyleOverlay) für Cocotrons AppKit.
 *
 * Cocotron zeichnet klassische Rollbalken mit Pfeilen und Schiene neben dem Inhalt, immer sichtbar (Fundus, Logic Pro).
 * macOS zeigt sie über dem Inhalt, unsichtbar, bis gescrollt wird: dann ein schmaler, abgerundeter Griff ohne Schiene, der
 * nach etwa einer Sekunde ausblendet. Beim Ziehen des Griffs ist er breiter und hat eine helle Schiene.
 *
 * Umsetzung (alles über die Laufzeit, Darlings Quellen bleiben unverändert):
 *  - NSScrollView: Clip-Ansicht und Tabellenkopf gehen über die volle Breite/Höhe (clipViewFrame/headerClipViewFrame bekommen
 *    die Breite des Rollbalkens zurück), keine Eckansicht; nach -tile liegen die Rollbalken über der Clip-Ansicht.
 *  - Sichtbar wird ein Rollbalken, wenn sich der Rollpunkt ändert (-reflectScrolledClipView:) oder der Griff gezogen wird;
 *    nach 1 s blendet er in 0,3 s aus. Unsichtbar nimmt er keine Klicks an (hitTest: nil), der Inhalt darunter bekommt sie.
 *  - +[NSScroller preferredScrollerStyle] und -[NSScrollView scrollerStyle] melden NSScrollerStyleOverlay.
 * Klassisch wie bisher: AppleShowScrollBars = Always (NSUserDefaults/-AppleShowScrollBars Always) oder CLOKE_SCROLLBARS=always. */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static BOOL overlay_on;
static char k_until, k_tracking, k_lastpt;
static IMP o_clip_frame, o_header_frame, o_corner_frame, o_tile, o_reflect, o_scroller_draw, o_scroller_hit, o_scroller_down, o_scroller_opaque;

static BOOL dark_mode(void) {
    NSAppearance *a = [NSApp respondsToSelector: @selector(effectiveAppearance)] ? [NSApp effectiveAppearance] : nil;
    return a && [[a name] rangeOfString: @"Dark"].location != NSNotFound;
}

static NSTimeInterval now(void) { return [NSDate timeIntervalSinceReferenceDate]; }

static CGFloat scroller_alpha(NSScroller *s) {
    if ([objc_getAssociatedObject(s, &k_tracking) boolValue]) return 1;
    NSTimeInterval until = [objc_getAssociatedObject(s, &k_until) doubleValue], t = now();
    if (t <= until) return 1;
    CGFloat a = 1 - (t - until) / 0.3;
    return a > 0 ? a : 0;
}

@interface NSScroller (ClokeOverlay)
- (void)clk_fadeTick;
- (void)clk_flash;
@end
@implementation NSScroller (ClokeOverlay)
- (void)clk_fadeTick {
    [self setNeedsDisplay: YES];
    if (scroller_alpha(self) > 0)
        [self performSelector: @selector(clk_fadeTick) withObject: nil afterDelay: 0.05
                      inModes: [NSArray arrayWithObjects: NSDefaultRunLoopMode, NSEventTrackingRunLoopMode, NSModalPanelRunLoopMode, nil]];
}
- (void)clk_flash {
    if (getenv("CLOKE_SCROLL_TRACE")) NSLog(@"[clokeos-overlay] flash %@ frame=%@ super=%s idx=%lu", self, NSStringFromRect([self frame]),
                                            object_getClassName([self superview]), (unsigned long)[[[self superview] subviews] indexOfObjectIdenticalTo: self]);
    BOOL was = scroller_alpha(self) > 0;
    objc_setAssociatedObject(self, &k_until, [NSNumber numberWithDouble: now() + 1.0], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [self setNeedsDisplay: YES];
    if (!was) [self clk_fadeTick];
}
@end

/* ------------------------------------------------------------------ NSScroller */

static BOOL sc_opaque(id self, SEL _cmd) { return NO; }

static void sc_draw(NSScroller *self, SEL _cmd, NSRect dirty) {
    CGFloat a = scroller_alpha(self);
    if (a <= 0) return;
    NSRect b = [self bounds], knob = [self rectForPart: NSScrollerKnob];
    if (NSIsEmptyRect(knob)) return;
    BOOL vertical = [self isVertical], wide = [objc_getAssociatedObject(self, &k_tracking) boolValue], dk = dark_mode();
    if (wide) {   /* beim Ziehen: helle Schiene wie bei macOS */
        [[NSColor colorWithCalibratedWhite: dk ? 1 : 0 alpha: 0.06 * a] setFill];
        NSRectFillUsingOperation(b, NSCompositeSourceOver);
    }
    CGFloat thick = wide ? 9 : 7, edge = 2;   /* Abstand zum Rand */
    NSRect r = vertical ? NSMakeRect(NSMaxX(b) - edge - thick, NSMinY(knob) + 2, thick, NSHeight(knob) - 4)
                        : NSMakeRect(NSMinX(knob) + 2, NSMaxY(b) - edge - thick, NSWidth(knob) - 4, thick);
    if (NSWidth(r) < thick || NSHeight(r) < thick) return;
    [[NSColor colorWithCalibratedWhite: dk ? 1 : 0 alpha: (dk ? 0.45 : 0.42) * a] setFill];
    [[NSBezierPath bezierPathWithRoundedRect: r xRadius: thick / 2 yRadius: thick / 2] fill];
}

static NSView *sc_hit(NSScroller *self, SEL _cmd, NSPoint p) {
    if (scroller_alpha(self) <= 0) return nil;   /* unsichtbar: Klick geht an den Inhalt */
    return ((NSView * (*)(id, SEL, NSPoint)) o_scroller_hit)(self, _cmd, p);
}

static void sc_down(NSScroller *self, SEL _cmd, NSEvent *e) {
    objc_setAssociatedObject(self, &k_tracking, [NSNumber numberWithBool: YES], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [self setNeedsDisplay: YES];
    ((void (*)(id, SEL, NSEvent *)) o_scroller_down)(self, _cmd, e);   /* Cocotron verfolgt die Maus bis zum Loslassen */
    objc_setAssociatedObject(self, &k_tracking, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [self clk_flash];
}

/* Cocotron legt die Richtung einmal in -initWithFrame: fest. NSScrollView erzeugt seine Rollbalken mit einem Rahmen
 * von 15 x 0 Punkten (vor dem ersten Anordnen), der senkrechte Balken hielt sich dann für waagerecht: kein Griff, das
 * Mausrad scrollte nicht. Gilt immer (auch mit klassischen Rollbalken): Richtung aus der aktuellen Größe. */
static IMP o_is_vertical;
static BOOL sc_is_vertical(NSScroller *self, SEL _cmd) {
    NSSize s = [self bounds].size;
    if (s.width > 0 && s.height > 0 && s.width != s.height) return s.height > s.width;
    return ((BOOL (*)(id, SEL)) o_is_vertical)(self, _cmd);
}

/* Mausrad über dem Rollbalken: scrollt den Inhalt wie über dem Inhalt (Cocotron verschob nur den Griff um Pixel der Schiene) */
static void sc_wheel(NSScroller *self, SEL _cmd, NSEvent *e) {
    NSScrollView *sv = (NSScrollView *)[self superview];
    if ([sv isKindOfClass: [NSScrollView class]] && [sv documentView]) [[sv documentView] scrollWheel: e];
}

static NSScrollerStyle preferred_style(id self, SEL _cmd) { return NSScrollerStyleOverlay; }

/* ------------------------------------------------------------------ NSScrollView */

static BOOL shows(NSScrollView *sv, BOOL vertical) {
    NSScroller *s = vertical ? [sv verticalScroller] : [sv horizontalScroller];
    return (vertical ? [sv hasVerticalScroller] : [sv hasHorizontalScroller]) && s && ![s isHidden];
}

static NSRect sv_clip_frame(NSScrollView *self, SEL _cmd) {
    NSRect r = ((NSRect (*)(id, SEL)) o_clip_frame)(self, _cmd);
    CGFloat w = [NSScroller scrollerWidth];
    if (shows(self, YES)) r.size.width += w;
    if (shows(self, NO)) r.size.height += w;
    return r;
}

/* Apple: bei Overlay-Rollbalken ist contentSize die ganze Fläche (Cocotron zieht die Rollbalken ab) */
static NSSize sv_content_size(NSScrollView *self, SEL _cmd) { return sv_clip_frame(self, sel_registerName("clipViewFrame")).size; }

static NSRect sv_header_frame(NSScrollView *self, SEL _cmd) {
    NSRect r = ((NSRect (*)(id, SEL)) o_header_frame)(self, _cmd);
    if (!NSIsEmptyRect(r)) r.size.width += [NSScroller scrollerWidth];
    return r;
}

static NSRect sv_corner_frame(NSScrollView *self, SEL _cmd) { return NSZeroRect; }

static void sv_tile(NSScrollView *self, SEL _cmd) {
    ((void (*)(id, SEL)) o_tile)(self, _cmd);
    /* Apple: Zeile = 10 Punkte (Cocotron 1, ein Mausrad-Schritt scrollte nur 3 Punkte); Programme, die selbst setzen, behalten ihren Wert */
    if ([self verticalLineScroll] == 1.0) [self setVerticalLineScroll: 10];
    if ([self horizontalLineScroll] == 1.0) [self setHorizontalLineScroll: 10];
    NSClipView *clip = [self contentView];
    NSScroller *v = [self verticalScroller], *h = [self horizontalScroller];
    NSScroller *both[2] = { v, h };
    for (int i = 0; i < 2; i++) {
        NSScroller *s = both[i];
        if (!s || [s superview] != self) continue;
        if ([s arrowsPosition] != NSScrollerArrowsNone) [s setArrowsPosition: NSScrollerArrowsNone];
        NSArray *sub = [self subviews];
        if ([sub indexOfObjectIdenticalTo: s] > [sub indexOfObjectIdenticalTo: clip] || [sub indexOfObjectIdenticalTo: clip] == NSNotFound) continue;
        [s retain];   /* über die Clip-Ansicht legen */
        [s removeFromSuperview];
        [self addSubview: s];
        [s release];
    }
}

static void sv_reflect(NSScrollView *self, SEL _cmd, NSClipView *clip) {
    ((void (*)(id, SEL, id)) o_reflect)(self, _cmd, clip);
    if (clip != [self contentView]) return;
    NSPoint p = [clip bounds].origin;
    NSValue *old = objc_getAssociatedObject(self, &k_lastpt);
    objc_setAssociatedObject(self, &k_lastpt, [NSValue valueWithPoint: p], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (!old) return;   /* erstes Anordnen: nichts zeigen */
    NSPoint q = [old pointValue];
    if (p.y != q.y && [self hasVerticalScroller]) [[self verticalScroller] clk_flash];
    if (p.x != q.x && [self hasHorizontalScroller]) [[self horizontalScroller] clk_flash];
}

static NSScrollerStyle sv_style(id self, SEL _cmd) { return NSScrollerStyleOverlay; }

static IMP swap(Class c, SEL s, IMP n) {
    Method m = class_getInstanceMethod(c, s);
    if (!m) return NULL;
    if (!class_addMethod(c, s, n, method_getTypeEncoding(m))) return method_setImplementation(m, n);
    return method_getImplementation(m);
}

static void clk_overlay_scrollers_install(void) {
    o_is_vertical = swap([NSScroller class], @selector(isVertical), (IMP)sc_is_vertical);
    swap([NSScroller class], @selector(scrollWheel:), (IMP)sc_wheel);
    const char *env = getenv("CLOKE_SCROLLBARS");
    if (env && !strcasecmp(env, "always")) return;
    NSString *pref = [[NSUserDefaults standardUserDefaults] stringForKey: @"AppleShowScrollBars"];
    if ([pref isEqualToString: @"Always"]) return;
    overlay_on = YES;
    Class SV = [NSScrollView class], SC = [NSScroller class];
    o_clip_frame = swap(SV, sel_registerName("clipViewFrame"), (IMP)sv_clip_frame);
    o_header_frame = swap(SV, sel_registerName("headerClipViewFrame"), (IMP)sv_header_frame);
    o_corner_frame = swap(SV, sel_registerName("cornerViewFrame"), (IMP)sv_corner_frame);
    o_tile = swap(SV, @selector(tile), (IMP)sv_tile);
    swap(SV, @selector(contentSize), (IMP)sv_content_size);
    o_reflect = swap(SV, @selector(reflectScrolledClipView:), (IMP)sv_reflect);
    class_replaceMethod(SV, @selector(scrollerStyle), (IMP)sv_style, "q@:");
    o_scroller_draw = swap(SC, @selector(drawRect:), (IMP)sc_draw);
    o_scroller_hit = swap(SC, @selector(hitTest:), (IMP)sc_hit);
    o_scroller_down = swap(SC, @selector(mouseDown:), (IMP)sc_down);
    o_scroller_opaque = swap(SC, @selector(isOpaque), (IMP)sc_opaque);
    class_replaceMethod(object_getClass(SC), @selector(preferredScrollerStyle), (IMP)preferred_style, "q@:");
}

__attribute__((constructor)) static void clk_overlay_scrollers_init(void) {
    /* Im AppKit-Bau lädt das X11-Backend spät, Foundation steht. Als eingeschobene Bibliothek (Tests mit DYLD_INSERT_LIBRARIES)
     * liefe das hier vor Foundations Start ([NSScroller class] -> +[NSView initialize] -> NSUserDefaults stürzte ab): dann im
     * ersten Durchlauf des Haupt-Runloops, vor dem ersten Zeichnen. */
    if (objc_getClass("NSApplication") && NSApp) clk_overlay_scrollers_install();
    else CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ clk_overlay_scrollers_install(); });
}
