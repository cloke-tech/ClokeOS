/* NSBundle-Bildmethoden, die Cocotrons AppKit nicht hat und Mac-Programme (Logic Pro) beim Start aufrufen:
 * -[NSBundle URLForImageResource:] und -pathForImageResource: (macOS 10.6), -imageForResource: (10.7).
 * Wie bei Apple: Name mit Endung wird direkt gesucht, sonst der Reihe nach mit den Bildendungen. Bilder aus der Assets.car des Bundles
 * kommen als PNG aus Resources/Assets.images/<Name>.png (macapps/carimages.m schreibt sie auf dem Mac mit Apples AppKit; Namen mit "/").
 * Liegt im X11-Backend (macapps/appkit.sh), das AppKit beim Start lädt; die Kategorie gilt dann für alle Bundles. */
#import <AppKit/NSImage.h>
#import <Foundation/Foundation.h>

@interface NSBundle (ClokeBundleImages)
- (NSURL *)URLForImageResource:(NSString *)name;
- (NSString *)pathForImageResource:(NSString *)name;
- (NSImage *)imageForResource:(NSString *)name;
@end

@implementation NSBundle (ClokeBundleImages)

- (NSURL *)URLForImageResource:(NSString *)name {
    if ([name length] == 0)
        return nil;
    NSString *ext = [name pathExtension];
    if ([ext length])
        return [self URLForResource:[name stringByDeletingPathExtension] withExtension:ext];
    static NSArray *exts;
    if (!exts)
        exts = [@[ @"png", @"jpg", @"jpeg", @"tiff", @"tif", @"gif", @"pdf", @"icns", @"bmp", @"heic", @"svg" ] retain];
    for (NSString *e in exts) {
        NSURL *url = [self URLForResource:name withExtension:e];
        if (url)
            return url;
    }
    NSString *res = [self resourcePath];
    if (res) {
        NSString *p = [[[res stringByAppendingPathComponent:@"Assets.images"] stringByAppendingPathComponent:name] stringByAppendingPathExtension:@"png"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:p])
            return [NSURL fileURLWithPath:p];
    }
    return nil;
}

- (NSString *)pathForImageResource:(NSString *)name {
    return [[self URLForImageResource:name] path];
}

- (NSImage *)imageForResource:(NSString *)name {
    NSURL *url = [self URLForImageResource:name];
    return url ? [[[NSImage alloc] initWithContentsOfURL:url] autorelease] : nil;
}

@end
