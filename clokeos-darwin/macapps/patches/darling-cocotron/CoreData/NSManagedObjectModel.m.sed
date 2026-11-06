# ClokeOS: im Code gesetzte Entitäten kennen ihr Modell (wie beim Laden aus einer .mom-Datei)
s/^(        \[_entities setObject: entity forKey: \[entity name\]\];)$/\1\n        if ([entity respondsToSelector: @selector(_clkSetModel:)])\n            [entity performSelector: @selector(_clkSetModel:) withObject: self];/
