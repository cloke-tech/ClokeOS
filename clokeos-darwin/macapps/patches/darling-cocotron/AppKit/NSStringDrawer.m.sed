# ClokeOS: drawAtPoint: ohne Größe beginnt den Text wie bei Apple am Punkt, auch rechtsbündig oder zentriert gesetzter Text. Cocotron setzt
# ihn in einen 1 000 000 Punkt breiten Behälter, rechtsbündiger Text landete so weit rechts außerhalb (Logic Pros M/S/I-Buchstaben: Absatz
# "zentriert" = 1 auf arm64, für Cocotron "rechts"). Verschoben wird um den Anfang der belegten Fläche.
/^               atPoint: \(NSPoint\) point$/,/^\}$/s/^(    glyphRange = \[_layoutManager glyphRangeForTextContainer: _textContainer\];)$/\1\
    if (maxSize.width == NSStringDrawerLargeDimension)\
        point.x -= [_layoutManager usedRectForTextContainer: _textContainer].origin.x;/
/^                      atPoint: \(NSPoint\) point$/,/^\}$/s/^(    glyphRange = \[_layoutManager glyphRangeForTextContainer: _textContainer\];)$/\1\
    if (maxSize.width == NSStringDrawerLargeDimension)\
        point.x -= [_layoutManager usedRectForTextContainer: _textContainer].origin.x;/
# ClokeOS: CLOKE_CT_TRACE meldet auch den Text, den AppKit selbst zeichnet (NSString/NSAttributedString draw…), höchstens 400 Zeilen
s/^    \[_textStorage setAttributedString: astring\];$/    [_textStorage setAttributedString: astring];\
    if (getenv("CLOKE_CT_TRACE")) {\
        static int clk_n;\
        if (clk_n++ < 400)\
            fprintf(stderr, "[clokeos-ct] AppKit \\"%.60s\\"\\n", [[astring string] UTF8String] ?: "");\
    }/
# … auch für NSString (drawString:withAttributes:… und Größenmessung; "AppKit-S" = einfache Zeichenkette)
s/^    \[_textStorage endEditing\];$/    [_textStorage endEditing];\
    if (getenv("CLOKE_CT_TRACE")) {\
        static int clk_n;\
        if (clk_n++ < 400)\
            fprintf(stderr, "[clokeos-ct] AppKit-S \\"%.60s\\"\\n", [[_textStorage string] UTF8String] ?: "");\
    }/
# CLOKE_DRAW_TRACE=1: jede Textausgabe von NSString/NSAttributedString mit Punkt, Behältergröße, Glyphenzahl, Farbe, Schrift und CTM
s/^    \[_layoutManager drawGlyphsForGlyphRange: glyphRange atPoint: (rect\.origin|point)\];$/    if (getenv("CLOKE_DRAW_TRACE")) {\
        static int clk_n;\
        if (clk_n++ < 600) {\
            NSPoint clk_p = \1;\
            CGAffineTransform clk_m = CGContextGetCTM([[NSGraphicsContext currentContext] graphicsPort]);\
            NSDictionary *clk_a = [_textStorage length] ? [_textStorage attributesAtIndex: 0 effectiveRange: NULL] : nil;\
            fprintf(stderr, "[clokeos-draw] \\"%.40s\\" at %.1f,%.1f box %.1fx%.1f glyphs %lu used %s color %s font %s ctm %.1f %.1f %.1f %.1f %.1f %.1f\\n", [[_textStorage string] UTF8String] ?: "", clk_p.x, clk_p.y, [_textContainer containerSize].width, [_textContainer containerSize].height, (unsigned long) glyphRange.length, [NSStringFromRect([_layoutManager usedRectForTextContainer: _textContainer]) UTF8String], [[[clk_a objectForKey: @"NSColor"] description] UTF8String] ?: "-", [[[clk_a objectForKey: @"NSFont"] description] UTF8String] ?: "-", clk_m.a, clk_m.b, clk_m.c, clk_m.d, clk_m.tx, clk_m.ty);\
            id clk_f = [clk_a objectForKey: @"NSFont"];\
            if (clk_f == nil)\
                fprintf(stderr, "[clokeos-draw]   attrs %s\\n", [[[clk_a description] stringByReplacingOccurrencesOfString: @"\\n" withString: @" "] UTF8String] ?: "-");\
            if (clk_f \&\& glyphRange.length == 0)\
                fprintf(stderr, "[clokeos-draw]   asc %.2f desc %.2f lead %.2f line %.2f para %s\\n", [clk_f ascender], [clk_f descender], [clk_f leading], [clk_f defaultLineHeightForFont], [[[clk_a objectForKey: @"NSParagraphStyle"] description] UTF8String] ?: "-");\
        }\
    }\
&/
# ClokeOS: drawInRect: in nicht gespiegelten Ansichten setzt die erste Zeile wie bei Apple an die Oberkante des Rechtecks. Cocotron
# setzte den Text an die Unterkante (Logic Pros „KEEP“ in einem 27 Punkt hohen Rechteck lag dadurch auf „TEMPO“).
s/^(#import <AppKit\/NSView.h>)$/\1\
static NSPoint clkTopOrigin(NSLayoutManager *lm, NSTextContainer *tc, NSRect rect) {\
    if ([[NSGraphicsContext currentContext] isFlipped])\
        return rect.origin;\
    NSRect used = [lm usedRectForTextContainer: tc];\
    return NSMakePoint(rect.origin.x, rect.origin.y + rect.size.height - NSMaxY(used));\
}/
s/atPoint: rect\.origin\]/atPoint: clkTopOrigin(_layoutManager, _textContainer, rect)]/g
