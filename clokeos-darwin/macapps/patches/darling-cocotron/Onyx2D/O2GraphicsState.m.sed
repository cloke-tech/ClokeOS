# ClokeOS: Rechtecke zwischen Benutzer- und Gerätesystem umrechnen (Hüllrechteck der vier Ecken; waren nicht umgesetzt)
/^- \(O2Rect\) convertRectToDeviceSpace: \(O2Rect\) rect \{$/,/^\}$/c\
static O2Rect clk_rectApply(O2Rect rect, O2AffineTransform t) {\
    O2Point p[4] = { O2PointApplyAffineTransform(O2PointMake(rect.origin.x, rect.origin.y), t),\
                     O2PointApplyAffineTransform(O2PointMake(rect.origin.x + rect.size.width, rect.origin.y), t),\
                     O2PointApplyAffineTransform(O2PointMake(rect.origin.x, rect.origin.y + rect.size.height), t),\
                     O2PointApplyAffineTransform(O2PointMake(rect.origin.x + rect.size.width, rect.origin.y + rect.size.height), t) };\
    O2Float x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;\
    for (int i = 1; i < 4; i++) {\
        x0 = MIN(x0, p[i].x); x1 = MAX(x1, p[i].x);\
        y0 = MIN(y0, p[i].y); y1 = MAX(y1, p[i].y);\
    }\
    return O2RectMake(x0, y0, x1 - x0, y1 - y0);\
}\
\
- (O2Rect) convertRectToDeviceSpace: (O2Rect) rect {\
    return clk_rectApply(rect, _deviceSpaceTransform);\
}
/^- \(O2Rect\) convertRectToUserSpace: \(O2Rect\) rect \{$/,/^\}$/c\
- (O2Rect) convertRectToUserSpace: (O2Rect) rect {\
    return clk_rectApply(rect, O2AffineTransformInvert(_deviceSpaceTransform));\
}
