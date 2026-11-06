/* Einrichtungshilfe für Mac-Programme, die viel mehr von AppKit/Foundation verlangen, als Darlings Fassungen bieten (Logic Pro):
 * Mit CLOKE_LENIENT=1 im Prozess geht eine Nachricht, die niemand umsetzt (sonst "unrecognized selector" und Abbruch), an ein gemeinsames
 * Senkenobjekt, das nil/0 zurückgibt und den Selektor einmal auf stderr meldet:  [clokeos-lenient] -[NSHelpManager registerBooksInBundle:]
 * So läuft das Programm weiter und liefert die ganze Liste der fehlenden Methoden auf einmal statt eines Absturzes je Lauf.
 * Ohne CLOKE_LENIENT ändert sich nichts. Gilt für Objekte und Klassen (+forwardingTargetForSelector:); respondsToSelector: bleibt unberührt.
 * Liegt im X11-Backend (macapps/appkit.sh), das AppKit beim Start lädt. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

@interface CKLenientSink : NSObject
@end

static id ck_sink_imp(id self, SEL _cmd) { return nil; }

@implementation CKLenientSink
+ (BOOL)resolveInstanceMethod:(SEL)sel {
    class_addMethod(self, sel, (IMP)ck_sink_imp, "@@:");
    return YES;
}
@end

static id CKSinkFor(id receiver, SEL sel, BOOL isClass) {
    if (!getenv("CLOKE_LENIENT"))
        return nil;
    static NSMutableSet *seen;
    static id sink;
    @synchronized([CKLenientSink class]) {
        if (!seen) { seen = [NSMutableSet new]; sink = [CKLenientSink new]; }
        const char *cn = class_getName(isClass ? (Class)receiver : object_getClass(receiver));
        NSString *key = [NSString stringWithFormat:@"%c[%s %s]", isClass ? '+' : '-', cn, sel_getName(sel)];
        if (![seen containsObject:key]) {
            [seen addObject:key];
            fprintf(stderr, "[clokeos-lenient] %s\n", [key UTF8String]);
        }
    }
    return sink;
}

@interface NSObject (CKLenient)
@end
@implementation NSObject (CKLenient)
- (id)forwardingTargetForSelector:(SEL)sel { return CKSinkFor(self, sel, NO); }
+ (id)forwardingTargetForSelector:(SEL)sel { return CKSinkFor(self, sel, YES); }
@end

/* CLOKE_TRACE=1: beim Start von AppKit den Programmdelegaten melden (für die Frage, warum -finishLaunching ein leeres Dokument öffnet) */
#import <AppKit/NSApplication.h>
#import <AppKit/NSWindow.h>
#import <AppKit/NSWindowController.h>
@interface CKTrace : NSObject
@end
@implementation CKTrace
+ (void)will:(NSNotification *)n {
    id d = [NSApp delegate];
    fprintf(stderr, "[clokeos-trace] willFinishLaunching: NSApp=%s delegate=%s shouldOpenUntitled=%d openUntitledFile=%d\n",
            class_getName(object_getClass(NSApp)), d ? class_getName(object_getClass(d)) : "(nil)",
            (int)[d respondsToSelector:@selector(applicationShouldOpenUntitledFile:)],
            (int)[d respondsToSelector:@selector(applicationOpenUntitledFile:)]);
    fprintf(stderr, "[clokeos-trace] NSApp overrides awakeFromNib: %d\n",
            (int)([NSApp methodForSelector:@selector(awakeFromNib)] != [NSObject instanceMethodForSelector:@selector(awakeFromNib)]));
}
+ (void)did:(NSNotification *)n {
    id d = [NSApp delegate];
    fprintf(stderr, "[clokeos-trace] didFinishLaunching: delegate=%s mainMenu=%p\n",
            d ? class_getName(object_getClass(d)) : "(nil)", [NSApp mainMenu]);
}
/* Fensterspur: welche Nibs geladen, welche Fenster gezeigt werden; 8 s nach dem Start die Fensterliste */
+ (void)windows {
    NSArray *ws = [NSApp windows];
    fprintf(stderr, "[clokeos-trace] %lu Fenster, keyWindow=%p mainWindow=%p\n", (unsigned long)[ws count], [NSApp keyWindow], [NSApp mainWindow]);
    for (NSWindow *w in ws) {
        NSRect f = [w frame];
        fprintf(stderr, "[clokeos-trace]   %s %p \"%s\" visible=%d frame=%.0f,%.0f %.0fx%.0f ctrl=%s\n", class_getName(object_getClass(w)), w,
                [[w title] UTF8String] ?: "", (int)[w isVisible], f.origin.x, f.origin.y, f.size.width, f.size.height,
                [w windowController] ? class_getName(object_getClass([w windowController])) : "-");
    }
}
/* ohne Blöcke: PureDarwins libobjc hat keine Trampoline für imp_implementationWithBlock */
static IMP ck_hook(Class c, SEL sel, IMP imp) {
    Method m = c ? class_getInstanceMethod(c, sel) : NULL;
    return m ? method_setImplementation(m, imp) : NULL;
}
static IMP o_nib, o_front, o_key, o_order, o_cwin, o_show;
static const char *ck_title(id w) { const char *t = [[w title] UTF8String]; return t ? t : ""; }
static id h_nib(id s, SEL c, NSString *p) {
    fprintf(stderr, "[clokeos-trace] nib %s\n", [[p lastPathComponent] UTF8String]);
    return ((id (*)(id, SEL, id))o_nib)(s, c, p);
}
static void h_front(id s, SEL c, id a) {
    fprintf(stderr, "[clokeos-trace] orderFront: %s \"%s\"\n", class_getName(object_getClass(s)), ck_title(s));
    ((void (*)(id, SEL, id))o_front)(s, c, a);
}
static void h_key(id s, SEL c, id a) {
    fprintf(stderr, "[clokeos-trace] makeKeyAndOrderFront: %s \"%s\"\n", class_getName(object_getClass(s)), ck_title(s));
    ((void (*)(id, SEL, id))o_key)(s, c, a);
}
static void h_order(id s, SEL c, NSInteger m, NSInteger r) {
    fprintf(stderr, "[clokeos-trace] orderWindow:%ld %s \"%s\"\n", (long)m, class_getName(object_getClass(s)), ck_title(s));
    ((void (*)(id, SEL, NSInteger, NSInteger))o_order)(s, c, m, r);
}
static id h_cwin(id s, SEL c) {
    id w = ((id (*)(id, SEL))o_cwin)(s, c);
    static NSMutableSet *seen;
    if (!seen) seen = [NSMutableSet new];
    NSValue *k = [NSValue valueWithPointer:s];
    if (![seen containsObject:k]) {
        [seen addObject:k];
        const char *nib = [[s windowNibName] UTF8String];
        fprintf(stderr, "[clokeos-trace] %s -window -> %s %p (nib %s)\n", class_getName(object_getClass(s)),
                w ? class_getName(object_getClass(w)) : "nil", w, nib ? nib : "-");
    }
    return w;
}
static void h_show(id s, SEL c, id a) {
    fprintf(stderr, "[clokeos-trace] %s -showWindow:\n", class_getName(object_getClass(s)));
    ((void (*)(id, SEL, id))o_show)(s, c, a);
}
+ (void)hooks {
    o_nib = ck_hook(NSClassFromString(@"NSNib"), @selector(initWithContentsOfFile:), (IMP)h_nib);
    o_front = ck_hook([NSWindow class], @selector(orderFront:), (IMP)h_front);
    o_key = ck_hook([NSWindow class], @selector(makeKeyAndOrderFront:), (IMP)h_key);
    o_order = ck_hook([NSWindow class], @selector(orderWindow:relativeTo:), (IMP)h_order);
    o_cwin = ck_hook(NSClassFromString(@"NSWindowController"), @selector(window), (IMP)h_cwin);
    o_show = ck_hook(NSClassFromString(@"NSWindowController"), @selector(showWindow:), (IMP)h_show);
}
/* CLOKE_TRACE: jede geworfene ObjC-Ausnahme mit Grund und Aufrufkette (Bild + Versatz) melden */
#include <execinfo.h>
#include <dlfcn.h>
static id ck_exc_pre(id e) {
    fprintf(stderr, "[clokeos-trace] Ausnahme %s: %s\n", [[e name] UTF8String] ?: "?", [[e reason] UTF8String] ?: "");
    void *bt[24]; int n = backtrace(bt, 24);
    for (int i = 1; i < n; i++) {
        Dl_info di; memset(&di, 0, sizeof di);
        dladdr(bt[i], &di);
        const char *img = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
        fprintf(stderr, "[clokeos-trace]   %-24s +0x%lx %s\n", img ? img + 1 : "?", (unsigned long)((char *)bt[i] - (char *)di.dli_fbase),
                di.dli_sname ? di.dli_sname : "");
    }
    return e;
}
extern void *objc_setExceptionPreprocessor(id (*)(id));
+ (void)load {
    if (!getenv("CLOKE_TRACE")) return;
    objc_setExceptionPreprocessor(ck_exc_pre);
    [self hooks];
    [self performSelector:@selector(windows) withObject:nil afterDelay:8];
    [self performSelector:@selector(windows) withObject:nil afterDelay:30];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(will:) name:@"NSApplicationWillFinishLaunchingNotification" object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(did:) name:@"NSApplicationDidFinishLaunchingNotification" object:nil];
}
@end
