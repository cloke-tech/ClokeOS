/* Neuere AppKit-Klassenmethoden, die Cocotron fehlt und die Mac-Programme beim Start brauchen (Logic Pro: Schriften und Farben für
 * Textattribute; ein nil an dieser Stelle wirft beim Anlegen des Attribut-Wörterbuchs eine Ausnahme mitten im Programmstart).
 * Eingetragen mit class_addMethod: was AppKit schon hat, bleibt unverändert.
 *   NSFont: systemFontOfSize:weight:, monospacedDigitSystemFontOfSize:weight:, monospacedSystemFontOfSize:weight:,
 *           preferredFontForTextStyle:options:
 *   NSColor: systemRed/Green/Blue/…/GrayColor, label-, secondaryLabel-, tertiaryLabel-, quaternaryLabelColor, separatorColor, linkColor,
 *            placeholderTextColor, controlAccentColor, … (Werte der hellen Darstellung von macOS)
 *   Foundation (Darling): -[NSNumberFormatter/NSDateFormatter stringForObjectValue:] über stringFromNumber:/stringFromDate:
 *            (sonst wirft NSFormatter "requires a subclass implementation", Logic Pro beim Laden einer Fenster-Nib)
 *   NSColor: colorWithColorSpace:components:count: (nach Zahl der Anteile: Grau+Alpha, RGB, RGB+Alpha; der Farbraum zählt nicht),
 *            type (immer NSColorTypeComponentBased)
 *   NSColor: colorNamed:, colorNamed:bundle: aus Assets.colors.plist neben der Assets.car des Bundles (macapps/carcolors.py erzeugt sie auf
 *            dem Mac mit Apples assetutil); Namen wie bei Apple, dazu "Colors/A/B" für "A.B" (so fragt Logic Pros MAAssetManager)
 *   NSFont: copyName, boundingRect (KTFont-Methoden, die CoreTexts CTFont*-Funktionen aufrufen; CTFontCreateWithName liefert NSFonts)
 *   NSCell: _textAttributes (private Apple-Methode, gibt ein veränderbares Wörterbuch mit Schrift, Farbe und Absatzstil; Logic Pros MAToolKit-
 *            Zellen rufen sie über super auf und ändern das Ergebnis)
 *   NSFontDescriptor: fontDescriptorWithDesign: gibt den Deskriptor unverändert zurück (keine abgerundeten/Monospace-Varianten der Systemschrift)
 *   NSGraphicsContext: CGContext (neuer Name von graphicsPort; ohne ihn zeichnete Logic Pro in einen NULL-Kontext)
 *   NSWindow/NSScreen: backingScaleFactor 1.0 (kein HiDPI); CALayer contentsScale (1.0, Setzen wird ignoriert); NSDate now;
 *   NSImage imageWithSize:flipped:drawingHandler: (zeichnet sofort in ein Bild der Größe), imageWithSymbolConfiguration: (das Bild selbst)
 *   NSImage imageNamed: findet Bilder aus der Assets.car des Hauptbundles über PNG-Dateien in Resources/Assets.images/ (macapps/carimages.m
 *            schreibt sie auf dem Mac mit Apples AppKit; für andere Bundles macht das -imageForResource:, appkit/nsbundle_images.m)
 *   NSTabViewItem: tabViewItemWithViewController:, viewController/setViewController: (Kennung = der Controller, Ansicht und Titel von ihm)
 *   NSWorkspace: type:conformsToType: (gleich, oder über die UTTypeConformsTo-Listen, die Info.plist des Programms erklärt, transitiv; dazu
 *            public.item/public.data/public.content für alles), isFilePackageAtPath: (Ordner mit Endung)
 *   NSArray (Darling): setValue:forKey: setzt den Wert wie bei Apple bei jedem Element (Cocotrons NSObjectController-Auswahl schreibt
 *            so in die gewählten Objekte; Logic Pro: Bindung selection.keySignature)
 *   NSImage: imageWithSystemSymbolName:accessibilityDescription:, imageWithSymbolName:…: SF Symbols gibt es hier nicht; statt nil (Logic
 *            legt das Ergebnis ungeprüft in ein Wörterbuch) ein leeres Bild 16 × 16 */
