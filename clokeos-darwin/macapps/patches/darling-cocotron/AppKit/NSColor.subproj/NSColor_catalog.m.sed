# ClokeOS: Katalogfarben (textColor, controlColor, …) reichen Anteile, Alpha und Kopien an ihre eigentliche Farbe weiter, wie bei Apple.
# Vorher liefen getRed:green:blue:alpha:, getComponents:, colorWithAlphaComponent: usw. in die abstrakte NSColor-Fassung (Ausnahme bzw. nil);
# Programme, die eine Systemfarbe in Anteile zerlegen (Logic Pro für seine Schriftfarben), bekamen (0, 0, 0, 0): durchsichtigen Text.
/^- \(CGColorRef\) CGColorRef \{$/i\
- (NSColor *) clk_rgb {\
    NSColor *c = [_color colorUsingColorSpaceName: NSCalibratedRGBColorSpace device: nil];\
    return c ? c : _color;\
}\
- (void) getRed: (CGFloat *) r green: (CGFloat *) g blue: (CGFloat *) b alpha: (CGFloat *) a {\
    if (r) *r = 0; if (g) *g = 0; if (b) *b = 0; if (a) *a = 1;\
    [[self clk_rgb] getRed: r green: g blue: b alpha: a];\
}\
- (void) getWhite: (CGFloat *) w alpha: (CGFloat *) a {\
    if (w) *w = 0; if (a) *a = 1;\
    [[_color colorUsingColorSpaceName: NSCalibratedWhiteColorSpace device: nil] getWhite: w alpha: a];\
}\
- (void) getHue: (CGFloat *) h saturation: (CGFloat *) s brightness: (CGFloat *) b alpha: (CGFloat *) a {\
    [[self clk_rgb] getHue: h saturation: s brightness: b alpha: a];\
}\
- (void) getCyan: (CGFloat *) c magenta: (CGFloat *) m yellow: (CGFloat *) y black: (CGFloat *) k alpha: (CGFloat *) a {\
    [_color getCyan: c magenta: m yellow: y black: k alpha: a];\
}\
- (NSInteger) numberOfComponents { return [_color numberOfComponents]; }\
- (void) getComponents: (CGFloat *) components { [_color getComponents: components]; }\
- (CGFloat) alphaComponent { return _color ? [_color alphaComponent] : 1.0; }\
- (NSColor *) colorWithAlphaComponent: (CGFloat) alpha { return [_color colorWithAlphaComponent: alpha]; }\

