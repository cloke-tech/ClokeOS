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

/* Audio-Treiber von MAAudioEngine (Aufrufe aus Logic.framework): Argumente in x0..x7 weiterreichen, Ergebnis melden.
 * Gemeldet wird erst nach dem Aufruf, damit fprintf keine Gleitkomma-Argumente in v0..v7 überschreibt. */
typedef unsigned long long u64;
#define AE_TRACE(name, sym) \
    extern u64 sym(u64, u64, u64, u64, u64, u64, u64, u64) __asm__("_" #sym); \
    static u64 my_##sym(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) { \
        u64 r = sym(a, b, c, d, e, f, g, h); \
        fprintf(stderr, "[clogic] %s(%#llx, %#llx) -> %#llx\n", name, a, b, r); return r; }
AE_TRACE("MDCoreAudioNewDriver", MDCoreAudioNewDriver)
AE_TRACE("ScanCoreAudioDrivers", _Z20ScanCoreAudioDriversv)
AE_TRACE("FindCoreAudioDriver", _Z19FindCoreAudioDriverPKc22MDAudioDriverDirection)
AE_TRACE("DriverDeviceID", _Z14DriverDeviceIDi22MDAudioDriverDirection)
AE_TRACE("MDInitDDS", _Z9MDInitDDSv)
AE_TRACE("MSuperclass::SetLaunchingAudioDevice", _ZN11MSuperclass23SetLaunchingAudioDeviceEb)

__attribute__((used, section("__DATA,__interpose"))) static struct { void *n, *o; } interposers[] = {
    { (void *)my_set, (void *)SetSetupAppFactoryCB },
    { (void *)my_setup, (void *)setupAppFactorySharedInstance },
    { (void *)my_MDCoreAudioNewDriver, (void *)MDCoreAudioNewDriver },
    { (void *)my__Z20ScanCoreAudioDriversv, (void *)_Z20ScanCoreAudioDriversv },
    { (void *)my__Z19FindCoreAudioDriverPKc22MDAudioDriverDirection, (void *)_Z19FindCoreAudioDriverPKc22MDAudioDriverDirection },
    { (void *)my__Z14DriverDeviceIDi22MDAudioDriverDirection, (void *)_Z14DriverDeviceIDi22MDAudioDriverDirection },
    { (void *)my__Z9MDInitDDSv, (void *)_Z9MDInitDDSv },
    { (void *)my__ZN11MSuperclass23SetLaunchingAudioDeviceEb, (void *)_ZN11MSuperclass23SetLaunchingAudioDeviceEb },
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
