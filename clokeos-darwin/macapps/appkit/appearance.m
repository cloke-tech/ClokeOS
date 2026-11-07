/* Erscheinungsbild (NSAppearance) für Cocotrons AppKit: hell oder dunkel.
 *
 * Cocotrons +[NSAppearance appearanceNamed:] war ein STUB (ein nicht initialisiertes Objekt ohne Namen), NSApplication/NSWindow kannten
 * appearance/effectiveAppearance nicht, und alle Systemfarben waren die der hellen Darstellung. Ein Programm, das sich dunkel stellt
 * ([NSApp setAppearance: [NSAppearance appearanceNamed: NSAppearanceNameDarkAqua]], Logic Pro), bekam so weiße Tabellenzeilen und
 * schwarze Standardschrift unter seine eigene helle Schrift.
 *
 *   NSAppearance: appearanceNamed: (je Name ein Objekt), name, currentAppearance/setCurrentAppearance:, currentDrawingAppearance,
 *                 bestMatchFromAppearancesWithNames:, performAsCurrentDrawingAppearance:, allowsVibrancy (NO)
 *   NSApplication: appearance, setAppearance:, effectiveAppearance (ohne Angabe Aqua)
 *   NSWindow: appearance, setAppearance:, effectiveAppearance (eigene, sonst die des Programms)
 *   NSView: effectiveAppearance (eigene, sonst Oberansicht, Fenster, Programm)
 *   System: hell/dunkel wie bei macOS aus CLOKEOS_DARK (von CloakServer) bzw. AppleInterfaceStyle; Aqua/DarkAqua entscheiden selbst,
 *           eigene Erscheinungsbilder (initWithAppearanceNamed:bundle:, Logic Pros MALogicAppearance) folgen dem System
 *   NSColor: ist das Programm dunkel, liefern die Systemfarben die Werte der dunklen Darstellung von macOS
 *            (Schrift, Hintergründe, Tabellen-Wechselfarben, Gitter, Auswahl, Beschriftungsfarben); sonst unverändert.
 * Eingetragen mit class_addMethod/Methodentausch beim Laden von AppKit. Nicht: Fenster-/Ansichts-genaue Farben (es zählt das Programm),
 * Vibrancy, Kontrast-Varianten. */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

static char k_name, k_app_appearance, k_win_appearance;
static NSMutableDictionary *g_named;
static id g_current;

static id ap_named(Class c, SEL s, NSString *name) {
    if (![name isKindOfClass: [NSString class]]) return nil;
    if (!g_named) g_named = [NSMutableDictionary new];
    id a = g_named[name];
    if (a == nil) {
        a = [[[NSAppearance alloc] init] autorelease];
        objc_setAssociatedObject(a, &k_name, name, OBJC_ASSOCIATION_COPY);
        g_named[name] = a;
    }
    return a;
}
static id ap_name(id self, SEL s) { return objc_getAssociatedObject(self, &k_name) ?: NSAppearanceNameAqua; }
/* System hell/dunkel wie bei macOS: CLOKEOS_DARK (setzt CloakServer für Programme, die es startet, auch im Automatik-Modus), sonst
 * AppleInterfaceStyle = Dark in den globalen Einstellungen */
