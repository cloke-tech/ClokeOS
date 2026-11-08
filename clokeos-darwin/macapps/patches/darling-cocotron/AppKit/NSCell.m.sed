# ClokeOS: NSDrawThreePartImage und NSDrawNinePartImage waren leer (Logic Pros Schalter und Regler zeichnen damit ihre Ränder).
# Ecken/Enden in ihrer Größe, die Füllstücke auf den Rest gestreckt (wie bei Apple).
/^void NSDrawThreePartImage\(NSRect frame, NSImage \*startCap, NSImage \*centerFill,$/,/^\}$/c\
static void clk_drawpart(NSImage *img, NSRect r, NSCompositingOperation op, CGFloat alpha) {\
    if (img == nil || r.size.width <= 0 || r.size.height <= 0)\
        return;\
    [img drawInRect: r fromRect: NSZeroRect operation: op fraction: alpha respectFlipped: YES hints: nil];\
}\
\
void NSDrawThreePartImage(NSRect frame, NSImage *startCap, NSImage *centerFill,\
                          NSImage *endCap, BOOL vertical,\
                          NSCompositingOperation operation, CGFloat alpha,\
                          BOOL flipped)\
{\
    NSSize s = startCap ? [startCap size] : NSZeroSize, e = endCap ? [endCap size] : NSZeroSize;\
    if (!vertical) {\
        CGFloat sw = MIN(s.width, frame.size.width), ew = MIN(e.width, frame.size.width - sw);\
        clk_drawpart(startCap, NSMakeRect(NSMinX(frame), NSMinY(frame), sw, frame.size.height), operation, alpha);\
        clk_drawpart(centerFill, NSMakeRect(NSMinX(frame) + sw, NSMinY(frame), frame.size.width - sw - ew, frame.size.height), operation, alpha);\
        clk_drawpart(endCap, NSMakeRect(NSMaxX(frame) - ew, NSMinY(frame), ew, frame.size.height), operation, alpha);\
    } else {\
        /* senkrecht: startCap oben; im nicht gekippten System liegt oben bei MaxY */\
        CGFloat sh = MIN(s.height, frame.size.height), eh = MIN(e.height, frame.size.height - sh);\
        CGFloat topY = flipped ? NSMinY(frame) : NSMaxY(frame) - sh;\
        CGFloat botY = flipped ? NSMaxY(frame) - eh : NSMinY(frame);\
        clk_drawpart(startCap, NSMakeRect(NSMinX(frame), topY, frame.size.width, sh), operation, alpha);\
        clk_drawpart(centerFill, NSMakeRect(NSMinX(frame), flipped ? NSMinY(frame) + sh : NSMinY(frame) + eh, frame.size.width, frame.size.height - sh - eh), operation, alpha);\
        clk_drawpart(endCap, NSMakeRect(NSMinX(frame), botY, frame.size.width, eh), operation, alpha);\
    }\
}
/^void NSDrawNinePartImage\(NSRect frame, NSImage\* topLeftCorner,$/,/^\};$/c\
void NSDrawNinePartImage(NSRect frame, NSImage* topLeftCorner,\
                          NSImage* topEdgeFill, NSImage* topRightCorner,\
                          NSImage* leftEdgeFill, NSImage* centerFill,\
                          NSImage* rightEdgeFill, NSImage* bottomLeftCorner,\
                          NSImage* bottomEdgeFill, NSImage* bottomRightCorner,\
                          NSCompositingOperation operation, CGFloat alpha,\
                          BOOL flipped)\
{\
    NSSize tl = topLeftCorner ? [topLeftCorner size] : NSZeroSize, br = bottomRightCorner ? [bottomRightCorner size] : NSZeroSize;\
    CGFloat lw = MIN(tl.width, frame.size.width), rw = MIN(br.width, frame.size.width - lw);\
    CGFloat th = MIN(tl.height, frame.size.height), bh = MIN(br.height, frame.size.height - th);\
    CGFloat cw = frame.size.width - lw - rw, ch = frame.size.height - th - bh;\
    CGFloat x[3] = { NSMinX(frame), NSMinX(frame) + lw, NSMaxX(frame) - rw }, w[3] = { lw, cw, rw };\
    /* Zeilen von oben: im gekippten System beginnt oben bei MinY */\
    CGFloat y[3], h[3] = { th, ch, bh };\
    if (flipped) { y[0] = NSMinY(frame); y[1] = y[0] + th; y[2] = NSMaxY(frame) - bh; }\
    else { y[0] = NSMaxY(frame) - th; y[1] = NSMinY(frame) + bh; y[2] = NSMinY(frame); }\
    NSImage *parts[9] = { topLeftCorner, topEdgeFill, topRightCorner, leftEdgeFill, centerFill, rightEdgeFill,\
                          bottomLeftCorner, bottomEdgeFill, bottomRightCorner };\
    for (int r = 0; r < 3; r++)\
        for (int c = 0; c < 3; c++)\
            clk_drawpart(parts[r * 3 + c], NSMakeRect(x[c], y[r], w[c], h[r]), operation, alpha);\
}
# ClokeOS: setIntValue:/setFloatValue:/setDoubleValue:/setIntegerValue: rufen wie bei Apple kein überschriebenes setObjectValue: einer
# Programm-Unterklasse auf (auf dem Mac nachgeprüft: NSCell, NSActionCell, NSSliderCell, NSButtonCell, NSSegmentedCell, NSImageCell; nur
# NSTextFieldCell geht über setObjectValue:). Gesetzt wird mit dem setObjectValue: der nächsten AppKit-Klasse. Logic Pros
# MAMixerPannerCell ruft in setObjectValue: [super setDoubleValue:] auf: Stapelüberlauf.
s|^(#import <AppKit/NSCell.h>)$|\1\n#import <AppKit/NSTextFieldCell.h>\n#import <objc/runtime.h>\n#import <string.h>\nstatic void clk_cell_set_number(NSCell *self, NSNumber *n) {\n    if ([self isKindOfClass: [NSTextFieldCell class]]) {\n        [self setObjectValue: n];\n        return;\n    }\n    Class c = object_getClass(self);\n    while (c != Nil) {\n        const char *img = class_getImageName(c);\n        if (img \&\& strstr(img, "/AppKit.framework/"))\n            break;\n        c = class_getSuperclass(c);\n    }\n    IMP imp = c ? class_getMethodImplementation(c, @selector(setObjectValue:)) : NULL;\n    if (imp)\n        ((void (*)(id, SEL, id)) imp)(self, @selector(setObjectValue:), n);\n    else\n        [self setObjectValue: n];\n}|
s/^    \[self setObjectValue: \[NSNumber numberWithInt: value\]\];$/    clk_cell_set_number(self, [NSNumber numberWithInt: value]);/
s/^    \[self setObjectValue: \[NSNumber numberWithFloat: value\]\];$/    clk_cell_set_number(self, [NSNumber numberWithFloat: value]);/
s/^    \[self setObjectValue: \[NSNumber numberWithDouble: value\]\];$/    clk_cell_set_number(self, [NSNumber numberWithDouble: value]);/
s/^    \[self setObjectValue: \[NSNumber numberWithInteger: value\]\];$/    clk_cell_set_number(self, [NSNumber numberWithInteger: value]);/
# ClokeOS: Textausrichtung in NSCellFlags2 steht wie bei Apple in Bit 26–28 (rechts 0x04000000, Mitte 0x08000000, natürlich 0x10000000;
# auf dem Mac nachgeprüft), in der alten Reihenfolge. Cocotron las Bit 16–18, so ging die Ausrichtung aus Nibs verloren (alles links).
s/^    NSCellAppleFlag2TextAlignmentShift    = 16,$/    NSCellAppleFlag2TextAlignmentShift    = 26,/
s/^    _textAlignment = \(flags2 \& NSCellAppleFlag2TextAlignmentMask\) >> NSCellAppleFlag2TextAlignmentShift;$/    _textAlignment = _CLKTextAlignmentFromArchive((flags2 \& NSCellAppleFlag2TextAlignmentMask) >> NSCellAppleFlag2TextAlignmentShift);/
s/^    \*flags2 \|= \(_textAlignment << NSCellAppleFlag2TextAlignmentShift\) \& NSCellAppleFlag2TextAlignmentMask;$/    *flags2 |= (_CLKTextAlignmentToArchive(_textAlignment) << NSCellAppleFlag2TextAlignmentShift) \& NSCellAppleFlag2TextAlignmentMask;/
# ClokeOS: -[NSCell drawInteriorWithFrame:inView:] war leer. Bei Apple zeichnet NSCell selbst Text (attributedStringValue) bzw. Bild.
# Direkte NSCell-Unterklassen, die [super drawWithFrame:inView:] rufen (MAToolKits Segment-Beschriftung), blieben sonst leer.
s/^(#import <AppKit\/NSCell.h>)$/\1\n#import "NSStringDrawer.h"/
/^- \(void\) drawInteriorWithFrame: \(NSRect\) frame inView: \(NSView \*\) view \{$/,/^\}$/c\
- (void) drawInteriorWithFrame: (NSRect) frame inView: (NSView *) view {\
    if (_image != nil && [self type] != NSTextCellType) {\
        NSSize s = [_image size];\
        if (s.width <= 0 || s.height <= 0)\
            return;\
        NSRect r = NSMakeRect(NSMidX(frame) - s.width / 2, NSMidY(frame) - s.height / 2, s.width, s.height);\
        [_image drawInRect: NSIntegralRect(r) fromRect: NSZeroRect operation: NSCompositeSourceOver fraction: 1.0 respectFlipped: YES hints: nil];\
        return;\
    }\
    if (_objectValue == nil || [self type] != NSTextCellType)\
        return;\
    NSAttributedString *s = [self attributedStringValue];\
    if ([s length] == 0)\
        return;\
    {\
        static int clk_t = -1;\
        if (clk_t < 0) clk_t = getenv("CLOKE_CELL_TRACE") != NULL;\
        if (clk_t) NSLog(@"[clokeos-cell] %@ type=%d str=%@ frame=%@ title=%@ ctl=%@", [self class], (int) [self type], [s string], NSStringFromRect(frame), NSStringFromRect([self titleRectForBounds: frame]), [view class]);\
    }\
    [s _clipAndDrawInRect: [self titleRectForBounds: frame] truncatingTail: _lineBreakMode > NSLineBreakByClipping];\
}