#import <AppKit/NSImage.h>
#import <AppKit/NSAttributedString.h>
#import <AppKit/NSParagraphStyle.h>
#import <AppKit/NSCell.h>
#import <Foundation/NSNumberFormatter.h>
#import <Foundation/NSDateFormatter.h>
#import <AppKit/NSFont.h>
#import <AppKit/NSColor.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <string.h>

/* Gewicht wie NSFontWeight (-1 … 1); ab Medium (0.23) fett, mehr Stufen hat Cocotrons Schriftwahl nicht */
static id f_sys_weight(Class c, SEL s, CGFloat size, CGFloat weight) {
    return weight >= 0.23 ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
}
static id f_mono_weight(Class c, SEL s, CGFloat size, CGFloat weight) {
    id f = [NSFont userFixedPitchFontOfSize:size];
    return f ? f : f_sys_weight(c, s, size, weight);
}
static id f_text_style(Class c, SEL s, id style, id options) {
    static const struct { const char *name; CGFloat size; int bold; } st[] = {
        { "NSCTFontUIFontBody", 13, 0 }, { "NSCTFontUIFontHeadline", 13, 1 }, { "NSCTFontUIFontSubheadline", 11, 0 },
        { "NSCTFontUIFontCallout", 12, 0 }, { "NSCTFontUIFontFootnote", 10, 0 }, { "NSCTFontUIFontCaption1", 10, 0 },
        { "NSCTFontUIFontCaption2", 10, 0 }, { "NSCTFontUIFontTitle1", 22, 0 }, { "NSCTFontUIFontTitle2", 17, 0 },
        { "NSCTFontUIFontTitle3", 15, 0 }, { "NSCTFontUIFontLargeTitle", 26, 0 } };
    CGFloat size = 13; int bold = 0;
    const char *n = [style isKindOfClass:[NSString class]] ? [style UTF8String] : NULL;
    for (size_t i = 0; n && i < sizeof st / sizeof *st; i++)
        if (!strcmp(n, st[i].name)) { size = st[i].size; bold = st[i].bold; break; }
    return bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
}

static const struct { const char *sel; CGFloat r, g, b, a; } colors[] = {
    { "systemRedColor", 1.0, 0.231, 0.188, 1 },    { "systemOrangeColor", 1.0, 0.584, 0.0, 1 },
    { "systemYellowColor", 1.0, 0.8, 0.0, 1 },     { "systemGreenColor", 0.157, 0.804, 0.255, 1 },
    { "systemMintColor", 0.0, 0.78, 0.745, 1 },    { "systemTealColor", 0.349, 0.678, 0.769, 1 },
    { "systemCyanColor", 0.333, 0.745, 0.941, 1 }, { "systemBlueColor", 0.0, 0.478, 1.0, 1 },
    { "systemIndigoColor", 0.345, 0.337, 0.839, 1 }, { "systemPurpleColor", 0.686, 0.322, 0.871, 1 },
    { "systemPinkColor", 1.0, 0.176, 0.333, 1 },   { "systemBrownColor", 0.635, 0.518, 0.369, 1 },
    { "systemGrayColor", 0.557, 0.557, 0.576, 1 }, { "labelColor", 0, 0, 0, 0.847 },
    { "secondaryLabelColor", 0, 0, 0, 0.498 },     { "tertiaryLabelColor", 0, 0, 0, 0.259 },
    { "quaternaryLabelColor", 0, 0, 0, 0.098 },    { "separatorColor", 0, 0, 0, 0.098 },
    { "linkColor", 0.0, 0.408, 0.855, 1 },          { "placeholderTextColor", 0, 0, 0, 0.247 },
    { "controlAccentColor", 0.0, 0.478, 1.0, 1 },  { "systemFillColor", 0, 0, 0, 0.098 },
    { "secondarySystemFillColor", 0, 0, 0, 0.078 }, { "tertiarySystemFillColor", 0, 0, 0, 0.047 },
    { "quaternarySystemFillColor", 0, 0, 0, 0.031 }, { "unemphasizedSelectedContentBackgroundColor", 0.863, 0.863, 0.863, 1 },
    { "unemphasizedSelectedTextBackgroundColor", 0.863, 0.863, 0.863, 1 }, { "selectedContentBackgroundColor", 0.0, 0.392, 0.882, 1 },
    { "underPageBackgroundColor", 0.588, 0.588, 0.588, 0.9 }, { "findHighlightColor", 1.0, 1.0, 0.0, 1 },
};

