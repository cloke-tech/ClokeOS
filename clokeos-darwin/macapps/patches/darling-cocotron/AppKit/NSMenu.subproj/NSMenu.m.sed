# ClokeOS: itemArray liefert wie bei Apple eine Kopie; Logic Pro ändert das Menü, während es die Einträge durchläuft
# ("Illegal mutation while fast enumerating" beim Aufbau des Hauptfensters).
/^- \(NSArray \*\) itemArray \{$/,/^\}$/s/^    return _itemArray;$/    return [[_itemArray copy] autorelease];/
