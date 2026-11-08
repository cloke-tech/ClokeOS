# ClokeOS: lineFragmentRectForGlyphAtIndex:effectiveRange:, locationForGlyphAtIndex: und lineFragmentUsedRectForGlyphAtIndex:effectiveRange:
# setzen wie bei Apple (und wie textContainerForGlyphAtIndex: hier) zuerst Glyphen und Layout bis zu dieser Glyphe. Ohne das lieferten sie
# direkt nach dem Aufbau eines Layouts Nullwerte: Logic Pros Zahlenfelder (+[MAKitSegmentedScrubberCell getXOffset:yOffset:…]) setzten ihre
# Ziffern an den Versatz 0, links neben ihren Clip (Transportanzeige, Inspektorwerte leer).
# Cocotrons Typesetter ruft diese Methoden selbst während des Layouts auf; dort wird nicht erneut gelayoutet (Tiefenzähler), sonst
# endlose Rekursion.
s/^@implementation NSLayoutManager$/static __thread int clk_lm_depth;\
@interface NSLayoutManager (ClkValidate)\
- (NSRange) clk_validateGlyphsAndLayoutForGlyphRange: (NSRange) glyphRange;\
@end\
static void clk_lm_validate(NSLayoutManager *lm, NSUInteger glyphIndex) {\
    if (clk_lm_depth > 0)\
        return;\
    [lm validateGlyphsAndLayoutForGlyphRange: NSMakeRange(glyphIndex, 1)];\
}\
\
@implementation NSLayoutManager/
/^- \(NSRect\) lineFragmentRectForGlyphAtIndex: \(NSUInteger\) glyphIndex$/,/^\}$/s/^(    NSGlyphFragment \*fragment =)$/    clk_lm_validate(self, glyphIndex);\
\1/
/^- \(NSPoint\) locationForGlyphAtIndex: \(NSUInteger\) glyphIndex \{$/,/^\}$/s/^(    NSGlyphFragment \*fragment = fragmentAtGlyphIndex\(self, glyphIndex, NULL\);)$/    clk_lm_validate(self, glyphIndex);\
\1/
/^- \(NSRect\) lineFragmentUsedRectForGlyphAtIndex: \(NSUInteger\) glyphIndex$/,/^\}$/s/^(    NSGlyphFragment \*fragment =)$/    clk_lm_validate(self, glyphIndex);\
\1/
# … dafür zählt jede Layout-Runde: die eigentliche Methode heißt clk_validateGlyphsAndLayoutForGlyphRange:, die alte Bezeichnung zählt mit
s/^- \(NSRange\) validateGlyphsAndLayoutForGlyphRange: \(NSRange\) glyphRange \{$/- (NSRange) validateGlyphsAndLayoutForGlyphRange: (NSRange) glyphRange {\
    clk_lm_depth++;\
    NSRange clk_r = [self clk_validateGlyphsAndLayoutForGlyphRange: glyphRange];\
    clk_lm_depth--;\
    return clk_r;\
}\
\
- (NSRange) clk_validateGlyphsAndLayoutForGlyphRange: (NSRange) glyphRange {/
# ClokeOS: lineFragmentRectForGlyphAtIndex: liefert wie bei Apple das Rechteck der ganzen Zeile (Anfang des Textbehälters, dessen Breite,
# Höhe des Fragments), nicht nur das der Glyphen: zentrierter Text "0" in 100 Punkt Breite -> {{0,0},{100,15}} (auf dem Mac nachgeprüft).
/^- \(NSRect\) lineFragmentRectForGlyphAtIndex: \(NSUInteger\) glyphIndex$/,/^\}$/s/^    return fragment->rect;$/    {\
        NSRect clk_line = fragment->rect;\
        NSTextContainer *clk_tc = fragment->container;\
        if (clk_tc != nil \&\& [clk_tc containerSize].width < 1e6) {\
            clk_line.origin.x = 0;\
            clk_line.size.width = [clk_tc containerSize].width;\
        }\
        return clk_line;\
    }/
