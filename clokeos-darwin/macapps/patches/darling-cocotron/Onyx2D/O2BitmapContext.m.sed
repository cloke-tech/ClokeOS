# ClokeOS: Bitmap-Kontexte unmöglicher Größe (über 1 GiB, auch mit vom Aufrufer gestelltem Speicher, Seiten über 32768 Punkte) werden
# 1 × 1 groß mit eigenem Speicher. Logic Pros MALiveLoopsUI bat (bei noch nicht ausgelegten Ansichten) um ein Bild von rund 16 TB; Onyx2D
# legte es an und kopierte minutenlang. Apple gäbe NULL zurück, aber Logic zeichnet dann ungeprüft in den NULL-Kontext, und nicht jede
# Onyx2D-Funktion verträgt das; so geht die Zeichnung ins Leere.
/^O2ContextRef O2BitmapContextCreateWithData\($/,/^\{$/s/^\{$/{\n    if ((width > 32768 || height > 32768 || (unsigned long long) height * (bytesPerRow ? bytesPerRow : width * 4) > (1ULL << 30))) {\n        static int warned;\n        if (!warned++)\n            NSLog(@"O2BitmapContextCreate: %zux%zu zu groß, 1x1", width, height);\n        data = NULL; width = 1; height = 1; bytesPerRow = 0;\n    }/
# … und ein Bild aus einem NULL-Kontext ist NULL (wie bei Apple), statt eines Absturzes
/^O2ImageRef O2BitmapContextCreateImage\(O2ContextRef selfX\) \{$/,/^\}$/s/^    return O2SurfaceCreateImage\(self->_surface\);$/    if (self == NULL)\n        return NULL;\n    return O2SurfaceCreateImage(self->_surface);/
