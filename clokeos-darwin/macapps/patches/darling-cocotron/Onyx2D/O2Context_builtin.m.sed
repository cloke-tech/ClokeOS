# ClokeOS: Transparenzebenen (CGContextBeginTransparencyLayer) leiteten das Zeichnen nicht um: alles ging weiter direkt ins Ziel, und am
# Ende wurde eine leere Ebene darübergelegt. Eine Einfärbung in einer Ebene (Bild zeichnen, dann mit "source-in" füllen; Logic Pros
# Spursymbole) traf so den ganzen Hintergrund. Jetzt zeichnet der Kontext bis endTransparencyLayer in eine eigene, leere Fläche gleicher
# Größe und Art; am Ende wird sie im Clip-Bereich mit dem Alpha des Grafikzustands über das Ziel gelegt (source-over; Schatten wie bisher).
s|^#import <Onyx2D/VGPath.h>$|#import <Onyx2D/VGPath.h>\n#import <objc/runtime.h>\n#include <string.h>\nstatic char clk_surface_stack;|
/^- \(void\) beginTransparencyLayerWithInfo: \(NSDictionary \*\) unused \{$/,/^\}$/c\
- (void) beginTransparencyLayerWithInfo: (NSDictionary *) unused {\
    NSMutableArray *stack = objc_getAssociatedObject(self, &clk_surface_stack);\
    if (stack == nil) {\
        stack = [NSMutableArray array];\
        objc_setAssociatedObject(self, &clk_surface_stack, stack, OBJC_ASSOCIATION_RETAIN);\
    }\
    O2Surface *layer = [self createSurfaceWithWidth: O2ImageGetWidth(_surface) height: O2ImageGetHeight(_surface)];\
    if (layer == nil)\
        return;\
    unsigned char *lb = O2SurfaceGetPixelBytes(layer);\
    if (lb)\
        memset(lb, 0, O2ImageGetBytesPerRow(layer) * O2ImageGetHeight(layer));\
    [stack addObject: _surface];\
    [_surface release];\
    _surface = layer;\
    O2ContextSaveGState(self);\
    O2GStateSetBlendMode(O2ContextCurrentGState(self), kO2BlendModeNormal);\
    [O2ContextCurrentGState(self) setShadowOffset: O2SizeZero blur: 0. color: nil];\
    O2GStateSetAlpha(O2ContextCurrentGState(self), 1.);\
}
/^- \(void\) endTransparencyLayer \{$/,/^\}$/c\
- (void) endTransparencyLayer {\
    NSMutableArray *stack = objc_getAssociatedObject(self, &clk_surface_stack);\
    if ([stack count] == 0)\
        return;\
    O2Surface *layer = _surface;\
    _surface = [[stack lastObject] retain];\
    [stack removeLastObject];\
    O2ContextRestoreGState(self);\
    O2GState *g = O2ContextCurrentGState(self);\
    if (g->_shadowKernel) {\
        O2Size size = [self size];\
        O2Surface *shadow = [self createSurfaceWithWidth: O2ImageGetWidth(_surface) height: O2ImageGetHeight(_surface)];\
        O2SurfaceGaussianBlur(shadow, layer, g->_shadowKernel, g->_shadowColor);\
        O2ContextDrawImage(self, O2RectMake(g->_shadowOffset.width, g->_shadowOffset.height, size.width, size.height), shadow);\
        [shadow release];\
    }\
    /* im Clip-Bereich (Viewport) vormultipliziert über das Ziel legen; Alphakanal je nach Bytefolge vorn oder hinten */\
    O2BitmapInfo info = O2ImageGetBitmapInfo(_surface);\
    int alphaFirst = ((info & kO2BitmapAlphaInfoMask) == kO2ImageAlphaPremultipliedFirst || (info & kO2BitmapAlphaInfoMask) == kO2ImageAlphaFirst || (info & kO2BitmapAlphaInfoMask) == kO2ImageAlphaNoneSkipFirst);\
    int little = (info & kO2BitmapByteOrderMask) == kO2BitmapByteOrder32Little;\
    int ai = (alphaFirst == little) ? 3 : 0;\
    unsigned char *src = O2SurfaceGetPixelBytes(layer), *dst = O2SurfaceGetPixelBytes(_surface);\
    size_t sbpr = O2ImageGetBytesPerRow(layer), dbpr = O2ImageGetBytesPerRow(_surface);\
    int w = (int) O2ImageGetWidth(_surface), h = (int) O2ImageGetHeight(_surface);\
    int x0 = MAX(0, _vpx), y0 = MAX(0, _vpy), x1 = MIN(w, _vpx + _vpwidth), y1 = MIN(h, _vpy + _vpheight);\
    int ga = (int) (O2GStateAlpha(g) * 255.0 + 0.5);\
    if (src && dst && O2ImageGetBitsPerPixel(_surface) == 32 && O2ImageGetBitsPerPixel(layer) == 32)\
        for (int y = y0; y < y1; y++) {\
            unsigned char *s = src + y * sbpr + x0 * 4, *d = dst + y * dbpr + x0 * 4;\
            for (int x = x0; x < x1; x++, s += 4, d += 4) {\
                int sa = s[ai] * ga / 255;\
                if (sa == 0)\
                    continue;\
                int inv = 255 - sa;\
                for (int k = 0; k < 4; k++)\
                    d[k] = (unsigned char) ((s[k] * ga / 255) + (d[k] * inv + 127) / 255);\
            }\
        }\
    [layer release];\
}
