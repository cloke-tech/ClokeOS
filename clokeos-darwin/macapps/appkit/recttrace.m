/* CLOKE_RECT_TRACE=Klasse:Selektor,Selektor,…  (Fehlersuche): sobald die Klasse geladen ist (auch aus einem später geladenen Framework),
 * meldet jeder genannte Selektor der Form -(NSRect)xxx:(NSRect) Eingabe und Ergebnis, höchstens 400 Zeilen.
 * Beispiel: CLOKE_RECT_TRACE=MAKitSegmentedScrubberCell:textRectForBounds:,incrementButtonRectForBounds: */
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>

static NSMutableDictionary *g_orig;   /* Selektorname -> NSValue(IMP) */
static NSString *g_class;
static NSArray *g_sels;
static BOOL g_done;

static NSRect rt_imp(id self, SEL s, NSRect r) {
    IMP o = [g_orig[NSStringFromSelector(s)] pointerValue];
    NSRect out = o ? ((NSRect (*)(id, SEL, NSRect)) o)(self, s, r) : NSZeroRect;
    static int n;
    if (n++ < 400)
        NSLog(@"[clokeos-rect] %s %@ %@ -> %@", object_getClassName(self), NSStringFromSelector(s), NSStringFromRect(r), NSStringFromRect(out));
    return out;
}

static void try_install(const struct mach_header *mh, intptr_t slide) {
    if (g_done) return;
    Class c = NSClassFromString(g_class);
    if (!c) return;
    g_done = YES;
    for (NSString *n in g_sels) {
        SEL s = NSSelectorFromString(n);
        Method m = class_getInstanceMethod(c, s);
        if (!m) { NSLog(@"[clokeos-rect] %@ hat %@ nicht", g_class, n); continue; }
        g_orig[n] = [NSValue valueWithPointer: (void *) method_getImplementation(m)];
        /* in der Klasse selbst eintragen (geerbte Methoden bleiben in der Oberklasse unverändert) */
        if (!class_addMethod(c, s, (IMP) rt_imp, method_getTypeEncoding(m)))
            method_setImplementation(m, (IMP) rt_imp);
    }
}

__attribute__((constructor)) static void clk_recttrace_init(void) {
    const char *e = getenv("CLOKE_RECT_TRACE");
    if (!e || !strchr(e, ':')) return;
    NSString *spec = @(e);
    NSRange colon = [spec rangeOfString: @":"];
    g_class = [[spec substringToIndex: colon.location] retain];
    g_sels = [[[spec substringFromIndex: colon.location + 1] componentsSeparatedByString: @","] retain];
    g_orig = [NSMutableDictionary new];
    _dyld_register_func_for_add_image(try_install);
}
