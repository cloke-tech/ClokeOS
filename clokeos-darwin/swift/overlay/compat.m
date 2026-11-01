/* Was FoundationEssentials (mit dem SDK 27 übersetzt) aus libSystem verlangt und PureDarwins libSystem nicht hat (ClokeOS), plus der Klasse
 * OS_os_log für das os-Overlay. Alles hier lebt in fo.dylib (swift/foundation-overlay.sh) und hat Vorrang vor der Suche in den anderen Bildern.
 *   malloc_type_*  getypte Fassungen von malloc/calloc/realloc (macOS 15): der Typ-Hinweis wird ignoriert
 *   fts_*, removefile*, chflags, quotactl   FileManager-Teile (Verzeichnisse durchlaufen, Löschen, Flags): melden ENOSYS; wer sie braucht, bekommt einen Fehler
 *   uuid_parse     richtig umgesetzt
 *   OS_os_log, os_log_create   Platzhalter, damit Logger angelegt werden kann (die Ausgabe selbst macht _os_log_impl aus libSystem) */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

void *malloc_type_malloc(size_t size, uint64_t type_id) { (void)type_id; return malloc(size); }
void *malloc_type_calloc(size_t count, size_t size, uint64_t type_id) { (void)type_id; return calloc(count, size); }
void *malloc_type_realloc(void *ptr, size_t size, uint64_t type_id) { (void)type_id; return realloc(ptr, size); }

#define ENOSYS_INT(name, ...) int name(__VA_ARGS__) { errno = ENOSYS; return -1; }
ENOSYS_INT(chflags, const char *path, unsigned flags)
ENOSYS_INT(quotactl, const char *path, int cmd, int id, void *addr)
void *fts_open(char *const *argv, int options, int (*compar)(const void **, const void **)) { errno = ENOSYS; return NULL; }
void *fts_read(void *ftsp) { errno = ENOSYS; return NULL; }
int fts_set(void *ftsp, void *f, int options) { errno = ENOSYS; return -1; }
int fts_close(void *ftsp) { errno = ENOSYS; return -1; }
void *removefile_state_alloc(void) { return calloc(1, 64); }
int removefile_state_free(void *s) { free(s); return 0; }
ENOSYS_INT(removefile_state_get, void *s, uint32_t k, void *v)
ENOSYS_INT(removefile_state_set, void *s, uint32_t k, const void *v)
ENOSYS_INT(removefile, const char *path, void *s, uint32_t flags)

int uuid_parse(const char *in, unsigned char out[16]) {
  if (strlen(in) != 36) return -1;
  int n = 0;
  for (int i = 0; i < 36;) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (in[i] != '-') return -1; i++; continue; }
    int v = 0;
    for (int k = 0; k < 2; k++, i++) {
      char c = in[i]; int d;
      if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else return -1;
      v = v * 16 + d;
    }
    out[n++] = (unsigned char)v;
  }
  return 0;
}

const char *const kOSThermalNotificationPressureLevelName = "com.apple.system.thermalpressurelevel";

__attribute__((objc_root_class)) @interface OS_os_log { void *isa; } @end
@implementation OS_os_log @end
void *os_log_create(const char *subsystem, const char *category) { return [OS_os_log alloc]; }

// Brücke für overlay/bridge.swift: NSString aus UTF-8 (Klasse zur Laufzeit gesucht, damit hier kein Foundation-Header nötig ist)
#include <objc/message.h>
#include <objc/runtime.h>
id clk_nsstring_utf8(const char *bytes, long length) {
  Class c = objc_getClass("NSString");
  id s = ((id (*)(id, SEL))objc_msgSend)((id)c, sel_registerName("alloc"));
  return ((id (*)(id, SEL, const void *, unsigned long, unsigned long))objc_msgSend)(s, sel_registerName("initWithBytes:length:encoding:"), bytes ? bytes : "", (unsigned long)length, 4UL);
}
