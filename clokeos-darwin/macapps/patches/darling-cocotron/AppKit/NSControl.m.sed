# ClokeOS: Steuerelemente ohne Zelle (Logic Pros TransportDisplayMIDIDisplay) behalten ihre Schrift selbst, Vorgabe die Systemschrift (wie
# bei Apple). Vorher lieferte -font nil, weil es nur die Zelle fragte; Logic legte das ungeprüft in ein Attribut-Wörterbuch.
s/^(#import <AppKit\/NSControl.h>)$/\1\n#import <objc\/runtime.h>\nstatic char clk_cellless_font;/
/^- \(NSFont \*\) font \{$/,/^\}$/s/^    return \[_cell font\];$/    if (_cell == nil) {\n        NSFont *f = objc_getAssociatedObject(self, \&clk_cellless_font);\n        return f ? f : [NSFont systemFontOfSize: [NSFont systemFontSize]];\n    }\n    return [_cell font];/
/^- \(void\) setFont: \(NSFont \*\) font \{$/,/^\}$/s/^    \[_cell setFont: font\];$/    if (_cell == nil)\n        objc_setAssociatedObject(self, \&clk_cellless_font, font, OBJC_ASSOCIATION_RETAIN);\n    [_cell setFont: font];/
