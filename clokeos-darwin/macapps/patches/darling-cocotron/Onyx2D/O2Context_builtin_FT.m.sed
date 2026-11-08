# ClokeOS: nach dem Zeichnen rückt die Textposition wie bei CoreGraphics um die Summe der Vorschübe vor (mit advances: deren Breiten und
# Höhen, sonst die Glyphenbreiten in Schriftgröße), entlang der Textmatrix. Onyx2D rechnete die Summe aus, setzte sie aber nicht: Logic Pros
# Transportanzeige zeichnet "00" (gedimmt) und "1" nacheinander ab der jeweils aktuellen Textposition, beide landeten übereinander.
s/^    total = \(total \/ O2FontGetUnitsPerEm\(font\)\) \* gState->_pointSize;$/    total = (total \/ O2FontGetUnitsPerEm(font)) * gState->_pointSize;\
    {\
        O2Float clk_dx = total, clk_dy = 0;\
        if (advances != NULL) {\
            clk_dx = 0;\
            for (i = 0; i < count; i++) {\
                clk_dx += advances[i].width;\
                clk_dy += advances[i].height;\
            }\
        }\
        _textMatrix.tx += clk_dx * _textMatrix.a + clk_dy * _textMatrix.c;\
        _textMatrix.ty += clk_dx * _textMatrix.b + clk_dy * _textMatrix.d;\
    }/
