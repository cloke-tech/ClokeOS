/* Auto Layout für Cocotrons AppKit: Bedingungen an Ansichten, intrinsische Größen, Lösen und Anwenden (docs/logic-launch.md, Runde 61).
 *
 * Cocotron kannte nur Autoresizing; NSLayoutConstraint war ein Platzhalter (Daten jetzt in macapps/fndlayout.m). Hier:
 *   - NSView: constraints, addConstraint(s):, removeConstraint(s):, Anker (leadingAnchor …), Layout-Hilfslinien, intrinsicContentSize,
 *     invalidateIntrinsicContentSize, alignmentRectInsets/-ForFrame:/frameForAlignmentRect:, Grundlinien, fittingSize,
 *     updateConstraints & Co., Prioritäten für Hugging/Kompression mit Apples Vorgaben (Steuerelemente 250/750, NSGridView 249,
 *     sonst 250/250; Kompression 750, NSGridView 1000).
 *   - Nibs: NSViewConstraints, NSDoNotTranslateAutoresizingMask, NSHuggingPriority, NSAntiCompressionPriority (in -initWithCoder:,
 *     bei NSCustomView über CLKCustomViewDecoded).
 *   - Intrinsische Größen wie macOS 27 (auf dem Mac gemessen, Textbreite aus Cocotrons Schrift): Druckknopf Text + 24 × 24,
 *     Ankreuzfeld/Optionsfeld Text + 22 × 16, Beschriftung Text × Zeilenhöhe mit 2 pt Ausrichtungsrand links/rechts,
 *     Eingabefeld keine Breite × 24, Aufklappmenü breitester Eintrag + 47,5 × 24.
 *   - Standardabstand (NSSpace in Nibs, "-" in der Formatsprache) wie macOS: 20 zum Rand der Oberansicht, sonst 8; waagrecht
 *     zwischen zwei Druckknöpfen 12; senkrecht Ankreuzfeld/Ankreuzfeld 6, Eingabefeld/Eingabefeld und Menü/Menü 10, über einem
 *     Aufklappmenü 20 (außer unter einer Beschriftung: 8).
 *
 * Der Löser (alsolver.c, Cassowary mit Prioritäten wie AppKit) läuft je Fenster: alle Bedingungen im Fenster, ihre Ansichten und deren
 * Vorfahren bis zur Inhaltsansicht. Unbekannte je Ansicht: Rahmen in Fensterkoordinaten mit y nach unten (links, oben, Breite, Höhe).
 * Ansichten mit translatesAutoresizingMaskIntoConstraints = YES werden über ihre Autoresizing-Maske an die Oberansicht gebunden
 * (Pflicht), wie AppKit es mit NSAutoresizingMaskLayoutConstraint tut. Die Inhaltsansicht möchte ihre Größe behalten (Priorität 500,
 * NSLayoutPriorityWindowSizeStayPut); Pflichtbedingungen des Inhalts vergrößern das Fenster dabei, die obere Kante bleibt stehen.
 * fittingSize löst den Teilbaum allein mit Größe 0 bei Priorität 50 (NSLayoutPriorityFittingSizeCompression).
 * Gelöst wird nur, wenn sich etwas geändert hat (Bedingungen, Inhalte, Rahmen beteiligter Ansichten, Ansichtsbaum); angestoßen vom
 * Layout-Durchgang (layout.m) und vor dem ersten Zeigen eines Fensters. Ergebnisse werden auf ganze Punkte gerundet.
 *
 * Spuren: CLOKE_AL_TRACE=1 (je Lösung Anzahl Bedingungen/Ansichten, gebrochene Pflichtbedingungen, Fenstergröße),
 *         CLOKE_AL_TRACE=2 zusätzlich jeder gesetzte Rahmen, =3 auch intrinsische Größen und Grundlinien,
 *         =4 auch alle Knoten und Bedingungen. */
#import <AppKit/AppKit.h>
#import <AppKit/NSLayoutGuide.h>
#import <Foundation/NSLayoutConstraint.h>
#import <Foundation/NSLayoutAnchor.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <math.h>
#include "alsolver.h"

enum {
    LA_NotAnAttribute = 0, LA_Left, LA_Right, LA_Top, LA_Bottom, LA_Leading, LA_Trailing, LA_Width, LA_Height, LA_CenterX,
    LA_CenterY, LA_LastBaseline, LA_FirstBaseline
};

@interface NSLayoutConstraint (CLKAppKit)
+ (instancetype) constraintWithItem: (id) v1 attribute: (NSInteger) a1 relatedBy: (NSInteger) rel toItem: (id) v2
                          attribute: (NSInteger) a2 multiplier: (double) m constant: (double) c;
- (id) firstItem;
- (id) secondItem;
- (NSInteger) firstAttribute;
- (NSInteger) secondAttribute;
- (NSInteger) relation;
- (double) multiplier;
- (double) constant;
- (float) priority;
- (void) setPriority: (float) p;
- (BOOL) isActive;
- (BOOL) _clkStandardSpacing;
- (double) _clkSpacingFactor;
- (id) _clkContainer;
- (void) _clkSetContainer: (id) v;
@end
@interface NSView (CLKAutoLayoutDecl)
- (float) contentHuggingPriorityForOrientation: (NSInteger) o;
- (float) contentCompressionResistancePriorityForOrientation: (NSInteger) o;
- (void) setContentHuggingPriority: (float) p forOrientation: (NSInteger) o;
- (void) setContentCompressionResistancePriority: (float) p forOrientation: (NSInteger) o;
- (void) setNeedsLayout: (BOOL) f;
- (void) layoutSubtreeIfNeeded;
@end
@interface NSLayoutAnchor (CLKAppKit)
+ (id) _clkAnchorWithItem: (id) item attribute: (NSInteger) a;
@end

static int g_trace = -1;
static int trace(void) {
    if (g_trace < 0) { const char *e = getenv("CLOKE_AL_TRACE"); g_trace = e ? atoi(e) : 0; }
    return g_trace;
}

/* ---- Zustand je Ansicht und je Fenster ---- */
@interface CLKViewAL : NSObject {
@public
    NSMutableArray *constraints, *guides;
    BOOL updated, needsUpdate, member;
    float hug[2], comp[2];       /* 0 = Vorgabe der Klasse */
    CGFloat pmlw;                /* preferredMaxLayoutWidth */
}
@end
@implementation CLKViewAL
- (void) dealloc { [constraints release]; [guides release]; [super dealloc]; }
@end

@interface CLKWindowAL : NSObject {
@public
    BOOL dirty, solved, hasConstraints;
}
@end
@implementation CLKWindowAL
@end

static char k_view, k_win;
static BOOL g_applying;   /* Rahmen werden gerade aus der Lösung gesetzt */

