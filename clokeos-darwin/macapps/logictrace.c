/* Zeigt, wann Logic Pros Startkette läuft (DYLD_INSERT_LIBRARIES=/usr/lib/libclogictrace.dylib):
 *   Der Starter ruft SetSetupAppFactoryCB(cb) (Logic.framework) und dann NSApplicationMain; cb führt zu setupAppFactorySharedInstance
 *   (LogicAppFramework, Swift-AppFactory). Beide werden per __interpose gemeldet, dazu jeder Aufruf von -[NSApplication setDelegate:].
 * Nur für Logic: ohne die Symbole lässt sich die Bibliothek in anderen Programmen nicht laden. */
#include <dirent.h>
#include <execinfo.h>
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <libkern/OSCacheControl.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
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

/* CLOKE_LOGIC_PATHS=<Teilstring>: Dateizugriffe (open/stat/lstat/access/getattrlist/opendir/fopen), deren Pfad den Teilstring enthält, mit Ergebnis melden
 * (z. B. ".caf", um zu sehen, wo Logic Samples sucht) */
static const char *path_watch(void) { static const char *w = (const char *)1; if (w == (const char *)1) w = getenv("CLOKE_LOGIC_PATHS"); return w; }
static void path_log(const char *fn, const char *p, int r) { const char *w = path_watch(); if (w && p && strstr(p, w)) fprintf(stderr, "[clogic] %s(%s) -> %d\n", fn, p, r); }
static int my_open(const char *p, int fl, ...) { va_list ap; va_start(ap, fl); int mode = va_arg(ap, int); va_end(ap); int r = open(p, fl, mode); path_log("open", p, r); return r; }
static int my_stat(const char *p, struct stat *st) { int r = stat(p, st); path_log("stat", p, r); return r; }
static int my_lstat(const char *p, struct stat *st) { int r = lstat(p, st); path_log("lstat", p, r); return r; }
static int my_access(const char *p, int m) { int r = access(p, m); path_log("access", p, r); return r; }
extern int getattrlist(const char *, void *, void *, size_t, unsigned int);
static int my_getattrlist(const char *p, void *a, void *b, size_t n, unsigned int o) { int r = getattrlist(p, a, b, n, o); path_log("getattrlist", p, r); return r; }
/* Pfade mit '%' (URL-kodiert) zusätzlich mit Aufrufkette melden: so findet man die Stelle, die eine URL nicht dekodiert */
static void pct_bt(const char *p) {
    if (!path_watch() || !p || !strchr(p, '%')) return;
    void *f[16]; int n = backtrace(f, 16);
    for (int i = 1; i < n; i++) { Dl_info di; if (dladdr(f[i], &di) && di.dli_fname) fprintf(stderr, "[clogic]   %%-bt %s +0x%lx %s\n", strrchr(di.dli_fname, '/') + 1, (unsigned long)((char *)f[i] - (char *)di.dli_fbase), di.dli_sname ? di.dli_sname : ""); }
}
/* CLOKE_LOGIC_STRBT=<Teilstring>: Aufrufkette, wenn Logic einen lokalisierten Text mit diesem Teilstring holt
 * (z. B. "too slow" für die Meldung "Disk is too slow. (Bounce)") */
#include <CoreFoundation/CoreFoundation.h>
static CFStringRef my_CFBundleCopyLocalizedString(CFBundleRef b, CFStringRef key, CFStringRef value, CFStringRef table) {
    static const char *w = (const char *)1; if (w == (const char *)1) w = getenv("CLOKE_LOGIC_STRBT");
    char k[256];
    if (w && key && CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8) && strstr(k, w)) {
        fprintf(stderr, "[clogic] Text \"%s\"\n", k);
        void *f[24]; int n = backtrace(f, 24);
        for (int i = 1; i < n; i++) { Dl_info di; if (dladdr(f[i], &di) && di.dli_fname) fprintf(stderr, "[clogic]   str-bt %s +0x%lx %s\n", strrchr(di.dli_fname, '/') + 1, (unsigned long)((char *)f[i] - (char *)di.dli_fbase), di.dli_sname ? di.dli_sname : ""); }
    }
    return CFBundleCopyLocalizedString(b, key, value, table);
}
static DIR *my_opendir(const char *p) { DIR *d = opendir(p); path_log("opendir", p, d ? 0 : -1); if (!d) pct_bt(p); return d; }
static FILE *my_fopen(const char *p, const char *m) { FILE *f = fopen(p, m); path_log("fopen", p, f ? 0 : -1); return f; }

