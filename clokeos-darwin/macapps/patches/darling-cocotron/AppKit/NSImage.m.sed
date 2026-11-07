# ClokeOS: drawInRect:fromRect:operation:fraction:respectFlipped:hints: (die übliche Zeichenmethode neuerer Programme) war ein STUB, der
# respectFlipped überging: in gekippten Ansichten standen Bilder auf dem Kopf. Jetzt wird bei respectFlipped in einem gekippten Kontext um
# das Zielrechteck gespiegelt; hints zählen nicht.
/^     respectFlipped: \(BOOL\) respectFlipped$/,/^\}$/{
s/^    printf\("STUB %s\\n", __PRETTY_FUNCTION__\);$/    NSGraphicsContext *clk_gc = [NSGraphicsContext currentContext];\n    CGContextRef clk_cg = [clk_gc graphicsPort];\n    BOOL clk_flip = respectFlipped \&\& clk_cg != NULL \&\& [clk_gc isFlipped];\n    if (clk_flip) {\n        CGContextSaveGState(clk_cg);\n        CGContextTranslateCTM(clk_cg, 0, NSMinY(rect) + NSMaxY(rect));\n        CGContextScaleCTM(clk_cg, 1, -1);\n    }/
s/^            fraction: fraction\];$/            fraction: fraction];\n    if (clk_flip)\n        CGContextRestoreGState(clk_cg);/
}
# … CLOKE_BUTTON_TRACE=1: die ersten 3000 Aufrufe mit Bildgröße, Zielrechteck, Operation, Deckkraft, CTM und Clip-Hülle (Fehlersuche)
/^     respectFlipped: \(BOOL\) respectFlipped$/,/^\}$/s/BOOL clk_flip = respectFlipped \&\& clk_cg != NULL \&\& \[clk_gc isFlipped\];\n/    BOOL clk_flip = respectFlipped \&\& clk_cg != NULL \&\& [clk_gc isFlipped];\n    {\n        static int clk_t = -1, clk_n;\n        if (clk_t < 0) clk_t = getenv("CLOKE_BUTTON_TRACE") != NULL;\n        if (clk_t \&\& clk_cg \&\& clk_n++ < 3000) {\n            CGAffineTransform m = CGContextGetCTM(clk_cg);\n            NSLog(@"[clokeos-image] %@ in %@ op=%ld frac=%.2f flip=%d ctx-flip=%d ctm=[%.0f %.0f %.0f %.0f %.0f %.0f] clip=%@", NSStringFromSize([self size]), NSStringFromRect(rect), (long) operation, fraction, (int) respectFlipped, (int) [clk_gc isFlipped], m.a, m.b, m.c, m.d, m.tx, m.ty, NSStringFromRect(NSRectFromCGRect(CGContextGetClipBoundingBox(clk_cg))));\n        }\n    }/
# … lockFocus auf ein NSCachedImageRep zeichnet in dessen eigenes Bitmap (NSCachedImageRep.m.sed), nicht in ein Fenster
/^                graphicsContextWithWindow: \[\(NSCachedImageRep \*\) representation$/,/^ +window\]\];$/c\
                graphicsContextWithGraphicsPort: (CGContextRef) [(id) representation performSelector: @selector(_clkGraphicsPort)]\
                                        flipped: NO];
