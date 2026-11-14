/* Dateien in andere Programme ziehen: Quelle des X-Drag-and-Drop-Protokolls (XDND 5, freedesktop.org) für Darlings X11-Backend.
 *
 * Darling kann zwischen Programmen nicht ziehen. Fundus (docs/fundus.md) zieht innerhalb seiner Fenster selbst und fragt, sobald der
 * Zeiger über einem fremden Fenster ist, diese Erweiterung von NSApplication:
 *   -clk_xdndUpdateFiles:atScreenPoint:copy:ignoring:  Ziel unter dem Zeiger suchen, XdndEnter/Position/Leave schicken.
 *                                                      0 = kein XDND-Ziel (oder eigenes Fenster), 1 = Ziel lehnt ab, 2 = Ziel nimmt an
 *   -clk_xdndDrop                                      ablegen (XdndDrop), bis 3 s auf XdndFinished warten; YES = erledigt
 *   -clk_xdndCancel                                    XdndLeave
 *   -clk_xdndAction                                    vom Ziel gewählte Aktion: 0 kopieren, 1 bewegen, 2 andere
 * Quelle ist ein eigenes, unsichtbares X-Fenster; die Antworten des Ziels (XdndStatus, XdndFinished) und die Anfrage nach den Daten
 * (SelectionRequest auf XdndSelection, text/uri-list) fängt ein Einschub vor -[X11Display postXEvent:] ab.
 * Ziele: CloakServers Dock (Programm = damit öffnen, Papierkorb), GTK- und andere X-Programme. */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import "X11Display.h"
#import "X11Window.h"
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

@interface X11Display (ClokeXdnd)
- (id)windowForID:(XID)i;
- (void)postXEvent:(XEvent *)ev;
@end

static Display *dpy;
static Window src_win, target, proxy;   /* proxy: Fenster, an das die Nachrichten gehen (XdndProxy oder target) */
static Atom A_aware, A_proxy, A_enter, A_position, A_status, A_leave, A_drop, A_finished, A_selection, A_copy, A_move,
        A_uri, A_plain, A_targets, A_type_list;
static int target_version, status;      /* status: 1 abgelehnt, 2 angenommen */
static Atom action_reply;
static BOOL waiting, pending, finished;
static int last_x = -1, last_y = -1;
static Atom want_action;
static NSData *uri_data;
static IMP o_post;

static void atoms(void) {
    if (A_aware) return;
    A_aware = XInternAtom(dpy, "XdndAware", False);
    A_proxy = XInternAtom(dpy, "XdndProxy", False);
    A_enter = XInternAtom(dpy, "XdndEnter", False);
    A_position = XInternAtom(dpy, "XdndPosition", False);
    A_status = XInternAtom(dpy, "XdndStatus", False);
    A_leave = XInternAtom(dpy, "XdndLeave", False);
    A_drop = XInternAtom(dpy, "XdndDrop", False);
    A_finished = XInternAtom(dpy, "XdndFinished", False);
    A_selection = XInternAtom(dpy, "XdndSelection", False);
    A_copy = XInternAtom(dpy, "XdndActionCopy", False);
    A_move = XInternAtom(dpy, "XdndActionMove", False);
    A_uri = XInternAtom(dpy, "text/uri-list", False);
    A_plain = XInternAtom(dpy, "text/plain", False);
    A_targets = XInternAtom(dpy, "TARGETS", False);
    A_type_list = XInternAtom(dpy, "XdndTypeList", False);
}

static long card(Window w, Atom a, Atom type) {
    Atom t; int f; unsigned long n, after; unsigned char *d = NULL;
    long v = -1;
    if (XGetWindowProperty(dpy, w, a, 0, 1, False, type, &t, &f, &n, &after, &d) == Success && d && n == 1) v = *(long *)d;
    if (d) XFree(d);
    return v;
}

/* XdndAware im Fenster w oder darunter (Rahmen von CloakServer -> Programmfenster) an der Stelle x, y (root) */
static Window aware_at(Window w, int x, int y, int depth) {
    if (card(w, A_aware, XA_ATOM) >= 0) return w;
    if (depth >= 3) return None;
    Window r, p, *kids = NULL; unsigned n = 0, i;
    Window found = None;
    if (!XQueryTree(dpy, w, &r, &p, &kids, &n)) return None;
    for (i = n; i-- > 0 && !found;) {
        XWindowAttributes a; int wx, wy; Window ch;
        if (!XGetWindowAttributes(dpy, kids[i], &a) || a.map_state != IsViewable || a.class == InputOnly) continue;
        XTranslateCoordinates(dpy, kids[i], DefaultRootWindow(dpy), 0, 0, &wx, &wy, &ch);
        if (x >= wx && y >= wy && x < wx + a.width && y < wy + a.height) found = aware_at(kids[i], x, y, depth + 1);
    }
    if (kids) XFree(kids);
    return found;
}

