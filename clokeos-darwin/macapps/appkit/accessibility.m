/* NSAccessibility-Eigenschaften (macOS 10.10) für Cocotrons AppKit: gespeicherte Werte statt Platzhalter.
 *
 * Darlings NSAccessibilityElement war ein Platzhalter (jede Nachricht "Stub called"), NSView/NSCell/NSWindow kannten setAccessibilityRole:,
 * setAccessibilityLabel: usw. nicht. Programme mit eigener Weiterleitung (Logic Pros MANodeAccessibilityRepresentation reicht an ihren
 * Knoten weiter, der wieder zurück) liefen damit endlos im Kreis, bis der Stapel überlief. Hier bekommen diese Klassen die üblichen
 * Eigenschaften des NSAccessibility-Protokolls als gespeicherte Werte (Objekt-, BOOL- und Rechteck-Eigenschaften) sowie
 * accessibilitySetOverrideValue:forAttribute:. Bedienungshilfen gibt es unter ClokeOS nicht; die Werte werden nur gemerkt.
 * Eingetragen mit class_addMethod: was eine Klasse selbst hat, bleibt. */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

static char k_ax;

static NSMutableDictionary *ax(id o) {
    NSMutableDictionary *d = objc_getAssociatedObject(o, &k_ax);
    if (!d) { d = [NSMutableDictionary dictionary]; objc_setAssociatedObject(o, &k_ax, d, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    return d;
}
/* Schlüssel aus dem Selektor: setAccessibilityRole: / accessibilityRole / isAccessibilityElement -> AccessibilityRole … */
static NSString *key(SEL s) {
    NSString *n = NSStringFromSelector(s);
    if ([n hasPrefix: @"set"]) n = [n substringWithRange: NSMakeRange(3, [n length] - 4)];
    else if ([n hasPrefix: @"is"]) n = [n substringFromIndex: 2];
    else n = [[[n substringToIndex: 1] uppercaseString] stringByAppendingString: [n substringFromIndex: 1]];
    return n;
}
static id g_obj(id s, SEL c) { return ax(s)[key(c)]; }
static void s_obj(id s, SEL c, id v) { if (v) ax(s)[key(c)] = v; else [ax(s) removeObjectForKey: key(c)]; }
static BOOL g_b(id s, SEL c) { return [ax(s)[key(c)] boolValue]; }
static void s_b(id s, SEL c, BOOL v) { ax(s)[key(c)] = @(v); }
static NSRect g_r(id s, SEL c) { return [ax(s)[key(c)] rectValue]; }
static void s_r(id s, SEL c, NSRect v) { ax(s)[key(c)] = [NSValue valueWithRect: v]; }
static BOOL s_override(id s, SEL c, id v, NSString *attr) { if (attr) { if (v) ax(s)[attr] = v; else [ax(s) removeObjectForKey: attr]; } return YES; }

static void add(Class cls, NSString *name, char t) {
    NSString *cap = [[[name substringToIndex: 1] uppercaseString] stringByAppendingString: [name substringFromIndex: 1]];
    SEL get = NSSelectorFromString(t == 'b' ? [@"is" stringByAppendingString: cap] : name);
    SEL set = NSSelectorFromString([NSString stringWithFormat: @"set%@:", cap]);
    switch (t) {
    case 'o': class_addMethod(cls, get, (IMP) g_obj, "@@:"); class_addMethod(cls, set, (IMP) s_obj, "v@:@"); break;
    case 'b': class_addMethod(cls, get, (IMP) g_b, "c@:"); class_addMethod(cls, NSSelectorFromString(name), (IMP) g_b, "c@:");
              class_addMethod(cls, set, (IMP) s_b, "v@:c"); break;
    case 'r': class_addMethod(cls, get, (IMP) g_r, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
              class_addMethod(cls, set, (IMP) s_r, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}"); break;
    }
}

__attribute__((constructor)) static void clk_accessibility_init(void) {
    static const char *objs[] = { "accessibilityRole", "accessibilitySubrole", "accessibilityRoleDescription", "accessibilityLabel",
        "accessibilityTitle", "accessibilityValue", "accessibilityHelp", "accessibilityIdentifier", "accessibilityParent",
        "accessibilityChildren", "accessibilityTitleUIElement", "accessibilityValueDescription", "accessibilityPlaceholderValue",
        "accessibilityCustomActions", "accessibilityCustomRotors", "accessibilityUserInputLabels", "accessibilityWindow",
        "accessibilityTopLevelUIElement", "accessibilitySelectedChildren", "accessibilityVisibleChildren", "accessibilityLinkedUIElements",
        "accessibilityMinValue", "accessibilityMaxValue", "accessibilityOrientation" };
    static const char *bools[] = { "accessibilityElement", "accessibilityEnabled", "accessibilityFocused", "accessibilitySelected",
        "accessibilityExpanded", "accessibilityHidden", "accessibilityRequired", "accessibilityDisclosed" };
    static const char *rects[] = { "accessibilityFrame", "accessibilityFrameInParentSpace" };
    const char *classes[] = { "NSAccessibilityElement", "NSView", "NSCell", "NSWindow", "NSMenu", "NSMenuItem", "NSApplication" };
    for (size_t k = 0; k < sizeof classes / sizeof *classes; k++) {
        Class c = objc_getClass(classes[k]);
        if (!c) continue;
        for (size_t i = 0; i < sizeof objs / sizeof *objs; i++) add(c, @(objs[i]), 'o');
        for (size_t i = 0; i < sizeof bools / sizeof *bools; i++) add(c, @(bools[i]), 'b');
        for (size_t i = 0; i < sizeof rects / sizeof *rects; i++) add(c, @(rects[i]), 'r');
        class_addMethod(c, @selector(accessibilitySetOverrideValue:forAttribute:), (IMP) s_override, "c@:@@");
    }
}
