/* Bildausgabe für Darlings AppKit-X11-Backend auf ClokeOS, ohne OpenGL.
 *
 * Darlings X11Window lädt den Fensterinhalt (ein O2Surface, das Onyx2D in Software malt) als
 * Textur in einen CGL/GLX-Kontext. ClokeOS hat dafür kein OpenGL.framework, und unter TCG wäre
 * Mesa llvmpipe ohnehin langsam. Hier wird das Surface direkt ins X-Fenster kopiert:
 *
 *   - XShmPutImage, wenn der X-Server MIT-SHM kann und das Segment in kern.sysv.shmmax passt
 *     (Darwin-Standard 4 MB, ein Fenster bis etwa 1280×800),
 *   - sonst XPutImage.
 *
 * Das Surface ist kO2ImageAlphaPremultipliedFirst | kO2BitmapByteOrder32Little, also BGRA von
 * oben nach unten. Das entspricht genau einem ZPixmap mit 32 Bit pro Pixel auf einem
 * TrueColor-Visual (Rot 0xff0000) bei LSBFirst, wie Xorg auf x86 es liefert. Es wird nur kopiert,
 * nicht umgerechnet.
 *
 * Eingebunden über patches/darling-cocotron/AppKit/X11.backend/X11Window.{h,m}.sed.
 * CLOKE_NO_XSHM=1 erzwingt XPutImage, CLOKE_X11_DEBUG=1 meldet Weg und Dauer jeder Ausgabe. */
#import "cloke_x11present.h"
#import "X11Display.h"
#import <Onyx2D/O2Surface.h>
#import <X11/extensions/XShm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

@interface X11Display (ClokePending)
- (void) processPendingEvents;
@end

typedef struct {
    XImage *image;
    XShmSegmentInfo shm;
    int useShm;
    size_t width, height;
    GC gc;
} ClokePresent;

static int debugEnabled(void) {
    static int v = -1;
    if (v < 0)
        v = getenv("CLOKE_X11_DEBUG") != NULL;
    return v;
}

XVisualInfo *ClokeX11VisualInfo(Display *dpy, int screen) {
    XVisualInfo tmpl = {0};
    int n = 0;
    tmpl.visualid = XVisualIDFromVisual(DefaultVisual(dpy, screen));
    tmpl.screen = screen;
    XVisualInfo *vi = XGetVisualInfo(dpy, VisualIDMask | VisualScreenMask, &tmpl, &n);
    if (vi != NULL && (vi->class != TrueColor || vi->red_mask != 0xff0000 ||
                       vi->green_mask != 0x00ff00 || vi->blue_mask != 0x0000ff))
        fprintf(stderr, "ClokeOS AppKit: Standard-Visual ist kein TrueColor-RGB (Tiefe %d), "
                        "Farben stimmen nicht\n", vi->depth);
    return vi;
}

static int shmFailed;
static int trapShmError(Display *dpy, XErrorEvent *ev) {
    shmFailed = 1;
    return 0;
}

static void destroyImage(ClokePresent *p, Display *dpy) {
    if (p->image == NULL)
        return;
    if (p->useShm) {
        XShmDetach(dpy, &p->shm);
        XSync(dpy, False);
        p->image->data = NULL;
        XDestroyImage(p->image);
        shmdt(p->shm.shmaddr);
    } else {
        XDestroyImage(p->image); // gibt auch die Pixel frei (malloc)
    }
    p->image = NULL;
}

// PureDarwin hat auf arm64 kein System-V-shm: shmget löst dort SIGSYS aus, und ohne SIG_IGN
// (CloakServer vererbt es, eine Shell nicht) endet das Programm. Der Aufruf läuft deshalb mit
// ignoriertem SIGSYS, liefert dann ENOSYS, und danach wird es nicht mehr versucht.
static int shmgetNoSigsys(size_t size) {
    static int missing;
    if (missing) {
        errno = ENOSYS;
        return -1;
    }
    struct sigaction ign, old;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGSYS, &ign, &old);
    int id = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
    int err = errno;
    sigaction(SIGSYS, &old, NULL);
    if (id < 0 && err == ENOSYS)
        missing = 1;
    errno = err;
    return id;
}

// Legt ein XImage in shm an. Scheitert still (Rückgabe 0), dann gilt XPutImage.
static int createShmImage(ClokePresent *p, Display *dpy, XVisualInfo *vi, size_t w, size_t h) {
    if (getenv("CLOKE_NO_XSHM") != NULL || !XShmQueryExtension(dpy))
        return 0;
    XImage *img = XShmCreateImage(dpy, vi->visual, vi->depth, ZPixmap, NULL, &p->shm, w, h);
    if (img == NULL)
        return 0;
    p->shm.shmid = shmgetNoSigsys((size_t) img->bytes_per_line * h);
    if (p->shm.shmid < 0) {
        if (debugEnabled())
            fprintf(stderr, "ClokeOS AppKit: shmget(%zu Bytes) fehlgeschlagen, nehme XPutImage "
                            "(kern.sysv.shmmax?)\n", (size_t) img->bytes_per_line * h);
        XDestroyImage(img);
        return 0;
    }
    p->shm.shmaddr = img->data = shmat(p->shm.shmid, NULL, 0);
    p->shm.readOnly = False;
    if (p->shm.shmaddr == (char *) -1) {
        shmctl(p->shm.shmid, IPC_RMID, NULL);
        img->data = NULL;
        XDestroyImage(img);
        return 0;
    }
    shmFailed = 0;
    XErrorHandler old = XSetErrorHandler(trapShmError);
    XShmAttach(dpy, &p->shm);
    XSync(dpy, False);
    XSetErrorHandler(old);
    // Der Server hängt das Segment jetzt an; es verschwindet, wenn beide es lösen.
    shmctl(p->shm.shmid, IPC_RMID, NULL);
    if (shmFailed) {
        shmdt(p->shm.shmaddr);
        img->data = NULL;
        XDestroyImage(img);
        return 0;
    }
    p->image = img;
    return 1;
}

