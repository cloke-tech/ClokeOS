/* Zeigt, wann Logic Pros Startkette läuft (DYLD_INSERT_LIBRARIES=/usr/lib/libclogictrace.dylib):
 *   Der Starter ruft SetSetupAppFactoryCB(cb) (Logic.framework) und dann NSApplicationMain; cb führt zu setupAppFactorySharedInstance
 *   (LogicAppFramework, Swift-AppFactory). Beide werden per __interpose gemeldet, dazu jeder Aufruf von -[NSApplication setDelegate:].
 * Nur für Logic: ohne die Symbole lässt sich die Bibliothek in anderen Programmen nicht laden. */
#include <stdio.h>
#include <objc/runtime.h>
#include <objc/message.h>

extern void SetSetupAppFactoryCB(void *cb);
extern void setupAppFactorySharedInstance(void);

static void my_set(void *cb) { fprintf(stderr, "[clogic] SetSetupAppFactoryCB(%p)\n", cb); SetSetupAppFactoryCB(cb); }
static void my_setup(void) { fprintf(stderr, "[clogic] setupAppFactorySharedInstance() begin\n"); setupAppFactorySharedInstance(); fprintf(stderr, "[clogic] setupAppFactorySharedInstance() end\n"); }

__attribute__((used, section("__DATA,__interpose"))) static struct { void *n, *o; } interposers[] = {
    { (void *)my_set, (void *)SetSetupAppFactoryCB },
    { (void *)my_setup, (void *)setupAppFactorySharedInstance },
};

static IMP orig_setDelegate;
static void my_setDelegate(id self, SEL _cmd, id d) {
    fprintf(stderr, "[clogic] -[%s setDelegate:] %s\n", class_getName(object_getClass(self)), d ? class_getName(object_getClass(d)) : "nil");
    ((void (*)(id, SEL, id))orig_setDelegate)(self, _cmd, d);
}
__attribute__((constructor)) static void init(void) {
    fprintf(stderr, "[clogic] trace loaded\n");
    Class c = objc_getClass("NSApplication");
    Method m = c ? class_getInstanceMethod(c, sel_registerName("setDelegate:")) : NULL;
    if (m) { orig_setDelegate = method_getImplementation(m); method_setImplementation(m, (IMP)my_setDelegate); }
}
