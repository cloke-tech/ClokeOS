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
