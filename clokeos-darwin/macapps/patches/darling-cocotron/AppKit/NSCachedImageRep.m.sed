# ClokeOS: ein NSCachedImageRep zeichnet in ein eigenes Bitmap genau seiner Größe statt in ein verstecktes X11-Fenster. Dessen Fläche
# unter CloakServer muss nicht dem Inhalt entsprechen (Mindestgrößen, Rahmen), das zurückgelesene Bild war verschoben/gestaucht: Bilder,
# die ein Programm mit -[NSImage lockFocus] selbst malt (Logic Pros Spursymbole), zeigten nur eine Ecke. Mit initWithWindow:rect: bleibt
# es beim Fenster.
s|^#import <CoreGraphics/CGWindow.h>$|#import <CoreGraphics/CGWindow.h>\n#import <objc/runtime.h>\n#include <math.h>\nstatic char clk_bitmap;|
/^- \(instancetype\) initWithSize: \(NSSize\) size$/,/^\}$/c\
- (instancetype) initWithSize: (NSSize) size\
                        depth: (NSWindowDepth) windowDepth\
                     separate: (BOOL) separateWindow\
                        alpha: (BOOL) hasAlpha\
{\
    _size = size;\
    _origin = NSZeroPoint;\
    _window = nil;\
    size_t w = size.width >= 1 ? (size_t) ceil(size.width) : 1, h = size.height >= 1 ? (size_t) ceil(size.height) : 1;\
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();\
    CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);\
    CGColorSpaceRelease(cs);\
    if (c) {\
        objc_setAssociatedObject(self, &clk_bitmap, (id) c, OBJC_ASSOCIATION_RETAIN);\
        CGContextRelease(c);\
    }\
    return self;\
}\
\
- (CGContextRef) _clkGraphicsPort {\
    CGContextRef c = (CGContextRef) objc_getAssociatedObject(self, &clk_bitmap);\
    return c ? c : [[_window graphicsContext] graphicsPort];\
}
s/^            \[\[_window graphicsContext\] graphicsPort\]\);$/            [self _clkGraphicsPort]);/
