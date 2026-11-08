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
 *   NSCell: _contents (privat; Wert bzw. Knopftitel, bei Bildzellen nil), calcDrawInfo: (leer wie bei Apple)
 *   NSButtonCell: _imageRectWithRect: (privat; Bild mittig, notfalls verkleinert), _buttonType (privat; aus Hervorhebung/Zustand)
 *   NSImageRep: drawInRect:fromRect:operation:fraction:respectFlipped:hints: (über ein NSImage); NSView prepareContentInRect:,
 *            preparedContentRect (gemerkt, sonst sichtbarer Bereich); NSTextFieldCell _setVerticallyCentered:, _typesetterBehavior
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
 *            public.item/public.data/public.content für alles), isFilePackageAtPath: (Ordner mit Endung),
 *            typeOfFile:error: (Typbezeichner über NSURLTypeIdentifierKey); auch in Cocotrons MacWorkspace, wo sie Platzhalter waren
 *   CAShapeLayer: path, fillColor, strokeColor, fillRule, lineCap, lineJoin, lineDashPattern (Objekte) und lineWidth, miterLimit, strokeStart,
 *            strokeEnd, lineDashPhase (CGFloat) als gespeicherte Werte mit KVC-tauglichen Zugriffsmethoden (Cocotrons Klasse ist leer; Logic Pro
 *            setzt sie per setValue:forKey:, die Ausnahme brach das Öffnen eines Projekts ab: "error 256"). Gezeichnet wird in softlayers.m.
 *   NSImage: CGImageForProposedRect:context:hints: (zeichnet in ein Bitmap), bestRepresentationForRect:context:hints: (eigene Darstellung,
 *            kein Zwischenspeicher), drawInRect:;
 *            _clkTintedWithColor: (eingefärbte Kopie für Vorlagenbilder in Zellen);
 *            NSBitmapImageRep colorSpace (Gerät-RGB)
 *   NSColor: colorWithCGColor:, colorWithHue:saturation:brightness:alpha:, colorWithDisplayP3Red:green:blue:alpha: (fehlten: nil, daraus
 *            machte Logic Pro Farben mit Anteilen 0, also durchsichtige Schrift im Lineal und in den Spurköpfen)
 *   UTType: +_typeOfCurrentDevice (private; Logic Pro setzt dessen localizedDescription in Texte ein) liefert ein Objekt mit "Mac"/com.apple.mac
 *   NSArray (Darling): setValue:forKey: setzt den Wert wie bei Apple bei jedem Element (Cocotrons NSObjectController-Auswahl schreibt
 *            so in die gewählten Objekte; Logic Pro: Bindung selection.keySignature)
 *   NSImage: imageWithSystemSymbolName:accessibilityDescription:, imageWithSymbolName:…: Vorlagenbilder aus /Library/ClokeOS/Symbols
 *            (auf dem Mac mit macapps/sfsymbols.m gerendert) bzw. aus dem Bundle; sonst ein leeres Bild 16 × 16 statt nil */
#import <AppKit/NSImage.h>
#import <AppKit/NSAttributedString.h>
#import <AppKit/NSParagraphStyle.h>
#import <AppKit/NSCell.h>
#import <AppKit/NSButtonCell.h>
#import <Foundation/NSNumberFormatter.h>
#import <Foundation/NSDateFormatter.h>
#import <AppKit/NSFont.h>
#import <AppKit/NSColor.h>
#import <AppKit/NSColorSpace.h>
#import <AppKit/NSGraphicsContext.h>
#import <AppKit/NSImageRep.h>
#import <objc/runtime.h>
#define CLK_VISIBLE __attribute__((visibility("default")))
#import <objc/message.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

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