static BOOL system_dark(void) {
    const char *e = getenv("CLOKEOS_DARK");
    if (e && *e) return e[0] == '1';
    return [[[NSUserDefaults standardUserDefaults] stringForKey: @"AppleInterfaceStyle"] isEqualToString: @"Dark"];
}
/* Aqua-/DarkAqua-Namen entscheiden selbst; eigene Erscheinungsbilder eines Programms (z. B. aus einer .car-Datei) folgen dem System */
static BOOL ap_is_dark(id a) {
    NSString *n = a ? ap_name(a, NULL) : nil;
    if ([n rangeOfString: @"Dark"].location != NSNotFound) return YES;
    if ([n rangeOfString: @"Aqua"].location != NSNotFound || [n rangeOfString: @"Light"].location != NSNotFound) return NO;
    return system_dark();
}
static id ap_init_named(id self, SEL s, NSString *name, NSBundle *bundle) {
    objc_setAssociatedObject(self, &k_name, name, OBJC_ASSOCIATION_COPY);
    return self;
}
static id ap_app_effective(void);
static id ap_current(Class c, SEL s) { return g_current ?: ap_app_effective(); }
static void ap_set_current(Class c, SEL s, id a) { [g_current release]; g_current = [a retain]; }
static id ap_best(id self, SEL s, NSArray *names) {
    NSString *mine = ap_name(self, NULL);
    if ([names containsObject: mine]) return mine;
    if (ap_is_dark(self) && [names containsObject: NSAppearanceNameDarkAqua]) return NSAppearanceNameDarkAqua;
    if (!ap_is_dark(self) && [names containsObject: NSAppearanceNameAqua]) return NSAppearanceNameAqua;
    BOOL dark = ap_is_dark(self);
    for (NSString *n in names)
        if (([n rangeOfString: @"Dark"].location != NSNotFound) == dark) return n;
    return nil;
}
static void ap_perform(id self, SEL s, void (^block)(void)) {
    id old = [g_current retain];
    ap_set_current(Nil, NULL, self);
    if (block) block();
    ap_set_current(Nil, NULL, old);
    [old release];
}
static BOOL ap_no(id self, SEL s) { return NO; }

static id app_appearance(id self, SEL s) { return objc_getAssociatedObject(self, &k_app_appearance); }
static void app_set_appearance(id self, SEL s, id a) { objc_setAssociatedObject(self, &k_app_appearance, a, OBJC_ASSOCIATION_RETAIN); }
static id ap_app_effective(void) {
    id a = NSApp ? objc_getAssociatedObject(NSApp, &k_app_appearance) : nil;
    return a ?: ap_named(Nil, NULL, system_dark() ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua);
}
static id app_effective(id self, SEL s) { return ap_app_effective(); }
static id win_appearance(id self, SEL s) { return objc_getAssociatedObject(self, &k_win_appearance); }
static void win_set_appearance(id self, SEL s, id a) {
    objc_setAssociatedObject(self, &k_win_appearance, a, OBJC_ASSOCIATION_RETAIN);
    [[self contentView] setNeedsDisplay: YES];
}
static id win_effective(id self, SEL s) { return win_appearance(self, s) ?: ap_app_effective(); }
static id view_effective(NSView *self, SEL s) {
    for (NSView *v = self; v != nil; v = [v superview]) {
        id a = [v respondsToSelector: @selector(appearance)] ? [v appearance] : nil;
        if (a) return a;
    }
    return [self window] ? win_effective([self window], s) : ap_app_effective();
}

/* --- Farben ----------------------------------------------------------------------------------------------------------------------- */

static BOOL dark(void) { return ap_is_dark(g_current ?: ap_app_effective()); }

/* dunkle Werte von macOS (Weiß mit Deckkraft für Schrift, Grau für Flächen) */
static const struct { const char *sel; CGFloat w, a; } dark_colors[] = {
    { "textColor", 1, 1 },                    { "controlTextColor", 1, 0.85 },        { "labelColor", 1, 0.85 },
    { "secondaryLabelColor", 1, 0.55 },       { "tertiaryLabelColor", 1, 0.25 },      { "quaternaryLabelColor", 1, 0.1 },
    { "disabledControlTextColor", 1, 0.25 },  { "headerTextColor", 1, 1 },            { "selectedTextColor", 1, 1 },
    { "selectedControlTextColor", 1, 1 },     { "windowFrameTextColor", 1, 0.85 },    { "placeholderTextColor", 1, 0.25 },
    { "windowBackgroundColor", 0.196, 1 },    { "controlBackgroundColor", 0.118, 1 }, { "textBackgroundColor", 0.118, 1 },
    { "underPageBackgroundColor", 0.157, 1 }, { "controlColor", 1, 0.25 },            { "headerColor", 0.196, 1 },
    { "gridColor", 0.1, 1 },                  { "separatorColor", 1, 0.1 },           { "systemFillColor", 1, 0.1 },
};
static IMP dark_orig[sizeof dark_colors / sizeof *dark_colors];

