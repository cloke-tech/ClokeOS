/* libclokeos_compat, Teil 8: Versionsabfragen, mit denen Programme zur Laufzeit Funktionen wählen
 * (@available in Swift/ObjC, __builtin_available): _availability_version_check und
 * __isPlatformVersionAtLeast. Die von PureDarwins libdyld antworten auf jede Frage mit 0 ("nicht
 * verfügbar", gemessen), auch auf macOS 11.7; build.sh blendet sie in libSystem.B und libdyld aus
 * (hidesym.py), diese hier ersetzen sie.
 *
 * Gemeldete macOS-Version ("Host-Version"), das Größte aus:
 *   1. ProductVersion in /System/Library/CoreServices/SystemVersion.plist (ClokeOS: 11.7, die Version,
 *      die Darwin 20.5 entspricht; Homebrew und sw_vers lesen dieselbe Datei),
 *   2. dem höchsten macOS-Mindestsystem (LC_BUILD_VERSION minos) aller geladenen Images: Code, der
 *      erst ab macOS 15.6 läuft, setzt voraus, dass das System sich so meldet; so bekommt Logic Pro
 *      (minos 15.6) 15.6 und ein Big-Sur-Programm weiterhin 11.7,
 *   3. der Umgebungsvariable CLOKEOS_MACOS_VERSION ("15.6" oder "15.6.1"), die 1 und 2 ersetzt.
 * Mac Catalyst (Plattform 6) wird mit iOS 18.6 = macOS 15.6 verglichen, andere Plattformen werden
 * wie bei Apple ignoriert (Antwort "ja"). */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { uint32_t platform; uint32_t version; } build_version;

#define PLATFORM_MACOS_ 1
#define PLATFORM_MACCATALYST_ 6
#define V(a, b, c) (((uint32_t)(a) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(c))

static uint32_t parse_version(const char *s) {
  unsigned p[3] = {0, 0, 0};
  int i = 0;
  while (*s && i < 3) {
    if (*s >= '0' && *s <= '9') p[i] = p[i] * 10 + (unsigned)(*s - '0');
    else if (*s == '.') i++;
    else break;
    s++;
  }
  return V(p[0], p[1], p[2]);
}

static uint32_t plist_version(void) {
  static uint32_t cached;
  if (cached) return cached;
  uint32_t v = V(11, 4, 0);   /* Darwin 20.5 */
  int fd = open("/System/Library/CoreServices/SystemVersion.plist", O_RDONLY);
  if (fd >= 0) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n > 0) {
      buf[n] = 0;
      char *k = strstr(buf, "<key>ProductVersion</key>");
      char *s = k ? strstr(k, "<string>") : NULL;
      if (s) v = parse_version(s + 8);
    }
  }
  cached = v;
  return v;
}

static uint32_t images_min_os(void) {
  static uint32_t count, best;
  uint32_t n = _dyld_image_count();
  if (n == count) return best;
  uint32_t m = 0;
  for (uint32_t i = 0; i < n; i++) {
    const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(i);
    if (!h || h->magic != MH_MAGIC_64) continue;
    const char *p = (const char *)(h + 1);
    for (uint32_t c = 0; c < h->ncmds; c++) {
      const struct load_command *lc = (const struct load_command *)p;
      if (lc->cmd == LC_BUILD_VERSION) {
        const struct build_version_command *b = (const void *)lc;
        if (b->platform == PLATFORM_MACOS_ && b->minos > m) m = b->minos;
      } else if (lc->cmd == LC_VERSION_MIN_MACOSX) {
        const struct version_min_command *b = (const void *)lc;
        if (b->version > m) m = b->version;
      }
      p += lc->cmdsize;
    }
  }
  best = m;
  count = n;
  return m;
}

static uint32_t host_version(void) {
  const char *e = getenv("CLOKEOS_MACOS_VERSION");
  if (e && *e) return parse_version(e);
  uint32_t a = plist_version(), b = images_min_os();
  return a > b ? a : b;
}

_Bool _availability_version_check(uint32_t count, const build_version versions[]) {
  uint32_t host = host_version();
  for (uint32_t i = 0; i < count; i++) {
    uint32_t want = versions[i].version;
    if (versions[i].platform == PLATFORM_MACOS_) {
      if (host < want) return 0;
    } else if (versions[i].platform == PLATFORM_MACCATALYST_) {
      if (host + V(3, 0, 0) < want) return 0;   /* macOS 15.x = iOS 18.x */
    }
  }
  return 1;
}

int32_t __isPlatformVersionAtLeast(uint32_t platform, uint32_t major, uint32_t minor, uint32_t subminor) {
  build_version v = {platform, V(major, minor, subminor)};
  return _availability_version_check(1, &v);
}
