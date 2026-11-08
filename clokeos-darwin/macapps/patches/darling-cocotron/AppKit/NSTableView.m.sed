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
# ClokeOS: ansichtsbasierte Tabellen (macapps/appkit/tableviews.m): Vorlagen-Nibs aus dem Archiv lesen; bei solchen Tabellen keine
# Zellen und keine Auswahlfläche zeichnen (die Zeilenansichten tun das), Knopfzellen nicht selbst anklicken, nicht per Doppelklick mit
# einer Zelle bearbeiten (stattdessen das Textfeld der Zellansicht); Ansichten bei reloadData, geänderter Zeilenzahl, Spaltenbreite
# und Auswahl nachführen.
s/^@implementation NSTableView$/void ClkTableDecode(NSTableView *t, NSCoder *c);\nBOOL ClkTableIsViewBased(NSTableView *t);\nvoid ClkTableReset(NSTableView *t);\nvoid ClkTableRowsChanged(NSTableView *t);\nvoid ClkTableNeedsLayout(NSTableView *t);\nvoid ClkTableSelectionChanged(NSTableView *t);\nBOOL ClkTableEdit(NSTableView *t, NSInteger column, NSInteger row, BOOL select);\n\n&/
s/^        _cornerView = \[\[keyed decodeObjectForKey: @"NSCornerView"\] retain\];$/&\n        ClkTableDecode(self, keyed);/
/^- \(void\) _setSelectedRowIndexes: /,/^\}$/s/^    \[self noteSelectionDidChange\];$/    ClkTableSelectionChanged(self);\n&/
/^- \(void\) noteNumberOfRowsChanged \{$/,/^\}$/s/^        \[self _setSelectedRowIndexes: selection\];$/&\n    ClkTableRowsChanged(self);/
s/^- \(void\) reloadData \{$/&\n    ClkTableReset(self);/
s/^- \(void\) highlightSelectionInClipRect: \(NSRect\) rect \{$/&\n    if (ClkTableIsViewBased(self))\n        return;/
/^- \(void\) drawRow: \(NSInteger\) row clipRect: \(NSRect\) clipRect \{$/,/^\}$/s/^        \} else \{$/        } else if (!ClkTableIsViewBased(self)) {/
s/^    if \(\[clickedCell isKindOfClass: \[NSButtonCell class\]\]\) \{$/    if (!ClkTableIsViewBased(self) \&\& [clickedCell isKindOfClass: [NSButtonCell class]]) {/
s/^        if \(\[\[_tableColumns objectAtIndex: _clickedColumn\] isEditable\]\) \{$/        if (!ClkTableIsViewBased(self) \&\& [[_tableColumns objectAtIndex: _clickedColumn] isEditable]) {/
/^- \(void\) editColumn: \(NSInteger\) column$/,/^\{$/s/^\{$/{\n    if (ClkTableEdit(self, column, row, select))\n        return;/
s/^- \(void\) noteColumnDidResizeWithOldWidth: \(CGFloat\) oldWidth \{$/&\n    ClkTableNeedsLayout(self);/
# ClokeOS: Zwischenraum der Zellen aus dem Archiv (NSIntercellSpacingWidth/-Height); Cocotron nahm immer 3 x 2.
s/^        _intercellSpacing = NSMakeSize\(3\.0, 2\.0\);$/        _intercellSpacing = NSMakeSize([keyed containsValueForKey: @"NSIntercellSpacingWidth"] ? [keyed decodeFloatForKey: @"NSIntercellSpacingWidth"] : 3.0,\n                                       [keyed containsValueForKey: @"NSIntercellSpacingHeight"] ? [keyed decodeFloatForKey: @"NSIntercellSpacingHeight"] : 2.0);/
# ClokeOS: wie bei Apple wählt eine Tabelle ohne leere Auswahl (allowsEmptySelection = NO) die erste Zeile, sobald es Zeilen gibt.
/^- \(void\) noteNumberOfRowsChanged \{$/,/^\}$/s/^    if \(!\[selection isEqualToIndexSet: _selectedRowIndexes\]\)$/    if (!_allowsEmptySelection \&\& [selection count] == 0 \&\& numberOfRows > 0)\n        [selection addIndex: 0];\n&/