/* SF Symbols: Apples Symbole liegen (auf dem Mac gerendert, macapps/sfsymbols.m; nicht im Repository) als Vorlagenbilder in
 * /Library/ClokeOS/Symbols/<Name>.png; eigene Symbole eines Bundles (imageWithSymbolName:bundle:…) über dessen Bilder. Fehlt eines:
 * leeres Bild 16 × 16 (nie nil; Logic legt das Ergebnis ungeprüft in ein Wörterbuch), CLOKE_TRACE meldet den Namen je einmal. */
static id symbol_file(NSString *name) {
    if (![name isKindOfClass: [NSString class]] || [name length] == 0 || [name rangeOfString: @"/"].location != NSNotFound) return nil;
    static NSMutableDictionary *cache;
    if (!cache) cache = [NSMutableDictionary new];
    id hit = cache[name];
    if (hit) return hit == [NSNull null] ? nil : hit;
    NSImage *img = [[[NSImage alloc] initWithContentsOfFile: [NSString stringWithFormat: @"/Library/ClokeOS/Symbols/%@.png", name]] autorelease];
    if (img) [img setTemplate: YES];
    cache[name] = img ?: (id)[NSNull null];
    return img;
}
static id symbol_missing(NSString *name) {
    if (getenv("CLOKE_TRACE")) {
        static NSMutableSet *seen;
        if (!seen) seen = [NSMutableSet new];
        if (name && ![seen containsObject: name]) { [seen addObject: name]; fprintf(stderr, "[clokeos-trace] Symbol fehlt: %s\n", [[name description] UTF8String]); }
    }
    return [[[NSImage alloc] initWithSize: NSMakeSize(16, 16)] autorelease];
}
static id img_symbol(Class c, SEL s, id name, id other) {
    id img = symbol_file(name);
    return img ? [[img copy] autorelease] : symbol_missing(name);
}
static id img_symbol3(Class c, SEL s, id name, id bundle, double value) {
    NSImage *img = [bundle isKindOfClass: [NSBundle class]] ? [bundle imageForResource: name] : nil;
    if (img) { [img setTemplate: YES]; return img; }
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
    if (!c) {
        if (getenv("CLOKE_TRACE")) fprintf(stderr, "[clokeos-trace] colorNamed: %s bundle %s -> nil\n", [name UTF8String],
                                           [[bundle bundleIdentifier] UTF8String] ?: "main");
        return nil;
    }
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

/* -[NSImage drawInRect:] (macOS 10.9) fehlte; wie bei Apple Quelle über, ganzes Bild, Spiegelung beachtet. Logic Pro zeichnet so seine
 * SVG-Instrumentensymbole (im Nachsichtmodus blieb der Aufruf folgenlos, die Symbole leer). */
static void img_draw_in_rect(NSImage *self, SEL s, NSRect r) {
    [self drawInRect: r fromRect: NSZeroRect operation: NSCompositeSourceOver fraction: 1.0 respectFlipped: YES hints: nil];
}

/* -[NSPopUpButton altersStateOfSelectedItem]/setAltersStateOfSelectedItem: reichen wie bei Apple an die Zelle weiter */
static BOOL pub_alters(id self, SEL s) { return ((BOOL (*)(id, SEL)) objc_msgSend)([self cell], s); }
static void pub_set_alters(id self, SEL s, BOOL v) { ((void (*)(id, SEL, BOOL)) objc_msgSend)([self cell], s, v); }

/* -[NSImage lockFocusFlipped:] (macOS 10.6) fehlte: wie lockFocus, mit YES zusätzlich gespiegelt (Ursprung oben links, isFlipped YES) */
static void img_lock_flipped(NSImage *self, SEL s, BOOL flipped) {
    [self lockFocus];
    if (!flipped) return;
    CGContextRef c = (CGContextRef) [[NSGraphicsContext currentContext] graphicsPort];
    CGContextTranslateCTM(c, 0, [self size].height);
    CGContextScaleCTM(c, 1, -1);
    [NSGraphicsContext setCurrentContext: [NSGraphicsContext graphicsContextWithGraphicsPort: c flipped: YES]];
}

/* _NSTextCellSize (private AppKit-Funktion, Logic Pros MAKitSegmentedScrubberCell ruft sie in textRectForBounds: auf): Größe des Texts
 * einer Zelle im Rechteck (Logic gibt 32000 als Breite = unbegrenzt), mindestens eine Zeile ihrer Schrift. Bisher ein erzeugter Platzhalter,
 * der nur x0 setzte: der Aufrufer las die Höhe aus d1 (einem alten Wert, z. B. 0) und klippte die Ziffern der Transportanzeige und der
 * Inspektorwerte auf 0–2 Punkte. */
CLK_VISIBLE NSSize _NSTextCellSize(NSCell *cell, NSRect rect) {
    NSAttributedString *a = [cell respondsToSelector: @selector(attributedStringValue)] ? [cell attributedStringValue] : nil;
    NSSize sz = [a length] ? [a size] : NSZeroSize;
    NSFont *f = [cell respondsToSelector: @selector(font)] ? [cell font] : nil;
    CGFloat line = f ? ceil([f ascender] - [f descender] + [f leading]) : 0;
    if (sz.height < line) sz.height = line;
    if (rect.size.width > 0 && sz.width > rect.size.width) sz.width = rect.size.width;
    return sz;
}

static IMP o_image_named;
static NSImage *asset_png(NSBundle *b, NSString *name) {
    if (b == nil || ![name isKindOfClass: [NSString class]] || [name length] == 0) return nil;
    NSString *res = [b resourcePath];
    if (res == nil) return nil;
    NSString *p = [[[res stringByAppendingPathComponent: @"Assets.images"] stringByAppendingPathComponent: name] stringByAppendingPathExtension: @"png"];
    if (![[NSFileManager defaultManager] fileExistsAtPath: p]) return nil;
    NSImage *img = [[[NSImage alloc] initWithContentsOfFile: p] autorelease];
    if ([[NSFileManager defaultManager] fileExistsAtPath: [[p stringByDeletingPathExtension] stringByAppendingPathExtension: @"template"]])
        [img setTemplate: YES];   /* Vorlagenbild (carimages.m) */
    return img;
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

static id ws_type_of_file(id self, SEL s, NSString *path, NSError **error) {
    id uti = nil;
    [[NSURL fileURLWithPath: path] getResourceValue: &uti forKey: NSURLTypeIdentifierKey error: error];
    return uti;
}

/* gespeicherte Eigenschaften je Selektor: Schlüssel ist die Adresse des (internierten) Getter-Selektors */
static id shape_get_obj(id self, SEL s) { return objc_getAssociatedObject(self, (const void *) s); }
static CGFloat shape_get_float(id self, SEL s) {
    NSNumber *n = objc_getAssociatedObject(self, (const void *) s);
    if (n) return [n doubleValue];
    return (s == sel_registerName("lineWidth") || s == sel_registerName("strokeEnd")) ? 1.0 : (s == sel_registerName("miterLimit") ? 10.0 : 0.0);
}
static SEL getter_for_setter(SEL s) {
    const char *n = sel_getName(s);   /* setXyz: -> xyz */
    char buf[128];
    size_t len = strlen(n);
    if (len < 5 || len - 4 >= sizeof buf) return s;
    memcpy(buf, n + 3, len - 4);
    buf[len - 4] = 0;
    buf[0] = (char) tolower((unsigned char) buf[0]);
    return sel_registerName(buf);
}
static void shape_set_obj(id self, SEL s, id v) {
    objc_setAssociatedObject(self, (const void *) getter_for_setter(s), v, OBJC_ASSOCIATION_RETAIN);
}
static void shape_set_float(id self, SEL s, CGFloat v) {
    objc_setAssociatedObject(self, (const void *) getter_for_setter(s), [NSNumber numberWithDouble: v], OBJC_ASSOCIATION_RETAIN);
}

@interface ClkDeviceType : NSObject
@end
@implementation ClkDeviceType
- (NSString *) localizedDescription { return @"Mac"; }
- (NSString *) identifier { return @"com.apple.mac"; }
- (NSString *) preferredFilenameExtension { return nil; }
- (BOOL) conformsToType: (id) other { return NO; }
@end
static id device_type(Class c, SEL s) {
    static id t;
    if (!t) t = [ClkDeviceType new];
    return t;
}

static void arr_setvalue(NSArray *self, SEL s, id value, NSString *key) {
    for (id obj in self)
        [obj setValue: value forKey: key];
}

/* +[NSColor colorWithCGColor:] und colorWithHue:saturation:brightness:alpha: fehlten in Cocotron (nil; Logic Pro baute daraus Farben mit
 * Anteilen 0, durchsichtige Schrift). Nach Zahl der Anteile: Grau+Alpha, RGB+Alpha, CMYK+Alpha. */
static id c_with_cgcolor(Class c, SEL s, CGColorRef cg) {
    if (cg == NULL) return nil;
    size_t n = CGColorGetNumberOfComponents(cg);
    const CGFloat *k = CGColorGetComponents(cg);
    if (n == 2) return [NSColor colorWithCalibratedWhite: k[0] alpha: k[1]];
    if (n == 4) return [NSColor colorWithCalibratedRed: k[0] green: k[1] blue: k[2] alpha: k[3]];
    if (n == 5) return [NSColor colorWithDeviceCyan: k[0] magenta: k[1] yellow: k[2] black: k[3] alpha: k[4]];
    if (n == 1) return [NSColor colorWithCalibratedWhite: k[0] alpha: 1];
    return nil;
}
static id c_with_hsb(Class c, SEL s, CGFloat h, CGFloat sat, CGFloat b, CGFloat a) {
    return [NSColor colorWithCalibratedHue: h saturation: sat brightness: b alpha: a];
}
static id c_with_p3(Class c, SEL s, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    return [NSColor colorWithCalibratedRed: r green: g blue: b alpha: a];
}

/* -[NSImage CGImageForProposedRect:context:hints:] fehlte (nil; Logic Pro zeichnet so seine Spursymbole): das Bild wird in ein Bitmap der
 * Größe des Rechtecks (ohne Angabe: Bildgröße) gezeichnet; proposedRect bekommt wie bei Apple das verwendete Rechteck. */
static CGImageRef img_cgimage(NSImage *self, SEL s, NSRect *proposed, id ctx, NSDictionary *hints) {
    NSSize sz = [self size];
    NSRect r = (proposed && proposed->size.width > 0 && proposed->size.height > 0) ? *proposed : NSMakeRect(0, 0, sz.width, sz.height);
    size_t w = (size_t) ceil(r.size.width), h = (size_t) ceil(r.size.height);
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return NULL;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (c == NULL) return NULL;
    NSGraphicsContext *g = [NSGraphicsContext graphicsContextWithGraphicsPort: c flipped: NO];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext: g];
    [self drawInRect: NSMakeRect(0, 0, w, h) fromRect: NSZeroRect operation: NSCompositeSourceOver fraction: 1.0];
    [NSGraphicsContext restoreGraphicsState];
    CGImageRef img = CGBitmapContextCreateImage(c);
    CGContextRelease(c);
    if (proposed) *proposed = r;
    return img ? (CGImageRef) [(id) img autorelease] : NULL;
}
/* -[NSImage bestRepresentationForRect:context:hints:] wählt wie bei Apple unter den eigenen Darstellungen (nie Cocotrons Zwischenspeicher
 * NSCachedImageRep, den bestRepresentationForDevice: liefern kann): die kleinste, deren Pixel das Rechteck abdecken, sonst die größte.
 * Logic Pros Symbolfärbung (-[NSImage ma_imageRep:tintedWithColor:]) nimmt nur NSBitmapImageRep an; mit dem Zwischenspeicher blieben
 * die Instrumentensymbole leer. */
static id img_best_for_rect(NSImage *self, SEL s, NSRect r, id ctx, NSDictionary *hints) {
    NSArray *reps = [self representations];
    NSImageRep *best = nil, *largest = nil;
    double need = r.size.width * r.size.height;
    for (NSImageRep *rep in reps) {
        if ([rep isKindOfClass: objc_getClass("NSCachedImageRep")] && [reps count] > 1) continue;
        double px = (double) [rep pixelsWide] * [rep pixelsHigh];
        if (px <= 0) px = [rep size].width * [rep size].height;
        if (largest == nil || px > (double) [largest pixelsWide] * [largest pixelsHigh]) largest = rep;
        if (px >= need && (best == nil || px < (double) [best pixelsWide] * [best pixelsHigh])) best = rep;
    }
    return best ?: largest ?: [self bestRepresentationForDevice: nil];
}
static id rep_colorspace(id self, SEL s) { return [NSColorSpace deviceRGBColorSpace]; }

/* -[NSCell _contents] (privat bei Apple, Logic Pros MAButtonCell & Co. rufen es auf): der rohe Inhalt der Zelle, wie auf dem Mac
 * nachgeprüft: bei Textzellen der Wert (Knöpfe: der Titel), bei Bildzellen nil */
static id cell_contents(NSCell *self, SEL s) {
    if ([self type] == NSImageCellType) return nil;
    if ([self isKindOfClass: [NSButtonCell class]]) return [(NSButtonCell *) self title];
    return [self objectValue];
}

/* -[NSCell calcDrawInfo:] (dokumentiert, tut bei Apple in NSCell nichts; Logic Pros MAMixerPannerCell ruft super) */
static void cell_calc_draw_info(id self, SEL s, NSRect r) {}

/* -[NSButtonCell _imageRectWithRect:] (privat; Logic Pros MAButtonCell legt damit das Bildrechteck fest, ohne sie war es leer): das Bild in
 * seiner Größe, bei Bedarf verhältnisgleich verkleinert, mittig im Rechteck */
static NSRect bc_image_rect(NSButtonCell *self, SEL s, NSRect r) {
    NSImage *img = [self image];
    if (img == nil) return NSZeroRect;
    NSSize sz = [img size];
    if (sz.width <= 0 || sz.height <= 0) return NSZeroRect;
    CGFloat f = MIN(1.0, MIN(r.size.width / sz.width, r.size.height / sz.height));
    if (f < 0) f = 0;
    sz.width *= f; sz.height *= f;
    return NSMakeRect(floor(NSMidX(r) - sz.width / 2), floor(NSMidY(r) - sz.height / 2), sz.width, sz.height);
}
/* -[NSButtonCell _buttonType] (privat): Knopfart aus Hervorhebung und Zustandsanzeige, wie setButtonType: sie setzt */
static NSInteger bc_button_type(NSButtonCell *self, SEL s) {
    NSInteger shows = [self showsStateBy], hl = [self highlightsBy];
    if (shows & NSContentsCellMask) return (hl & NSContentsCellMask) ? NSToggleButton : NSOnOffButton;
    if (shows & (NSChangeBackgroundCellMask | NSChangeGrayCellMask)) return NSPushOnPushOffButton;
    if (hl & NSPushInCellMask) return NSMomentaryPushInButton;
    if (hl & NSContentsCellMask) return NSMomentaryChangeButton;
    return NSMomentaryLightButton;
}

/* -[NSImageRep drawInRect:fromRect:operation:fraction:respectFlipped:hints:] (10.6; fehlte, Logic Pro zeichnet so Darstellungen direkt):
 * über ein NSImage mit dieser Darstellung, das dieselbe Methode hat (Ausschnitt, Operation, Deckkraft, Kippen wie dort) */
static BOOL rep_draw_full(NSImageRep *self, SEL s, NSRect dst, NSRect src, NSCompositingOperation op, CGFloat fraction, BOOL respectFlipped,
                          NSDictionary *hints) {
    NSSize sz = [self size];
    if (getenv("CLOKE_BUTTON_TRACE")) {
        static int n;
        if (n++ < 200)
            NSLog(@"[clokeos-rep] %@ size=%@ pixels=%ldx%ld from=%@ in=%@ op=%ld flip=%d", [self class], NSStringFromSize(sz), (long)[self pixelsWide],
                  (long)[self pixelsHigh], NSStringFromRect(src), NSStringFromRect(dst), (long)op, (int)respectFlipped);
    }
    if (sz.width <= 0 || sz.height <= 0) return NO;
    {   /* CLOKE_DUMP_REP=<B>x<H>: die erste so große Darstellung als PNG (hex) in die Ausgabe (Fehlersuche) */
        static int dumped;
        const char *want = getenv("CLOKE_DUMP_REP");
        if (want && !dumped && [[NSString stringWithFormat: @"%.0fx%.0f", sz.width, sz.height] isEqualToString: @(want)]) {
            dumped = 1;
            NSImage *tmp = [[[NSImage alloc] initWithSize: sz] autorelease];
            [tmp addRepresentation: self];
            NSBitmapImageRep *bm = [[[NSBitmapImageRep alloc] initWithCGImage: [tmp CGImageForProposedRect: NULL context: nil hints: nil]] autorelease];
            NSData *d = [bm representationUsingType: NSPNGFileType properties: @{}];
            const unsigned char *b = [d bytes];
            for (NSUInteger i = 0; i < [d length]; i += 48) {
                fprintf(stderr, "[rephex] ");
                for (NSUInteger j = i; j < i + 48 && j < [d length]; j++) fprintf(stderr, "%02x", b[j]);
                fprintf(stderr, "\n");
            }
        }
    }
    NSImage *img = [[NSImage alloc] initWithSize: sz];
    [img addRepresentation: self];
    [img drawInRect: dst fromRect: src operation: op fraction: fraction respectFlipped: respectFlipped hints: hints];
    [img release];
    return YES;
}
/* NSView prepareContentInRect:/preparedContentRect (10.9, reaktives Scrollen): ohne Vorausberechnung ist der vorbereitete Bereich, was
 * die Ansicht setzt, sonst der sichtbare */
static char k_prepared;
static void view_prepare(NSView *self, SEL s, NSRect r) {
    objc_setAssociatedObject(self, &k_prepared, [NSValue valueWithRect: r], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static NSRect view_prepared(NSView *self, SEL s) {
    NSValue *v = objc_getAssociatedObject(self, &k_prepared);
    return v ? [v rectValue] : [self visibleRect];
}
/* NSTextFieldCell _setVerticallyCentered: / _typesetterBehavior (privat): gemerkt bzw. die aktuelle Typesetter-Fassung */
static char k_vcenter;
static void tfc_set_vcenter(id self, SEL s, BOOL v) { objc_setAssociatedObject(self, &k_vcenter, @(v), OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
static BOOL tfc_vcenter(id self, SEL s) { return [objc_getAssociatedObject(self, &k_vcenter) boolValue]; }
static NSInteger tfc_typesetter(id self, SEL s) { return 2; /* NSTypesetterBehavior_10_4 */ }

/* Vorlagenbilder (isTemplate) zeichnen Bedienelemente bei Apple in der Schriftfarbe des Elements (hell in der dunklen Darstellung). Hilfe für
 * die Zellen (NSGraphicsStyle-, NSImageCell-Patches): Kopie des Bildes, mit der Farbe über seinen Deckungsbereich gefüllt (source-atop) */
static id img_tinted(NSImage *self, SEL s, NSColor *color) {
    NSSize sz = [self size];
    if (color == nil || sz.width <= 0 || sz.height <= 0 || sz.width > 2048 || sz.height > 2048) return self;
    NSImage *t = [[[NSImage alloc] initWithSize: sz] autorelease];
    NSRect r = NSMakeRect(0, 0, sz.width, sz.height);
    [t lockFocus];
    [self drawInRect: r fromRect: NSZeroRect operation: NSCompositeSourceOver fraction: 1.0];
    [color set];
    NSRectFillUsingOperation(r, NSCompositeSourceAtop);
    [t unlockFocus];
    return t;
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
    class_addMethod(cm, sel_registerName("colorWithCGColor:"), (IMP)c_with_cgcolor, "@@:^v");
    class_addMethod([NSImage class], sel_registerName("_clkTintedWithColor:"), (IMP)img_tinted, "@@:@");
    class_addMethod([NSImage class], sel_registerName("CGImageForProposedRect:context:hints:"), (IMP)img_cgimage, "^v@:^{CGRect={CGPoint=dd}{CGSize=dd}}@@");
    class_addMethod([NSImage class], sel_registerName("bestRepresentationForRect:context:hints:"), (IMP)img_best_for_rect, "@@:{CGRect={CGPoint=dd}{CGSize=dd}}@@");
    class_addMethod(objc_getClass("NSBitmapImageRep"), sel_registerName("colorSpace"), (IMP)rep_colorspace, "@@:");
    class_addMethod(cm, sel_registerName("colorWithHue:saturation:brightness:alpha:"), (IMP)c_with_hsb, "@@:dddd");
    class_addMethod(cm, sel_registerName("colorWithDisplayP3Red:green:blue:alpha:"), (IMP)c_with_p3, "@@:dddd");
    class_addMethod([NSFont class], sel_registerName("copyName"), (IMP)font_copyname, "@@:");
    class_addMethod([NSCell class], sel_registerName("_textAttributes"), (IMP)cell_textattrs, "@@:");
    class_addMethod([NSCell class], sel_registerName("_contents"), (IMP)cell_contents, "@@:");
    class_addMethod([NSButtonCell class], sel_registerName("_imageRectWithRect:"), (IMP)bc_image_rect, "{CGRect={CGPoint=dd}{CGSize=dd}}@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_addMethod([NSButtonCell class], sel_registerName("_buttonType"), (IMP)bc_button_type, "q@:");
    class_addMethod([NSImageRep class], sel_registerName("drawInRect:fromRect:operation:fraction:respectFlipped:hints:"), (IMP)rep_draw_full,
                    "c@:{CGRect={CGPoint=dd}{CGSize=dd}}{CGRect={CGPoint=dd}{CGSize=dd}}Qdc@");
    class_addMethod([NSView class], sel_registerName("prepareContentInRect:"), (IMP)view_prepare, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_addMethod([NSView class], sel_registerName("setPreparedContentRect:"), (IMP)view_prepare, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_addMethod([NSView class], sel_registerName("preparedContentRect"), (IMP)view_prepared, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
    class_addMethod(objc_getClass("NSTextFieldCell"), sel_registerName("_setVerticallyCentered:"), (IMP)tfc_set_vcenter, "v@:c");
    class_addMethod(objc_getClass("NSTextFieldCell"), sel_registerName("_isVerticallyCentered"), (IMP)tfc_vcenter, "c@:");
    class_addMethod(objc_getClass("NSTextFieldCell"), sel_registerName("_typesetterBehavior"), (IMP)tfc_typesetter, "q@:");
    class_addMethod([NSCell class], sel_registerName("calcDrawInfo:"), (IMP)cell_calc_draw_info, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_addMethod(objc_getClass("NSGraphicsContext"), sel_registerName("CGContext"), (IMP)gc_cgcontext, "^v@:");
    if (objc_getClass("UTType"))
        class_addMethod(object_getClass(objc_getClass("UTType")), sel_registerName("_typeOfCurrentDevice"), (IMP)device_type, "@@:");
    {   /* Cocotrons MacWorkspace (die einzige NSWorkspace-Unterklasse) hat diese als "nicht umgesetzt": dort ersetzen */
        Class mw = objc_getClass("MacWorkspace"), nw = objc_getClass("NSWorkspace");
        struct { const char *sel; IMP imp; const char *types; } ws[] = {
            { "isFilePackageAtPath:", (IMP)ws_is_package, "c@:@" },
            { "type:conformsToType:", (IMP)ws_conforms, "c@:@@" },
            { "typeOfFile:error:", (IMP)ws_type_of_file, "@@:@^@" },
        };
        for (size_t i = 0; i < sizeof ws / sizeof *ws; i++) {
            SEL sel = sel_registerName(ws[i].sel);
            Method m = mw ? class_getInstanceMethod(mw, sel) : NULL;
            if (m) method_setImplementation(m, ws[i].imp);
            class_addMethod(nw, sel, ws[i].imp, ws[i].types);
        }
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
    {
        Class shape = objc_getClass("CAShapeLayer");
        const char *objs[] = { "path", "fillColor", "strokeColor", "fillRule", "lineCap", "lineJoin", "lineDashPattern" };
        const char *floats[] = { "lineWidth", "miterLimit", "strokeStart", "strokeEnd", "lineDashPhase" };
        char setter[64];
        for (size_t i = 0; shape && i < sizeof objs / sizeof *objs; i++) {
            class_addMethod(shape, sel_registerName(objs[i]), (IMP)shape_get_obj, "@@:");
            snprintf(setter, sizeof setter, "set%c%s:", toupper((unsigned char) objs[i][0]), objs[i] + 1);
            class_addMethod(shape, sel_registerName(setter), (IMP)shape_set_obj, "v@:@");
        }
        for (size_t i = 0; shape && i < sizeof floats / sizeof *floats; i++) {
            class_addMethod(shape, sel_registerName(floats[i]), (IMP)shape_get_float, "d@:");
            snprintf(setter, sizeof setter, "set%c%s:", toupper((unsigned char) floats[i][0]), floats[i] + 1);
            class_addMethod(shape, sel_registerName(setter), (IMP)shape_set_float, "v@:d");
        }
    }
    class_addMethod(objc_getClass("NSScreen"), sel_registerName("backingScaleFactor"), (IMP)one_scale, "d@:");
    class_addMethod(objc_getClass("CALayer"), sel_registerName("contentsScale"), (IMP)one_scale, "d@:");
    class_addMethod(objc_getClass("CALayer"), sel_registerName("setContentsScale:"), (IMP)ignore_scale, "v@:d");
    class_addMethod(object_getClass([NSDate class]), sel_registerName("now"), (IMP)date_now, "@@:");
    class_addMethod([NSImage class], sel_registerName("imageWithSymbolConfiguration:"), (IMP)img_self, "@@:@");
    class_addMethod(objc_getClass("NSPopUpButton"), sel_registerName("altersStateOfSelectedItem"), (IMP)pub_alters, "c@:");
    class_addMethod(objc_getClass("NSPopUpButton"), sel_registerName("setAltersStateOfSelectedItem:"), (IMP)pub_set_alters, "v@:c");
    class_addMethod([NSImage class], sel_registerName("lockFocusFlipped:"), (IMP)img_lock_flipped, "v@:c");
    class_addMethod([NSImage class], sel_registerName("drawInRect:"), (IMP)img_draw_in_rect, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_addMethod(object_getClass([NSImage class]), sel_registerName("imageWithSize:flipped:drawingHandler:"), (IMP)img_with_handler, "@@:{CGSize=dd}c@?");
    class_addMethod(objc_getClass("NSFontDescriptor"), sel_registerName("fontDescriptorWithDesign:"), (IMP)fd_design, "@@:@");
    class_addMethod([NSFont class], sel_registerName("boundingRect"), (IMP)font_bounds, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
}
