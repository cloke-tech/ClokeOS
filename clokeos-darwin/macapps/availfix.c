/* libclokeos-availfix: Mac-Programme und die Swift-Laufzeit fragen mit _availability_version_check ab, ob das System neu genug ist
 * (#available, __builtin_available). PureDarwin meldet macOS 11; Programme wie Logic Pro erwarten 26 und brechen sonst ab
 * ("this should never happen" in _Concurrency). Der Interposer lässt jede Abfrage bestehen.
 *   DYLD_INSERT_LIBRARIES=/usr/lib/libclokeos-availfix.dylib <programm>   (macapps/crashpc.sh baut sie mit) */
#include <stdint.h>
#include <objc/runtime.h>
extern int32_t _availability_version_check(uint32_t count, const void *versions);
static int32_t always_yes(uint32_t count, const void *versions) { (void)count; (void)versions; return 1; }

/* PureDarwins os_log_create liefert eine einfache Struktur ohne isa; Programme halten sie in ObjC-Eigenschaften (NSObject<OS_os_log>) und
 * ARC ruft objc_release darauf (Logic Pro: Absturz in -[MAUCContentPackageImporter .cxx_destruct]). Hier bekommt sie die Klasse OS_os_log
 * (liegt in fo.dylib, swift/overlay/compat.m; NSObject-Unterklasse). */
extern void *os_log_create(const char *subsystem, const char *category);
static void *fixed_os_log_create(const char *subsystem, const char *category) {
  void *o = os_log_create(subsystem, category);
  if (o && *(void **)o == 0) {
    Class k = objc_getClass("OS_os_log");
    if (k) *(Class *)o = k;
  }
  return o;
}
__attribute__((used)) static const struct { const void *replacement, *original; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = { { (const void *)always_yes, (const void *)_availability_version_check },
    { (const void *)fixed_os_log_create, (const void *)os_log_create } };
