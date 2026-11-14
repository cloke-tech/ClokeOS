# ClokeOS: utlists DL_INSERT(head, x, add) fügt add VOR x ein. Darling übergab den Bereich, NACH dem der neue kommen muss:
# Die Bereichsliste wurde unsortiert (z. B. {4-5, 11, 10} nach addIndex: 4, 5, 10, 11), containsIndex: und Auszählen gingen daneben
# (Fundus: Gummiband-Auswahl, gefunden am 14.11.2026). Gilt für addIndexesInRange und für das Aufteilen in removeIndexesInRange.
s/DL_INSERT\(MULTIPLE_RANGE_DATA\(self\), ptr, newRange\);/DL_INSERT(MULTIPLE_RANGE_DATA(self), ptr->next, newRange);/
s/DL_INSERT\(MULTIPLE_RANGE_DATA\(self\), ptr, second\);/DL_INSERT(MULTIPLE_RANGE_DATA(self), ptr->next, second);/
