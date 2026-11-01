/* libclokeos_carbon: die wenigen Carbon-Funktionen aus CoreServices, die Logic Pros OMF.framework
 * bindet und PureDarwins CoreServices-Attrappe nicht hat. macapps/carbon.sh trägt die Bibliothek
 * als LC_REEXPORT_DYLIB in CoreServices und in das Carbon-Framework (eine leere Hülle) ein.
 *
 * Nur das Nötige, mit dem Verhalten der Mac-Fassung, soweit es aus der Beschreibung folgt:
 *   NewPtr / DisposePtr           malloc / free (NewPtr(0) liefert einen gültigen Zeiger)
 *   TickCount                     60-Hz-Ticks seit dem Start
 *   UCConvertCFAbsoluteTimeToSeconds
 *                                 CFAbsoluteTime (Sekunden seit 2001) -> Sekunden seit 1904
 *   PBGetVolumeInfoSync           meldet nsvErr (-35): es gibt keine Volume-Verwaltung des Classic-Mac-OS */
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdlib.h>

void *NewPtr(long size) { return malloc(size > 0 ? (size_t)size : 1); }
void DisposePtr(void *p) { free(p); }

uint32_t TickCount(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  uint64_t ns = mach_absolute_time() * tb.numer / tb.denom;
  return (uint32_t)(ns / (1000000000ull / 60));
}

int32_t UCConvertCFAbsoluteTimeToSeconds(double abs, uint32_t *secs) {
  double s = abs + 3061152000.0;   /* kCFAbsoluteTimeIntervalSince1904 */
  if (s < 0 || s > 4294967295.0) return -50;   /* paramErr */
  *secs = (uint32_t)s;
  return 0;
}

int16_t PBGetVolumeInfoSync(void *pb) { (void)pb; return -35; }

/* Text Input Services (HIToolbox): Logic Pro fragt beim Start die aktuelle Tastaturbelegung ab und gibt das Ergebnis ohne Prüfung mit CFRelease frei
 * (bei Apple nie NULL). Hier: ein leeres CFDictionary als Eingabequelle (CoreFoundation wird erst zur Laufzeit gesucht, die Bibliothek bindet sie nicht),
 * Eigenschaften gibt es keine (NULL; Logic hat dafür einen Ersatzwert). */
#include <dlfcn.h>
static void *ck_empty_dict(void) {
  typedef void *(*Create)(void *alloc, const void **keys, const void **vals, long n, const void *kcb, const void *vcb);
  Create f = (Create)dlsym(RTLD_DEFAULT, "CFDictionaryCreate");
  return f ? f(0, 0, 0, 0, 0, 0) : 0;
}
void *TISCopyCurrentKeyboardInputSource(void) { return ck_empty_dict(); }
void *TISCopyCurrentKeyboardLayoutInputSource(void) { return ck_empty_dict(); }
void *TISCopyCurrentASCIICapableKeyboardInputSource(void) { return ck_empty_dict(); }
void *TISGetInputSourceProperty(void *source, void *key) { (void)source; (void)key; return 0; }