static void createImage(ClokePresent *p, Display *dpy, XVisualInfo *vi, size_t w, size_t h) {
    p->width = w;
    p->height = h;
    p->useShm = createShmImage(p, dpy, vi, w, h);
    if (!p->useShm) {
        p->image = XCreateImage(dpy, vi->visual, vi->depth, ZPixmap, 0, NULL, w, h, 32, 0);
        if (p->image != NULL) {
            p->image->data = malloc((size_t) p->image->bytes_per_line * h);
            p->image->byte_order = LSBFirst; // so liegen die Onyx2D-Pixel im Speicher
        }
    }
    if (p->image != NULL && p->image->bits_per_pixel != 32) {
        fprintf(stderr, "ClokeOS AppKit: X-Server will %d Bit pro Pixel, erwartet 32\n",
                p->image->bits_per_pixel);
        destroyImage(p, dpy);
    }
    if (debugEnabled())
        fprintf(stderr, "ClokeOS AppKit: Fensterpuffer %zux%zu über %s\n", w, h,
                p->useShm ? "XShmPutImage" : "XPutImage");
}

void ClokeX11Present(void **state, Display *dpy, Window win, XVisualInfo *vi, O2Surface *surface) {
    if (surface == nil || vi == NULL || win == 0)
        return;
    size_t w = O2ImageGetWidth(surface), h = O2ImageGetHeight(surface);
    size_t bpr = O2ImageGetBytesPerRow(surface);
    const unsigned char *px = [surface pixelBytes];
    if (px == NULL || w == 0 || h == 0)
        return;

    struct timespec t0;
    if (debugEnabled())
        clock_gettime(CLOCK_MONOTONIC, &t0);

    ClokePresent *p = *state;
    if (p == NULL) {
        p = calloc(1, sizeof(*p));
        p->gc = XCreateGC(dpy, win, 0, NULL);
        *state = p;
    }
    if (p->image == NULL || p->width != w || p->height != h) {
        destroyImage(p, dpy);
        createImage(p, dpy, vi, w, h);
        if (p->image == NULL)
            return;
    }

    char *dst = p->image->data;
    size_t rowBytes = w * 4;
    if (bpr == (size_t) p->image->bytes_per_line)
        memcpy(dst, px, bpr * h);
    else
        for (size_t y = 0; y < h; y++)
            memcpy(dst + y * p->image->bytes_per_line, px + y * bpr, rowBytes);

    if (p->useShm) {
        XShmPutImage(dpy, win, p->gc, p->image, 0, 0, 0, 0, w, h, False);
        // Das Segment darf erst wieder beschrieben werden, wenn der Server es gelesen hat.
        XSync(dpy, False);
    } else {
        XPutImage(dpy, win, p->gc, p->image, 0, 0, 0, 0, w, h);
        XFlush(dpy);
    }

    // XSync kann Ereignisse in Xlibs Warteschlange lesen. Dann meldet der Socket nichts mehr und
    // X11Display holt sie erst beim nächsten Ereignis ab. Deshalb gleich nach dieser Runde.
    if (XEventsQueued(dpy, QueuedAlready) > 0) {
        id display = [NSDisplay currentDisplay];
        if ([display respondsToSelector: @selector(processPendingEvents)])
            [display performSelector: @selector(processPendingEvents)
                          withObject: nil
                          afterDelay: 0
                             inModes: @[ NSRunLoopCommonModes ]];
    }

    if (debugEnabled()) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        fprintf(stderr, "ClokeOS AppKit: Ausgabe %zux%zu in %.1f ms\n", w, h,
                (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    }
}

void ClokeX11PresentFree(void **state, Display *dpy) {
    ClokePresent *p = *state;
    if (p == NULL)
        return;
    destroyImage(p, dpy);
    if (p->gc != NULL)
        XFreeGC(dpy, p->gc);
    free(p);
    *state = NULL;
}

/* -[CGWindow setAlphaValue:] ist in Darlings CGWindow abstrakt, X11Window setzt es nicht um (Logic Pro: Ausnahme in einem dispatch_once,
 * das endet in std::terminate). Hier über die EWMH-Eigenschaft _NET_WM_WINDOW_OPACITY (0 … 0xffffffff); ein Fenstermanager, der sie nicht
 * auswertet, zeigt das Fenster weiter deckend. */
#import "X11Window.h"
#import <X11/Xatom.h>
@implementation X11Window (ClokeAlpha)
- (void) setAlphaValue: (CGFloat) alpha {
    if (_display == NULL || _window == 0)
        return;
    if (alpha < 0) alpha = 0;
    if (alpha > 1) alpha = 1;
    Atom a = XInternAtom(_display, "_NET_WM_WINDOW_OPACITY", False);
    if (alpha >= 1.0) {
        XDeleteProperty(_display, _window, a);
    } else {
        unsigned long v = (unsigned long)(alpha * 0xffffffffu);
        XChangeProperty(_display, _window, a, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
    }
    XFlush(_display);
}
@end
