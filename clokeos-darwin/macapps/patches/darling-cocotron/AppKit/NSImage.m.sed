# ClokeOS: drawInRect:fromRect:operation:fraction:respectFlipped:hints: (die übliche Zeichenmethode neuerer Programme) war ein STUB, der
# respectFlipped überging: in gekippten Ansichten standen Bilder auf dem Kopf. Jetzt wird bei respectFlipped in einem gekippten Kontext um
# das Zielrechteck gespiegelt; hints zählen nicht.
/^     respectFlipped: \(BOOL\) respectFlipped$/,/^\}$/{
s/^    printf\("STUB %s\\n", __PRETTY_FUNCTION__\);$/    NSGraphicsContext *clk_gc = [NSGraphicsContext currentContext];\n    CGContextRef clk_cg = [clk_gc graphicsPort];\n    BOOL clk_flip = respectFlipped \&\& clk_cg != NULL \&\& [clk_gc isFlipped];\n    if (clk_flip) {\n        CGContextSaveGState(clk_cg);\n        CGContextTranslateCTM(clk_cg, 0, NSMinY(rect) + NSMaxY(rect));\n        CGContextScaleCTM(clk_cg, 1, -1);\n    }/
s/^            fraction: fraction\];$/            fraction: fraction];\n    if (clk_flip)\n        CGContextRestoreGState(clk_cg);/
}
