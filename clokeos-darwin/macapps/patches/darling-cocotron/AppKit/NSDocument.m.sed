# ClokeOS: -init darf ein anderes Objekt liefern (Logic Pros Dokumentklasse tut es): "[self init];" warf das Ergebnis weg und
# die Methode arbeitete mit dem freigegebenen alten self weiter (Absturz in objc_msgSend, isa == 0). Wie bei Apple: self = [self init].
s/^( *)\[self init\];/\1self = [self init];/
