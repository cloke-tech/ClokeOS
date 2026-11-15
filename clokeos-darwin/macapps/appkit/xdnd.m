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
 * Ziele: CloakServers Dock (Programm = damit öffnen, Papierkorb), GTK- und andere X-Programme.
 *
 * Ziel (Ablegen aus anderen Programmen in dieses): -[NSWindow clk_setXdndHandler:] meldet das X-Fenster als XdndAware an. Der
 * Handler bekommt (Punkte in Bildschirmkoordinaten von Cocoa, Ursprung unten links):
 *   -(NSInteger)clk_xdndOverScreenPoint:(NSPoint)p copy:(BOOL)c   0 = hier nicht, 1 = kopieren, 2 = bewegen, 3 = anderes (öffnen)
 *   -(void)clk_xdndExited
 *   -(BOOL)clk_xdndDropFiles:(NSArray *)paths atScreenPoint:(NSPoint)p copy:(BOOL)c
 * Die Dateien holt die Erweiterung selbst (XConvertSelection auf XdndSelection, text/uri-list) und antwortet mit XdndFinished. */
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
@interface NSObject (ClokeXdndHandler)
- (NSInteger)clk_xdndOverScreenPoint:(NSPoint)p copy:(BOOL)c;
- (void)clk_xdndExited;
- (BOOL)clk_xdndDropFiles:(NSArray *)paths atScreenPoint:(NSPoint)p copy:(BOOL)c;
@end

static IMP o_post;
static char k_handler;
/* Zielseite: aktuelle Quelle und ihr Fenster */
static Window in_src;
static NSWindow *in_win;
static BOOL in_uri;
static int in_version;
static NSInteger in_answer;
static NSPoint in_pt;
static Atom in_action;
static Window xwin_of(NSWindow *w);
static BOOL setup(void);

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
static NSWindow *nswindow_for(X11Display *d, Window w) {
    id xw = [d windowForID: w];
    if (![xw isKindOfClass: [X11Window class]]) return nil;
    id del = [(X11Window *)xw delegate];
    return [del isKindOfClass: [NSWindow class]] ? del : nil;
}

static void in_reply(Atom type, long l1, long l2, long l4) {
    XEvent r;
    memset(&r, 0, sizeof r);
    r.xclient.type = ClientMessage;
    r.xclient.window = in_src;
    r.xclient.message_type = type;
    r.xclient.format = 32;
    r.xclient.data.l[0] = xwin_of(in_win);
    r.xclient.data.l[1] = l1;
    r.xclient.data.l[2] = l2;
    r.xclient.data.l[4] = l4;
    XSendEvent(dpy, in_src, False, NoEventMask, &r);
    XFlush(dpy);
}

/* file:///a%20b -> /a b */
static NSArray *paths_from_uris(const char *p, unsigned long n) {
    NSMutableArray *out = [NSMutableArray array];
    const char *end = p + n;
    while (p < end) {
        const char *nl = memchr(p, '\n', end - p);
        size_t len = (nl ? nl : end) - p;
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == 0)) len--;
        if (len > 7 && !strncmp(p, "file://", 7)) {
            const char *u = p + 7, *ue = p + len;
            if (u < ue && *u != '/') { const char *sl = memchr(u, '/', ue - u); u = sl ? sl : ue; }
            NSMutableData *d = [NSMutableData data];
            for (; u < ue; u++) {
                char c = *u;
                if (c == '%' && ue - u > 2) { char h[3] = { u[1], u[2], 0 }; c = (char)strtol(h, NULL, 16); u += 2; }
                [d appendBytes: &c length: 1];
            }
            NSString *s = [[[NSString alloc] initWithData: d encoding: NSUTF8StringEncoding] autorelease];
            if ([s length]) [out addObject: s];
        }
        p = nl ? nl + 1 : end;
    }
    return out;
}

static NSPoint screen_point(long packed) {
    int x = (int)((packed >> 16) & 0xffff), y = (int)(packed & 0xffff);
    return NSMakePoint(x, DisplayHeight(dpy, DefaultScreen(dpy)) - y);
}

