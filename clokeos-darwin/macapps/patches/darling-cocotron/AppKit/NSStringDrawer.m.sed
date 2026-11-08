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
