/* NSScrollView contentInsets (macOS 10.10), scrollerInsets, automaticallyAdjustsContentInsets für Cocotrons AppKit.
 *
 * Cocotrons NSScrollView kannte sie nicht (im Nachsichtmodus folgenlos). Logic Pro rückt so sein Lineal um die Breite der Spurköpfe ein:
 * ohne Einzug lag Takt 1 des Lineals hinter den Spurköpfen, das Lineal passte nicht zu den Regionen darunter.
 * Hier legt -clipViewFrame die Clip-Ansicht in den Einzug (NSScrollView ist bei Cocotron gespiegelt: oben = kleines y). Anders als bei
 * Apple läuft der Inhalt nicht unter den Einzug; was dort liegt, zeigt den Hintergrund der Scroll-Ansicht. scrollerInsets und
 * automaticallyAdjustsContentInsets werden nur gemerkt. */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <dlfcn.h>

static char k_ci, k_si, k_auto;

static NSEdgeInsets get_insets(id self, char *key) {
    NSValue *v = objc_getAssociatedObject(self, key);
    NSEdgeInsets e = { 0, 0, 0, 0 };
    if (v) [v getValue: &e];
    return e;
}
static void set_insets(id self, char *key, NSEdgeInsets e) {
    objc_setAssociatedObject(self, key, [NSValue valueWithBytes: &e objCType: @encode(NSEdgeInsets)], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static NSEdgeInsets sv_content_insets(id self, SEL s) {
    static int n;
    if (getenv("CLOKE_SCROLL_TRACE") && n++ < 40) {
        void *ra = __builtin_return_address(0); Dl_info di;
        NSLog(@"[clokeos-scroll] %s contentInsets gelesen von %s+0x%lx", object_getClassName(self),
              dladdr(ra, &di) && di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?", dladdr(ra, &di) ? (unsigned long) ((char *) ra - (char *) di.dli_fbase) : 0);
    }
    return get_insets(self, &k_ci);
}
static void sv_set_content_insets(NSScrollView *self, SEL s, NSEdgeInsets e) {
    if (getenv("CLOKE_SCROLL_TRACE"))   /* CLOKE_SCROLL_TRACE: wer welchen Einzug setzt (Fehlersuche) */
        NSLog(@"[clokeos-scroll] %s %@ contentInsets %.0f %.0f %.0f %.0f doc=%s", object_getClassName(self), NSStringFromRect([self frame]),
              e.top, e.left, e.bottom, e.right, object_getClassName([self documentView]));
    set_insets(self, &k_ci, e);
    [self tile];
    [self setNeedsDisplay: YES];
}
static NSEdgeInsets sv_scroller_insets(id self, SEL s) { return get_insets(self, &k_si); }
static void sv_set_scroller_insets(id self, SEL s, NSEdgeInsets e) { set_insets(self, &k_si, e); }
static BOOL sv_auto(id self, SEL s) {
    NSNumber *n = objc_getAssociatedObject(self, &k_auto);
    return n ? [n boolValue] : YES;
}
static void sv_set_auto(id self, SEL s, BOOL v) { objc_setAssociatedObject(self, &k_auto, @(v), OBJC_ASSOCIATION_RETAIN_NONATOMIC); }

static IMP o_clip_frame;
static NSRect sv_clip_frame(id self, SEL s) {
    NSRect r = ((NSRect (*)(id, SEL)) o_clip_frame)(self, s);
    NSEdgeInsets e = get_insets(self, &k_ci);
    if (e.top == 0 && e.left == 0 && e.bottom == 0 && e.right == 0) return r;
    r.origin.x += e.left;
    r.size.width -= e.left + e.right;
    r.origin.y += e.top;
    r.size.height -= e.top + e.bottom;
    if (r.size.width < 0) r.size.width = 0;
    if (r.size.height < 0) r.size.height = 0;
    return r;
}

/* CLOKE_SCROLL_TRACE: angeforderte Rollpunkte der Clip-Ansichten (Fehlersuche, höchstens 400) */
static IMP o_scroll_to, o_set_origin;
static void trace_pt(id self, const char *what, NSPoint p) {
    static int n;
    if (n++ < 400)
        NSLog(@"[clokeos-scroll] %s %s doc=%s frame=%@ -> %.1f,%.1f", object_getClassName(self), what, object_getClassName([self documentView]),
              NSStringFromRect([self frame]), p.x, p.y);
}
static void cv_scroll_to(id self, SEL s, NSPoint p) { trace_pt(self, "scrollToPoint", p); ((void (*)(id, SEL, NSPoint)) o_scroll_to)(self, s, p); }
static void cv_set_origin(id self, SEL s, NSPoint p) { trace_pt(self, "setBoundsOrigin", p); ((void (*)(id, SEL, NSPoint)) o_set_origin)(self, s, p); }

#include <execinfo.h>
#include <dlfcn.h>
static IMP o_sv_set_frame;
static void sv_set_frame(NSScrollView *self, SEL s, NSRect r) {
    static int n;
    if (n < 300 && ![[self documentView] isKindOfClass: [NSTableView class]]) {
        n++;
        void *pcs[6]; int k = backtrace(pcs, 6);
        NSMutableString *bt = [NSMutableString string];
        for (int i = 1; i < k; i++) { Dl_info di; if (dladdr(pcs[i], &di) && di.dli_sname) [bt appendFormat: @" < %s", di.dli_sname]; }
        NSLog(@"[clokeos-scroll] %s setFrame %@ doc=%s%@", object_getClassName(self), NSStringFromRect(r), object_getClassName([self documentView]), bt);
    }
    ((void (*)(id, SEL, NSRect)) o_sv_set_frame)(self, s, r);
}

__attribute__((constructor)) static void clk_scrollinsets_init(void) {
    if (getenv("CLOKE_SCROLL_TRACE")) {
        Method fm = class_getInstanceMethod([NSScrollView class], @selector(setFrame:));
        o_sv_set_frame = method_getImplementation(fm);
        class_addMethod([NSScrollView class], @selector(setFrame:), (IMP) sv_set_frame, method_getTypeEncoding(fm));
        Class V = [NSClipView class];
        o_scroll_to = method_setImplementation(class_getInstanceMethod(V, @selector(scrollToPoint:)), (IMP) cv_scroll_to);
        Method m = class_getInstanceMethod(V, @selector(setBoundsOrigin:));
        if (m) { o_set_origin = method_getImplementation(m); class_addMethod(V, @selector(setBoundsOrigin:), (IMP) cv_set_origin, method_getTypeEncoding(m)); }
    }
    Class C = [NSScrollView class];
    const char *ei = "{NSEdgeInsets=dddd}";
    char t1[64], t2[64];
    snprintf(t1, sizeof t1, "%s@:", ei);
    snprintf(t2, sizeof t2, "v@:%s", ei);
    class_addMethod(C, sel_registerName("contentInsets"), (IMP) sv_content_insets, t1);
    class_addMethod(C, sel_registerName("setContentInsets:"), (IMP) sv_set_content_insets, t2);
    class_addMethod(C, sel_registerName("scrollerInsets"), (IMP) sv_scroller_insets, t1);
    class_addMethod(C, sel_registerName("setScrollerInsets:"), (IMP) sv_set_scroller_insets, t2);
    class_addMethod(C, sel_registerName("automaticallyAdjustsContentInsets"), (IMP) sv_auto, "c@:");
    class_addMethod(C, sel_registerName("setAutomaticallyAdjustsContentInsets:"), (IMP) sv_set_auto, "v@:c");
    Method m = class_getInstanceMethod(C, sel_registerName("clipViewFrame"));
    if (m) o_clip_frame = method_setImplementation(m, (IMP) sv_clip_frame);
}
