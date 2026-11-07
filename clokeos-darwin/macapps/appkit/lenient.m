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

/* Antwort für jede fehlende Methode: 0 in x0/x1 und in d0…d3 (auf arm64 kommen CGFloat-Ergebnisse und kleine Gleitkomma-Strukturen
 * wie NSSize/NSRect in d0…d3 zurück; nur x0 zu nullen ließ dort Zufallswerte stehen, z. B. bei -backingScaleFactor) */
#if defined(__arm64__)
extern id ck_sink_imp(id self, SEL _cmd);
__asm__(".text\n.p2align 2\n_ck_sink_imp:\n  mov x0, #0\n  mov x1, #0\n  movi d0, #0\n  movi d1, #0\n  movi d2, #0\n  movi d3, #0\n  ret\n");
#else
static id ck_sink_imp(id self, SEL _cmd) { return nil; }
#endif

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
#import <AppKit/NSEvent.h>
#import <AppKit/NSFont.h>
#import <AppKit/NSControl.h>
#import <AppKit/NSCell.h>
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
static IMP o_nib, o_front, o_key, o_order, o_cwin, o_show, o_appev, o_winev, o_action, o_stop, o_stopc, o_mdown, o_tracking, o_hide, o_nserr, o_alerterr, o_colsec, o_asset, o_nsfont, o_mafont, o_imgnamed, o_bundleimg, o_imgfile;
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
/* Maustasten (NSEventType 1…4, 25/26): wer bekommt das Ereignis */
static void ck_ev(const char *who, id s, NSEvent *e) {
    NSUInteger t = [e type];
    if (t != 1 && t != 2 && t != 3 && t != 4 && t != 25 && t != 26) return;
    NSPoint p = [e locationInWindow];
    id w = [e window];
    fprintf(stderr, "[clokeos-trace] %s type=%lu at %.0f,%.0f window=%s \"%s\" modal=%s\n", who, (unsigned long)t, p.x, p.y,
            w ? class_getName(object_getClass(w)) : "nil", w ? ck_title(w) : "",
            [NSApp modalWindow] ? ck_title([NSApp modalWindow]) : "-");
}
static void h_appev(id s, SEL c, NSEvent *e) { ck_ev("NSApp sendEvent", s, e); ((void (*)(id, SEL, id))o_appev)(s, c, e); }
static void h_winev(id s, SEL c, NSEvent *e) { ck_ev("NSWindow sendEvent", s, e); ((void (*)(id, SEL, id))o_winev)(s, c, e); }
static BOOL h_action(id s, SEL c, SEL a, id to, id from) {
    fprintf(stderr, "[clokeos-trace] sendAction:%s to:%s from:%s\n", a ? sel_getName(a) : "-", to ? class_getName(object_getClass(to)) : "nil (Responderkette)",
            from ? class_getName(object_getClass(from)) : "nil");
    return ((BOOL (*)(id, SEL, SEL, id, id))o_action)(s, c, a, to, from);
}
static void h_stop(id s, SEL c) { fprintf(stderr, "[clokeos-trace] stopModal\n"); ((void (*)(id, SEL))o_stop)(s, c); }
static void h_stopc(id s, SEL c, NSInteger code) { fprintf(stderr, "[clokeos-trace] stopModalWithCode:%ld\n", (long)code); ((void (*)(id, SEL, NSInteger))o_stopc)(s, c, code); }
static void h_mdown(id s, SEL c, NSEvent *e) {
    fprintf(stderr, "[clokeos-trace] mouseDown: -> %s (%s)\n", class_getName(object_getClass(s)), [[s cell] respondsToSelector:@selector(title)] ? [[[s cell] title] UTF8String] ?: "" : "");
    ((void (*)(id, SEL, id))o_mdown)(s, c, e);
    fprintf(stderr, "[clokeos-trace] mouseDown: <- %s\n", class_getName(object_getClass(s)));
}
static void h_hide(id s, SEL c) {
    fprintf(stderr, "[clokeos-trace] X11Window hideWindow %p windowNumber=%ld\n", s, (long)[s windowNumber]);
    ((void (*)(id, SEL))o_hide)(s, c);
}
/* NSError mit "Datei fehlt" (POSIX/Cocoa-Code 2, 4, 260): userInfo und Aufrufkette, damit ein Fehlerdialog einer Ursache zuzuordnen ist */
static void ck_bt(int skip);
static id h_nserr(id s, SEL c, NSString *domain, NSInteger code, NSDictionary *info) {
    if (code == 2 || code == 4 || code == 260) {
        fprintf(stderr, "[clokeos-trace] NSError %s %ld %s\n", [domain UTF8String] ?: "", (long)code, [[info description] UTF8String] ?: "");
        ck_bt(2);
    }
    return ((id (*)(id, SEL, id, NSInteger, id))o_nserr)(s, c, domain, code, info);
}
/* Fehlerdialoge: welcher Fehler, von wo */
static id h_alerterr(id s, SEL c, NSError *e) {
    fprintf(stderr, "[clokeos-trace] alertWithError: %s %ld %s\n", [[e domain] UTF8String] ?: "", (long)[e code], [[[e userInfo] description] UTF8String] ?: "");
    ck_bt(2);
    return ((id (*)(id, SEL, id))o_alerterr)(s, c, e);
}
/* Logic Pro (MAToolKit): Farben aus Abschnitt/Name; nil-Ergebnisse melden, mit dem Bestandteil davor (Asset) */
static id h_colsec(id s, SEL c, id section, id name) {
    id r = ((id (*)(id, SEL, id, id))o_colsec)(s, c, section, name);
    if (r == nil) fprintf(stderr, "[clokeos-trace] MAKitColor %s/%s -> nil\n", [[section description] UTF8String] ?: "", [[name description] UTF8String] ?: "");
    return r;
}
static id h_asset(id s, SEL c, id name, id set, id family) {
    id r = ((id (*)(id, SEL, id, id, id))o_asset)(s, c, name, set, family);
    if (r == nil) fprintf(stderr, "[clokeos-trace] MAAssetManager asset %s set %s family %s -> nil\n", [[name description] UTF8String] ?: "",
                          [[set description] UTF8String] ?: "", [[family description] UTF8String] ?: "");
    return r;
}
static id h_nsfont(id s, SEL c, NSString *name, CGFloat size) {
    id r = ((id (*)(id, SEL, id, CGFloat))o_nsfont)(s, c, name, size);
    if (r == nil) fprintf(stderr, "[clokeos-trace] NSFont %s %.1f -> nil\n", [name UTF8String] ?: "(nil)", size);
    return r;
}
static id h_mafont(id s, SEL c, NSString *name, CGFloat size) {
    id r = ((id (*)(id, SEL, id, CGFloat))o_mafont)(s, c, name, size);
    if (r == nil) fprintf(stderr, "[clokeos-trace] MAAssetManager font %s %.1f -> nil\n", [name UTF8String] ?: "(nil)", size);
    return r;
}
/* Bilder, die nicht gefunden werden (Name bzw. Pfad), je einmal */
static void ck_img_nil(const char *what, id name) {
    static NSMutableSet *seen;
    if (!seen) seen = [NSMutableSet new];
    NSString *k = [NSString stringWithFormat: @"%s %@", what, name];
    if ([seen containsObject: k]) return;
    [seen addObject: k];
    fprintf(stderr, "[clokeos-trace] Bild fehlt: %s\n", [k UTF8String]);
}
static id h_imgnamed(id s, SEL c, id name) {
    id r = ((id (*)(id, SEL, id))o_imgnamed)(s, c, name);
    if (!r) ck_img_nil("imageNamed:", name);
    return r;
}
static id h_bundleimg(id s, SEL c, id name) {
    id r = ((id (*)(id, SEL, id))o_bundleimg)(s, c, name);
    if (!r) ck_img_nil("imageForResource:", [NSString stringWithFormat: @"%@ (%@)", name, [[s bundlePath] lastPathComponent]]);
    return r;
}
static id h_imgfile(id s, SEL c, id path) {
    id r = ((id (*)(id, SEL, id))o_imgfile)(s, c, path);
    if (!r) ck_img_nil("initWithContentsOfFile:", path);
    return r;
}
+ (void)hooks {
    { Method m = class_getClassMethod(NSClassFromString(@"NSImage"), @selector(imageNamed:)); if (m) o_imgnamed = method_setImplementation(m, (IMP)h_imgnamed); }
    { Method m = class_getInstanceMethod([NSBundle class], @selector(imageForResource:)); if (m) o_bundleimg = method_setImplementation(m, (IMP)h_bundleimg); }
    { Method m = class_getInstanceMethod(NSClassFromString(@"NSImage"), @selector(initWithContentsOfFile:)); if (m) o_imgfile = method_setImplementation(m, (IMP)h_imgfile); }
    { Method m = class_getClassMethod([NSFont class], @selector(fontWithName:size:)); if (m) o_nsfont = method_setImplementation(m, (IMP)h_nsfont); }
    { Class mc = NSClassFromString(@"MAAssetManager"); Method m = class_getClassMethod(mc, @selector(fontWithName:size:));
      if (!m) m = class_getInstanceMethod(mc, @selector(fontWithName:size:)); if (m) o_mafont = method_setImplementation(m, (IMP)h_mafont); }
    { Method m = class_getClassMethod(NSClassFromString(@"MAKitColor"), @selector(colorFromSectionUncached:withName:)); if (m) o_colsec = method_setImplementation(m, (IMP)h_colsec); }
    { Method m = class_getInstanceMethod(NSClassFromString(@"MAAssetManager"), @selector(assetWithName:fromAssetSet:fromFamily:)); if (m) o_asset = method_setImplementation(m, (IMP)h_asset); }
    { Method m = class_getClassMethod(NSClassFromString(@"NSAlert"), @selector(alertWithError:)); if (m) o_alerterr = method_setImplementation(m, (IMP)h_alerterr); }
    o_nserr = ck_hook([NSError class], @selector(initWithDomain:code:userInfo:), (IMP)h_nserr);
    o_hide = ck_hook(NSClassFromString(@"X11Window"), @selector(hideWindow), (IMP)h_hide);
    o_action = ck_hook([NSApplication class], @selector(sendAction:to:from:), (IMP)h_action);
    o_stop = ck_hook([NSApplication class], @selector(stopModal), (IMP)h_stop);
    o_stopc = ck_hook([NSApplication class], @selector(stopModalWithCode:), (IMP)h_stopc);
    o_mdown = ck_hook(NSClassFromString(@"NSControl"), @selector(mouseDown:), (IMP)h_mdown);
    o_appev = ck_hook([NSApplication class], @selector(sendEvent:), (IMP)h_appev);
    o_winev = ck_hook([NSWindow class], @selector(sendEvent:), (IMP)h_winev);
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
static void ck_bt(int skip) {
    void *bt[24]; int n = backtrace(bt, 24);
    for (int i = skip; i < n; i++) {
        Dl_info di; memset(&di, 0, sizeof di);
        dladdr(bt[i], &di);
        const char *img = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
        fprintf(stderr, "[clokeos-trace]   %-24s +0x%lx %s\n", img ? img + 1 : "?", (unsigned long)((char *)bt[i] - (char *)di.dli_fbase),
                di.dli_sname ? di.dli_sname : "");
    }
}
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
