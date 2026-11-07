/* Titelleiste von Mac-Fenstern unter CloakServer (docs/titlebar.md).
 *
 * Bei macOS hat ein modernes Fenster oft keine eigene Titelleiste: mit NSWindowStyleMaskFullSizeContentView reicht der Inhalt bis
 * an die Oberkante, mit titlebarAppearsTransparent und titleVisibility = NSWindowTitleHidden zeichnet AppKit dort nichts, und eine
 * NSToolbar im Stil "unified" steht in der Titelleiste. Die Ampelknöpfe liegen dann über dem Inhalt des Programms.
 *
 * Cocotron kennt titlebarAppearsTransparent, titleVisibility und toolbarStyle nicht (die Aufrufe gingen ins Leere). Hier bekommt
 * NSWindow diese Eigenschaften, und das X11-Fenster trägt das Ergebnis als Eigenschaft _CLOKE_TITLEBAR (CARDINAL[6]) für
 * CloakServer:
 *   [0] Stil   0 = CloakServer zeichnet eine Titelleiste, 1 = das Programm zeichnet die Fläche selbst (CloakServer legt nur die
 *              Ampelknöpfe darüber), 2 = das Programm zeichnet Titelleiste und Fensterknöpfe selbst
 *   [1] Höhe der Fläche, die das Programm oben zeichnet (Ampelknöpfe senkrecht in deren Mitte); 0 = 32 wie bei macOS 27
 *   [2] Merkmale: 1 FullSizeContentView, 2 transparent, 4 Titel ausgeblendet, 8 Werkzeugleiste sichtbar, 16 unified (styleMask)
 *   [3] Mitte des ersten Ampelknopfs von links; 0 = 16 wie bei macOS 27
 *   [4] ausgeblendete Ampelknöpfe: Bit 0 Schließen, 1 Ins Dock, 2 Zoomen (standardWindowButton: … setHidden:); Bits 3–5 dieselben
 *       grau und ohne Wirkung, weil styleMask Closable, Miniaturizable oder Resizable fehlt (wie bei macOS)
 *   [5] Mitte der Ampelknöpfe von oben; 0 = Mitte der Fläche
 * standardWindowButton: liefert je Fenster drei unsichtbare Platzhalter mit den Maßen von macOS 27 (14 × 14 bei x = 9, 32, 55);
 * Ausblenden und Verschieben gehen über die Eigenschaft an CloakServer, das die Knöpfe zeichnet.
 * Neu geschrieben wird, wenn sich styleMask, Werkzeugleiste, Transparenz, Titelsichtbarkeit oder toolbarStyle ändern.
 *
 * Ohne Titelleiste von CloakServer verschiebt das Programm das Fenster selbst: Ein Klick in die Fläche oben auf eine Ansicht, die
 * die Maus nicht braucht (mouseDownCanMoveWindow, bei Apple: nicht deckend und kein Bedienelement), schickt _NET_WM_MOVERESIZE an
 * CloakServer; ein Doppelklick zoomt wie bei macOS. */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import "X11Display.h"
#import "X11Window.h"
#include <X11/Xatom.h>
#include <stdlib.h>
#include <dlfcn.h>

#define CK_FULLSIZE 0x8000 /* NSWindowStyleMaskFullSizeContentView */
#define CK_TITLED 1
#define CK_DEFAULT_H 32    /* macOS 27: Titelleiste eines Dokumentfensters (auf dem Mac nachgemessen) */

static const void *const kPublished = &kPublished, *const kTransparent = &kTransparent, *const kTitleHidden = &kTitleHidden, *const kToolbarStyle = &kToolbarStyle;

static NSInteger ck_get(id w, const void *key) {
    NSNumber *n = objc_getAssociatedObject(w, key);
    return n ? [n integerValue] : 0;
}

void ClokeTitlebarPublish(NSWindow *w);

