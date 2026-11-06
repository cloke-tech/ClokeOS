# ClokeOS: Schlüssel- und Wertelisten der Nib können verschieden lang sein, wenn ein Objekt beim Entpacken nil ergibt (Klassen, die es hier
# nicht oder nur als Platzhalter gibt; Darlings NSArray lässt nil aus). Dann nur die vorhandenen Paare (Logic Pro, Nibs aus MALiveLoopsUI:
# NSRangeException und Abbruch).
s/^(        for \(NSUInteger idxPlusOne = )objectValues.count(; idxPlusOne > 0; --idxPlusOne\) \{)$/\1MIN(objectValues.count, objectKeys.count)\2/
s/^(        for \(NSUInteger i = 0; i < )classesKeys.count(; \+\+i\) \{)$/\1MIN(classesKeys.count, classesValues.count)\2/
s/^(        count = )\[namesValues count\];$/\1MIN([namesValues count], [namedObjects count]);/
