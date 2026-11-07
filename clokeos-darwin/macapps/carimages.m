/* Werkzeug für den Mac (nicht für ClokeOS): Bilder aus den Assets.car-Dateien eines Programms als PNG-Dateien für ClokeOS' AppKit.
 *
 * Für jede Assets.car nimmt es die Bildnamen aus Apples `assetutil --info` und lädt jedes Bild mit Apples AppKit
 * (-[NSBundle imageForResource:] des Bundles, zu dem die Assets.car gehört). Geschrieben wird in einen Spiegelordner:
 *   <Ziel>/<Pfad der Resources relativ zum Programm>/Assets.images/<Name>.png (mit CARIMAGES_2X=1 auch <Name>@2x.png)
 * (Namen mit "/" ergeben Unterordner). ClokeOS' AppKit (macapps/appkit/modern.m) sucht dort, wenn imageForResource:/imageNamed: im
 * Bundle nichts finden. Vorlagenbilder (Template) bleiben schwarz mit Alpha, so wie sie gezeichnet werden.
 *
 *   clang -fobjc-arc -framework AppKit macapps/carimages.m -o carimages && ./carimages "/Applications/Logic Pro.app" <Ziel>
 * macapps/logicimage.sh baut und ruft es auf. Nichts davon landet im Repository (abgeleitet aus den Dateien des Herstellers). */
#import <AppKit/AppKit.h>

static NSArray<NSString *> *imageNames(NSString *car) {
    NSTask *t = [NSTask new];
    t.launchPath = @"/usr/bin/xcrun";
    t.arguments = @[ @"assetutil", @"--info", car ];
    NSPipe *p = [NSPipe pipe];
    t.standardOutput = p;
    t.standardError = [NSFileHandle fileHandleWithNullDevice];
    [t launch];
    NSData *d = [[p fileHandleForReading] readDataToEndOfFile];
    [t waitUntilExit];
    NSArray *items = [NSJSONSerialization JSONObjectWithData: d options: 0 error: nil];
    NSMutableOrderedSet *names = [NSMutableOrderedSet orderedSet];
    NSSet *types = [NSSet setWithArray: @[ @"Image", @"Vector", @"MultiSized Image", @"Icon Image", @"PackedImage" ]];
    for (NSDictionary *it in items)
        if ([it isKindOfClass: [NSDictionary class]] && [types containsObject: it[@"AssetType"]] && it[@"Name"])
            [names addObject: it[@"Name"]];
    return names.array;
}

static BOOL writePNG(NSImage *img, CGFloat scale, NSString *path) {
    NSSize s = img.size;
    if (s.width <= 0 || s.height <= 0 || s.width > 4096 || s.height > 4096) return NO;
    NSInteger w = (NSInteger) ceil(s.width * scale), h = (NSInteger) ceil(s.height * scale);
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
        if (argc < 3) { fprintf(stderr, "Aufruf: carimages <Programm.app> <Ziel>\n"); return 2; }
        NSString *app = [NSString stringWithUTF8String: argv[1]], *dst = [NSString stringWithUTF8String: argv[2]];
        NSString *parent = [app stringByDeletingLastPathComponent];
        NSDirectoryEnumerator *e = [[NSFileManager defaultManager] enumeratorAtPath: app];
        NSUInteger files = 0, images = 0;
        for (NSString *rel in e) {
            if (![rel.lastPathComponent isEqualToString: @"Assets.car"]) continue;
            NSString *car = [app stringByAppendingPathComponent: rel];
            NSString *res = [car stringByDeletingLastPathComponent];                  /* …/Resources */
            NSString *bpath = [res stringByDeletingLastPathComponent];                /* …/Contents oder …/Versions/A */
            if ([bpath.lastPathComponent isEqualToString: @"Contents"]) bpath = [bpath stringByDeletingLastPathComponent];
            else bpath = [[bpath stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];   /* …/X.framework */
            NSBundle *b = [NSBundle bundleWithPath: bpath];
            if (!b) continue;
            NSString *out = [[dst stringByAppendingPathComponent: [res substringFromIndex: parent.length + 1]] stringByAppendingPathComponent: @"Assets.images"];
            NSUInteger n = 0;
            for (NSString *name in imageNames(car)) {
                NSImage *img = [b imageForResource: name];
                if (!img) continue;
                BOOL ok = writePNG(img, 1, [out stringByAppendingPathComponent: [name stringByAppendingString: @".png"]]);
                if (getenv("CARIMAGES_2X"))   /* ClokeOS zeichnet mit Faktor 1; @2x nur auf Wunsch (verdreifacht den Platz) */
                    writePNG(img, 2, [out stringByAppendingPathComponent: [name stringByAppendingString: @"@2x.png"]]);
                if (ok) n++;
            }
            printf("%s: %lu Bilder\n", [rel UTF8String], (unsigned long) n);
            files++; images += n;
        }
        printf("%lu Assets.car, %lu Bilder nach %s\n", (unsigned long) files, (unsigned long) images, argv[2]);
    }
    return 0;
}