static BOOL own_window(X11Display *d, Window w, int depth) {
    if ([[d windowForID: w] isKindOfClass: [X11Window class]]) return YES;
    if (depth >= 2) return NO;
    Window r, p, *kids = NULL; unsigned n = 0, i; BOOL own = NO;
    if (XQueryTree(dpy, w, &r, &p, &kids, &n)) {
        for (i = 0; i < n && !own; i++) own = own_window(d, kids[i], depth + 1);
        if (kids) XFree(kids);
    }
    return own;
}

/* oberstes Fenster an x, y (root) ohne das Ziehbild; eigenes Fenster -> None */
static Window target_at(X11Display *d, int x, int y, Window ignore) {
    Window root = DefaultRootWindow(dpy), r, p, *kids = NULL; unsigned n = 0, i;
    Window t = None;
    if (!XQueryTree(dpy, root, &r, &p, &kids, &n)) return None;
    for (i = n; i-- > 0;) {
        XWindowAttributes a;
        if (kids[i] == ignore || kids[i] == src_win) continue;
        if (!XGetWindowAttributes(dpy, kids[i], &a) || a.map_state != IsViewable || a.class == InputOnly) continue;
        if (x < a.x || y < a.y || x >= a.x + a.width + 2 * a.border_width || y >= a.y + a.height + 2 * a.border_width) continue;
        if (!own_window(d, kids[i], 0)) t = aware_at(kids[i], x, y, 0);
        break;
    }
    if (kids) XFree(kids);
    return t;
}

static void send(Atom type, long l1, long l2, long l3, long l4) {
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = target;
    e.xclient.message_type = type;
    e.xclient.format = 32;
    e.xclient.data.l[0] = src_win;
    e.xclient.data.l[1] = l1;
    e.xclient.data.l[2] = l2;
    e.xclient.data.l[3] = l3;
    e.xclient.data.l[4] = l4;
    XSendEvent(dpy, proxy, False, NoEventMask, &e);
}

static void send_position(void) {
    send(A_position, 0, (last_x << 16) | (last_y & 0xffff), CurrentTime, want_action);
    waiting = YES;
    pending = NO;
    XFlush(dpy);
}

static void leave(void) {
    if (target) send(A_leave, 0, 0, 0, 0);
    target = proxy = None;
    status = 0;
    waiting = pending = NO;
    XFlush(dpy);
}

/* Einschub vor Darlings Ereignisverteilung: Antworten des Ziels und die Datenanfrage */
static void x_post(id self, SEL _cmd, XEvent *ev) {
    if (src_win && ev->xany.window == src_win) {
        if (ev->type == ClientMessage && ev->xclient.message_type == A_status && (Window)ev->xclient.data.l[0] == target) {
            status = (ev->xclient.data.l[1] & 1) ? 2 : 1;
            if (getenv("CLOKE_XDND_TRACE")) fprintf(stderr, "[xdnd] XdndStatus %d\n", status);
            action_reply = ev->xclient.data.l[4];
            waiting = NO;
            if (pending) send_position();
            return;
        }
        if (ev->type == ClientMessage && ev->xclient.message_type == A_finished) {
            finished = YES;
            if (target_version >= 5) action_reply = ev->xclient.data.l[2];
            return;
        }
        if (ev->type == SelectionRequest) {
            XSelectionRequestEvent *r = &ev->xselectionrequest;
            XEvent n;
            memset(&n, 0, sizeof n);
            n.xselection.type = SelectionNotify;
            n.xselection.requestor = r->requestor;
            n.xselection.selection = r->selection;
            n.xselection.target = r->target;
            n.xselection.time = r->time;
            n.xselection.property = r->property ? r->property : r->target;
            if (r->target == A_targets) {
                Atom t[3] = { A_targets, A_uri, A_plain };
                XChangeProperty(dpy, r->requestor, n.xselection.property, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 3);
            } else if ((r->target == A_uri || r->target == A_plain) && uri_data) {
                XChangeProperty(dpy, r->requestor, n.xselection.property, r->target, 8, PropModeReplace,
                                (const unsigned char *)[uri_data bytes], (int)[uri_data length]);
            } else n.xselection.property = None;
            XSendEvent(dpy, r->requestor, False, NoEventMask, &n);
            XFlush(dpy);
            return;
        }
        return;
    }
    ((void (*)(id, SEL, XEvent *))o_post)(self, _cmd, ev);
}