static id c_named(Class c, SEL s) {
    const char *n = sel_getName(s);
    for (size_t i = 0; i < sizeof colors / sizeof *colors; i++)
        if (!strcmp(n, colors[i].sel))
            return [NSColor colorWithCalibratedRed:colors[i].r green:colors[i].g blue:colors[i].b alpha:colors[i].a];
    return [NSColor blackColor];
}

static id fmt_number(id self, SEL s, id v) {
    if ([v isKindOfClass:[NSNumber class]]) return [self stringFromNumber:v];
    return [v isKindOfClass:[NSString class]] ? v : nil;
}
static id fmt_date(id self, SEL s, id v) {
    if ([v isKindOfClass:[NSDate class]]) return [self stringFromDate:v];
    return [v isKindOfClass:[NSString class]] ? v : nil;
}

static id img_symbol(Class c, SEL s, id name, id other) {
    return [[[NSImage alloc] initWithSize: NSMakeSize(16, 16)] autorelease];
}
static id img_symbol3(Class c, SEL s, id name, id bundle, double value) {
    return img_symbol(c, s, name, bundle);
}

static id c_space_components(Class c, SEL s, id space, const CGFloat *comp, NSInteger count) {
    if (comp == NULL || count <= 0) return [NSColor blackColor];
    if (count <= 2) return [NSColor colorWithCalibratedWhite: comp[0] alpha: count == 2 ? comp[1] : 1];
    return [NSColor colorWithCalibratedRed: comp[0] green: comp[1] blue: comp[2] alpha: count >= 4 ? comp[3] : 1];
}
static NSInteger c_type(id self, SEL s) { return 0; }

static NSDictionary *named_colors(NSBundle *b) {
    static NSMutableDictionary *cache;
    if (b == nil) return nil;
    @synchronized([NSColor class]) {
        if (!cache) cache = [NSMutableDictionary new];
        NSString *key = [b bundlePath] ?: @"";
        id d = cache[key];
        if (!d) {
            NSString *p = [b pathForResource: @"Assets.colors" ofType: @"plist"];
            d = (p ? [NSDictionary dictionaryWithContentsOfFile: p] : nil) ?: (id)[NSNull null];
            cache[key] = d;
        }
        return d == [NSNull null] ? nil : d;
    }
}
static NSArray *lookup_color(NSDictionary *d, NSString *name) {
    NSArray *c = d[name];
    if (!c && [name rangeOfString: @"."].location != NSNotFound) {
        NSString *slash = [name stringByReplacingOccurrencesOfString: @"." withString: @"/"];
        c = d[[@"Colors/" stringByAppendingString: slash]] ?: d[slash];
    }
    if (!c) c = d[[@"Colors/" stringByAppendingString: name]];
    return [c isKindOfClass: [NSArray class]] && [c count] >= 4 ? c : nil;
}
static id c_named_bundle(Class cls, SEL s, NSString *name, NSBundle *bundle) {
    if (![name isKindOfClass: [NSString class]]) return nil;
    NSArray *c = lookup_color(named_colors(bundle ?: [NSBundle mainBundle]), name);
    if (!c && bundle) c = lookup_color(named_colors([NSBundle mainBundle]), name);
    if (!c) return nil;
    return [NSColor colorWithCalibratedRed: [c[0] doubleValue] green: [c[1] doubleValue] blue: [c[2] doubleValue] alpha: [c[3] doubleValue]];
}
static id c_named_main(Class cls, SEL s, NSString *name) { return c_named_bundle(cls, s, name, nil); }

