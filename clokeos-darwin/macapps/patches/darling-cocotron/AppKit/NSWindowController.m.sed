# ClokeOS: -window nicht erneut betreten, während das Fenster lädt. Schlägt das Laden der Nib fehl, fragt Code aus loadWindow/windowDidLoad
# wieder nach -window und lud die Nib endlos neu (Stapelüberlauf bei Logic Pro). Merker je Objekt über ein zugeordnetes Objekt.
s/^(- \(NSWindow \*\) window \{)$/static char clk_loading_window;\n\1/
s/^    if \(_window == nil && \[self windowNibPath\] != nil\) \{$/    if (_window == nil \&\& !objc_getAssociatedObject(self, \&clk_loading_window) \&\& [self windowNibPath] != nil) {\n        objc_setAssociatedObject(self, \&clk_loading_window, self, OBJC_ASSOCIATION_ASSIGN);/
s/^        \[_document windowControllerDidLoadNib: self\];$/&\n        objc_setAssociatedObject(self, \&clk_loading_window, nil, OBJC_ASSOCIATION_ASSIGN);/
s/^(#import <AppKit\/NSWindowController.h>)$/\1\n#import <objc\/runtime.h>/
