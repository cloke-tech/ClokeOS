# ClokeOS: Vorlagenbilder in Knöpfen in der Schriftfarbe (aktiv controlTextColor, sonst disabledControlTextColor), wie bei Apple; vorher
# schwarz (in der dunklen Darstellung unsichtbar). -[NSImage _clkTintedWithColor:] steht in macapps/appkit/modern.m.
/^- \(void\) drawButtonImage: \(NSImage \*\) image$/,/^\}$/s/^    CGFloat fraction = enabled \? 1.0 : 0.5;$/    CGFloat fraction = enabled ? 1.0 : 0.5;\n    if ([image isTemplate] \&\& [image respondsToSelector: @selector(_clkTintedWithColor:)]) {\n        image = [image performSelector: @selector(_clkTintedWithColor:)\n                            withObject: enabled ? [NSColor controlTextColor] : [NSColor disabledControlTextColor]];\n        fraction = 1.0;\n    }/
# ClokeOS: Druckknöpfe und Aufklappmenüs (gleicher Rahmen) in der dunklen Darstellung dunkel wie bei macOS (Füllung Weiß 37 %, Rand
# Weiß 25 %), sonst hell wie bisher. Logic Pros Inspektor zeigte seine helle Schrift auf weißen Aufklappmenüs. Hell/dunkel entscheidet
# das aktuelle Erscheinungsbild (macapps/appkit/appearance.m: bestMatchFromAppearancesWithNames:).
/^- \(void\) drawPushButtonNormalInRect: \(NSRect\) rect defaulted: \(BOOL\) defaulted \{$/i\
static BOOL clk_style_dark(void) {\
    id a = [NSClassFromString(@"NSAppearance") performSelector: @selector(currentAppearance)];\
    SEL s = NSSelectorFromString(@"bestMatchFromAppearancesWithNames:");\
    if (a == nil || ![a respondsToSelector: s])\
        return NO;\
    return [[a performSelector: s withObject: [NSArray arrayWithObjects: @"NSAppearanceNameAqua", @"NSAppearanceNameDarkAqua", nil]] isEqual: @"NSAppearanceNameDarkAqua"];\
}\

/^- \(void\) drawPushButtonNormalInRect: \(NSRect\) rect defaulted: \(BOOL\) defaulted \{$/,/^\}$/s/^    \[\[NSColor colorWithCalibratedWhite: 0.78 alpha: 1.0\] set\];$/    BOOL clk_dark = clk_style_dark();\
    [[NSColor colorWithCalibratedWhite: clk_dark ? 1.0 : 0.78 alpha: clk_dark ? 0.25 : 1.0] set];/
/^- \(void\) drawPushButtonNormalInRect: \(NSRect\) rect defaulted: \(BOOL\) defaulted \{$/,/^\}$/s/^    \[\[NSColor whiteColor\] set\];$/    [(clk_dark ? [NSColor colorWithCalibratedWhite: 0.37 alpha: 1.0] : [NSColor whiteColor]) set];/
