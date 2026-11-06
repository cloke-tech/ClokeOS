/* Neuere AppKit-Klassenmethoden, die Cocotron fehlt und die Mac-Programme beim Start brauchen (Logic Pro: Schriften und Farben für
 * Textattribute; ein nil an dieser Stelle wirft beim Anlegen des Attribut-Wörterbuchs eine Ausnahme mitten im Programmstart).
 * Eingetragen mit class_addMethod: was AppKit schon hat, bleibt unverändert.
 *   NSFont: systemFontOfSize:weight:, monospacedDigitSystemFontOfSize:weight:, monospacedSystemFontOfSize:weight:,
 *           preferredFontForTextStyle:options:
 *   NSColor: systemRed/Green/Blue/…/GrayColor, label-, secondaryLabel-, tertiaryLabel-, quaternaryLabelColor, separatorColor, linkColor,
 *            placeholderTextColor, controlAccentColor, … (Werte der hellen Darstellung von macOS)
 *   Foundation (Darling): -[NSNumberFormatter/NSDateFormatter stringForObjectValue:] über stringFromNumber:/stringFromDate:
 *            (sonst wirft NSFormatter "requires a subclass implementation", Logic Pro beim Laden einer Fenster-Nib)
 *   NSArray (Darling): setValue:forKey: setzt den Wert wie bei Apple bei jedem Element (Cocotrons NSObjectController-Auswahl schreibt
 *            so in die gewählten Objekte; Logic Pro: Bindung selection.keySignature)
 *   NSImage: imageWithSystemSymbolName:accessibilityDescription:, imageWithSymbolName:…: SF Symbols gibt es hier nicht; statt nil (Logic
 *            legt das Ergebnis ungeprüft in ein Wörterbuch) ein leeres Bild 16 × 16 */
#import <AppKit/NSImage.h>
#import <Foundation/NSNumberFormatter.h>
#import <Foundation/NSDateFormatter.h>
#import <AppKit/NSFont.h>
#import <AppKit/NSColor.h>
#import <objc/runtime.h>
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
}
