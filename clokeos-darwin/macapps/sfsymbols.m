/* Werkzeug für den Mac (nicht für ClokeOS): SF Symbols als Vorlagenbilder (PNG, schwarz mit Alpha) für ClokeOS' AppKit.
 *
 * Apples Symbole sind Teil von macOS und dürfen nicht ins Repository; dieses Werkzeug rendert die Symbole, die ein Programm braucht, mit
 * Apples AppKit (+[NSImage imageWithSystemSymbolName:accessibilityDescription:], Standardgröße) nach <Ziel>/<Name>.png. ClokeOS' AppKit
 * (macapps/appkit/modern.m) lädt sie aus /Library/ClokeOS/Symbols als Vorlagenbilder.
 *
 *   clang -fobjc-arc -framework AppKit macapps/sfsymbols.m -o sfsymbols && ./sfsymbols <Namensliste> <Ziel>
 * Namensliste: ein Name je Zeile (z. B. aus "Symbol fehlt: <Name>" in CLOKE_TRACE-Ausgaben, macapps/sfsymbols.sh sammelt sie).
 * Unbekannte Namen werden übersprungen. */
#import <AppKit/AppKit.h>

static BOOL writePNG(NSImage *img, NSString *path) {
    NSSize s = img.size;
    if (s.width <= 0 || s.height <= 0 || s.width > 512 || s.height > 512) return NO;
    NSInteger w = (NSInteger) ceil(s.width), h = (NSInteger) ceil(s.height);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes: NULL pixelsWide: w pixelsHigh: h bitsPerSample: 8
                                                                samplesPerPixel: 4 hasAlpha: YES isPlanar: NO
                                                                 colorSpaceName: NSDeviceRGBColorSpace bytesPerRow: 0 bitsPerPixel: 32];
    rep.size = s;
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep: rep];
    [img drawInRect: NSMakeRect(0, 0, s.width, s.height) fromRect: NSZeroRect operation: NSCompositingOperationSourceOver fraction: 1];
    [NSGraphicsContext restoreGraphicsState];
    NSData *png = [rep representationUsingType: NSBitmapImageFileTypePNG properties: @{}];
    [[NSFileManager defaultManager] createDirectoryAtPath: [path stringByDeletingLastPathComponent] withIntermediateDirectories: YES
                                               attributes: nil error: nil];
    return [png writeToFile: path atomically: NO];
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) { fprintf(stderr, "Aufruf: sfsymbols <Namensliste> <Ziel>\n"); return 2; }
        NSString *list = [NSString stringWithContentsOfFile: [NSString stringWithUTF8String: argv[1]] encoding: NSUTF8StringEncoding error: nil];
        NSString *dst = [NSString stringWithUTF8String: argv[2]];
        NSUInteger ok = 0, missing = 0;
        for (NSString *line in [list componentsSeparatedByCharactersInSet: [NSCharacterSet newlineCharacterSet]]) {
            NSString *name = [line stringByTrimmingCharactersInSet: [NSCharacterSet whitespaceCharacterSet]];
            if (name.length == 0 || [name rangeOfString: @"/"].location != NSNotFound) continue;
            NSImage *img = [NSImage imageWithSystemSymbolName: name accessibilityDescription: nil];
            if (img && writePNG(img, [dst stringByAppendingPathComponent: [name stringByAppendingString: @".png"]])) ok++;
            else missing++;
        }
        printf("%lu Symbole nach %s, %lu unbekannt\n", (unsigned long) ok, argv[2], (unsigned long) missing);
    }
    return 0;
}
