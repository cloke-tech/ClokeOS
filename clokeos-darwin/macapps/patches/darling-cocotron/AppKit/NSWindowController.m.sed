# ClokeOS: -window nicht erneut betreten, während das Fenster lädt. Schlägt das Laden der Nib fehl, fragt Code aus loadWindow/windowDidLoad
# wieder nach -window und lud die Nib endlos neu (Stapelüberlauf bei Logic Pro). Merker je Objekt über ein zugeordnetes Objekt.
s/^(- \(NSWindow \*\) window \{)$/static char clk_loading_window;\n\1/
s/^    if \(_window == nil && \[self windowNibPath\] != nil\) \{$/    if (_window == nil \&\& !objc_getAssociatedObject(self, \&clk_loading_window) \&\& [self windowNibPath] != nil) {\n        objc_setAssociatedObject(self, \&clk_loading_window, self, OBJC_ASSOCIATION_ASSIGN);/
s/^        \[_document windowControllerDidLoadNib: self\];$/&\n        objc_setAssociatedObject(self, \&clk_loading_window, nil, OBJC_ASSOCIATION_ASSIGN);/
s/^(#import <AppKit\/NSWindowController.h>)$/\1\n#import <objc\/runtime.h>/
# ClokeOS: ohne Nib-Namen gibt es keinen Nib-Pfad (wie bei Apple). pathForResource:nil ofType:@"nib" lieferte die erstbeste Nib des Bundles:
# Logic Pros LgAlertCollector (baut sein Fenster selbst) lud so Beta.nib mit sich als Besitzer, dessen OK-Knopf ins Leere ging.
s/^(        NSString \*name = \[self windowNibName\];)$/\1\n        if (name == nil)\n            return nil;/