static id font_copyname(NSFont *self, SEL s) { return [[self fontName] copy]; }
static NSRect font_bounds(NSFont *self, SEL s) { return [self boundingRectForFont]; }

static id cell_textattrs(NSCell *self, SEL s) {
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    NSFont *f = [self font];
    d[NSFontAttributeName] = f ? f : [NSFont systemFontOfSize: [NSFont systemFontSize]];
    NSColor *c = [self respondsToSelector: @selector(textColor)] ? [(id)self textColor] : nil;
    d[NSForegroundColorAttributeName] = c ? c : ([self isEnabled] ? [NSColor controlTextColor] : [NSColor disabledControlTextColor]);
    NSMutableParagraphStyle *p = [[[NSParagraphStyle defaultParagraphStyle] mutableCopy] autorelease];
    [p setAlignment: [self alignment]];
    [p setLineBreakMode: [self lineBreakMode]];
    d[NSParagraphStyleAttributeName] = p;
    return d;
}

static id fd_design(id self, SEL s, id design) { return self; }

static void *gc_cgcontext(id self, SEL s) { return ((void *(*)(id, SEL)) objc_msgSend)(self, sel_registerName("graphicsPort")); }

static CGFloat one_scale(id self, SEL s) { return 1.0; }
static void ignore_scale(id self, SEL s, CGFloat v) {}
static id date_now(Class c, SEL s) { return [NSDate date]; }
static id img_self(id self, SEL s, id cfg) { return self; }
static id img_with_handler(Class c, SEL s, NSSize size, BOOL flipped, BOOL (^handler)(NSRect)) {
    NSImage *img = [[[NSImage alloc] initWithSize: size] autorelease];
    if (handler && size.width > 0 && size.height > 0 && size.width <= 4096 && size.height <= 4096) {
        [img lockFocus];
        handler(NSMakeRect(0, 0, size.width, size.height));
        [img unlockFocus];
    }
    return img;
}

static IMP o_image_named;
static NSImage *asset_png(NSBundle *b, NSString *name) {
    if (b == nil || ![name isKindOfClass: [NSString class]] || [name length] == 0) return nil;
    NSString *res = [b resourcePath];
    if (res == nil) return nil;
    NSString *p = [[[res stringByAppendingPathComponent: @"Assets.images"] stringByAppendingPathComponent: name] stringByAppendingPathExtension: @"png"];
    if (![[NSFileManager defaultManager] fileExistsAtPath: p]) return nil;
    return [[[NSImage alloc] initWithContentsOfFile: p] autorelease];
}
static id image_named(Class c, SEL s, NSString *name) {
    id r = o_image_named ? ((id (*)(id, SEL, id)) o_image_named)(c, s, name) : nil;
    if (r == nil && (r = asset_png([NSBundle mainBundle], name)) != nil)
        [r setName: name];   /* wie bei Apple im Namensverzeichnis, das nächste imageNamed: findet es direkt */
    return r;
}

