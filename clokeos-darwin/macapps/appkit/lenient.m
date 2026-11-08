/* Einrichtungshilfe für Mac-Programme, die viel mehr von AppKit/Foundation verlangen, als Darlings Fassungen bieten (Logic Pro):
 * Mit CLOKE_LENIENT=1 im Prozess geht eine Nachricht, die niemand umsetzt (sonst "unrecognized selector" und Abbruch), an ein gemeinsames
 * Senkenobjekt, das nil/0 zurückgibt und den Selektor einmal auf stderr meldet:  [clokeos-lenient] -[NSHelpManager registerBooksInBundle:]
 * So läuft das Programm weiter und liefert die ganze Liste der fehlenden Methoden auf einmal statt eines Absturzes je Lauf.
 * Ohne CLOKE_LENIENT ändert sich nichts. Gilt für Objekte und Klassen (+forwardingTargetForSelector:); respondsToSelector: bleibt unberührt.
 * Liegt im X11-Backend (macapps/appkit.sh), das AppKit beim Start lädt. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>

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

/* Eine Klasse mit eigenem forwardInvocation: leitet selbst weiter (Logic Pros MAButtonCell, MATextFieldCell, MASliderCell … reichen
 * resourceBundleID, assetSetPath usw. an ihr Asset-Objekt weiter). Die Laufzeit fragt forwardingTargetForSelector: aber vorher; gäbe die
 * Senke dort ein Ziel, käme die eigene Weiterleitung nie an die Reihe (so fehlten Logics Knopfbilder). Daher: kennt die Klasse den
 * Selektor über methodSignatureForSelector:, bleibt die Weiterleitung ihr überlassen. */
static BOOL CKOwnForwarding(id receiver, SEL sel, BOOL isClass) {
    static IMP base;
    if (!base) base = class_getMethodImplementation([NSObject class], @selector(forwardInvocation:));
    Class c = object_getClass(receiver);
    IMP fwd = class_getMethodImplementation(c, @selector(forwardInvocation:));
    if (fwd == base) return NO;
    /* nur Weiterleitung des Programms selbst; Darlings Platzhalterklassen (NSGridRow, Gestenerkenner …) leiten "nicht umgesetzt" weiter */
    Dl_info di;
    if (!dladdr((void *) fwd, &di) || !di.dli_fname || strncmp(di.dli_fname, "/Library/ClokeOS/", 17) == 0 ||
        strncmp(di.dli_fname, "/usr/lib/", 9) == 0 || strncmp(di.dli_fname, "/System/", 8) == 0)
        return NO;
    /* methodSignatureForSelector: solcher Klassen fragt selbst weiter (Elternobjekt, Asset-Objekt …) und kann dabei wieder hier landen:
     * verschachtelt gilt die Senke */
    static __thread int depth;
    if (depth > 0) return NO;
    depth++;
    BOOL known = NO;
    @try { known = [receiver methodSignatureForSelector: sel] != nil; }
    @catch (id e) { known = NO; }
    depth--;
    return known;
}

