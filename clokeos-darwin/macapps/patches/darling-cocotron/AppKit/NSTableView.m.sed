# ClokeOS: Textfarbe nur bei Cocotrons eigenen NSTextFieldCells setzen; Zellen eigener Klassen (Logic Pros Inspektor: helle Schrift auf
# dunklem Grund) behalten ihre Farbe, wie bei Apple, das die Textfarbe in preparedCellAtColumn:row: nicht anfasst.
s/^    if \(\[dataCell respondsToSelector: @selector\(setTextColor:\)\]\) \{$/    if ([dataCell class] == [NSTextFieldCell class] \&\& [dataCell respondsToSelector: @selector(setTextColor:)]) {/
# ClokeOS: der Wert der Datenquelle geht wie bei Apple über setObjectValue: an die Zelle, auch bei Popup-Zellen. Cocotron wählte bei ihnen
# nur den Eintrag [value intValue]; Logic Pros MyPopUpButtonCell (eigenes setObjectValue:, Wert ist der Text) baute so nie ihr Menü auf
# und zeigte den Platzhalter "dummy". Popup-Zellen ohne eigenes setObjectValue: wählen weiter den Eintrag nach Nummer.
/^    if \(\[dataCell isKindOfClass: \[NSPopUpButtonCell class\]\]\) \{$/,/^    \}$/c\
    [dataCell setObjectValue: value];\
    if ([dataCell isKindOfClass: [NSPopUpButtonCell class]] &&\
        [dataCell methodForSelector: @selector(setObjectValue:)] == [NSCell instanceMethodForSelector: @selector(setObjectValue:)])\
        [(NSPopUpButtonCell *) dataCell selectItemAtIndex: [value intValue]];