static char tab_vc_key;
static id tab_vc(id self, SEL s) { return objc_getAssociatedObject(self, &tab_vc_key); }
static void tab_set_vc(id self, SEL s, id vc) {
    objc_setAssociatedObject(self, &tab_vc_key, vc, OBJC_ASSOCIATION_RETAIN);
    if (vc) {
        ((void (*)(id, SEL, id)) objc_msgSend)(self, sel_registerName("setView:"), ((id (*)(id, SEL)) objc_msgSend)(vc, sel_registerName("view")));
        id title = ((id (*)(id, SEL)) objc_msgSend)(vc, sel_registerName("title"));
        ((void (*)(id, SEL, id)) objc_msgSend)(self, sel_registerName("setLabel:"), title ? title : @"");
    }
}
static id tab_with_vc(Class c, SEL s, id vc) {
    id item = ((id (*)(id, SEL, id)) objc_msgSend)([c alloc], sel_registerName("initWithIdentifier:"), vc);
    [item autorelease];
    tab_set_vc(item, s, vc);
    return item;
}

static NSArray *declared_parents(NSString *type) {
    NSDictionary *info = [[NSBundle mainBundle] infoDictionary];
    for (NSString *key in @[ @"UTExportedTypeDeclarations", @"UTImportedTypeDeclarations" ])
        for (NSDictionary *d in info[key])
            if ([d isKindOfClass: [NSDictionary class]] && [d[@"UTTypeIdentifier"] isEqual: type]) {
                id c = d[@"UTTypeConformsTo"];
                return [c isKindOfClass: [NSString class]] ? @[ c ] : ([c isKindOfClass: [NSArray class]] ? c : @[]);
            }
    return @[];
}
static BOOL ws_conforms(id self, SEL s, NSString *type, NSString *parent) {
    if (![type isKindOfClass: [NSString class]] || ![parent isKindOfClass: [NSString class]]) return NO;
    if ([type isEqualToString: parent]) return YES;
    if ([parent isEqualToString: @"public.item"] || [parent isEqualToString: @"public.data"] || [parent isEqualToString: @"public.content"]) return YES;
    NSMutableArray *todo = [NSMutableArray arrayWithObject: type];
    NSMutableSet *seen = [NSMutableSet set];
    while ([todo count]) {
        NSString *t = [todo lastObject];
        [todo removeLastObject];
        if ([seen containsObject: t]) continue;
        [seen addObject: t];
        for (NSString *p in declared_parents(t)) {
            if ([p isEqualToString: parent]) return YES;
            [todo addObject: p];
        }
    }
    return NO;
}
static BOOL ws_is_package(id self, SEL s, NSString *path) {
    BOOL dir = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath: path isDirectory: &dir] || !dir) return NO;
    return [[path pathExtension] length] > 0;
}

static void arr_setvalue(NSArray *self, SEL s, id value, NSString *key) {
    for (id obj in self)
        [obj setValue: value forKey: key];
}

