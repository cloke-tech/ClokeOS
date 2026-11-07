# ClokeOS: CGContextGetClipBoundingBox gab immer ein leeres Rechteck (O2GState clipBoundingBox war nicht umgesetzt). Ansichten, die nur
# zeichnen, was im Clip liegt (Logic Pros Spurköpfe und Arrangierbereich), zeichneten so nichts. Jetzt: Gerätefläche (bei Bitmap-Kontexten
# deren Größe) geschnitten mit den Hüllrechtecken aller Clip-Pfade (im Gerätesystem gespeichert), zurück ins Benutzersystem gerechnet.
# Masken schränken nicht ein (wie ein Rechteck um die ganze Fläche).
/^O2Rect O2ContextGetClipBoundingBox\(O2ContextRef self\) \{$/,/^\}$/c\
O2Rect O2ContextGetClipBoundingBox(O2ContextRef self) {\
    if (self == nil)\
        return O2RectZero;\
\
    O2GState *gState = O2ContextCurrentGState(self);\
    O2Rect r = O2RectMake(-1.0e7, -1.0e7, 2.0e7, 2.0e7);\
    if ([self isKindOfClass: [O2BitmapContext class]])\
        r = O2RectMake(0, 0, O2BitmapContextGetWidth(self), O2BitmapContextGetHeight(self));\
    for (O2ClipPhase *phase in [O2GStateClipState(gState) clipPhases]) {\
        if (O2ClipPhasePhaseType(phase) == O2ClipPhaseMask)\
            continue;\
        r = O2RectIntersection(r, O2PathGetBoundingBox((O2PathRef) O2ClipPhaseObject(phase)));\
    }\
    if (r.size.width <= 0 || r.size.height <= 0)\
        return O2RectZero;\
    O2AffineTransform inv = O2AffineTransformInvert(gState->_deviceSpaceTransform);\
    O2Point p[4] = { O2PointApplyAffineTransform(O2PointMake(O2RectGetMinX(r), O2RectGetMinY(r)), inv),\
                     O2PointApplyAffineTransform(O2PointMake(O2RectGetMaxX(r), O2RectGetMinY(r)), inv),\
                     O2PointApplyAffineTransform(O2PointMake(O2RectGetMinX(r), O2RectGetMaxY(r)), inv),\
                     O2PointApplyAffineTransform(O2PointMake(O2RectGetMaxX(r), O2RectGetMaxY(r)), inv) };\
    O2Float x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;\
    for (int i = 1; i < 4; i++) {\
        x0 = MIN(x0, p[i].x); x1 = MAX(x1, p[i].x);\
        y0 = MIN(y0, p[i].y); y1 = MAX(y1, p[i].y);\
    }\
    return O2RectMake(x0, y0, x1 - x0, y1 - y0);\
}
s|^#import <Onyx2D/O2ClipPhase.h>$|#import <Onyx2D/O2ClipPhase.h>\
#import <Onyx2D/O2ClipState.h>\
#import <Onyx2D/O2Path.h>|
# ClokeOS: CGLayerCreateWithContext rief -initWithSize:context: auf, das es in Onyx2D nirgends gibt; jede CGLayer hatte so keinen Kontext
# und alles, was ein Programm über CGLayers zwischenspeichert (Logic Pros Spurköpfe, Regionen), verschwand. Jetzt ein durchsichtiger
# RGBA-Bitmap-Kontext in der Größe der Ebene (Onyx2Ds eigener Rasterer, wie das Fenster).
/^- \(O2ContextRef\) createCompatibleContextWithSize: \(O2Size\) size$/,/^\}$/c\
- (O2ContextRef) createCompatibleContextWithSize: (O2Size) size\
                                          unused: (NSDictionary *) unused\
{\
    size_t w = size.width >= 1 ? (size_t) ceil(size.width) : 1, h = size.height >= 1 ? (size_t) ceil(size.height) : 1;\
    O2ColorSpaceRef cs = O2ColorSpaceCreateDeviceRGB();\
    O2ContextRef result = O2BitmapContextCreate(NULL, w, h, 8, 0, cs, kO2ImageAlphaPremultipliedFirst | kO2BitmapByteOrder32Little);\
    O2ColorSpaceRelease(cs);\
    return result;\
}
