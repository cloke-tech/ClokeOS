# ClokeOS: Fensterinhalt per XShmPutImage/XPutImage statt über CGL/GLX (macapps/appkit/x11present.m).
#  1. Kein GLX-Visual: das Fenster bekommt das Standard-Visual des Bildschirms.
#  2. Kein CGL-Fenster.
#  3. flushBuffer kopiert das Onyx2D-Surface direkt ins X-Fenster.
#  4. Der Ausgabepuffer wird mit dem Fenster freigegeben.
s|^#import "X11Window.h"$|#import "X11Window.h"\
#import "cloke_x11present.h"|
/^    \/\/ TODO: get rid of these glX calls$/,/^                   None};$/d
s|_visualInfo = glXChooseVisual\(_display, screen, att\)|_visualInfo = ClokeX11VisualInfo(_display, screen)|
s|NSLog\(@"glXChooseVisual failed|NSLog(@"ClokeX11VisualInfo failed|
s|^    _cglWindow = CGLGetWindow\(\(void \*\) _window\);$|    _cglWindow = NULL;|
s|^    \[self openGLFlushBuffer\];$|    ClokeX11Present(\&_clokePresent, _display, _window, _visualInfo, [_context surface]);|
s|^        XDestroyWindow\(_display, _window\);$|        ClokeX11PresentFree(\&_clokePresent, _display);\
        XDestroyWindow(_display, _window);|
# ClokeOS: Ausblenden und Umstapeln sofort an den X-Server schicken. Danach wartet die Ereignisschleife in poll(), und Xlibs Ausgabepuffer
# blieb liegen: ein mit orderOut: geschlossenes Fenster (Logic Pros Hinweisdialog) blieb auf dem Bildschirm und verdeckte die nächsten.
# Ausblenden nach ICCCM mit XWithdrawWindow: zusätzlich zum echten UnmapNotify ein synthetisches an das Wurzelfenster. CloakServer übergeht
# ein UnmapNotify, wenn es ein schon sichtbares Fenster umgehängt hat (ignore_unmaps); dann blieb das Fenster stehen.
s/^    XUnmapWindow\(_display, _window\);$/    XWithdrawWindow(_display, _window, DefaultScreen(_display));\n    XFlush(_display);/
s/^(        XRaiseWindow\(_display, _window\);)$/\1\n        XFlush(_display);/
s/^(        XLowerWindow\(_display, _window\);)$/\1\n        XFlush(_display);/
s/^(        XRestackWindows\(_display, w, 1\);)$/\1\n        XFlush(_display);/
# ClokeOS: makeKey zeigt ein ausgeblendetes Fenster nicht wieder an (wie bei Apple). Nach orderOut: machte AppKit dasselbe Fenster wieder zum
# Schlüsselfenster, und ensureMapped holte Logic Pros geschlossenen Hinweisdialog sofort zurück.
/^- \(void\) makeKey \{$/{
n
s/^    \[self ensureMapped\];$/    if (!_mapped)\n        return;/
}
# ClokeOS: Titelleiste wie bei macOS (macapps/appkit/titlebar.m): das Fenster meldet CloakServer vor dem Zeigen über _CLOKE_TITLEBAR,
# ob es die Fläche oben selbst zeichnet (FullSizeContentView, Werkzeugleiste in der Titelleiste, NSAlert).
s|^    \[self setWindowIcon\];$|    [self setWindowIcon];\
    { extern void ClokeTitlebarPublishX(id, Display *, Window); ClokeTitlebarPublishX(delegate, _display, _window); }|
