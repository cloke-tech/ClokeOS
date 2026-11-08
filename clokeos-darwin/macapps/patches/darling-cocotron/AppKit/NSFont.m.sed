# ClokeOS: Größe 0 (oder kleiner) heißt wie bei Apple: die Größe aus dem Deskriptor, ohne Angabe 12 Punkt (fontWithName: ebenso 12).
# Logic Pro baut die M/S/I-Buchstaben seiner Spurköpfe mit fontWithDescriptor:size:0.0; Cocotron machte eine 0-Punkt-Schrift (Bild 2 × 1).
/^\+ \(NSFont \*\) fontWithName: \(NSString \*\) name size: \(CGFloat\) size \{$/a\
    if (size <= 0)\
        size = 12;
/^\+ \(NSFont \*\) fontWithDescriptor: \(NSFontDescriptor \*\) descriptor$/,/^\{$/s/^\{$/{\
    if (size <= 0) {\
        size = [[[descriptor fontAttributes] objectForKey: NSFontSizeAttribute] doubleValue];\
        if (size <= 0)\
            size = 12;\
    }/
# ClokeOS: maximumAdvancement maß bei jedem Aufruf alle Glyphen der Schrift (FreeType lädt jede einzeln) und fontDescriptor rief es jedes
# Mal auf: bei Logic Pro die Hälfte der Rechenzeit des untätigen Hauptfadens. Der Wert wird je Schrift (Name, Größe) gemerkt.
# fontDescriptor trägt NSFontFixedAdvanceAttribute nicht mehr ein: Apple tut das nur für Schriften mit fester Breite.
/^- \(NSSize\) maximumAdvancement \{$/,/^\}$/s/^    CGSize max = CGSizeZero;$/    static NSMutableDictionary *clk_maxadv;\
    NSString *clk_key = [NSString stringWithFormat: @"%@ %g", [self fontName], (double) [self pointSize]];\
    NSValue *clk_hit;\
    @synchronized([NSFont class]) {\
        clk_hit = [[[clk_maxadv objectForKey: clk_key] retain] autorelease];\
    }\
    if (clk_hit != nil)\
        return [clk_hit sizeValue];\
    CGSize max = CGSizeZero;/
/^- \(NSSize\) maximumAdvancement \{$/,/^\}$/s/^    return max;$/    @synchronized([NSFont class]) {\
        if (clk_maxadv == nil)\
            clk_maxadv = [NSMutableDictionary new];\
        [clk_maxadv setObject: [NSValue valueWithSize: max] forKey: clk_key];\
    }\
    return max;/
/^                    \[NSNumber$/{
N
N
s/^                    \[NSNumber\n                            numberWithDouble: \[self maximumAdvancement\]\.width\],\n                    NSFontFixedAdvanceAttribute, \[self displayName\],$/                    [self displayName],/
}
# ClokeOS: boundingRectForGlyph: war nicht umgesetzt (leeres Rechteck); jetzt über CTFontGetBoundingRectsForGlyphs
/^- \(NSRect\) boundingRectForGlyph: \(NSGlyph\) glyph \{$/,/^\}$/c\
- (NSRect) boundingRectForGlyph: (NSGlyph) glyph {\
    CGGlyph clk_g = (CGGlyph) glyph;\
    CGRect clk_r = CGRectZero;\
    CTFontGetBoundingRectsForGlyphs(_ctFont, 0, \&clk_g, \&clk_r, 1);\
    return NSRectFromCGRect(clk_r);\
}
# ClokeOS: -[NSFont set] setzt wie bei Apple nur Schrift und Größe im CGContext, nicht die Textmatrix. Cocotrons setInContext: kippte sie in
# gespiegelten Ansichten zusätzlich; Programme, die selbst spiegeln (Logic Pros Zahlenfelder: CGContextScaleCTM(1, -1), dann
# CGContextShowGlyphsWithAdvances), zeichneten ihre Ziffern so doppelt gekippt außerhalb des Clips (Transportanzeige, Inspektorwerte leer).
# setInContext: bleibt, wie es war: Cocotrons NSLayoutManager verlässt sich darauf.
/^- \(void\) set \{$/,/^\}$/c\
- (void) set {\
    CGContextRef clk_cg = [[NSGraphicsContext currentContext] graphicsPort];\
    CGContextSetFont(clk_cg, _cgFont);\
    CGContextSetFontSize(clk_cg, _pointSize);\
}
