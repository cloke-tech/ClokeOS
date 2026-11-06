# ClokeOS: wie bei Apple liefert ein NULL-Bild NULL statt eines Absturzes (Logic Pro gibt ein fehlendes @2x-Bild weiter)
s/^(O2ImageRef O2ImageCreateWithImageInRect\(O2ImageRef self, O2Rect rect\) \{)$/\1\n    if (self == NULL)\n        return NULL;/