/* Antworten liegen oft schon in Xlibs Warteschlange (unsere eigenen Abfragen lesen den Socket leer); Darlings Runloop sieht dann
 * keine neuen Daten und verteilt sie nicht. Deshalb hier selbst verteilen. */
static void drain(void) {
    X11Display *d = (X11Display *)[NSDisplay currentDisplay];
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        [d postXEvent: &e];
    }
}

static NSData *uri_list(NSArray *paths) {
    NSMutableString *s = [NSMutableString string];
    for (NSString *p in paths) {
        const char *c = [p UTF8String];
        [s appendString: @"file://"];
        for (; *c; c++) {   /* RFC 3986: alles außer unreservierten Zeichen und '/' kodieren */
            unsigned char ch = (unsigned char)*c;
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("-._~/", ch))
                [s appendFormat: @"%c", ch];
            else
                [s appendFormat: @"%%%02X", ch];
        }
        [s appendString: @"\r\n"];
    }
    return [s dataUsingEncoding: NSUTF8StringEncoding];
}

static BOOL setup(void) {
    X11Display *d = (X11Display *)[NSDisplay currentDisplay];
    if (![d isKindOfClass: [X11Display class]]) return NO;
    if (src_win) return YES;
    dpy = [d display];
    atoms();
    XSetWindowAttributes a;
    a.override_redirect = True;
    src_win = XCreateWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, 0, InputOnly, CopyFromParent, CWOverrideRedirect, &a);
    o_post = method_setImplementation(class_getInstanceMethod([X11Display class], @selector(postXEvent:)), (IMP)x_post);
    return YES;
}

@interface NSApplication (ClokeXdnd)
@end
@implementation NSApplication (ClokeXdnd)
- (NSInteger)clk_xdndUpdateFiles:(NSArray *)paths atScreenPoint:(NSPoint)pt copy:(BOOL)copy ignoring:(NSWindow *)ignore {
    if (!setup()) return 0;
    drain();
    X11Display *d = (X11Display *)[NSDisplay currentDisplay];
    int x = (int)pt.x, y = DisplayHeight(dpy, DefaultScreen(dpy)) - (int)pt.y;
    id pw = [ignore respondsToSelector: @selector(platformWindow)] ? [ignore performSelector: @selector(platformWindow)] : nil;
    Window ign = [pw isKindOfClass: [X11Window class]] ? [(X11Window *)pw windowHandle] : None;
    Window t = target_at(d, x, y, ign);
    if (getenv("CLOKE_XDND_TRACE") && t != target)
        fprintf(stderr, "[xdnd] %d,%d -> Ziel 0x%lx (ohne 0x%lx), Status %d\n", x, y, (unsigned long)t, (unsigned long)ign, status);
    want_action = copy ? A_copy : A_move;
    if (t != target) {
        leave();
        if (!t) return 0;
        target = t;
        long px = card(t, A_proxy, XA_WINDOW);   /* -1: keine Eigenschaft (als Window wäre das "gültig") */
        proxy = px > 0 ? (Window)px : t;
        target_version = (int)card(t, A_aware, XA_ATOM);
        if (target_version > 5) target_version = 5;
        [uri_data release];
        uri_data = [uri_list(paths) retain];
        XSetSelectionOwner(dpy, A_selection, src_win, CurrentTime);
        Atom types[2] = { A_uri, A_plain };
        XChangeProperty(dpy, src_win, A_type_list, XA_ATOM, 32, PropModeReplace, (unsigned char *)types, 2);
        send(A_enter, (long)target_version << 24, A_uri, A_plain, None);
        last_x = -1;
    }
    if (x != last_x || y != last_y) {
        last_x = x, last_y = y;
        if (waiting) pending = YES;
        else send_position();
    }
    return status;
}

- (NSInteger)clk_xdndAction {
    return action_reply == A_copy ? 0 : action_reply == A_move ? 1 : 2;
}

- (void)clk_xdndCancel {
    if (dpy) leave();
}

- (BOOL)clk_xdndDrop {
    if (!target || status != 2) { [self clk_xdndCancel]; return NO; }
    finished = NO;
    send(A_drop, 0, CurrentTime, 0, 0);
    XFlush(dpy);
    NSDate *limit = [NSDate dateWithTimeIntervalSinceNow: 3];
    while (!finished && [limit timeIntervalSinceNow] > 0) {   /* Ereignisse verarbeiten: SelectionRequest, XdndFinished */
        drain();
        if (!finished) usleep(10000);
    }
    target = proxy = None;
    status = 0;
    waiting = pending = NO;
    return finished;
}
@end
