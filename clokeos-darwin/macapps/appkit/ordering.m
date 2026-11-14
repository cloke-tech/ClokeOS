/* Fensterreihenfolge wie bei macOS beim Ziehen (docs/fundus.md, "Drag and drop"):
 *
 * -[NSApplication preventWindowOrdering] (Darling: nicht umgesetzt). Ein Klick in ein Hintergrundfenster hebt es bei macOS
 * erst beim Loslassen an, und gar nicht, wenn dazwischen etwas gezogen wurde: Die Ansicht ruft dann preventWindowOrdering.
 * Unter ClokeOS hebt CloakServer das Fenster beim Loslassen an (pending_focus). Hier bekommt das X11-Fenster des
 * aktuellen Ereignisses die Eigenschaft _CLOKE_PREVENT_ORDERING (CARDINAL 1); CloakServer lässt das Anheben dann aus
 * und löscht die Eigenschaft.
 *
 * +[NSWindow windowNumberAtPoint:belowWindowWithWindowNumber:] (Darling: nicht umgesetzt): oberstes Fenster dieses
 * Programms an einem Bildschirmpunkt, nach der echten Stapelreihenfolge des X-Servers (CloakServers Rahmen liegen
 * direkt unter root). Liegt dort ein Fenster eines anderen Programms, ist das Ergebnis 0. Ziehziele brauchen das,
 * Darlings -orderedWindows ist nicht geordnet.
 *
 * -[NSWindow clk_setShapeRects:] (eigene Erweiterung): Darlings X11-Fenster haben keinen Alphakanal, ein durchsichtiges
 * randloses Fenster (Ziehbild) wäre schwarz hinterlegt. Die Rechtecke (NSValue, Fensterpunkte, Ursprung oben links)
 * werden mit XShape zur sichtbaren Fläche des Fensters; der Rest ist ausgeschnitten. Fundus übergibt die Pixel seines
 * Ziehbilds, deren Alpha deckend genug ist. */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import "X11Display.h"
#import "X11Window.h"
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <stdlib.h>

@interface X11Display (ClokeOrdering)
- (id)windowForID:(XID)i;
@end

static void ck_preventWindowOrdering(id self, SEL _cmd) {
    NSWindow *w = [[NSApp currentEvent] window];
    if (!w) w = [NSApp keyWindow];
    id pw = [w respondsToSelector: @selector(platformWindow)] ? [w performSelector: @selector(platformWindow)] : nil;
    if (![pw isKindOfClass: [X11Window class]]) return;
    Display *dpy = [(X11Display *)[NSDisplay currentDisplay] display];
    long one = 1;
    XChangeProperty(dpy, [(X11Window *)pw windowHandle], XInternAtom(dpy, "_CLOKE_PREVENT_ORDERING", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&one, 1);
    XFlush(dpy);
}

/* Fenster dieses Programms in w oder darunter (CloakServer: Rahmen -> Programmfenster), sonst nil */
static NSWindow *ck_ownWindow(X11Display *d, Display *dpy, Window w, int depth) {
    id xw = [d windowForID: w];
    if ([xw isKindOfClass: [X11Window class]]) {
        id del = [(X11Window *)xw delegate];
        if ([del isKindOfClass: [NSWindow class]]) return del;
    }
    if (depth >= 2) return nil;
    Window r, p, *kids = NULL;
    unsigned n = 0;
    NSWindow *found = nil;
    if (XQueryTree(dpy, w, &r, &p, &kids, &n)) {
        for (unsigned i = n; i-- > 0 && !found;) found = ck_ownWindow(d, dpy, kids[i], depth + 1);
        if (kids) XFree(kids);
    }
    return found;
}

static NSInteger ck_windowNumberAtPoint(id self, SEL _cmd, NSPoint pt, NSInteger below) {
    X11Display *d = (X11Display *)[NSDisplay currentDisplay];
    if (![d isKindOfClass: [X11Display class]]) return 0;
    Display *dpy = [d display];
    Window root = DefaultRootWindow(dpy), r, p, *kids = NULL;
    unsigned n = 0;
    int x = (int)pt.x, y = DisplayHeight(dpy, DefaultScreen(dpy)) - (int)pt.y;   /* Cocoa: Ursprung unten links */
    NSInteger result = 0;
    BOOL skipping = below > 0;
    if (!XQueryTree(dpy, root, &r, &p, &kids, &n)) return 0;
    for (unsigned i = n; i-- > 0;) {   /* von oben nach unten */
        XWindowAttributes a;
        if (!XGetWindowAttributes(dpy, kids[i], &a) || a.map_state != IsViewable || a.class == InputOnly) continue;
        if (x < a.x || y < a.y || x >= a.x + a.width + 2 * a.border_width || y >= a.y + a.height + 2 * a.border_width) continue;
        NSWindow *w = ck_ownWindow(d, dpy, kids[i], 0);
        if (w && [w ignoresMouseEvents]) continue;   /* Ziehbild und Ähnliches */
        if (skipping) {
            if (w && [w windowNumber] == below) skipping = NO;
            continue;
        }
        result = w ? [w windowNumber] : 0;   /* fremdes Fenster verdeckt alles darunter */
        break;
    }
    if (kids) XFree(kids);
    return result;
}

static void ck_setShapeRects(NSWindow *self, SEL _cmd, NSArray *rects) {
    id pw = [self respondsToSelector: @selector(platformWindow)] ? [self performSelector: @selector(platformWindow)] : nil;
    if (![pw isKindOfClass: [X11Window class]]) return;
    Display *dpy = [(X11Display *)[NSDisplay currentDisplay] display];
    int n = (int)[rects count];
    XRectangle *xr = calloc(n ? n : 1, sizeof *xr);
    for (int i = 0; i < n; i++) {
        NSRect r = [[rects objectAtIndex: i] rectValue];
        xr[i].x = (short)r.origin.x; xr[i].y = (short)r.origin.y;
        xr[i].width = (unsigned short)r.size.width; xr[i].height = (unsigned short)r.size.height;
    }
    XShapeCombineRectangles(dpy, [(X11Window *)pw windowHandle], ShapeBounding, 0, 0, xr, n, ShapeSet, Unsorted);
    XFlush(dpy);
    free(xr);
}

__attribute__((constructor)) static void ck_ordering_install(void) {
    class_replaceMethod([NSApplication class], @selector(preventWindowOrdering), (IMP)ck_preventWindowOrdering, "v@:");
    class_replaceMethod(object_getClass([NSWindow class]), @selector(windowNumberAtPoint:belowWindowWithWindowNumber:),
                        (IMP)ck_windowNumberAtPoint, "q@:{CGPoint=dd}q");
    class_addMethod([NSWindow class], @selector(clk_setShapeRects:), (IMP)ck_setShapeRects, "v@:@");
}