static BOOL target_message(X11Display *d, XClientMessageEvent *e) {
    if (e->message_type != A_enter && e->message_type != A_position && e->message_type != A_leave && e->message_type != A_drop)
        return NO;
    NSWindow *w = nswindow_for(d, e->window);
    id h = w ? objc_getAssociatedObject(w, &k_handler) : nil;
    if (getenv("CLOKE_XDND_TRACE")) fprintf(stderr, "[xdnd] Nachricht %lu an 0x%lx, Fenster %p, Handler %p\n",
                                            (unsigned long)e->message_type, (unsigned long)e->window, w, h);
    if (!h) return NO;
    if (e->message_type == A_enter) {
        in_src = (Window)e->data.l[0];
        in_win = w;
        in_version = (int)((unsigned long)e->data.l[1] >> 24);
        in_uri = NO;
        if (e->data.l[1] & 1) {
            Atom t; int f; unsigned long n = 0, after; unsigned char *data = NULL;
            if (XGetWindowProperty(dpy, in_src, A_type_list, 0, 64, False, XA_ATOM, &t, &f, &n, &after, &data) == Success && data)
                for (unsigned long i = 0; i < n; i++) in_uri |= ((Atom *)data)[i] == A_uri;
            if (data) XFree(data);
        }
        for (int i = 2; i < 5; i++) in_uri |= (Atom)e->data.l[i] == A_uri;
        in_answer = 0;
    } else if (e->message_type == A_position) {
        in_src = (Window)e->data.l[0];
        in_win = w;
        in_pt = screen_point(e->data.l[2]);
        in_action = (Atom)e->data.l[4];
        in_answer = in_uri ? [h clk_xdndOverScreenPoint: in_pt copy: in_action == A_copy] : 0;
        Atom act = in_answer == 1 ? A_copy : in_answer == 2 ? A_move : in_answer == 3 ? A_copy : None;
        in_reply(A_status, (in_answer ? 1 : 0) | 2, 0, act);
    } else if (e->message_type == A_leave) {
        [h clk_xdndExited];
        in_src = None;
    } else {   /* Drop: Dateien holen, ablegen, XdndFinished */
        in_src = (Window)e->data.l[0];
        BOOL ok = NO;
        if (in_answer) {
            Atom prop = XInternAtom(dpy, "_CLOKE_XDND_IN", False);
            Window me = xwin_of(w);
            XConvertSelection(dpy, A_selection, A_uri, prop, me, in_version >= 1 ? (Time)e->data.l[2] : CurrentTime);
            XFlush(dpy);
            XEvent n;
            BOOL got = NO;
            for (int i = 0; i < 200 && !got; i++) {   /* bis 2 s */
                if (XCheckTypedWindowEvent(dpy, me, SelectionNotify, &n)) got = YES;
                else usleep(10000);
            }
            if (got && n.xselection.property != None) {
                Atom t; int f; unsigned long cnt = 0, after; unsigned char *data = NULL;
                if (XGetWindowProperty(dpy, me, prop, 0, 1 << 20, True, AnyPropertyType, &t, &f, &cnt, &after, &data) == Success && data)
                    ok = [h clk_xdndDropFiles: paths_from_uris((const char *)data, cnt) atScreenPoint: in_pt copy: in_action == A_copy];
                if (data) XFree(data);
            }
        }
        [h clk_xdndExited];
        Atom act = !ok ? None : in_answer == 2 ? A_move : A_copy;
        in_reply(A_finished, ok ? 1 : 0, act, 0);
        in_src = None;
        in_answer = 0;
    }
    return YES;
}

static void x_post(id self, SEL _cmd, XEvent *ev) {
    if (ev->type == ClientMessage && target_message((X11Display *)self, &ev->xclient)) return;
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

static Window xwin_of(NSWindow *w) {
    id pw = [w respondsToSelector: @selector(platformWindow)] ? [w performSelector: @selector(platformWindow)] : nil;
    return [pw isKindOfClass: [X11Window class]] ? [(X11Window *)pw windowHandle] : None;
}

@interface NSWindow (ClokeXdnd)
- (void)clk_setXdndHandler:(id)h;
@end
@implementation NSWindow (ClokeXdnd)
- (void)clk_publishXdnd {
    Window xw = xwin_of(self);
    if (!xw || !setup()) {   /* X-Fenster gibt es erst nach dem ersten Zeigen */
        [self performSelector: @selector(clk_publishXdnd) withObject: nil afterDelay: 0.3];
        return;
    }
    long v = 5;
    if (getenv("CLOKE_XDND_TRACE")) fprintf(stderr, "[xdnd] Ziel angemeldet: 0x%lx\n", (unsigned long)xw);
    XChangeProperty(dpy, xw, A_aware, XA_ATOM, 32, PropModeReplace, (unsigned char *)&v, 1);
    XFlush(dpy);
}
- (void)clk_setXdndHandler:(id)h {
    objc_setAssociatedObject(self, &k_handler, h, OBJC_ASSOCIATION_ASSIGN);   /* der Handler lebt im Fenster */
    if (h) [self clk_publishXdnd];
}
@end

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
