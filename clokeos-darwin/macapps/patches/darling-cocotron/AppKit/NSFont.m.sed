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
