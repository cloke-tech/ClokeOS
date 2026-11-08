/* Layout-Durchgang für Cocotrons NSView (ab macOS 10.7 Teil jedes Zeichendurchgangs, Cocotron hat ihn nicht): needsLayout,
 * setNeedsLayout:, layout, layoutSubtreeIfNeeded. Programme, die Unteransichten in -layout anlegen oder anordnen (Logic Pro: rund 30
 * Klassen, z. B. die Kacheln des Inspektors), bekamen den Aufruf nie.
 *
 *   - Eine Ansicht braucht ein Layout, bis sie einmal ausgelegt wurde, nach setNeedsLayout:YES und wenn sich ihre Größe ändert
 *     (wie bei Apple).
 *   - -[NSWindow displayIfNeeded]/-display legen vor dem Zeichnen den ganzen Baum aus (höchstens 4 Durchgänge, falls -layout neue
 *     Layouts anfordert); -layoutSubtreeIfNeeded geht von oben nach unten: erst die Ansicht selbst, dann ihre Unteransichten.
 *   - Auto Layout (autolayout.m in AppKit): vor jedem Durchgang werden die Bedingungen des Fensters gelöst, falls sich etwas
 *     geändert hat (-_clkSolveConstraintsIfNeeded setzt die Rahmen); -[NSView layout] selbst tut darum nichts. Ruft ein Programm
 *     -layoutSubtreeIfNeeded direkt auf, wird ebenfalls zuerst gelöst (bei losgelösten Bäumen mit fester Größe der Wurzel).
 *     Ist die Ansicht die eines NSViewController (nextResponder), werden dessen viewWillLayout/viewDidLayout gerufen.
 * Eingetragen mit class_addMethod/Methodentausch beim Laden von AppKit. */
#import <AppKit/NSView.h>
#import <AppKit/NSWindow.h>
#import <AppKit/NSViewController.h>
#import <objc/runtime.h>

static char k_state;   /* NSNumber: Bit 0 = ausgelegt, Bit 1 = braucht Layout */
static char k_clean;   /* am Fenster: ja = seit dem letzten Durchgang hat keine Ansicht ein Layout angefordert */
static BOOL g_pending;

/* Fenster als „Layout nötig" markieren; ohne Markierung überspringt displayIfNeeded den Gang durch den ganzen Baum (vorher lief er
 * bei jedem Zeichnen, Logic: hunderte Ansichten, ein Großteil der Leerlauf-CPU). */
static void mark_window(NSWindow *w) { if (w) objc_setAssociatedObject(w, &k_clean, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }

static unsigned st(id v) { return [objc_getAssociatedObject(v, &k_state) unsignedIntValue]; }
static void set_st(id v, unsigned s) { objc_setAssociatedObject(v, &k_state, @(s), OBJC_ASSOCIATION_RETAIN_NONATOMIC); }

static BOOL v_needs(id self, SEL c) {
    unsigned s = st(self);
    return !(s & 1) || (s & 2);
}
static void v_setneeds(id self, SEL c, BOOL flag) {
    unsigned s = st(self);
    set_st(self, flag ? (s | 2) : (s & ~2u));
    if (flag) {
        g_pending = YES;
        mark_window([self window]);
        [[self window] setViewsNeedDisplay: YES];
    }
}
static void v_layout(id self, SEL c) {}
static void v_noop(id self, SEL c) {}
static int g_depth;
static void solve(id v) {
    if ([v respondsToSelector: @selector(_clkSolveConstraintsIfNeeded)]) [v performSelector: @selector(_clkSolveConstraintsIfNeeded)];
}

static void layout_one(NSView *v) {
    set_st(v, (st(v) | 1) & ~2u);
    id vc = [v nextResponder];
    BOOL isvc = [vc isKindOfClass: [NSViewController class]] && [vc view] == v;
    if (isvc && [vc respondsToSelector: @selector(viewWillLayout)]) [vc viewWillLayout];
    [v layout];
    if (isvc && [vc respondsToSelector: @selector(viewDidLayout)]) [vc viewDidLayout];
}
static void v_subtree(id self, SEL c) {
    if ([self isHidden]) return;
    if (!g_depth) solve(self);
    g_depth++;
    @try {
        if (v_needs(self, c)) layout_one(self);
        for (NSView *sub in [[[self subviews] copy] autorelease]) [sub layoutSubtreeIfNeeded];
    } @finally {
        g_depth--;
    }
}

static IMP o_setframe;
static void v_setframe(id self, SEL c, NSRect f) {
    NSSize old = [self frame].size;
    ((void (*)(id, SEL, NSRect)) o_setframe)(self, c, f);
    if (!NSEqualSizes(old, f.size)) v_setneeds(self, c, YES);
}

static void layout_window(NSWindow *w) {
    if (![w isVisible]) return;
    if (objc_getAssociatedObject(w, &k_clean)) { solve(w); return; }
    NSView *root = [[w contentView] superview] ?: [w contentView];
    for (int pass = 0; pass < 4; pass++) {
        g_pending = NO;
        @try { solve(w); [root layoutSubtreeIfNeeded]; }
        @catch (id e) { NSLog(@"layoutSubtreeIfNeeded: %@", e); break; }
        if (!g_pending) {
            objc_setAssociatedObject(w, &k_clean, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            break;
        }
    }
}
/* neue Ansichten im Fenster brauchen ihr erstes Layout */
static IMP o_setwindow;
static void v_setwindow(id self, SEL c, NSWindow *w) {
    ((void (*)(id, SEL, id)) o_setwindow)(self, c, w);
    mark_window(w);
}
static IMP o_disp_needed, o_disp;
static void w_disp_needed(id self, SEL c) { layout_window(self); ((void (*)(id, SEL)) o_disp_needed)(self, c); }
static void w_disp(id self, SEL c) { layout_window(self); ((void (*)(id, SEL)) o_disp)(self, c); }

__attribute__((constructor)) static void clk_layout_init(void) {
    Class v = [NSView class];
    class_addMethod(v, @selector(needsLayout), (IMP) v_needs, "c@:");
    class_addMethod(v, @selector(setNeedsLayout:), (IMP) v_setneeds, "v@:c");
    class_addMethod(v, @selector(layout), (IMP) v_layout, "v@:");
    class_addMethod(v, @selector(layoutSubtreeIfNeeded), (IMP) v_subtree, "v@:");
    Class vc = [NSViewController class];
    class_addMethod(vc, @selector(viewWillLayout), (IMP) v_noop, "v@:");
    class_addMethod(vc, @selector(viewDidLayout), (IMP) v_noop, "v@:");
    Method m = class_getInstanceMethod(v, @selector(setFrame:));
    o_setframe = method_setImplementation(m, (IMP) v_setframe);
    Method sw = class_getInstanceMethod(v, sel_registerName("_setWindow:"));
    if (sw) o_setwindow = method_setImplementation(sw, (IMP) v_setwindow);
    Class w = [NSWindow class];
    o_disp_needed = method_setImplementation(class_getInstanceMethod(w, @selector(displayIfNeeded)), (IMP) w_disp_needed);
    o_disp = method_setImplementation(class_getInstanceMethod(w, @selector(display)), (IMP) w_disp);
}