static id c_dark(Class c, SEL s) {
    for (size_t i = 0; i < sizeof dark_colors / sizeof *dark_colors; i++) {
        if (s != sel_registerName(dark_colors[i].sel)) continue;
        if (dark()) return [NSColor colorWithCalibratedWhite: dark_colors[i].w alpha: dark_colors[i].a];
        return dark_orig[i] ? ((id (*)(Class, SEL)) dark_orig[i])(c, s) : nil;
    }
    return nil;
}

static IMP o_alt_rows, o_alt_content, o_sel_text_bg;
static id c_alt(Class c, SEL s) {
    if (dark()) return @[ [NSColor colorWithCalibratedWhite: 0.118 alpha: 1], [NSColor colorWithCalibratedWhite: 0.157 alpha: 1] ];
    IMP o = s == @selector(controlAlternatingRowBackgroundColors) ? o_alt_rows : o_alt_content;
    return o ? ((id (*)(Class, SEL)) o)(c, s) : @[ [NSColor whiteColor] ];
}
static id c_sel_text_bg(Class c, SEL s) {
    if (dark()) return [NSColor colorWithCalibratedRed: 0.247 green: 0.388 blue: 0.6 alpha: 1];
    return o_sel_text_bg ? ((id (*)(Class, SEL)) o_sel_text_bg)(c, s) : nil;
}

static IMP replace_class(Class meta, SEL s, IMP imp, const char *types) {
    Method m = class_getInstanceMethod(meta, s);
    if (m) return method_setImplementation(m, imp);
    class_addMethod(meta, s, imp, types);
    return NULL;
}

__attribute__((constructor)) static void clk_appearance_init(void) {
    Class A = [NSAppearance class], AM = object_getClass(A);
    replace_class(AM, @selector(appearanceNamed:), (IMP) ap_named, "@@:@");
    class_addMethod(A, @selector(name), (IMP) ap_name, "@@:");
    replace_class(A, sel_registerName("initWithAppearanceNamed:bundle:"), (IMP) ap_init_named, "@@:@@");
    replace_class(AM, @selector(currentAppearance), (IMP) ap_current, "@@:");
    replace_class(AM, sel_registerName("currentDrawingAppearance"), (IMP) ap_current, "@@:");
    replace_class(AM, @selector(setCurrentAppearance:), (IMP) ap_set_current, "v@:@");
    class_addMethod(A, sel_registerName("bestMatchFromAppearancesWithNames:"), (IMP) ap_best, "@@:@");
    class_addMethod(A, sel_registerName("performAsCurrentDrawingAppearance:"), (IMP) ap_perform, "v@:@?");
    class_addMethod(A, sel_registerName("allowsVibrancy"), (IMP) ap_no, "c@:");

    Class P = [NSApplication class];
    class_addMethod(P, @selector(appearance), (IMP) app_appearance, "@@:");
    class_addMethod(P, @selector(setAppearance:), (IMP) app_set_appearance, "v@:@");
    class_addMethod(P, @selector(effectiveAppearance), (IMP) app_effective, "@@:");
    Class W = [NSWindow class];
    class_addMethod(W, @selector(appearance), (IMP) win_appearance, "@@:");
    class_addMethod(W, @selector(setAppearance:), (IMP) win_set_appearance, "v@:@");
    class_addMethod(W, @selector(effectiveAppearance), (IMP) win_effective, "@@:");
    class_addMethod([NSView class], @selector(effectiveAppearance), (IMP) view_effective, "@@:");

    Class CM = object_getClass([NSColor class]);
    for (size_t i = 0; i < sizeof dark_colors / sizeof *dark_colors; i++)
        dark_orig[i] = replace_class(CM, sel_registerName(dark_colors[i].sel), (IMP) c_dark, "@@:");
    o_alt_rows = replace_class(CM, @selector(controlAlternatingRowBackgroundColors), (IMP) c_alt, "@@:");
    o_alt_content = replace_class(CM, sel_registerName("alternatingContentBackgroundColors"), (IMP) c_alt, "@@:");
    o_sel_text_bg = replace_class(CM, @selector(selectedTextBackgroundColor), (IMP) c_sel_text_bg, "@@:");
}
