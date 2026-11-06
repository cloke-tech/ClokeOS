/* Wrapper vor libswiftCore für die Objektspur (macapps/swifttrace.sh): überschreibt swift_allocObject/retain/release (und die _n- und nonatomic-Formen) und meldet
 * jedes Ereignis für Instanzen einer Klasse, deren Name SWIFT_TRACE_CLASS enthält. Alles andere re-exportiert das echte libswiftCoreReal.dylib.
 * Nötig, weil PureDarwins dyld Interposing bei Programmen mit Chained Fixups (Logic Pro) nicht anwendet. */
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *cls;
static void *real_h;
#define NT 64
static uint64_t tr[NT];
static volatile uint32_t ntr;
static int ready;

static void *R(const char *n) { if (!real_h) real_h = dlopen("/usr/lib/swift/libswiftCoreReal.dylib", RTLD_NOW); return real_h ? dlsym(real_h, n) : 0; }
static void put(const char *s) {
  int fd = open("/tmp/swifttrace.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) { write(fd, s, strlen(s)); close(fd); }
}
static void hex(char *b, uint64_t v) { b[0] = '0'; b[1] = 'x'; for (int i = 0; i < 16; i++) b[17 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15]; b[18] = 0; }
static int safe(uint64_t a, uint64_t *v) {
  int p[2]; if (pipe(p)) return 0;
  int ok = write(p[1], (const void *)(uintptr_t)a, 8) == 8 && read(p[0], v, 8) == 8; close(p[0]); close(p[1]); return ok;
}
static void sym(const char *tag, uint64_t a) {
  char b[24]; Dl_info d; hex(b, a); put("    "); put(tag); put(" "); put(b);
  if (a && dladdr((void *)(uintptr_t)a, &d)) {
    put("  "); put(d.dli_fname ? d.dli_fname : "?");
    if (d.dli_sname) { put(" "); put(d.dli_sname); }
    else if (d.dli_fbase) { put(" +"); hex(b, a - (uint64_t)(uintptr_t)d.dli_fbase); put(b); }
  }
  put("\n");
}
static int is_tr(void *p) { uint32_t n = ntr; for (uint32_t i = 0; i < n && i < NT; i++) if (tr[i] == (uint64_t)(uintptr_t)p) return 1; return 0; }
static void event(const char *what, void *p, uint64_t ra) {
  char b[24]; hex(b, (uint64_t)(uintptr_t)p); put("[trace] "); put(what); put(" "); put(b); put("\n");
  sym("von", ra);
  uint64_t *fp = (uint64_t *)__builtin_frame_address(1);
  for (int k = 0; k < 3; k++) {
    uint64_t a = (uint64_t)(uintptr_t)fp, nx, ret;
    if (!fp || (a & 7) || a < 0x1000 || !safe(a, &nx) || !safe(a + 8, &ret)) break;
    if (ret) sym("und", ret);
    fp = (uint64_t *)(uintptr_t)nx;
  }
}
__attribute__((constructor)) static void init(void) { cls = getenv("SWIFT_TRACE_CLASS"); ready = 1; }

void *swift_allocObject(void *md, size_t size, size_t align) {
  void *(*f)(void *, size_t, size_t) = R("swift_allocObject");
  void *o = f(md, size, align);
  { static uint32_t n; if (ready && cls && ((++n) & 0x7ff) == 0) { char b[24]; hex(b, n); put("[trace] swift_allocObject-Aufrufe bisher: "); put(b); put("\n"); } }
  if (ready && cls && md && ntr < NT) {
    uint64_t isa, bits;
    if (safe((uint64_t)(uintptr_t)md, &isa) && isa > 0x100000000ULL && safe((uint64_t)(uintptr_t)md + 0x20, &bits) && bits > 0x100000000ULL) {
      const char *nm = class_getName((Class)md);
      if (nm && strstr(nm, cls)) { tr[ntr++] = (uint64_t)(uintptr_t)o; event("allocObject", o, (uint64_t)(uintptr_t)__builtin_return_address(0)); put("    Klasse "); put(nm); put("\n"); }
    }
  }
  return o;
}
#define HOOK_RETAIN(name) void *name(void *p) { void *(*f)(void *) = R(#name); if (ready && cls && p && is_tr(p)) event(#name, p, (uint64_t)(uintptr_t)__builtin_return_address(0)); return f(p); }
#define HOOK_RELEASE(name) void name(void *p) { void (*f)(void *) = R(#name); if (ready && cls && p && is_tr(p)) event(#name, p, (uint64_t)(uintptr_t)__builtin_return_address(0)); f(p); }
#define HOOK_RETAIN_N(name) void *name(void *p, uint32_t n) { void *(*f)(void *, uint32_t) = R(#name); if (ready && cls && p && is_tr(p)) event(#name, p, (uint64_t)(uintptr_t)__builtin_return_address(0)); return f(p, n); }
#define HOOK_RELEASE_N(name) void name(void *p, uint32_t n) { void (*f)(void *, uint32_t) = R(#name); if (ready && cls && p && is_tr(p)) event(#name, p, (uint64_t)(uintptr_t)__builtin_return_address(0)); f(p, n); }
HOOK_RETAIN(swift_retain) HOOK_RELEASE(swift_release) HOOK_RETAIN_N(swift_retain_n) HOOK_RELEASE_N(swift_release_n)
HOOK_RETAIN(swift_nonatomic_retain) HOOK_RELEASE(swift_nonatomic_release) HOOK_RETAIN_N(swift_nonatomic_retain_n) HOOK_RELEASE_N(swift_nonatomic_release_n)
