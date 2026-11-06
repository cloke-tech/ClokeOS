# ClokeOS: Bitmap-Kontexte unmöglicher Größe (über 1 GiB, auch mit vom Aufrufer gestelltem Speicher, Seiten über 32768 Punkte) gibt es nicht: NULL wie bei Apple, wenn
# CoreGraphics den Speicher nicht anlegen kann. Logic Pros MALiveLoopsUI bat (bei noch nicht ausgelegten Ansichten) um ein Bild von rund
# 16 TB; Onyx2D legte es an und kopierte minutenlang.
/^O2ContextRef O2BitmapContextCreateWithData\($/,/^\{$/s/^\{$/{\n    if ((width > 32768 || height > 32768 || (unsigned long long) height * (bytesPerRow ? bytesPerRow : width * 4) > (1ULL << 30))) {\n        static int warned;\n        if (!warned++)\n            NSLog(@"O2BitmapContextCreate: %zux%zu zu groß, NULL", width, height);\n        return NULL;\n    }/
# … und ein Bild aus einem NULL-Kontext ist NULL (wie bei Apple), statt eines Absturzes
/^O2ImageRef O2BitmapContextCreateImage\(O2ContextRef selfX\) \{$/,/^\}$/s/^    return O2SurfaceCreateImage\(self->_surface\);$/    if (self == NULL)\n        return NULL;\n    return O2SurfaceCreateImage(self->_surface);/
