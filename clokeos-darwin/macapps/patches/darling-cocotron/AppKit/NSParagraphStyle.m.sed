# ClokeOS: _tabStops ist immer veränderlich. NSMutableParagraphStyle ändert das Feld in setTabStops:/addTabStop: an Ort und Stelle; aus einer
# Kopie (copy) oder einem Archiv kam bisher ein unveränderliches Feld ("attempting to mutate an immutable object", Logic Pros Bounce-Dialog).
s/^( *)_tabStops = \[other->_tabStops copy\];/\1_tabStops = [other->_tabStops mutableCopy];/
s/^( *)_tabStops = \[\[coder decodeObjectForKey: @"Tabs"\] retain\];/\1_tabStops = [[coder decodeObjectForKey: @"Tabs"] mutableCopy];/
