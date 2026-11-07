/* Zeigt, wann Logic Pros Startkette läuft (DYLD_INSERT_LIBRARIES=/usr/lib/libclogictrace.dylib):
 *   Der Starter ruft SetSetupAppFactoryCB(cb) (Logic.framework) und dann NSApplicationMain; cb führt zu setupAppFactorySharedInstance
 *   (LogicAppFramework, Swift-AppFactory). Beide werden per __interpose gemeldet, dazu jeder Aufruf von -[NSApplication setDelegate:].
 * Nur für Logic: ohne die Symbole lässt sich die Bibliothek in anderen Programmen nicht laden. */
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>
#include <mach-o/dyld.h>
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
extern u64 _Z20ScanCoreAudioDriversv(void) __asm__("__Z20ScanCoreAudioDriversv");
/* Nach dem Scan: Logics Gerätetabellen in MAAudioEngine (Logic Pro 12.3, arm64; Versätze aus der Disassemblierung von
 * FindCoreAudioDriver): Eingänge 0xf7af28/Anzahl 0xf7af48, Ausgänge 0xf7af30/0xf7af4c, Einträge 0x50 Bytes, Name bei +0, Geräte-ID +0x20/+0x24 */
static u64 my__Z20ScanCoreAudioDriversv(void) {
    u64 r = _Z20ScanCoreAudioDriversv();
    Dl_info di;
    if (dladdr((void *)_Z20ScanCoreAudioDriversv, &di) && di.dli_fbase) {
        char *b = (char *)di.dli_fbase;
        fprintf(stderr, "[clogic] ScanCoreAudioDrivers -> %#llx, Modus %d, Eingänge %d, Ausgänge %d\n", r, *(int *)(b + 0xf7a584), *(int *)(b + 0xf7af48), *(int *)(b + 0xf7af4c));
        for (int dir = 0; dir < 2; dir++) {
            char *t = *(char **)(b + (dir ? 0xf7af30 : 0xf7af28)); int n = *(int *)(b + (dir ? 0xf7af4c : 0xf7af48));
            for (int i = 0; t && i < n && i < 8; i++)
                fprintf(stderr, "[clogic]   %s %d: '%.40s' ids %u/%u\n", dir ? "aus" : "ein", i, t + 0x50 * i, *(unsigned *)(t + 0x50 * i + 0x20), *(unsigned *)(t + 0x50 * i + 0x24));
        }
    }
    return r;
}
extern u64 _Z19FindCoreAudioDriverPKc22MDAudioDriverDirection(const char *, u64) __asm__("__Z19FindCoreAudioDriverPKc22MDAudioDriverDirection");
static u64 my__Z19FindCoreAudioDriverPKc22MDAudioDriverDirection(const char *name, u64 dir) {
    u64 r = _Z19FindCoreAudioDriverPKc22MDAudioDriverDirection(name, dir);
    fprintf(stderr, "[clogic] FindCoreAudioDriver('%s', %llu) -> %#llx\n", name ? name : "(null)", dir, r); return r;
}
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
/* Vtable von MDCA (MAAudioEngine, Logic 12.3 arm64: __ZTV4MDCA+0x10 = 0xe02070): Prime (+0x88), PrepareForStart (+0xa0) und Start (+0xa8)
 * mit Aufrufkette melden. Die Einträge liegen in __DATA_CONST; die Seite wird dafür beschreibbar gemacht. */
static u64 (*o_prime)(void *), (*o_prep)(void *, u64, u64), (*o_start)(void *, u64);
static void vt_bt(const char *what, void *self) {
    fprintf(stderr, "[clogic] MDCA::%s(%p)\n", what, self);
    void **fp = (void **)__builtin_frame_address(0);
    for (int i = 0; i < 14 && fp && !((unsigned long)fp & 7); i++) {
        void *ra = fp[1]; Dl_info di;
        if (!ra) break;
        if (dladdr(ra, &di) && di.dli_fname) {
            const char *b = strrchr(di.dli_fname, '/');
            fprintf(stderr, "[clogic]   %s +%#lx %s\n", b ? b + 1 : di.dli_fname, (unsigned long)((char *)ra - (char *)di.dli_fbase), di.dli_sname ? di.dli_sname : "");
        }
        void **next = (void **)fp[0];
        if (next <= fp) break;
        fp = next;
    }
}
static u64 my_prime(void *self) { vt_bt("Prime", self); u64 r = o_prime(self); fprintf(stderr, "[clogic] MDCA::Prime -> %#llx\n", r); return r; }
static u64 my_prep(void *self, u64 a, u64 b) { vt_bt("PrepareForStart", self); u64 r = o_prep(self, a, b); fprintf(stderr, "[clogic] MDCA::PrepareForStart(%llu, %llu) -> %#llx\n", a, b, r); return r; }
static u64 my_start(void *self, u64 a) { vt_bt("Start", self); u64 r = o_start(self, a); fprintf(stderr, "[clogic] MDCA::Start(%llu) -> %#llx\n", a, r); return r; }
static void hook_mdca_vtable(void) {
    const char *base = NULL;   /* Bildanfang aus der dyld-Liste (dlsym traf ein anderes Bild) */
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (n && strstr(n, "/MAAudioEngine.framework/")) { base = (const char *)_dyld_get_image_header(i); break; }
    }
    if (!base) { fprintf(stderr, "[clogic] MAAudioEngine nicht gefunden\n"); return; }
    void **vt = (void **)(base + 0xe02070);
    Dl_info chk;   /* Probe: Eintrag +0xa8 muss in MAAudioEngine auf MDCA::Start zeigen */
    if (!dladdr(vt[0xa8 / 8], &chk) || (const char *)chk.dli_fbase != base || (char *)vt[0xa8 / 8] - base != 0x71f210) {
        fprintf(stderr, "[clogic] MDCA-Vtable passt nicht (%p), nicht gehakt\n", vt[0xa8 / 8]); return; }
    long pg = (long)vt & ~0x3fffL;
    if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE)) { perror("[clogic] mprotect"); return; }
    o_prime = vt[0x88 / 8]; vt[0x88 / 8] = (void *)my_prime;
    o_prep = vt[0xa0 / 8]; vt[0xa0 / 8] = (void *)my_prep;
    o_start = vt[0xa8 / 8]; vt[0xa8 / 8] = (void *)my_start;
    mprotect((void *)pg, 0x4000, PROT_READ);
    fprintf(stderr, "[clogic] MDCA-Vtable gehakt (%p)\n", (void *)vt);
}

__attribute__((constructor)) static void init(void) {
    hook_mdca_vtable();
    fprintf(stderr, "[clogic] trace loaded\n");
    Class c = objc_getClass("NSApplication");
    Method m = c ? class_getInstanceMethod(c, sel_registerName("setDelegate:")) : NULL;
    if (m) { orig_setDelegate = method_getImplementation(m); method_setImplementation(m, (IMP)my_setDelegate); }
}
