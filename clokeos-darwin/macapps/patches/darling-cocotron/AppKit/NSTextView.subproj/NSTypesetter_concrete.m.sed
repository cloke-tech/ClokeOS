# ClokeOS: die erste Zeile eines Textbehälters wird wie bei Apple auch dann gesetzt, wenn sie höher ist als der Behälter (sie wird
# beim Zeichnen nur beschnitten). Vorher gab es 0 Glyphen: Logic Pros LCD-Beschriftungen (BAR, BEAT, TEMPO) in 12 Punkt hohen
# Rechtecken und jedes drawInRect: mit knapper Höhe blieben leer.
s/^    if \(_scanRect\.size\.height < wantedHeight\) \{$/    if (_scanRect.size.height > 0 \&\& _scanRect.size.height < wantedHeight \&\& proposedRect.origin.y <= 0 \&\&\
        [_glyphRangesInLine count] == 0)\
        _scanRect.size.height = wantedHeight;\
&/