__attribute__((constructor)) static void clk_modern_appkit(void) {
    class_addMethod([NSArray class], @selector(setValue:forKey:), (IMP)arr_setvalue, "v@:@@");
    Class im = object_getClass([NSImage class]);
    class_addMethod(im, sel_registerName("imageWithSystemSymbolName:accessibilityDescription:"), (IMP)img_symbol, "@@:@@");
    class_addMethod(im, sel_registerName("imageWithSymbolName:bundle:variableValue:"), (IMP)img_symbol3, "@@:@@d");
    class_addMethod(im, sel_registerName("imageWithSymbolName:variableValue:"), (IMP)img_symbol, "@@:@d");
    class_addMethod([NSNumberFormatter class], @selector(stringForObjectValue:), (IMP)fmt_number, "@@:@");
    class_addMethod([NSDateFormatter class], @selector(stringForObjectValue:), (IMP)fmt_date, "@@:@");
    Class fm = object_getClass([NSFont class]), cm = object_getClass([NSColor class]);
    class_addMethod(fm, sel_registerName("systemFontOfSize:weight:"), (IMP)f_sys_weight, "@@:dd");
    class_addMethod(fm, sel_registerName("monospacedDigitSystemFontOfSize:weight:"), (IMP)f_sys_weight, "@@:dd");
    class_addMethod(fm, sel_registerName("monospacedSystemFontOfSize:weight:"), (IMP)f_mono_weight, "@@:dd");
    class_addMethod(fm, sel_registerName("preferredFontForTextStyle:options:"), (IMP)f_text_style, "@@:@@");
    for (size_t i = 0; i < sizeof colors / sizeof *colors; i++)
        class_addMethod(cm, sel_registerName(colors[i].sel), (IMP)c_named, "@@:");
    class_addMethod(cm, sel_registerName("colorWithColorSpace:components:count:"), (IMP)c_space_components, "@@:@^dq");
    class_addMethod([NSColor class], sel_registerName("type"), (IMP)c_type, "q@:");
    class_addMethod(cm, sel_registerName("colorNamed:bundle:"), (IMP)c_named_bundle, "@@:@@");
    class_addMethod(cm, sel_registerName("colorNamed:"), (IMP)c_named_main, "@@:@");
    class_addMethod([NSFont class], sel_registerName("copyName"), (IMP)font_copyname, "@@:");
    class_addMethod([NSCell class], sel_registerName("_textAttributes"), (IMP)cell_textattrs, "@@:");
    class_addMethod(objc_getClass("NSGraphicsContext"), sel_registerName("CGContext"), (IMP)gc_cgcontext, "^v@:");
    class_addMethod(objc_getClass("NSWorkspace"), sel_registerName("type:conformsToType:"), (IMP)ws_conforms, "c@:@@");
    {   /* Cocotrons MacWorkspace meldet isFilePackageAtPath: als nicht umgesetzt: ersetzen */
        Class mw = objc_getClass("MacWorkspace");
        SEL sel = sel_registerName("isFilePackageAtPath:");
        if (mw && class_getInstanceMethod(mw, sel)) method_setImplementation(class_getInstanceMethod(mw, sel), (IMP)ws_is_package);
        class_addMethod(objc_getClass("NSWorkspace"), sel, (IMP)ws_is_package, "c@:@");
    }
    {
        Class tv = objc_getClass("NSTabViewItem");
        class_addMethod(object_getClass(tv), sel_registerName("tabViewItemWithViewController:"), (IMP)tab_with_vc, "@@:@");
        class_addMethod(tv, sel_registerName("viewController"), (IMP)tab_vc, "@@:");
        class_addMethod(tv, sel_registerName("setViewController:"), (IMP)tab_set_vc, "v@:@");
    }
    {
        /* -[NSBundle imageForResource:] sucht selbst in Assets.images (appkit/nsbundle_images.m im X11-Backend) */
        Method m = class_getClassMethod([NSImage class], @selector(imageNamed:));
        if (m) o_image_named = method_setImplementation(m, (IMP) image_named);
    }
    class_addMethod(objc_getClass("NSWindow"), sel_registerName("backingScaleFactor"), (IMP)one_scale, "d@:");
    class_addMethod(objc_getClass("NSScreen"), sel_registerName("backingScaleFactor"), (IMP)one_scale, "d@:");
    class_addMethod(objc_getClass("CALayer"), sel_registerName("contentsScale"), (IMP)one_scale, "d@:");
    class_addMethod(objc_getClass("CALayer"), sel_registerName("setContentsScale:"), (IMP)ignore_scale, "v@:d");
    class_addMethod(object_getClass([NSDate class]), sel_registerName("now"), (IMP)date_now, "@@:");
    class_addMethod([NSImage class], sel_registerName("imageWithSymbolConfiguration:"), (IMP)img_self, "@@:@");
    class_addMethod(object_getClass([NSImage class]), sel_registerName("imageWithSize:flipped:drawingHandler:"), (IMP)img_with_handler, "@@:{CGSize=dd}c@?");
    class_addMethod(objc_getClass("NSFontDescriptor"), sel_registerName("fontDescriptorWithDesign:"), (IMP)fd_design, "@@:@");
    class_addMethod([NSFont class], sel_registerName("boundingRect"), (IMP)font_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
}
