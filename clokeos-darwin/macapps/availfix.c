/* libclokeos-availfix: Mac-Programme und die Swift-Laufzeit fragen mit _availability_version_check ab, ob das System neu genug ist
 * (#available, __builtin_available). PureDarwin meldet macOS 11; Programme wie Logic Pro erwarten 26 und brechen sonst ab
 * ("this should never happen" in _Concurrency). Der Interposer lässt jede Abfrage bestehen.
 *   DYLD_INSERT_LIBRARIES=/usr/lib/libclokeos-availfix.dylib <programm>   (macapps/crashpc.sh baut sie mit) */
#include <stdint.h>
extern int32_t _availability_version_check(uint32_t count, const void *versions);
static int32_t always_yes(uint32_t count, const void *versions) { (void)count; (void)versions; return 1; }
__attribute__((used)) static const struct { const void *replacement, *original; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = { { (const void *)always_yes, (const void *)_availability_version_check } };