static void ck_set(id w, const void *key, NSInteger v) {
    objc_setAssociatedObject(w, key, [NSNumber numberWithInteger: v], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    ClokeTitlebarPublish(w);
}

static BOOL ck_transparent(id self, SEL _cmd) { return ck_get(self, kTransparent) != 0; }
static void ck_setTransparent(id self, SEL _cmd, BOOL v) { ck_set(self, kTransparent, v); }
static NSInteger ck_titleVisibility(id self, SEL _cmd) { return ck_get(self, kTitleHidden); }
static void ck_setTitleVisibility(id self, SEL _cmd, NSInteger v) { ck_set(self, kTitleHidden, v); }
static NSInteger ck_toolbarStyle(id self, SEL _cmd) { return ck_get(self, kToolbarStyle); }
static void ck_setToolbarStyle(id self, SEL _cmd, NSInteger v) { ck_set(self, kToolbarStyle, v); }

/* Höhe der sichtbaren Werkzeugleiste in Pixeln, 0 ohne */
static int ck_toolbar_h(NSWindow *w) {
    NSToolbar *tb = [w toolbar];
    if (!tb || ![tb isVisible]) return 0;
    NSView *v = [tb respondsToSelector: @selector(_view)] ? [tb performSelector: @selector(_view)] : nil;
    if (!v || [v isHidden]) return 0;
    return (int)[v frame].size.height;
}

/* Platzhalter für standardWindowButton: (Cocotron gab nil zurück; Logic Pro blendet so die Ampel seines Startfensters aus) */
@interface CKWindowButton : NSButton {
@public
    NSWindow *_ckWindow; /* nicht gehalten: das Fenster hält den Knopf */
}
@end

static const void *const kButtons = &kButtons;

@implementation CKWindowButton
- (void) setHidden: (BOOL) h {
    [super setHidden: h];
    ClokeTitlebarPublish(_ckWindow);
}
- (void) setFrameOrigin: (NSPoint) p {
    [super setFrameOrigin: p];
    ClokeTitlebarPublish(_ckWindow);
}
- (void) setFrame: (NSRect) r {
    [super setFrame: r];
    ClokeTitlebarPublish(_ckWindow);
}
@end

static NSArray *ck_buttons(NSWindow *w, BOOL make) {
    NSArray *a = objc_getAssociatedObject(w, kButtons);
    if (a || !make) return a;
    NSMutableArray *m = [NSMutableArray array];
    for (int i = 0; i < 3; i++) {
        CKWindowButton *b = [[CKWindowButton alloc] initWithFrame: NSMakeRect(9 + 23 * i, 9, 14, 14)];
        b->_ckWindow = w;
        [m addObject: b];
        [b release];
    }
    objc_setAssociatedObject(w, kButtons, m, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return m;
}

/* NSWindowCloseButton 0, NSWindowMiniaturizeButton 1, NSWindowZoomButton 2; die übrigen (Werkzeugleiste, Dokumentsymbol) gibt es nicht */
static id ck_standardWindowButton(NSWindow *self, SEL _cmd, NSInteger which) {
    return which >= 0 && which <= 2 ? [ck_buttons(self, YES) objectAtIndex: which] : nil;
}

/* NSAlert: bei macOS ohne Titelleiste und ohne Ampelknöpfe */
static const void *const kAlert = &kAlert;
static BOOL ck_is_alert(NSWindow *w) { return objc_getAssociatedObject(w, kAlert) != nil; }

/* aus X11Window initWithDelegate: (patches/…/X11Window.m.sed), bevor das Fenster gezeigt wird */
void ClokeTitlebarPublishX(NSWindow *w, Display *dpy, Window xw) {
    if (!w || !dpy || !xw || !([w styleMask] & CK_TITLED)) return;

    BOOL full = ([w styleMask] & CK_FULLSIZE) != 0, transparent = ck_get(w, kTransparent) != 0,
         hidden = ck_get(w, kTitleHidden) == 1; /* NSWindowTitleHidden */
    NSInteger style = ck_get(w, kToolbarStyle); /* 0 automatic, 1 expanded, 2 preference, 3 unified, 4 unifiedCompact */
    int tbh = ck_toolbar_h(w);
    long v[6] = {0, 0, 0, 0, 0, 0};
    /* 16: NSUnifiedTitleAndToolbarWindowMask, Titelzeile und Werkzeugleiste eine Fläche ohne Trennlinie */
    v[2] = (full ? 1 : 0) | (transparent ? 2 : 0) | (hidden ? 4 : 0) | (tbh ? 8 : 0) | ([w styleMask] & 0x1000 ? 16 : 0);
    /* Das Programm zeichnet die Fläche oben selbst: Inhalt bis an die Oberkante, oder eine Werkzeugleiste in der Titelleiste
     * (unified, oder ausgeblendeter Titel); Ampelknöpfe dann wie bei unified ab x = 26. Sonst steht die Werkzeugleiste unter einer
     * Titelzeile ("expanded"): so zeigt macOS 27 Logic Pros Projektfenster (auf dem Mac verglichen, docs/titlebar.md). */
    if (full || (tbh && (hidden || style == 3 || style == 4))) {
        v[0] = 1;
        v[1] = tbh ? tbh : CK_DEFAULT_H;
        if (tbh) v[3] = 26;
    }
    NSUInteger m = [w styleMask];
    if (!(m & NSClosableWindowMask)) v[4] |= 8;
    if (!(m & NSMiniaturizableWindowMask)) v[4] |= 16;
    if (!(m & NSResizableWindowMask)) v[4] |= 32;
    NSArray *bs = ck_buttons(w, NO);
    if (bs) {
        CKWindowButton *b0 = [bs objectAtIndex: 0];
        NSRect r = [b0 frame];
        for (int i = 0; i < 3; i++)
            if ([[bs objectAtIndex: i] isHidden]) v[4] |= 1 << i;
        if (r.origin.x != 9) v[3] = (long)(NSMidX(r) + 0.5);
        /* Ursprung unten links in der Titelleiste wie bei Apple: Mitte von oben = Höhe - Mitte */
        if (r.origin.y != 9) v[5] = (long)((v[1] ? v[1] : CK_DEFAULT_H) - NSMidY(r) + 0.5);
    }
    if (ck_is_alert(w)) v[0] = 2;
    NSInteger was = ck_get(w, kPublished);
    objc_setAssociatedObject(w, kPublished, [NSNumber numberWithInteger: v[0] == 1 ? v[1] : 0], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    Atom a = XInternAtom(dpy, "_CLOKE_TITLEBAR", False);
    XChangeProperty(dpy, xw, a, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)v, 6);
    XFlush(dpy);
    /* Werkzeugleiste neu anordnen, wenn sich ihr linker Rand (Platz für die Ampelknöpfe) ändert */
    NSView *tv = tbh && [[w toolbar] respondsToSelector: @selector(_view)] ? [[w toolbar] performSelector: @selector(_view)] : nil;
    if (tv && (was > 0) != (v[0] == 1) && [tv respondsToSelector: @selector(layoutViews)]) {
        [tv performSelector: @selector(layoutViews)];
        [tv setNeedsDisplay: YES];
    }
    if (getenv("CLOKE_TRACE"))
        fprintf(stderr, "[clokeos-titlebar] %s mask=0x%lx tb=%d transparent=%d hidden=%d style=%ld -> %ld/%ld x=%ld hide=%ld y=%ld\n",
                [[w title] UTF8String] ?: "", (unsigned long)[w styleMask], tbh, transparent, hidden, (long)style, v[0], v[1], v[3],
                v[4], v[5]);
}

void ClokeTitlebarPublish(NSWindow *w) {
    id pw = [w respondsToSelector: @selector(platformWindow)] ? [w performSelector: @selector(platformWindow)] : nil;
    if (![pw isKindOfClass: [X11Window class]]) return;
    ClokeTitlebarPublishX(w, [(X11Display *)[NSDisplay currentDisplay] display], [(X11Window *)pw windowHandle]);
}

/* Apple: eine Ansicht, die nicht deckend ist, lässt einen Klick zum Verschieben durch; Bedienelemente nicht.
 * (Cocotrons NSView meldete hier "nicht umgesetzt".) */
static BOOL ck_canMove(id self, SEL _cmd) { return ![self isOpaque]; }
static BOOL ck_cannotMove(id self, SEL _cmd) { return NO; }

/* EWMH _NET_WM_MOVERESIZE an CloakServer: verschieben ab der Lage des Zeigers */
static void ck_move(NSWindow *w, Display *dpy, Window xw) {
    Window root = DefaultRootWindow(dpy), rr, cw;
    int rx, ry, wx, wy;
    unsigned mask;
    if (!XQueryPointer(dpy, xw, &rr, &cw, &rx, &ry, &wx, &wy, &mask)) return;
    XUngrabPointer(dpy, CurrentTime);
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = xw;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_MOVERESIZE", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = rx;
    ev.xclient.data.l[1] = ry;
    ev.xclient.data.l[2] = 8; /* _NET_WM_MOVERESIZE_MOVE */
    ev.xclient.data.l[3] = Button1;
    ev.xclient.data.l[4] = 1;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);
}

static IMP orig_sendEvent;
static void ck_sendEvent(NSWindow *self, SEL _cmd, NSEvent *ev) {
    NSInteger h = [ev type] == NSLeftMouseDown ? ck_get(self, kPublished) : 0;
    if (h > 0) {
        NSPoint p = [ev locationInWindow];
        CGFloat top = [self frame].size.height - p.y;
        NSView *frameView = [[self contentView] superview] ?: [self contentView];
        NSView *v = top >= 0 && top < h ? [frameView hitTest: p] : nil;
        BOOL move = v != nil;
        for (NSView *x = v; x && move; x = [x superview]) {
            if (x == frameView) break;
            if (![x mouseDownCanMoveWindow]) move = NO;
        }
        id pw = [self respondsToSelector: @selector(platformWindow)] ? [self performSelector: @selector(platformWindow)] : nil;
        if (move && [pw isKindOfClass: [X11Window class]]) {
            if ([ev clickCount] == 2) [self performZoom: nil];
            else ck_move(self, [(X11Display *)[NSDisplay currentDisplay] display], [(X11Window *)pw windowHandle]);
            return;
        }
    }
    ((void (*)(id, SEL, NSEvent *))orig_sendEvent)(self, _cmd, ev);
}

/* NSToolbarView (patches/…/NSToolbarView.m.sed): linker Rand der Werkzeugleiste; in der Titelleiste Platz für die Ampelknöpfe
 * wie bei macOS 27 unified (erstes Element bei etwa x = 86) */
static CGFloat ck_toolbar_margin(NSView *v) {
    NSWindow *w = [v window];
    return w && ck_get(w, kPublished) > 0 ? 86 : 5;
}

static IMP orig_tbSize;
static void ck_tbSize(id self, SEL _cmd, CGFloat old) {
    ((void (*)(id, SEL, CGFloat))orig_tbSize)(self, _cmd, old);
    ClokeTitlebarPublish(self);
}

/* vorhandene Methode umleiten: erst die alte, dann neu veröffentlichen */
static IMP orig_setStyleMask, orig_setToolbar, orig_tbSetVisible, orig_alertInit;
static id ck_alertInit(id self, SEL _cmd) {
    self = ((id (*)(id, SEL))orig_alertInit)(self, _cmd);
    NSWindow *w = [self window];
    if (w) {
        objc_setAssociatedObject(w, kAlert, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        ClokeTitlebarPublish(w);
    }
    return self;
}
static void ck_setStyleMask(id self, SEL _cmd, NSUInteger m) {
    ((void (*)(id, SEL, NSUInteger))orig_setStyleMask)(self, _cmd, m);
    ClokeTitlebarPublish(self);
}
static void ck_setToolbar(id self, SEL _cmd, id tb) {
    ((void (*)(id, SEL, id))orig_setToolbar)(self, _cmd, tb);
    ClokeTitlebarPublish(self);
}
static void ck_tbSetVisible(id self, SEL _cmd, BOOL vis) {
    ((void (*)(id, SEL, BOOL))orig_tbSetVisible)(self, _cmd, vis);
    for (NSWindow *w in [NSApp windows])
        if ([w toolbar] == self) ClokeTitlebarPublish(w);
}

static IMP hook(Class c, SEL s, IMP neu) {
    Method m = class_getInstanceMethod(c, s);
    if (!m) return NULL;
    IMP old = method_getImplementation(m);
    if (!class_addMethod(c, s, neu, method_getTypeEncoding(m))) method_setImplementation(m, neu);
    return old;
}

__attribute__((constructor)) static void ck_titlebar_init(void) {
    Class W = objc_getClass("NSWindow"), T = objc_getClass("NSToolbar");
    if (!W) return;
    class_addMethod(W, @selector(titlebarAppearsTransparent), (IMP)ck_transparent, "c@:");
    class_addMethod(W, @selector(setTitlebarAppearsTransparent:), (IMP)ck_setTransparent, "v@:c");
    class_addMethod(W, sel_registerName("titleVisibility"), (IMP)ck_titleVisibility, "q@:");
    class_addMethod(W, sel_registerName("setTitleVisibility:"), (IMP)ck_setTitleVisibility, "v@:q");
    class_addMethod(W, sel_registerName("toolbarStyle"), (IMP)ck_toolbarStyle, "q@:");
    class_addMethod(W, sel_registerName("setToolbarStyle:"), (IMP)ck_setToolbarStyle, "v@:q");
    orig_setStyleMask = hook(W, @selector(setStyleMask:), (IMP)ck_setStyleMask);
    orig_sendEvent = hook(W, @selector(sendEvent:), (IMP)ck_sendEvent);
    hook(W, @selector(standardWindowButton:), (IMP)ck_standardWindowButton);
    orig_tbSize = hook(W, sel_registerName("_toolbarSizeDidChangeFromOldHeight:"), (IMP)ck_tbSize);
    /* nur mit AppKit aus patches/…/NSToolbarView.m.sed vorhanden */
    CGFloat (**margin)(NSView *) = dlsym(RTLD_DEFAULT, "ClokeToolbarLeftMargin");
    if (margin) *margin = ck_toolbar_margin;
    Class V = objc_getClass("NSView");
    Method mv = V ? class_getInstanceMethod(V, @selector(mouseDownCanMoveWindow)) : NULL;
    if (mv) method_setImplementation(mv, (IMP)ck_canMove);
    else if (V) class_addMethod(V, @selector(mouseDownCanMoveWindow), (IMP)ck_canMove, "c@:");
    static const char *const fixed[] = {"NSControl", "NSText", "NSScrollView", "NSTableView", "NSOutlineView"};
    for (int i = 0; i < 5; i++)
        if (objc_getClass(fixed[i])) class_addMethod(objc_getClass(fixed[i]), @selector(mouseDownCanMoveWindow), (IMP)ck_cannotMove, "c@:");
    orig_setToolbar = hook(W, @selector(setToolbar:), (IMP)ck_setToolbar);
    Class A = objc_getClass("NSAlert");
    if (A) orig_alertInit = hook(A, @selector(init), (IMP)ck_alertInit);
    if (T) orig_tbSetVisible = hook(T, @selector(setVisible:), (IMP)ck_tbSetVisible);
}