__attribute__((used, section("__DATA,__interpose"))) static struct { void *n, *o; } interposers[] = {
    { (void *)my_open, (void *)open }, { (void *)my_stat, (void *)stat }, { (void *)my_lstat, (void *)lstat },
    { (void *)my_access, (void *)access }, { (void *)my_getattrlist, (void *)getattrlist },
    { (void *)my_opendir, (void *)opendir }, { (void *)my_CFBundleCopyLocalizedString, (void *)CFBundleCopyLocalizedString }, { (void *)my_fopen, (void *)fopen },
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
static const char *mae_base(void) {   /* Bildanfang von MAAudioEngine aus der dyld-Liste (dlsym traf ein anderes Bild) */
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i);
        if (n && strstr(n, "/MAAudioEngine.framework/")) return (const char *)_dyld_get_image_header(i);
    }
    return NULL;
}
/* Export-Absturz (File > Export > All Tracks): MD::NewFile ruft MDFile::CheckDisposeFiles, das alle Dateien freigibt, auf die gerade kein
 * Stream zeigt – auch die der eben fertigen Spur, deren MDFile Logic danach noch an FileRecordDisable gibt (use-after-free). Auf dem Mac
 * verhindert das meist, dass noch asynchrone Schreibaufträge offen sind (dann kehrt CheckDisposeFiles sofort zurück); hier ist der
 * Datei-Faden schneller. Der Aufruf in NewFile wird durch nop ersetzt; Prime und Exit räumen weiter auf. Logic 12.3 arm64,
 * MAAudioEngine +0x6ee2f4 = bl 0x711058. Aus mit CLOKE_LOGIC_KEEPFILES=0. */
static void patch_newfile_dispose(void) {
    const char *v = getenv("CLOKE_LOGIC_KEEPFILES");
    if (v && *v == '0') return;
    const char *base = mae_base();
    if (!base) return;
    /* Ganze Funktion MDFile::CheckDisposeFiles (+0x711058) kehrt sofort zurück: sie wird aus NewFile, MD::Prime und drei weiteren
     * Stellen gerufen (keine nutzt einen Rückgabewert) und gibt dort Dateien frei, die Logic noch benutzt (doppeltes free beim
     * zweiten Export einer Sitzung). Prüfung: erster Befehl "sub sp, sp, #0x80". */
    uint32_t *ins = (uint32_t *)(base + 0x711058);
    if (*ins != 0xd10203ffu) { fprintf(stderr, "[clogic] CheckDisposeFiles: unerwarteter Befehl %#x, nicht gepatcht\n", *ins); return; }
    long pg = (long)ins & ~0x3fffL;
    if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE)) { perror("[clogic] mprotect CheckDisposeFiles"); return; }
    *ins = 0xd65f03c0;   /* ret */
    mprotect((void *)pg, 0x4000, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(ins, 4);
    fprintf(stderr, "[clogic] MDFile::CheckDisposeFiles abgeschaltet\n");
}
static void hook_mdca_vtable(void) {
    if (!getenv("CLOKE_LOGIC_MDCA_TRACE")) return;
    const char *base = mae_base();
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
    patch_newfile_dispose();
    fprintf(stderr, "[clogic] trace loaded\n");
    Class c = objc_getClass("NSApplication");
    Method m = c ? class_getInstanceMethod(c, sel_registerName("setDelegate:")) : NULL;
    if (m) { orig_setDelegate = method_getImplementation(m); method_setImplementation(m, (IMP)my_setDelegate); }
}
