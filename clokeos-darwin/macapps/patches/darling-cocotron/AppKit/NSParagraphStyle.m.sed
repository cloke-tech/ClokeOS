# ClokeOS: _tabStops ist immer veränderlich. NSMutableParagraphStyle ändert das Feld in setTabStops:/addTabStop: an Ort und Stelle; aus einer
# Kopie (copy) oder einem Archiv kam bisher ein unveränderliches Feld ("attempting to mutate an immutable object", Logic Pros Bounce-Dialog).
s/^( *)_tabStops = \[other->_tabStops copy\];/\1_tabStops = [other->_tabStops mutableCopy];/
s/^( *)_tabStops = \[\[coder decodeObjectForKey: @"Tabs"\] retain\];/\1_tabStops = [[coder decodeObjectForKey: @"Tabs"] mutableCopy];/
# ClokeOS: Apple archiviert die Ausrichtung unter "NSAlignment" in der alten Reihenfolge (Mitte 2, rechts 1); Cocotron kannte nur den
# eigenen Schlüssel "Alignment". Beide werden gelesen ("NSAlignment" umgerechnet), geschrieben wird zusätzlich "NSAlignment".
s/^        _alignment = \[coder decodeIntForKey: @"Alignment"\];$/        _alignment = [coder containsValueForKey: @"NSAlignment"] ? _CLKTextAlignmentFromArchive([coder decodeIntForKey: @"NSAlignment"]) : (NSTextAlignment) [coder decodeIntForKey: @"Alignment"];/
s/^        \[coder encodeInt: _alignment forKey: @"Alignment"\];$/        [coder encodeInt: _alignment forKey: @"Alignment"];\
        [coder encodeInt: _CLKTextAlignmentToArchive(_alignment) forKey: @"NSAlignment"];/