static CLKViewAL *vstate(NSView *v, BOOL make) {
    CLKViewAL *s = objc_getAssociatedObject(v, &k_view);
    if (!s && make) {
        s = [[CLKViewAL new] autorelease];
        s->needsUpdate = YES;
        objc_setAssociatedObject(v, &k_view, s, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return s;
}
static CLKWindowAL *wstate(NSWindow *w, BOOL make) {
    if (!w) return nil;
    CLKWindowAL *s = objc_getAssociatedObject(w, &k_win);
    if (!s && make) {
        s = [[CLKWindowAL new] autorelease];
        s->dirty = YES;
        objc_setAssociatedObject(w, &k_win, s, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return s;
}

/* Fenster (oder losgelöster Baum) muss neu gelöst werden */
static void mark_dirty(NSView *v) {
    NSWindow *w = [v window];
    if (!w) return;
    CLKWindowAL *ws = wstate(w, YES);
    BOOL was = ws->dirty;
    ws->dirty = YES;
    if (!was && ws->hasConstraints) {
        NSView *cv = [w contentView];
        if ([cv respondsToSelector: @selector(setNeedsLayout:)]) [cv setNeedsLayout: YES];
        else [w setViewsNeedDisplay: YES];
    }
}

/* ---- Bedingungen an der Ansicht ---- */
@implementation NSView (CLKAutoLayout)

- (NSArray *) constraints {
    CLKViewAL *s = vstate(self, NO);
    return s && s->constraints ? [[s->constraints copy] autorelease] : [NSArray array];
}

- (void) addConstraint: (NSLayoutConstraint *) c {
    if (!c) return;
    id old = [c _clkContainer];
    if (old == self) return;
    if (old) [old removeConstraint: c];
    CLKViewAL *s = vstate(self, YES);
    if (!s->constraints) s->constraints = [NSMutableArray new];
    [s->constraints addObject: c];
    [c _clkSetContainer: self];
    NSWindow *w = [self window];
    if (w) wstate(w, YES)->hasConstraints = YES;
    mark_dirty(self);
}
- (void) addConstraints: (NSArray *) cs { for (NSLayoutConstraint *c in cs) [self addConstraint: c]; }
- (void) removeConstraint: (NSLayoutConstraint *) c {
    CLKViewAL *s = vstate(self, NO);
    if (!s || ![s->constraints containsObject: c]) return;
    [c retain];
    [s->constraints removeObjectIdenticalTo: c];
    [c _clkSetContainer: nil];
    [c release];
    mark_dirty(self);
}
- (void) removeConstraints: (NSArray *) cs { for (NSLayoutConstraint *c in [[cs copy] autorelease]) [self removeConstraint: c]; }
- (void) _clkConstraintChanged: (NSLayoutConstraint *) c { mark_dirty(self); }

- (NSArray *) constraintsAffectingLayoutForOrientation: (NSInteger) o { return [NSArray array]; }
- (BOOL) hasAmbiguousLayout { return NO; }
- (void) exerciseAmbiguityInLayout {}
- (NSInteger) userInterfaceLayoutDirection { return 0; }

/* Anker */
- (id) _clkAnchorForAttribute: (NSInteger) a { return [NSLayoutAnchor _clkAnchorWithItem: self attribute: a]; }
- (id) leadingAnchor { return [self _clkAnchorForAttribute: LA_Leading]; }
- (id) trailingAnchor { return [self _clkAnchorForAttribute: LA_Trailing]; }
- (id) leftAnchor { return [self _clkAnchorForAttribute: LA_Left]; }
- (id) rightAnchor { return [self _clkAnchorForAttribute: LA_Right]; }
- (id) topAnchor { return [self _clkAnchorForAttribute: LA_Top]; }
- (id) bottomAnchor { return [self _clkAnchorForAttribute: LA_Bottom]; }
- (id) widthAnchor { return [self _clkAnchorForAttribute: LA_Width]; }
- (id) heightAnchor { return [self _clkAnchorForAttribute: LA_Height]; }
- (id) centerXAnchor { return [self _clkAnchorForAttribute: LA_CenterX]; }
- (id) centerYAnchor { return [self _clkAnchorForAttribute: LA_CenterY]; }
- (id) firstBaselineAnchor { return [self _clkAnchorForAttribute: LA_FirstBaseline]; }
- (id) lastBaselineAnchor { return [self _clkAnchorForAttribute: LA_LastBaseline]; }

/* Layout-Hilfslinien */
- (NSArray *) layoutGuides {
    CLKViewAL *s = vstate(self, NO);
    return s && s->guides ? [[s->guides copy] autorelease] : [NSArray array];
}
- (void) addLayoutGuide: (NSLayoutGuide *) g {
    CLKViewAL *s = vstate(self, YES);
    if (!s->guides) s->guides = [NSMutableArray new];
    if ([s->guides containsObject: g]) return;
    [s->guides addObject: g];
    [g setOwningView: self];
    mark_dirty(self);
}
- (void) removeLayoutGuide: (NSLayoutGuide *) g {
    CLKViewAL *s = vstate(self, NO);
    if (!s || ![s->guides containsObject: g]) return;
    [g setOwningView: nil];
    [s->guides removeObjectIdenticalTo: g];
    mark_dirty(self);
}

/* Aktualisieren der Bedingungen (vor dem Lösen, von unten nach oben) */
- (BOOL) needsUpdateConstraints { CLKViewAL *s = vstate(self, NO); return !s || s->needsUpdate; }
- (void) setNeedsUpdateConstraints: (BOOL) f {
    CLKViewAL *s = vstate(self, YES);
    s->needsUpdate = f;
    if (f) mark_dirty(self);
}
- (void) updateConstraints {}
- (void) updateConstraintsForSubtreeIfNeeded {
    for (NSView *v in [[[self subviews] copy] autorelease]) [v updateConstraintsForSubtreeIfNeeded];
    CLKViewAL *s = vstate(self, YES);
    if (s->needsUpdate) {
        s->needsUpdate = NO;
        s->updated = YES;
        [self updateConstraints];
    }
}
+ (BOOL) requiresConstraintBasedLayout { return NO; }

/* Ausrichtungsrechteck */
- (NSEdgeInsets) alignmentRectInsets { return NSEdgeInsetsMake(0, 0, 0, 0); }
- (NSRect) alignmentRectForFrame: (NSRect) f {
    NSEdgeInsets in = [self alignmentRectInsets];
    /* y: Rahmen in Koordinaten der Oberansicht; oben/unten wie sichtbar */
    BOOL flip = [[self superview] isFlipped];
    return NSMakeRect(f.origin.x + in.left, f.origin.y + (flip ? in.top : in.bottom), f.size.width - in.left - in.right,
                      f.size.height - in.top - in.bottom);
}
- (NSRect) frameForAlignmentRect: (NSRect) a {
    NSEdgeInsets in = [self alignmentRectInsets];
    BOOL flip = [[self superview] isFlipped];
    return NSMakeRect(a.origin.x - in.left, a.origin.y - (flip ? in.top : in.bottom), a.size.width + in.left + in.right,
                      a.size.height + in.top + in.bottom);
}

/* intrinsische Größe */
- (NSSize) intrinsicContentSize { return NSMakeSize(NSViewNoIntrinsicMetric, NSViewNoIntrinsicMetric); }
- (void) invalidateIntrinsicContentSize { mark_dirty(self); }

- (CGFloat) firstBaselineOffsetFromTop { return 0; }
- (CGFloat) lastBaselineOffsetFromBottom { return 0; }
- (CGFloat) baselineOffsetFromBottom { return [self lastBaselineOffsetFromBottom]; }

@end

/* Prioritäten: 0 im Ivar = Vorgabe der Klasse (Cocotron initialisiert sie nicht) */
static float default_hug(NSView *v, int o);
static float default_comp(NSView *v, int o);
static IMP o_gethug, o_getcomp;
static float v_gethug(NSView *self, SEL c, NSInteger o) {
    float p = ((float (*)(id, SEL, NSInteger)) o_gethug)(self, c, o);
    return p > 0 ? p : default_hug(self, (int) o);
}
static float v_getcomp(NSView *self, SEL c, NSInteger o) {
    float p = ((float (*)(id, SEL, NSInteger)) o_getcomp)(self, c, o);
    return p > 0 ? p : default_comp(self, (int) o);
}
static IMP o_sethug, o_setcomp;
static void v_sethug(NSView *self, SEL c, float p, NSInteger o) {
    ((void (*)(id, SEL, float, NSInteger)) o_sethug)(self, c, p, o);
    mark_dirty(self);
}
static void v_setcomp(NSView *self, SEL c, float p, NSInteger o) {
    ((void (*)(id, SEL, float, NSInteger)) o_setcomp)(self, c, p, o);
    mark_dirty(self);
}

/* ---- Art eines Steuerelements (für Größen, Grundlinien und Standardabstände) ---- */
enum { K_OTHER, K_PUSH, K_CHECK, K_RADIO, K_LABEL, K_FIELD, K_POPUP };

static int ctl_kind(NSView *v) {
    if ([v isKindOfClass: [NSPopUpButton class]]) return K_POPUP;
    if ([v isKindOfClass: [NSButton class]]) {
        NSButtonCell *c = [(NSButton *) v cell];
        if (![c isKindOfClass: [NSButtonCell class]]) return K_OTHER;
        NSString *img = [[c image] name], *alt = [[c alternateImage] name];
        if ([img isEqualToString: @"NSSwitch"] || [alt isEqualToString: @"NSSwitch"] || [alt isEqualToString: @"NSHighlightedSwitch"])
            return K_CHECK;
        if ([img isEqualToString: @"NSRadioButton"] || [alt isEqualToString: @"NSRadioButton"]) return K_RADIO;
        if (([c isBordered] || [c isBezeled]) && [c bezelStyle] == NSRoundedBezelStyle) return K_PUSH;
        return K_OTHER;
    }
    if ([v isKindOfClass: [NSTextField class]]) {
        NSTextField *t = (NSTextField *) v;
        return ([t isBezeled] || [t isBordered] || [t isEditable]) ? K_FIELD : K_LABEL;
    }
    return K_OTHER;
}

/* Vorgaben wie macOS (auf dem Mac abgefragt): Steuerelemente 250/750, NSGridView 249/249 (Kompression 1000), sonst 250/250;
 * Beschriftungen aus +labelWithString: setzen 251 selbst (aus Nibs ohne gespeicherten Wert: 250) */
static float default_hug(NSView *v, int o) {
    if ([v isKindOfClass: NSClassFromString(@"NSGridView")]) return 249;
    if (o == 1 && [v isKindOfClass: [NSControl class]]) return 750;
    return 250;
}
static float default_comp(NSView *v, int o) {
    return [v isKindOfClass: NSClassFromString(@"NSGridView")] ? 1000 : 750;
}

static NSFont *ctl_font(NSView *v) {
    NSFont *f = nil;
    if ([v respondsToSelector: @selector(cell)]) {
        id c = [(NSControl *) v cell];
        if ([c respondsToSelector: @selector(font)]) f = [c font];
    }
    if (!f && [v respondsToSelector: @selector(font)]) f = [(id) v font];
    return f;
}
/* Zeilenhöhe wie bei Apple aufgerundet (Cocotrons defaultLineHeightForFont rundet: 15 statt 16 bei 13 pt) */
static CGFloat line_height(NSFont *f) {
    if (!f) return 16;
    CGFloat h = [f ascender] - [f descender] + [f leading];
    return h > 0 ? ceil(h - 0.001) : 16;
}
static CGFloat half_up(CGFloat x) { return ceil(x * 2 - 0.001) / 2; }

/* Höhe je Steuerelementgröße (regular/small/mini) wie macOS 27 */
static CGFloat size_by_control(NSView *v, CGFloat regular, CGFloat small, CGFloat mini) {
    NSControlSize cs = NSRegularControlSize;
    if ([v respondsToSelector: @selector(cell)] && [[(NSControl *) v cell] respondsToSelector: @selector(controlSize)])
        cs = [[(NSControl *) v cell] controlSize];
    return cs == NSSmallControlSize ? small : cs == NSMiniControlSize ? mini : regular;
}

@implementation NSControl (CLKAutoLayout)
/* Eigene Zellen: deren cellSize, sofern die Zelle sie wirklich berechnet (NSCell liefert 10000 × 10000) */
- (NSSize) intrinsicContentSize {
    id c = [self cell];
    if (c && [c isKindOfClass: [NSCell class]] &&
        [c methodForSelector: @selector(cellSize)] != [NSCell instanceMethodForSelector: @selector(cellSize)]) {
        NSSize s = [c cellSize];
        if (s.width > 0 && s.width < 10000 && s.height > 0 && s.height < 10000) return s;
    }
    return NSMakeSize(NSViewNoIntrinsicMetric, NSViewNoIntrinsicMetric);
}
/* Grundlinie: Text senkrecht mittig in der Höhe des Elements (bei Beschriftungen und Ankreuzfeldern = oben) */
- (CGFloat) firstBaselineOffsetFromTop {
    NSFont *f = ctl_font(self);
    if (!f || ![self cell]) return 0;
    NSSize ics = [self intrinsicContentSize];
    NSEdgeInsets in = [self alignmentRectInsets];
    CGFloat h = ics.height > 0 ? ics.height : [self frame].size.height - in.top - in.bottom;
    CGFloat lh = line_height(f);
    return (h - lh) / 2 + [f ascender];
}
- (CGFloat) lastBaselineOffsetFromBottom {
    NSFont *f = ctl_font(self);
    if (!f || ![self cell]) return 0;
    NSSize ics = [self intrinsicContentSize];
    NSEdgeInsets in = [self alignmentRectInsets];
    CGFloat h = ics.height > 0 ? ics.height : [self frame].size.height - in.top - in.bottom;
    return h - [self firstBaselineOffsetFromTop];
}
@end

@implementation NSButton (CLKAutoLayout)
- (NSSize) intrinsicContentSize {
    NSButtonCell *c = [self cell];
    int k = ctl_kind(self);
    NSString *t = [c title];
    CGFloat tw = [t length] ? half_up([[c attributedTitle] size].width) : 0;
    NSSize own = [super intrinsicContentSize];
    switch (k) {
    case K_CHECK:
    case K_RADIO: {
        CGFloat w = tw ? tw + 22 : 16;
        if (own.width > 0 && own.width < 10000) w = MAX(w, own.width - 4);   /* Cocotron: Bild + 4 + Titel + 4 Rand */
        return NSMakeSize(w, size_by_control(self, 16, 14, 12));
    }
    case K_PUSH: {
        CGFloat w = tw + 24;
        if ([c image] && [c imagePosition] != NSNoImage) w = MAX(w, own.width);
        return NSMakeSize(w, size_by_control(self, 24, 20, 16));
    }
    }
    return own;
}
@end

@implementation NSPopUpButton (CLKAutoLayout)
- (NSSize) intrinsicContentSize {
    NSFont *f = ctl_font(self) ?: [NSFont systemFontOfSize: 13];
    NSDictionary *a = [NSDictionary dictionaryWithObject: f forKey: NSFontAttributeName];
    CGFloat w = 0;
    for (NSMenuItem *it in [self itemArray]) w = MAX(w, [[it title] sizeWithAttributes: a].width);
    if ([self pullsDown] && [self numberOfItems]) w = MAX(w, [[[self itemAtIndex: 0] title] sizeWithAttributes: a].width);
    return NSMakeSize(half_up(w) + 47.5, size_by_control(self, 24, 20, 16));
}
@end

@implementation NSTextField (CLKAutoLayout)
- (NSSize) intrinsicContentSize {
    NSTextFieldCell *c = [self cell];
    NSFont *f = ctl_font(self);
    if (ctl_kind(self) == K_FIELD) return NSMakeSize(NSViewNoIntrinsicMetric, size_by_control(self, 24, 20, 16));
    NSAttributedString *s = [c attributedStringValue];
    NSSize ts = [s length] ? [s size] : NSZeroSize;
    CGFloat lh = line_height(f);
    CGFloat h = MAX(lh, ceil(ts.height));
    CGFloat w = half_up(ts.width);
    CLKViewAL *st = vstate(self, NO);
    if (st && st->pmlw > 0 && w > st->pmlw) {
        /* umbrechende Beschriftung: Breite begrenzen, Zeilen grob schätzen */
        CGFloat lines = ceil(w / st->pmlw);
        w = st->pmlw;
        h = lines * lh;
    }
    return NSMakeSize(w, h);
}
- (NSEdgeInsets) alignmentRectInsets {
    return ctl_kind(self) == K_LABEL ? NSEdgeInsetsMake(0, 2, 0, 2) : NSEdgeInsetsMake(0, 0, 0, 0);
}
- (CGFloat) preferredMaxLayoutWidth { CLKViewAL *s = vstate(self, NO); return s ? s->pmlw : 0; }
- (void) setPreferredMaxLayoutWidth: (CGFloat) w { vstate(self, YES)->pmlw = w; mark_dirty(self); }
@end

@implementation NSImageView (CLKAutoLayout)
- (NSSize) intrinsicContentSize {
    NSImage *i = [self image];
    return i ? [i size] : NSMakeSize(NSViewNoIntrinsicMetric, NSViewNoIntrinsicMetric);
}
@end

/* ---- Fabrikmethoden aus macOS 10.12 (fehlten bei Cocotron); Rahmen = intrinsische Größe wie bei Apple ---- */
static id size_to_content(NSView *v) {
    NSSize s = [v intrinsicContentSize];
    NSEdgeInsets in = [v alignmentRectInsets];
    NSSize f = [v frame].size;
    if (s.width >= 0) f.width = ceil(s.width + in.left + in.right);
    if (s.height >= 0) f.height = ceil(s.height + in.top + in.bottom);
    [v setFrameSize: f];
    return v;
}

@implementation NSTextField (CLKFactories)
+ (instancetype) labelWithString: (NSString *) s {
    NSTextField *t = [[[self alloc] initWithFrame: NSMakeRect(0, 0, 100, 16)] autorelease];
    [t setStringValue: s ?: @""];
    [t setEditable: NO]; [t setSelectable: NO]; [t setBezeled: NO]; [t setBordered: NO]; [t setDrawsBackground: NO];
    [t setFont: [NSFont systemFontOfSize: [NSFont systemFontSize]]];
    [t setTextColor: [NSColor controlTextColor]];
    [t setContentHuggingPriority: 251 forOrientation: 0];
    return size_to_content(t);
}
+ (instancetype) wrappingLabelWithString: (NSString *) s {
    NSTextField *t = [self labelWithString: s];
    [t setSelectable: YES];
    [[t cell] setWraps: YES];
    return t;
}
+ (instancetype) labelWithAttributedString: (NSAttributedString *) s {
    NSTextField *t = [self labelWithString: @""];
    [t setAttributedStringValue: s];
    return size_to_content(t);
}
+ (instancetype) textFieldWithString: (NSString *) s {
    NSTextField *t = [[[self alloc] initWithFrame: NSMakeRect(0, 0, 96, 22)] autorelease];
    [t setStringValue: s ?: @""];
    [t setEditable: YES]; [t setSelectable: YES]; [t setBezeled: YES]; [t setDrawsBackground: YES];
    [t setFont: [NSFont systemFontOfSize: [NSFont systemFontSize]]];
    return size_to_content(t);
}
@end

@implementation NSButton (CLKFactories)
+ (instancetype) _clkButton: (NSString *) title type: (NSInteger) type target: (id) target action: (SEL) action {
    NSButton *b = [[[self alloc] initWithFrame: NSMakeRect(0, 0, 80, 24)] autorelease];
    [b setButtonType: type];
    if (type == NSMomentaryPushInButton) [b setBezelStyle: NSRoundedBezelStyle];
    [b setTitle: title ?: @""];
    [b setTarget: target];
    [b setAction: action];
    [b setFont: [NSFont systemFontOfSize: [NSFont systemFontSize]]];
    return size_to_content(b);
}
+ (instancetype) buttonWithTitle: (NSString *) t target: (id) target action: (SEL) a {
    return [self _clkButton: t type: NSMomentaryPushInButton target: target action: a];
}
+ (instancetype) checkboxWithTitle: (NSString *) t target: (id) target action: (SEL) a {
    return [self _clkButton: t type: NSSwitchButton target: target action: a];
}
+ (instancetype) radioButtonWithTitle: (NSString *) t target: (id) target action: (SEL) a {
    return [self _clkButton: t type: NSRadioButton target: target action: a];
}
+ (instancetype) buttonWithImage: (NSImage *) i target: (id) target action: (SEL) a {
    NSButton *b = [self _clkButton: @"" type: NSMomentaryPushInButton target: target action: a];
    [b setImage: i];
    [b setImagePosition: NSImageOnly];
    return size_to_content(b);
}
+ (instancetype) buttonWithTitle: (NSString *) t image: (NSImage *) i target: (id) target action: (SEL) a {
    NSButton *b = [self _clkButton: t type: NSMomentaryPushInButton target: target action: a];
    [b setImage: i];
    [b setImagePosition: NSImageLeft];
    return size_to_content(b);
}
@end

@implementation NSImageView (CLKFactories)
+ (instancetype) imageViewWithImage: (NSImage *) i {
    NSImageView *v = [[[self alloc] initWithFrame: NSMakeRect(0, 0, 16, 16)] autorelease];
    [v setImage: i];
    return size_to_content(v);
}
@end

/* ---- Layout-Hilfslinie (Cocotrons NSLayoutGuide war ein Platzhalter) ---- */
@interface NSLayoutGuide () {
    id _owner;            /* schwach */
    NSRect _frame;        /* Ausrichtungsrechteck in Koordinaten der besitzenden Ansicht */
    NSString *_identifier;
}
@end

@implementation NSLayoutGuide
- (void) dealloc { objc_storeWeak(&_owner, nil); [_identifier release]; [super dealloc]; }
- (NSView *) owningView { return objc_loadWeak(&_owner); }
- (void) setOwningView: (NSView *) v { objc_storeWeak(&_owner, v); }
- (NSRect) frame { return _frame; }
- (void) _clkSetFrame: (NSRect) f { _frame = f; }
- (NSString *) identifier { return _identifier; }
- (void) setIdentifier: (NSString *) s { [_identifier autorelease]; _identifier = [s copy]; }
- (BOOL) hasAmbiguousLayout { return NO; }
- (NSArray *) constraintsAffectingLayoutForOrientation: (NSInteger) o { return [NSArray array]; }
- (id) _clkAnchorForAttribute: (NSInteger) a { return [NSLayoutAnchor _clkAnchorWithItem: self attribute: a]; }
- (id) leadingAnchor { return [self _clkAnchorForAttribute: LA_Leading]; }
- (id) trailingAnchor { return [self _clkAnchorForAttribute: LA_Trailing]; }
- (id) leftAnchor { return [self _clkAnchorForAttribute: LA_Left]; }
- (id) rightAnchor { return [self _clkAnchorForAttribute: LA_Right]; }
- (id) topAnchor { return [self _clkAnchorForAttribute: LA_Top]; }
- (id) bottomAnchor { return [self _clkAnchorForAttribute: LA_Bottom]; }
- (id) widthAnchor { return [self _clkAnchorForAttribute: LA_Width]; }
- (id) heightAnchor { return [self _clkAnchorForAttribute: LA_Height]; }
- (id) centerXAnchor { return [self _clkAnchorForAttribute: LA_CenterX]; }
- (id) centerYAnchor { return [self _clkAnchorForAttribute: LA_CenterY]; }
- (id) initWithCoder: (NSCoder *) c {
    if (!(self = [super init])) return nil;
    if ([c allowsKeyedCoding]) {
        [self setOwningView: [c decodeObjectForKey: @"NSOwningView"]];
        _identifier = [[c decodeObjectForKey: @"NSIdentifier"] copy];
    }
    return self;
}
- (void) encodeWithCoder: (NSCoder *) c {}
@end

/* ======================================================================================================================
 * Lösen
 * ====================================================================================================================== */
typedef struct {
    id obj;
    BOOL view, tamic, root;
    int parent;                    /* Knoten der Oberansicht bzw. der besitzenden Ansicht */
    int L, T, W, H;                /* Variablen: Rahmen in Wurzelkoordinaten, y nach unten */
    double il, it, ir, ib;         /* Ausrichtungsränder */
    double b1, b2;                 /* Grundlinien */
    int kind;
    double cl, ct, cw, ch;         /* aktuelle Lage (Wurzelkoordinaten) für die schwachen Haltebedingungen */
    double rl, rt, rr, rb;         /* gerundete Kanten nach dem Lösen */
} alnode;

typedef struct {
    NSView *root;
    al_solver *s;
    alnode *n; int nn, cap;
    CFMutableDictionaryRef map;    /* Objekt -> Knotennummer + 1 */
    int broken;
} engine;

enum { MODE_WINDOW, MODE_VIEW, MODE_FIT };

static int find_node(engine *e, id o) {
    intptr_t i = (intptr_t) CFDictionaryGetValue(e->map, o);
    return i ? (int) i - 1 : -1;
}

static BOOL in_subtree(NSView *v, NSView *root) {
    for (NSView *x = v; x; x = [x superview]) if (x == root) return YES;
    return NO;
}

static int add_node(engine *e, id o);

static int new_node(engine *e, id o) {
    if (e->nn == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 64;
        e->n = realloc(e->n, e->cap * sizeof(alnode));
    }
    int i = e->nn++;
    alnode *n = &e->n[i];
    memset(n, 0, sizeof *n);
    n->obj = o;
    n->parent = -1;
    n->L = al_var(e->s); n->T = al_var(e->s); n->W = al_var(e->s); n->H = al_var(e->s);
    CFDictionarySetValue(e->map, o, (const void *) (intptr_t) (i + 1));
    return i;
}

/* Knoten für Ansicht/Hilfslinie anlegen (mit allen Vorfahren bis zur Wurzel); -1 = gehört nicht zum Baum */
static int add_node(engine *e, id o) {
    if (!o) return -1;
    int i = find_node(e, o);
    if (i >= 0) return i;
    if ([o isKindOfClass: [NSView class]]) {
        NSView *v = o;
        if (!in_subtree(v, e->root)) return -1;
        int p = v == e->root ? -1 : add_node(e, [v superview]);
        i = new_node(e, v);
        alnode *n = &e->n[i];
        n->view = YES;
        n->root = v == e->root;
        n->parent = p;
        n->tamic = [v translatesAutoresizingMaskIntoConstraints];
        NSEdgeInsets in = [v alignmentRectInsets];
        n->il = in.left; n->it = in.top; n->ir = in.right; n->ib = in.bottom;
        n->b1 = [v firstBaselineOffsetFromTop];
        n->b2 = [v lastBaselineOffsetFromBottom];
        n->kind = ctl_kind(v);
        NSRect f = [v frame];
        n->cw = f.size.width; n->ch = f.size.height;
        if (p >= 0) {
            alnode *pn = &e->n[p];
            NSView *sv = pn->obj;
            NSRect b = [sv bounds];
            n->cl = pn->cl + f.origin.x - b.origin.x;
            n->ct = pn->ct + ([sv isFlipped] ? f.origin.y - b.origin.y : (b.origin.y + b.size.height) - (f.origin.y + f.size.height));
        }
        return i;
    }
    if ([o isKindOfClass: [NSLayoutGuide class]]) {
        NSView *ov = [(NSLayoutGuide *) o owningView];
        int p = ov ? add_node(e, ov) : -1;
        if (p < 0) return -1;
        i = new_node(e, o);
        alnode *n = &e->n[i], *pn = &e->n[p];
        n->parent = p;
        NSRect f = [(NSLayoutGuide *) o frame], b = [ov bounds];
        n->cl = pn->cl + f.origin.x - b.origin.x; n->cw = f.size.width; n->ch = f.size.height;
        n->ct = pn->ct + ([ov isFlipped] ? f.origin.y - b.origin.y : (b.origin.y + b.size.height) - (f.origin.y + f.size.height));
        return i;
    }
    return -1;
}

typedef struct { int n; int var[4]; double c[4]; double k; } aexpr;

static void ax(aexpr *a, int var, double c) { a->var[a->n] = var; a->c[a->n] = c; a->n++; }

/* Attribut als lineare Funktion der Knotenvariablen (Ausrichtungsrechteck) */
static BOOL attr_expr(engine *e, int i, NSInteger attr, aexpr *a) {
    alnode *n = &e->n[i];
    a->n = 0; a->k = 0;
    switch (attr) {
    case LA_Left: case LA_Leading: ax(a, n->L, 1); a->k = n->il; break;
    case LA_Right: case LA_Trailing: ax(a, n->L, 1); ax(a, n->W, 1); a->k = -n->ir; break;
    case LA_Top: ax(a, n->T, 1); a->k = n->it; break;
    case LA_Bottom: ax(a, n->T, 1); ax(a, n->H, 1); a->k = -n->ib; break;
    case LA_Width: ax(a, n->W, 1); a->k = -n->il - n->ir; break;
    case LA_Height: ax(a, n->H, 1); a->k = -n->it - n->ib; break;
    case LA_CenterX: ax(a, n->L, 1); ax(a, n->W, 0.5); a->k = (n->il - n->ir) / 2; break;
    case LA_CenterY: ax(a, n->T, 1); ax(a, n->H, 0.5); a->k = (n->it - n->ib) / 2; break;
    case LA_FirstBaseline: ax(a, n->T, 1); a->k = n->it + n->b1; break;
    case LA_LastBaseline: ax(a, n->T, 1); ax(a, n->H, 1); a->k = -n->ib - n->b2; break;
    default: return NO;
    }
    return YES;
}

/* Bedingung  a  rel  m * b + k  eintragen */
static void add_rel(engine *e, aexpr *a, aexpr *b, double m, double k, NSInteger rel, double prio, id why) {
    int vars[8]; double cs[8]; int n = 0;
    for (int i = 0; i < a->n; i++) { vars[n] = a->var[i]; cs[n] = a->c[i]; n++; }
    double kk = a->k - k;
    if (b) {
        for (int i = 0; i < b->n; i++) { vars[n] = b->var[i]; cs[n] = -m * b->c[i]; n++; }
        kk -= m * b->k;
    }
    if (prio > 1000) prio = 1000;
    int op = rel < 0 ? AL_LE : rel > 0 ? AL_GE : AL_EQ;
    if (al_add(e->s, n, vars, cs, kk, op, prio) < 0) {
        e->broken++;
        if (trace() && why) fprintf(stderr, "[clokeos-al] Pflichtbedingung nicht erfüllbar, gebrochen: %s\n", [[why description] UTF8String]);
    }
}
static void add_var_rel(engine *e, int var, double c, double k, NSInteger rel, double prio) {
    aexpr a = { 1, { var }, { c }, 0 };
    add_rel(e, &a, NULL, 1, k, rel, prio, nil);
}

/* Standardabstand für ein Paar */
static double standard_space(engine *e, int fi, NSInteger fa, int si) {
    if (si < 0) return 8;
    alnode *f = &e->n[fi], *s = &e->n[si];
    if (!f->view || !s->view) return 8;
    if (f->parent == si || s->parent == fi) return 20;
    BOOL horiz = fa == LA_Leading || fa == LA_Trailing || fa == LA_Left || fa == LA_Right || fa == LA_CenterX;
    if (horiz) return (f->kind == K_PUSH && s->kind == K_PUSH) ? 12 : 8;
    /* senkrecht: obere und untere Ansicht */
    alnode *up = fa == LA_Top || fa == LA_FirstBaseline || fa == LA_CenterY ? s : f, *lo = up == s ? f : s;
    if (lo->kind == K_POPUP) return up->kind == K_LABEL ? 8 : up->kind == K_POPUP ? 10 : 20;
    if (up->kind == K_CHECK && lo->kind == K_CHECK) return 6;
    if (up->kind == K_FIELD && lo->kind == K_FIELD) return 10;
    return 8;
}

static void add_constraint(engine *e, NSLayoutConstraint *c, BOOL internal) {
    if (!internal && ![c isActive]) return;
    id a = [c firstItem], b = [c secondItem];
    int ai = add_node(e, a);
    if (ai < 0) return;
    int bi = -1;
    if (b) {
        bi = add_node(e, b);
        if (bi < 0) return;
    }
    aexpr ea, eb;
    if (!attr_expr(e, ai, [c firstAttribute], &ea)) return;
    BOOL hasb = b && attr_expr(e, bi, [c secondAttribute], &eb);
    double k = [c constant];
    if ([c _clkStandardSpacing]) k = standard_space(e, ai, [c firstAttribute], bi) * [c _clkSpacingFactor];
    double prio = [c priority];
    add_rel(e, &ea, hasb ? &eb : NULL, [c multiplier], k, [c relation], prio < 1 ? 1 : prio, c);
}

/* Autoresizing-Maske einer Ansicht als Pflichtbedingungen an die Oberansicht (aktuelle Werte, Änderung anteilig verteilt) */
static void add_autoresizing(engine *e, int i) {
    alnode *n = &e->n[i], *p = &e->n[n->parent];
    NSView *v = n->obj, *sv = p->obj;
    NSRect f = [v frame], b = [sv bounds];
    NSSize ps = [sv frame].size;
    NSUInteger m = [v autoresizingMask];
    BOOL flip = [sv isFlipped];
    for (int axis = 0; axis < 2; axis++) {
        double off, len, plen;
        BOOL flexA, flexL, flexB;
        int cv, cl, pv, pl;
        if (axis == 0) {
            off = f.origin.x - b.origin.x; len = f.size.width; plen = ps.width;
            flexA = (m & NSViewMinXMargin) != 0; flexL = (m & NSViewWidthSizable) != 0; flexB = (m & NSViewMaxXMargin) != 0;
            cv = n->L; cl = n->W; pv = p->L; pl = p->W;
        } else {
            off = flip ? f.origin.y - b.origin.y : (b.origin.y + b.size.height) - (f.origin.y + f.size.height);
            len = f.size.height; plen = ps.height;
            flexA = (m & (flip ? NSViewMinYMargin : NSViewMaxYMargin)) != 0;
            flexL = (m & NSViewHeightSizable) != 0;
            flexB = (m & (flip ? NSViewMaxYMargin : NSViewMinYMargin)) != 0;
            cv = n->T; cl = n->H; pv = p->T; pl = p->H;
        }
        double after = plen - off - len;
        double sum = (flexA ? off : 0) + (flexL ? len : 0) + (flexB ? after : 0);
        int nflex = flexA + flexL + flexB;
        double ka = 0, kl = 0;
        if (nflex) {
            if (sum > 0.001) { ka = flexA ? off / sum : 0; kl = flexL ? len / sum : 0; }
            else { ka = flexA ? 1.0 / nflex : 0; kl = flexL ? 1.0 / nflex : 0; }
        }
        /* c.pos = p.pos + off + ka * (p.len - plen);   c.len = len + kl * (p.len - plen) */
        int vs[3] = { cv, pv, pl };
        double cs[3] = { 1, -1, -ka };
        if (al_add(e->s, ka ? 3 : 2, vs, cs, -(off - ka * plen), AL_EQ, 1000) < 0) e->broken++;
        int vl[2] = { cl, pl };
        double cl2[2] = { 1, -kl };
        if (al_add(e->s, kl ? 2 : 1, vl, cl2, -(len - kl * plen), AL_EQ, 1000) < 0) e->broken++;
    }
}

static void add_intrinsic(engine *e, int i) {
    alnode *n = &e->n[i];
    NSView *v = n->obj;
    NSSize s = [v intrinsicContentSize];
    if (trace() > 2) fprintf(stderr, "[clokeos-al]   ics %s %p: %gx%g Grundlinie %g/%g Hugging %g/%g\n", object_getClassName(v), v, s.width, s.height,
                             n->b1, n->b2, [v contentHuggingPriorityForOrientation: 0], [v contentHuggingPriorityForOrientation: 1]);
    for (int o = 0; o < 2; o++) {
        double want = o ? s.height : s.width;
        if (want < 0) continue;
        int var = o ? n->H : n->W;
        double ins = o ? n->it + n->ib : n->il + n->ir;
        float hug = [v contentHuggingPriorityForOrientation: o], cr = [v contentCompressionResistancePriorityForOrientation: o];
        add_var_rel(e, var, 1, want + ins, 1, cr);    /* nicht kleiner (Kompressionswiderstand) */
        add_var_rel(e, var, 1, want + ins, -1, hug);  /* nicht größer (Hugging) */
    }
}

/* cs: eingetragene Bedingungen; gs: Bedingungen, die Ansichten selbst erzeugen (NSGridView), gelten ohne Eintrag als aktiv */
static void collect(engine *e, NSView *v, NSMutableArray *cs, NSMutableArray *gs) {
    CLKViewAL *s = vstate(v, NO);
    if (s && s->constraints) for (NSLayoutConstraint *c in s->constraints) if ([c isActive]) [cs addObject: c];
    if ([v respondsToSelector: @selector(_clkGridConstraints)]) [gs addObjectsFromArray: [(id) v performSelector: @selector(_clkGridConstraints)]];
    for (NSView *sub in [v subviews]) collect(e, sub, cs, gs);
}

static double rnd(double x) { return floor(x + 0.5); }

static void set_member(engine *e, BOOL f) {
    for (int i = 0; i < e->nn; i++) if (e->n[i].view) vstate(e->n[i].obj, YES)->member = f;
}

/* löst den Baum unter root; MODE_WINDOW/MODE_VIEW setzen die Rahmen, MODE_FIT liefert nur die Größe. Rückgabe: Größe der Wurzel */
static NSSize solve_tree(NSView *root, int mode, BOOL *any) {
    NSMutableArray *cs = [NSMutableArray array], *gs = [NSMutableArray array];
    engine E = { root, al_new(), NULL, 0, 0, CFDictionaryCreateMutable(NULL, 0, NULL, NULL), 0 }, *e = &E;
    collect(e, root, cs, gs);
    if (any) *any = [cs count] + [gs count] > 0;
    NSSize result = [root frame].size;
    if (![cs count] && ![gs count] && mode != MODE_FIT) goto done;
    [cs addObjectsFromArray: gs];

    int r = add_node(e, root);
    /* Knoten zuerst anlegen (Bedingungen erst danach, damit Kinder ihre Oberansicht kennen) */
    for (NSLayoutConstraint *c in cs) {
        if (add_node(e, [c firstItem]) < 0) continue;
        if ([c secondItem]) add_node(e, [c secondItem]);
    }
    alnode *rn = &e->n[r];
    add_var_rel(e, rn->L, 1, 0, 0, 1000);
    add_var_rel(e, rn->T, 1, 0, 0, 1000);
    NSSize rs = [root frame].size;
    if (mode == MODE_WINDOW) {
        add_var_rel(e, rn->W, 1, rs.width, 0, 500);
        add_var_rel(e, rn->H, 1, rs.height, 0, 500);
        add_var_rel(e, rn->W, 1, 0, 1, 1000);
        add_var_rel(e, rn->H, 1, 0, 1, 1000);
    } else if (mode == MODE_VIEW) {
        add_var_rel(e, rn->W, 1, rs.width, 0, 1000);
        add_var_rel(e, rn->H, 1, rs.height, 0, 1000);
    } else {
        add_var_rel(e, rn->W, 1, 0, 0, 50);
        add_var_rel(e, rn->H, 1, 0, 0, 50);
        add_var_rel(e, rn->W, 1, 0, 1, 1000);
        add_var_rel(e, rn->H, 1, 0, 1, 1000);
        add_intrinsic(e, r);
    }
    int nviews = e->nn;
    for (int i = 0; i < nviews; i++) {
        alnode *n = &e->n[i];
        if (!n->view || n->root) continue;
        if (n->tamic) add_autoresizing(e, i);
        else add_intrinsic(e, i);
    }
    if (trace() > 3) {
        for (int i = 0; i < nviews; i++)
            if (e->n[i].view) fprintf(stderr, "[clokeos-al]   Knoten %d %s %p Eltern %d tamic %d\n", i, object_getClassName(e->n[i].obj),
                                      e->n[i].obj, e->n[i].parent, e->n[i].tamic);
        for (NSLayoutConstraint *c in cs) fprintf(stderr, "[clokeos-al]   %s\n", [[c description] UTF8String]);
    }
    NSUInteger ncs = [cs count] - [gs count], ci = 0;
    for (NSLayoutConstraint *c in cs) add_constraint(e, c, ci++ >= ncs);
    /* Was die Bedingungen offen lassen, bleibt, wo es ist: Größe und Abstand zur Oberansicht wie bisher, senkrecht von deren
     * Ursprung aus gemessen (bei nicht gespiegelten Oberansichten von unten), wie AppKits Löser, der minX/minY in Koordinaten der
     * Oberansicht führt. Ohne das landeten offene Werte bei 0 (etwa die Höhe einer Tab-Ansicht ohne Höhenbedingung), und Kinder
     * einer verschobenen Ansicht blieben an der alten Stelle im Fenster stehen. */
    for (int i = 0; i < e->nn; i++) {
        alnode *n = &e->n[i];
        if (n->root || n->tamic || n->parent < 0) continue;
        alnode *p = &e->n[n->parent];
        NSView *pv = p->obj;
        int vh[2] = { n->L, p->L };
        double ch[2] = { 1, -1 };
        al_add(e->s, 2, vh, ch, -(n->cl - p->cl), AL_EQ, 0.5);
        if ([pv isFlipped]) {
            int vv[2] = { n->T, p->T };
            al_add(e->s, 2, vv, ch, -(n->ct - p->ct), AL_EQ, 0.5);
        } else {
            /* (p.T + p.H) - (n.T + n.H) == Abstand von unten */
            int vv[4] = { p->T, p->H, n->T, n->H };
            double cv[4] = { 1, 1, -1, -1 };
            al_add(e->s, 4, vv, cv, -((p->ct + p->ch) - (n->ct + n->ch)), AL_EQ, 0.5);
        }
        add_var_rel(e, n->W, 1, n->cw, 0, 0.5);
        add_var_rel(e, n->H, 1, n->ch, 0, 0.5);
    }
    /* Ergebnis: Kanten runden; Wurzel bei (0, 0) */
    for (int i = 0; i < e->nn; i++) {
        alnode *n = &e->n[i];
        double L = al_value(e->s, n->L), T = al_value(e->s, n->T), W = al_value(e->s, n->W), H = al_value(e->s, n->H);
        if (W < 0) W = 0;
        if (H < 0) H = 0;
        n->rl = rnd(L); n->rt = rnd(T); n->rr = rnd(L + W); n->rb = rnd(T + H);
    }
    result = NSMakeSize(e->n[r].rr - e->n[r].rl, e->n[r].rb - e->n[r].rt);
    if (mode == MODE_FIT) {
        result = NSMakeSize(ceil(al_value(e->s, rn->W) - 0.001), ceil(al_value(e->s, rn->H) - 0.001));
        goto done;
    }
    if (trace()) fprintf(stderr, "[clokeos-al] %s %s: %lu Bedingungen, %d Knoten, %d gebrochen, Wurzel %gx%g -> %gx%g\n",
                         mode == MODE_WINDOW ? "Fenster" : "Ansicht", object_getClassName(root), (unsigned long) [cs count], e->nn,
                         e->broken, rs.width, rs.height, result.width, result.height);

    g_applying = YES;
    @try {
        if (mode == MODE_WINDOW && !NSEqualSizes(result, rs)) {
            NSWindow *w = [root window];
            NSRect fr = [w frame];
            NSRect cr = [w contentRectForFrameRect: fr];
            cr.size = result;
            NSRect nf = [w frameRectForContentRect: cr];
            nf.origin.y = NSMaxY(fr) - nf.size.height;   /* obere Kante bleibt */
            nf.origin.x = fr.origin.x;
            if (trace()) fprintf(stderr, "[clokeos-al] Fenster %s: %gx%g -> %gx%g\n", [[w title] UTF8String], fr.size.width,
                                 fr.size.height, nf.size.width, nf.size.height);
            [w setFrame: nf display: NO];
            if (!NSEqualSizes([root frame].size, result)) [root setFrameSize: result];
        }
        /* Knoten sind so angelegt, dass Oberansichten vor ihren Kindern stehen */
        for (int i = 0; i < e->nn; i++) {
            alnode *n = &e->n[i];
            if (n->root || n->parent < 0) continue;
            alnode *p = &e->n[n->parent];
            if (!n->view) {
                NSView *ov = p->obj;
                NSRect b = [ov bounds];
                double w = n->rr - n->rl, h = n->rb - n->rt;
                double y = [ov isFlipped] ? n->rt - p->rt + b.origin.y : b.origin.y + (p->rb - p->rt) - (n->rt - p->rt) - h;
                [(NSLayoutGuide *) n->obj _clkSetFrame: NSMakeRect(n->rl - p->rl + b.origin.x, y, w, h)];
                continue;
            }
            NSView *v = n->obj, *sv = p->obj;
            NSRect b = [sv bounds];
            double w = n->rr - n->rl, h = n->rb - n->rt;
            double x = n->rl - p->rl + b.origin.x;
            double y = [sv isFlipped] ? n->rt - p->rt + b.origin.y : b.origin.y + b.size.height - (n->rt - p->rt) - h;
            NSRect nf = NSMakeRect(x, y, w, h), of = [v frame];
            if (fabs(nf.origin.x - of.origin.x) > 0.01 || fabs(nf.origin.y - of.origin.y) > 0.01 || fabs(nf.size.width - of.size.width) > 0.01 ||
                fabs(nf.size.height - of.size.height) > 0.01) {
                if (trace() > 1) fprintf(stderr, "[clokeos-al]   %s %p: {%g,%g %gx%g} -> {%g,%g %gx%g}\n", object_getClassName(v), v,
                                         of.origin.x, of.origin.y, of.size.width, of.size.height, x, y, w, h);
                [v setFrame: nf];
            }
        }
    } @finally {
        g_applying = NO;
    }
    set_member(e, YES);
done:
    al_free(e->s);
    free(e->n);
    CFRelease(e->map);
    return result;
}

/* ---- Einstieg: Fenster und Ansichten ---- */
static void window_solve(NSWindow *w, BOOL force) {
    NSView *cv = [w contentView];
    if (!cv) return;
    CLKWindowAL *ws = wstate(w, YES);
    if (!ws->dirty && !force) return;
    ws->dirty = NO;
    [cv updateConstraintsForSubtreeIfNeeded];
    BOOL any = NO;
    solve_tree(cv, MODE_WINDOW, &any);
    ws->hasConstraints = any;
    ws->solved = YES;
    ws->dirty = NO;
}

@implementation NSWindow (CLKAutoLayout)
- (void) _clkSolveConstraintsIfNeeded { window_solve(self, NO); }
- (void) updateConstraintsIfNeeded { [[self contentView] updateConstraintsForSubtreeIfNeeded]; }
- (void) layoutIfNeeded {
    window_solve(self, NO);
    NSView *root = [[self contentView] superview] ?: [self contentView];
    if ([root respondsToSelector: @selector(layoutSubtreeIfNeeded)]) [root layoutSubtreeIfNeeded];
}
- (void) setNeedsLayout: (BOOL) f { if (f) mark_dirty([self contentView]); }
- (void) setNeedsUpdateConstraints: (BOOL) f { if (f) mark_dirty([self contentView]); }
- (void) setAnchorAttribute: (NSInteger) a forOrientation: (NSInteger) o {}
- (NSInteger) anchorAttributeForOrientation: (NSInteger) o { return o ? LA_Top : LA_Leading; }
- (void) visualizeConstraints: (NSArray *) cs {}
@end

@implementation NSView (CLKAutoLayoutSolve)
- (NSSize) fittingSize {
    [self updateConstraintsForSubtreeIfNeeded];
    return solve_tree(self, MODE_FIT, NULL);
}
/* von layout.m vor dem Durchgang gerufen: Fenster lösen, sonst (losgelöster Baum) den Teilbaum mit fester Größe */
- (void) _clkSolveConstraintsIfNeeded {
    NSWindow *w = [self window];
    if (w) { window_solve(w, NO); return; }
    NSView *top = self;
    while ([top superview]) top = [top superview];
    [top updateConstraintsForSubtreeIfNeeded];
    solve_tree(top, MODE_VIEW, NULL);
}
@end

/* ---- Haken in Cocotrons NSView/NSWindow ---- */
static IMP o_setframe;
static void h_setframe(NSView *self, SEL c, NSRect f) {
    ((void (*)(id, SEL, NSRect)) o_setframe)(self, c, f);
    /* Wie bei Apple: Rahmen von Ansichten, die die Bedingungen führen (translatesAutoresizingMaskIntoConstraints = NO), darf das
     * Programm selbst setzen, ohne neu zu lösen; Ansichten mit Autoresizing-Maske gehen dagegen mit ihrem Rahmen in die Lösung ein */
    if (!g_applying) {
        CLKViewAL *s = vstate(self, NO);
        if (s && s->member && [self translatesAutoresizingMaskIntoConstraints]) mark_dirty(self);
    }
}

static IMP o_insert;
static void h_insert(NSView *self, SEL c, NSView *v, NSUInteger idx) {
    ((void (*)(id, SEL, id, NSUInteger)) o_insert)(self, c, v, idx);
    mark_dirty(self);
}

/* Herausnehmen: Bedingungen der Vorfahren, die Ansichten des herausgenommenen Teilbaums betreffen, fallen weg (wie bei Apple) */
static BOOL item_in(id item, NSView *sub) {
    if ([item isKindOfClass: [NSLayoutGuide class]]) item = [(NSLayoutGuide *) item owningView];
    return [item isKindOfClass: [NSView class]] && in_subtree(item, sub);
}
static IMP o_remove;
static void h_remove(NSView *self, SEL c) {
    NSView *sv = [self superview];
    if (sv) {
        mark_dirty(sv);
        for (NSView *a = sv; a; a = [a superview]) {
            CLKViewAL *s = vstate(a, NO);
            if (!s || ![s->constraints count]) continue;
            for (NSLayoutConstraint *k in [[s->constraints copy] autorelease])
                if (item_in([k firstItem], self) || ([k secondItem] && item_in([k secondItem], self))) [a removeConstraint: k];
        }
    }
    ((void (*)(id, SEL)) o_remove)(self, c);
}

static IMP o_settamic;
static void h_settamic(NSView *self, SEL c, BOOL f) {
    ((void (*)(id, SEL, BOOL)) o_settamic)(self, c, f);
    mark_dirty(self);
}

static IMP o_sethidden;
static void h_sethidden(NSView *self, SEL c, BOOL f) {
    ((void (*)(id, SEL, BOOL)) o_sethidden)(self, c, f);
    CLKViewAL *s = vstate(self, NO);
    if (s && s->member) mark_dirty(self);
    else if ([[self superview] isKindOfClass: NSClassFromString(@"NSGridView")]) mark_dirty(self);
}

/* Inhalt geändert: intrinsische Größe neu (Apple macht das in den Settern der Steuerelemente selbst) */
static IMP o_setstr, o_setobj, o_setattr, o_settitle, o_setfont;
static void ctl_changed(NSView *v) {
    CLKViewAL *s = vstate(v, NO);
    if (s && s->member) mark_dirty(v);
}
static void h_setstr(NSControl *self, SEL c, id x) { ((void (*)(id, SEL, id)) o_setstr)(self, c, x); ctl_changed(self); }
static void h_setobj(NSControl *self, SEL c, id x) { ((void (*)(id, SEL, id)) o_setobj)(self, c, x); ctl_changed(self); }
static void h_setattr(NSControl *self, SEL c, id x) { ((void (*)(id, SEL, id)) o_setattr)(self, c, x); ctl_changed(self); }
static void h_settitle(NSButton *self, SEL c, id x) { ((void (*)(id, SEL, id)) o_settitle)(self, c, x); ctl_changed(self); }
static void h_setfont(NSControl *self, SEL c, id x) { ((void (*)(id, SEL, id)) o_setfont)(self, c, x); ctl_changed(self); }

/* vor dem ersten Zeigen auslegen (Fenster wachsen sonst sichtbar nach) */
static IMP o_order;
static void h_order(NSWindow *self, SEL c, NSInteger place, NSInteger rel) {
    if (place != NSWindowOut && ![self isVisible]) {
        CLKWindowAL *ws = wstate(self, NO);
        if (!ws || ws->dirty || !ws->solved) {
            @try { [self layoutIfNeeded]; }
            @catch (id e) { NSLog(@"Auto Layout vor dem Zeigen: %@", e); }
        }
    }
    ((void (*)(id, SEL, NSInteger, NSInteger)) o_order)(self, c, place, rel);
}

/* Nib: Bedingungen der Ansicht, translatesAutoresizingMaskIntoConstraints, Prioritäten */
static void decode_layout(NSView *self, NSCoder *coder) {
    @try {
        if ([coder containsValueForKey: @"NSDoNotTranslateAutoresizingMask"] && [coder decodeBoolForKey: @"NSDoNotTranslateAutoresizingMask"])
            [self setTranslatesAutoresizingMaskIntoConstraints: NO];
        id hug = [coder decodeObjectForKey: @"NSHuggingPriority"], comp = [coder decodeObjectForKey: @"NSAntiCompressionPriority"];
        if ([hug isKindOfClass: [NSString class]]) {
            NSSize p = NSSizeFromString(hug);
            if (p.width > 0) [self setContentHuggingPriority: p.width forOrientation: 0];
            if (p.height > 0) [self setContentHuggingPriority: p.height forOrientation: 1];
        }
        if ([comp isKindOfClass: [NSString class]]) {
            NSSize p = NSSizeFromString(comp);
            if (p.width > 0) [self setContentCompressionResistancePriority: p.width forOrientation: 0];
            if (p.height > 0) [self setContentCompressionResistancePriority: p.height forOrientation: 1];
        }
        id cs = [coder decodeObjectForKey: @"NSViewConstraints"];
        if ([cs isKindOfClass: [NSArray class]])
            for (id k in cs) if ([k isKindOfClass: [NSLayoutConstraint class]]) [self addConstraint: k];
        id gs = [coder decodeObjectForKey: @"NSViewLayoutGuides"];
        if ([gs isKindOfClass: [NSArray class]])
            for (id g in gs) if ([g isKindOfClass: [NSLayoutGuide class]]) [self addLayoutGuide: g];
    } @catch (id e) {
        NSLog(@"Auto Layout aus der Nib: %@", e);
    }
}

static IMP o_initcoder;
static id h_initcoder(NSView *self, SEL c, NSCoder *coder) {
    self = ((id (*)(id, SEL, id)) o_initcoder)(self, c, coder);
    if (self && [coder allowsKeyedCoding]) decode_layout(self, coder);
    return self;
}

/* von NSCustomView (patches/…/nib.subproj/NSCustomView.m.sed) gerufen, bevor es den Platzhalter freigibt */
__attribute__((visibility("hidden"))) void CLKCustomViewDecoded(NSCoder *coder, id placeholder, NSView *view) {
    if (!view || ![coder allowsKeyedCoding]) return;
    if ([coder respondsToSelector: @selector(replaceObject:withObject:)]) [(NSKeyedUnarchiver *) coder replaceObject: placeholder withObject: view];
    decode_layout(view, coder);
}

static IMP swap(Class k, SEL s, IMP n) {
    Method m = class_getInstanceMethod(k, s);
    if (!m) return NULL;
    IMP old = method_getImplementation(m);
    if (class_addMethod(k, s, n, method_getTypeEncoding(m))) return old;
    return method_setImplementation(m, n);
}

__attribute__((constructor)) static void clk_autolayout_init(void) {
    Class v = [NSView class];
    o_gethug = swap(v, @selector(contentHuggingPriorityForOrientation:), (IMP) v_gethug);
    o_getcomp = swap(v, @selector(contentCompressionResistancePriorityForOrientation:), (IMP) v_getcomp);
    o_sethug = swap(v, @selector(setContentHuggingPriority:forOrientation:), (IMP) v_sethug);
    o_setcomp = swap(v, @selector(setContentCompressionResistancePriority:forOrientation:), (IMP) v_setcomp);
    o_setframe = swap(v, @selector(setFrame:), (IMP) h_setframe);
    o_insert = swap(v, @selector(_insertSubview:atIndex:), (IMP) h_insert);
    o_remove = swap(v, @selector(removeFromSuperviewWithoutNeedingDisplay), (IMP) h_remove);
    o_settamic = swap(v, @selector(setTranslatesAutoresizingMaskIntoConstraints:), (IMP) h_settamic);
    o_sethidden = swap(v, @selector(setHidden:), (IMP) h_sethidden);
    o_initcoder = swap(v, @selector(initWithCoder:), (IMP) h_initcoder);
    Class ctl = [NSControl class];
    o_setstr = swap(ctl, @selector(setStringValue:), (IMP) h_setstr);
    o_setobj = swap(ctl, @selector(setObjectValue:), (IMP) h_setobj);
    o_setattr = swap(ctl, @selector(setAttributedStringValue:), (IMP) h_setattr);
    o_setfont = swap(ctl, @selector(setFont:), (IMP) h_setfont);
    o_settitle = swap([NSButton class], @selector(setTitle:), (IMP) h_settitle);
    o_order = swap([NSWindow class], @selector(orderWindow:relativeTo:), (IMP) h_order);
}