static id CKSinkFor(id receiver, SEL sel, BOOL isClass) {
    if (!getenv("CLOKE_LENIENT"))
        return nil;
    if (CKOwnForwarding(receiver, sel, isClass)) {
        /* Kreislauf erkennen: dieselbe Nachricht an dasselbe Objekt kommt tiefer im Stapel wieder hierher (die eigene Weiterleitung
         * führt zurück, weil eine AppKit-Methode fehlt) -> ab dem dritten Mal die Senke, statt bis zum Stapelüberlauf */
        static __thread struct { id r; SEL s; uintptr_t sp; int n; } loop;
        uintptr_t sp = (uintptr_t) __builtin_frame_address(0);
        if (loop.r == receiver && loop.s == sel && sp < loop.sp) loop.n++;
        else { loop.r = receiver; loop.s = sel; loop.sp = sp; loop.n = 0; }
        if (loop.n < 3)
            return nil;
        static NSMutableSet *loops;
        @synchronized([CKLenientSink class]) {
            if (!loops) loops = [NSMutableSet new];
            NSString *k = [NSString stringWithFormat: @"%s %s", class_getName(object_getClass(receiver)), sel_getName(sel)];
            if (![loops containsObject: k]) { [loops addObject: k]; fprintf(stderr, "[clokeos-lenient] Weiterleitung im Kreis: -[%s]\n", [k UTF8String]); }
        }
    }
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
#import <AppKit/NSTableView.h>
#import <AppKit/NSButton.h>
#import <AppKit/NSButtonCell.h>
#import <AppKit/NSImage.h>
#import <AppKit/NSBitmapImageRep.h>
#import <QuartzCore/CALayer.h>
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
static IMP o_nib, o_front, o_key, o_order, o_cwin, o_show, o_appev, o_winev, o_action, o_stop, o_stopc, o_mdown, o_tracking, o_hide, o_nserr, o_alerterr, o_colsec, o_asset, o_nsfont, o_mafont, o_imgnamed, o_bundleimg, o_imgfile, o_typeurl, o_docclass, o_readurl, o_readlx, o_docinit, o_docinitalt;
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
/* Maustasten (NSEventType 1…4, 25/26) und Tasten (10/11): wer bekommt das Ereignis */
/* F12 (Tastencode 111): Ansichtsbaum aller sichtbaren Fenster nach stderr – Klasse, Rahmen, verborgen, wantsLayer,
 * ob die Klasse drawRect:/updateLayer selbst implementiert, Unteransichten eingerückt (bis Tiefe 14) */
static BOOL ck_overrides(Class c, SEL sel) {
    IMP mine = class_getMethodImplementation(c, sel), base = class_getMethodImplementation([NSView class], sel);
    return mine && mine != base;
}
static void ck_dumpview(id v, int depth) {
    NSRect f = [v frame];
    BOOL layer = [v respondsToSelector: @selector(wantsLayer)] && [v wantsLayer];
    fprintf(stderr, "[viewdump] %*s%s {%.0f,%.0f %.0fx%.0f}%s%s%s%s%s\n", depth * 2, "", class_getName(object_getClass(v)),
            f.origin.x, f.origin.y, f.size.width, f.size.height, [v isHidden] ? " hidden" : "", layer ? " layer" : "",
            ck_overrides(object_getClass(v), @selector(drawRect:)) ? " drawRect" : "",
            ck_overrides(object_getClass(v), @selector(updateLayer)) ? " updateLayer" : "",
            [v respondsToSelector: @selector(stringValue)] && [v isKindOfClass: [NSControl class]] ? " ctl" : "");
    /* Tabellen: Delegat und welche Zeichenwege er anbietet (zellen- oder ansichtsbasiert) */
    if ([v isKindOfClass: [NSTableView class]]) {
        id d = [v delegate];
        static const char *const sels[] = {"tableView:viewForTableColumn:row:", "outlineView:viewForTableColumn:item:",
            "tableView:dataCellForTableColumn:row:", "outlineView:dataCellForTableColumn:item:", "tableView:rowViewForRow:",
            "outlineView:rowViewForItem:", "tableView:willDisplayCell:forTableColumn:row:"};
        fprintf(stderr, "[viewdump] %*s  delegate %s rows=%ld cols=%ld:", depth * 2, "", d ? class_getName(object_getClass(d)) : "nil",
                (long)[v numberOfRows], (long)[[v tableColumns] count]);
        for (unsigned i = 0; i < sizeof sels / sizeof *sels; i++)
            if ([d respondsToSelector: sel_registerName(sels[i])]) fprintf(stderr, " %s", sels[i]);
        fprintf(stderr, "\n");
        {   /* Hintergrund: Tabelle, Clip- und Scroll-Ansicht */
            NSColor *bg = [v backgroundColor];
            id clip = [v superview], scroll = [clip superview];
            fprintf(stderr, "[viewdump] %*s  table bg=%s alt=%d grid=%lu clip=%s draws=%d bg=%s scroll draws=%d\n", depth * 2, "",
                    [[bg description] UTF8String] ?: "nil", (int)[v usesAlternatingRowBackgroundColors], (unsigned long)[v gridStyleMask],
                    class_getName(object_getClass(clip)), [clip respondsToSelector: @selector(drawsBackground)] ? (int)[clip drawsBackground] : -1,
                    [clip respondsToSelector: @selector(backgroundColor)] ? [[[clip backgroundColor] description] UTF8String] ?: "nil" : "-",
                    [scroll respondsToSelector: @selector(drawsBackground)] ? (int)[scroll drawsBackground] : -1);
        }
        /* je Zeile die vorbereitete Zelle (Klasse, Titel, Wert), höchstens 12 Zeilen */
        NSInteger rows = MIN((NSInteger)[v numberOfRows], 12), cols = (NSInteger)[[v tableColumns] count];
        for (NSInteger r = 0; r < rows; r++)
            for (NSInteger c = 0; c < cols; c++) {
                NSCell *cell = nil;
                @try { cell = [v preparedCellAtColumn: c row: r]; } @catch (id e) {}
                if (!cell) continue;
                id t = [cell respondsToSelector: @selector(titleOfSelectedItem)] ? [(id)cell titleOfSelectedItem]
                     : [cell respondsToSelector: @selector(title)] ? [(id)cell title] : nil;
                fprintf(stderr, "[viewdump] %*s    [%ld,%ld] %s align=%ld fmt=%s vclass=%s str=\"%s\" title=\"%s\" value=%s\n", depth * 2, "", (long)r, (long)c,
                        class_getName(object_getClass(cell)), (long)[cell alignment],
                        [cell formatter] ? class_getName(object_getClass([cell formatter])) : "-",
                        [cell objectValue] ? class_getName(object_getClass([cell objectValue])) : "nil",
                        [[[cell stringValue] description] UTF8String] ?: "", [[t description] UTF8String] ?: "",
                        [[[[cell objectValue] description] substringToIndex: MIN((NSUInteger)40, [[[cell objectValue] description] length])] UTF8String] ?: "nil");
            }
    }
    /* Knöpfe: Titel, Bild, Bildlage, Vorlage */
    if ([v isKindOfClass: [NSButton class]]) {
        NSButtonCell *bc = [v cell];
        NSImage *im = [bc respondsToSelector: @selector(image)] ? [bc image] : nil;
        fprintf(stderr, "[viewdump] %*s  button %s title=\"%s\" image=%.0fx%.0f tmpl=%d pos=%ld bordered=%d\n", depth * 2, "",
                class_getName(object_getClass(bc)), [[bc title] UTF8String] ?: "", im ? [im size].width : -1, im ? [im size].height : -1,
                im ? (int)[im isTemplate] : -1, (long)[bc imagePosition], (int)[bc isBordered]);
        if ([bc respondsToSelector: sel_registerName("offImages")]) {   /* Zellen mit Zustandsbildern aus Asset-Sätzen */
            id (*m)(id, SEL) = (id (*)(id, SEL)) objc_msgSend;
            id off = m(bc, sel_registerName("offImages")), rb = nil, path = nil;
            @try { rb = m(bc, sel_registerName("resourceBundleID")); path = m(bc, sel_registerName("assetSetPath")); } @catch (id e) {}
            fprintf(stderr, "[viewdump] %*s    assets bundle=%s path=%s offImages=%lu\n", depth * 2, "", [[rb description] UTF8String] ?: "nil",
                    [[path description] UTF8String] ?: "nil", (unsigned long)([off respondsToSelector: @selector(count)] ? [off count] : 0));
        }
        for (id rep in [im representations])
            fprintf(stderr, "[viewdump] %*s    rep %s %s\n", depth * 2, "", class_getName(object_getClass(rep)),
                    [[[rep description] substringToIndex: MIN((NSUInteger)120, [[rep description] length])] UTF8String] ?: "");
    }
    /* Ebene mit eigenen Unterebenen (nicht von Unteransichten) */
    CALayer *L = [v respondsToSelector: @selector(layer)] ? [v layer] : nil;
    if (L != nil && [[L sublayers] count])
        fprintf(stderr, "[viewdump] %*s  layer %s sublayers=%lu\n", depth * 2, "", class_getName(object_getClass(L)), (unsigned long)[[L sublayers] count]);
    if (depth >= 18) return;
    for (id sub in [v subviews]) ck_dumpview(sub, depth + 1);
}
static void ck_dumpwindows(void) {
    for (NSWindow *w in [NSApp windows]) {
        if (![w isVisible]) continue;
        fprintf(stderr, "[viewdump] Fenster %s \"%s\"\n", class_getName(object_getClass(w)), ck_title(w));
        ck_dumpview([w contentView], 1);
    }
}
static void ck_ev(const char *who, id s, NSEvent *e) {
    NSUInteger t = [e type];
    if (t == 10 && [e keyCode] == 111 && who[2] == 'A') { ck_dumpwindows(); return; }
    if (t == 10 || t == 11) {   /* Tasten: Tastencode, Zeichen, Schlüsselfenster */
        id kw = [NSApp keyWindow];
        fprintf(stderr, "[clokeos-trace] %s key%s code=%u chars=\"%s\" flags=%#lx keyWindow=%s \"%s\" firstResponder=%s\n", who, t == 10 ? "Down" : "Up",
                (unsigned)[e keyCode], [[e characters] UTF8String] ?: "", (unsigned long)[e modifierFlags],
                kw ? class_getName(object_getClass(kw)) : "nil", kw ? ck_title(kw) : "",
                kw && [kw firstResponder] ? class_getName(object_getClass([kw firstResponder])) : "nil");
        return;
    }
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
    if (code == 2 || code == 4 || code == 256 || code == 260) {
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
    if ([r isKindOfClass: [NSColor class]] && [r respondsToSelector: @selector(alphaComponent)] && ((CGFloat (*)(id, SEL)) objc_msgSend)(r, @selector(alphaComponent)) == 0) {
        static NSMutableSet *seen;   /* durchsichtige Farb-Assets je Name einmal */
        if (!seen) seen = [NSMutableSet new];
        NSString *k = [NSString stringWithFormat: @"%@|%@|%@", name, set, family];
        if (![seen containsObject: k]) {
            [seen addObject: k];
            fprintf(stderr, "[clokeos-trace] MAAssetManager asset %s set %s family %s -> durchsichtig %s\n", [[name description] UTF8String] ?: "",
                    [[set description] UTF8String] ?: "", [[family description] UTF8String] ?: "", [[r description] UTF8String] ?: "");
        }
    }
    if (r == nil) fprintf(stderr, "[clokeos-trace] MAAssetManager asset %s set %s family %s -> nil\n", [[name description] UTF8String] ?: "",
                          [[set description] UTF8String] ?: "", [[family description] UTF8String] ?: "");
    return r;
}
/* Logic Pro (MACore): Farbtexte der Assets -> Farbe; durchsichtige Ergebnisse mit dem Text melden (je Text einmal) */
static IMP o_colstr[4];
static id h_colstr_n(int i, id s, SEL c, id str) {
    id r = ((id (*)(id, SEL, id))o_colstr[i])(s, c, str);
    CGFloat a = -1;
    if ([r isKindOfClass: [NSColor class]]) a = ((CGFloat (*)(id, SEL)) objc_msgSend)(r, @selector(alphaComponent));
    else if (r && strcmp(class_getName(object_getClass(r)), "O2Color") == 0) a = CGColorGetAlpha((CGColorRef) r);
    if (a == 0) {
        static NSMutableSet *seen;
        if (!seen) seen = [NSMutableSet new];
        if (![seen containsObject: str ?: @""]) {
            [seen addObject: str ?: @""];
            fprintf(stderr, "[clokeos-trace] %s createFromString \"%s\" -> durchsichtig\n", class_getName(object_getClass(s)), [[str description] UTF8String] ?: "");
        }
    }
    return r;
}
static id h_colstr0(id s, SEL c, id t) { return h_colstr_n(0, s, c, t); }
static id h_colstr1(id s, SEL c, id t) { return h_colstr_n(1, s, c, t); }
static id h_colstr2(id s, SEL c, id t) { return h_colstr_n(2, s, c, t); }
static id h_colstr3(id s, SEL c, id t) { return h_colstr_n(3, s, c, t); }
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
/* Öffnen eines Dokuments: Typ, Klasse, Lesen (Logic Pro: CLgSongDocument) */
static id h_typeurl(id s, SEL c, NSURL *u, NSError **e) {
    id r = ((id (*)(id, SEL, id, NSError **))o_typeurl)(s, c, u, e);
    fprintf(stderr, "[clokeos-trace] typeForContentsOfURL %s -> %s\n", [[u path] UTF8String] ?: "", [[r description] UTF8String] ?: "nil");
    return r;
}
static Class h_docclass(id s, SEL c, NSString *t) {
    Class r = ((Class (*)(id, SEL, id))o_docclass)(s, c, t);
    fprintf(stderr, "[clokeos-trace] documentClassForType %s -> %s\n", [t UTF8String] ?: "nil", r ? class_getName(r) : "nil");
    return r;
}
static BOOL h_readurl(id s, SEL c, NSURL *u, NSString *t, NSError **e) {
    BOOL r = ((BOOL (*)(id, SEL, id, id, NSError **))o_readurl)(s, c, u, t, e);
    fprintf(stderr, "[clokeos-trace] %s readFromURL %s type %s -> %d %s\n", class_getName(object_getClass(s)), [[u path] UTF8String] ?: "",
            [t UTF8String] ?: "nil", (int) r, (!r && e && *e) ? [[*e description] UTF8String] : "");
    return r;
}
static BOOL h_readlx(id s, SEL c, NSURL *u, NSError **e) {
    BOOL r = ((BOOL (*)(id, SEL, id, NSError **))o_readlx)(s, c, u, e);
    fprintf(stderr, "[clokeos-trace] _readLogicXProjectFromURL %s -> %d %s\n", [[u path] UTF8String] ?: "", (int) r,
            (!r && e && *e) ? [[*e description] UTF8String] : "");
    return r;
}
static id h_docinit(id s, SEL c) {
    fprintf(stderr, "[clokeos-trace] CLgSongDocument init ->\n");
    id r = ((id (*)(id, SEL))o_docinit)(s, c);
    fprintf(stderr, "[clokeos-trace] CLgSongDocument init <- %p\n", r);
    return r;
}
static id h_docinitalt(id s, SEL c, NSURL *u, NSString *t, NSInteger alt, NSError **e) {
    fprintf(stderr, "[clokeos-trace] CLgSongDocument initWithContentsOfURL:%s alt:%ld ->\n", [[u path] UTF8String] ?: "", (long) alt);
    id r = ((id (*)(id, SEL, id, id, NSInteger, NSError **))o_docinitalt)(s, c, u, t, alt, e);
    fprintf(stderr, "[clokeos-trace] CLgSongDocument initWithContentsOfURL <- %p %s\n", r, (!r && e && *e) ? [[*e description] UTF8String] : "");
    return r;
}
+ (void)hooks {
    { Method m = class_getInstanceMethod(NSClassFromString(@"CLgSongDocument"), @selector(init)); if (m) o_docinit = method_setImplementation(m, (IMP)h_docinit); }
    { Method m = class_getInstanceMethod(NSClassFromString(@"CLgSongDocument"), sel_registerName("initWithContentsOfURL:ofType:withAlternativeIndex:error:")); if (m) o_docinitalt = method_setImplementation(m, (IMP)h_docinitalt); }
    { Method m = class_getInstanceMethod(NSClassFromString(@"NSDocumentController"), @selector(typeForContentsOfURL:error:)); if (m) o_typeurl = method_setImplementation(m, (IMP)h_typeurl); }
    { Method m = class_getInstanceMethod(NSClassFromString(@"NSDocumentController"), @selector(documentClassForType:)); if (m) o_docclass = method_setImplementation(m, (IMP)h_docclass); }
    { Method m = class_getInstanceMethod(NSClassFromString(@"CLgSongDocument"), @selector(readFromURL:ofType:error:)); if (m) o_readurl = method_setImplementation(m, (IMP)h_readurl); }
    { Method m = class_getInstanceMethod(NSClassFromString(@"CLgSongDocument"), sel_registerName("_readLogicXProjectFromURL:error:")); if (m) o_readlx = method_setImplementation(m, (IMP)h_readlx); }
    { Method m = class_getClassMethod(NSClassFromString(@"NSImage"), @selector(imageNamed:)); if (m) o_imgnamed = method_setImplementation(m, (IMP)h_imgnamed); }
    { Method m = class_getInstanceMethod([NSBundle class], @selector(imageForResource:)); if (m) o_bundleimg = method_setImplementation(m, (IMP)h_bundleimg); }
    { Method m = class_getInstanceMethod(NSClassFromString(@"NSImage"), @selector(initWithContentsOfFile:)); if (m) o_imgfile = method_setImplementation(m, (IMP)h_imgfile); }
    { Method m = class_getClassMethod([NSFont class], @selector(fontWithName:size:)); if (m) o_nsfont = method_setImplementation(m, (IMP)h_nsfont); }
    { Class mc = NSClassFromString(@"MAAssetManager"); Method m = class_getClassMethod(mc, @selector(fontWithName:size:));
      if (!m) m = class_getInstanceMethod(mc, @selector(fontWithName:size:)); if (m) o_mafont = method_setImplementation(m, (IMP)h_mafont); }
    { Method m = class_getClassMethod(NSClassFromString(@"MAKitColor"), @selector(colorFromSectionUncached:withName:)); if (m) o_colsec = method_setImplementation(m, (IMP)h_colsec); }
    { Method m = class_getInstanceMethod(NSClassFromString(@"MAAssetManager"), @selector(assetWithName:fromAssetSet:fromFamily:)); if (m) o_asset = method_setImplementation(m, (IMP)h_asset); }
    {
        const char *cls[4] = { "MAAssetTypeSRGBColorCreator", "MAAssetTypeNSColorCreator", "MAAssetTypeGrayColorCreator", "MAAssetTypeSRGBFloatColorCreator" };
        IMP hs[4] = { (IMP)h_colstr0, (IMP)h_colstr1, (IMP)h_colstr2, (IMP)h_colstr3 };
        for (int i = 0; i < 4; i++) {
            Class k = objc_getClass(cls[i]);
            /* nur die eigene Methode der Klasse, nicht die geerbte */
            unsigned n = 0; Method *ms = k ? class_copyMethodList(k, &n) : NULL;
            for (unsigned j = 0; j < n; j++)
                if (method_getName(ms[j]) == @selector(createFromString:)) o_colstr[i] = method_setImplementation(ms[j], hs[i]);
            free(ms);
        }
    }
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
