/* libclokeos_compat, Teil 4: libSystem-Symbole, die Logic Pro 12.3 (und andere Programme aus Apples
 * SDK 26) importieren, PureDarwins libSystem 20.5 aber nicht exportiert. Welche das sind, zeigt
 * libcompat/missing.py; docs/logic-libsystem.md hat die Liste und die Tests.
 * Hier: Einzelfunktionen ohne eigenes Thema (Hashes, Zufall, libatomic, Dateisystem-Systemaufrufe,
 * Mach-Thread-Richtlinien). Dispatch/Signpost in dispatch.c, os_workgroup in workgroup.m,
 * Versionsabfragen in version.c, Mathematik in mathx.c. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_traps.h>
#include <sys/attr.h>
#include <sys/event.h>
#include <sched.h>
#include <stdio.h>
#include <mach/message.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* ---- CommonCrypto: Einmal-Aufrufe (Init/Update/Final stehen in system.c) ---- */
extern int CC_MD5_Init(void *), CC_MD5_Update(void *, const void *, uint32_t), CC_MD5_Final(unsigned char *, void *);
extern int CC_SHA1_Init(void *), CC_SHA1_Update(void *, const void *, uint32_t), CC_SHA1_Final(unsigned char *, void *);

unsigned char *CC_MD5(const void *data, uint32_t len, unsigned char *md) {
  uint64_t ctx[16];   /* Apples CC_MD5_CTX hat 92 Bytes */
  CC_MD5_Init(ctx); CC_MD5_Update(ctx, data, len); CC_MD5_Final(md, ctx);
  return md;
}

unsigned char *CC_SHA1(const void *data, uint32_t len, unsigned char *md) {
  uint64_t ctx[16];   /* CC_SHA1_CTX: 96 Bytes */
  CC_SHA1_Init(ctx); CC_SHA1_Update(ctx, data, len); CC_SHA1_Final(md, ctx);
  return md;
}

/* ---- OSAtomic-Warteschlangen (<libkern/OSAtomic.h>) ----
 * LIFO (OSQueueHead = {Zeiger, Zähler}): lock-frei, der Zähler gegen das ABA-Problem. Der
 * 16-Byte-Vergleichstausch ist auf arm64 ldaxp/stlxp (läuft auch ohne LSE, wie in QEMU/TCG),
 * auf x86_64 cmpxchg16b. */
typedef struct { void *head; uintptr_t count; } qhead;

static inline int cas128(qhead *q, qhead old, qhead new) {
#if defined(__arm64__)
  uint64_t lo, hi, fail;
  do {
    __asm__ volatile("ldaxp %0, %1, [%2]" : "=&r"(lo), "=&r"(hi) : "r"(q) : "memory");
    if (lo != (uint64_t)old.head || hi != old.count) {
      __asm__ volatile("clrex" ::: "memory");
      return 0;
    }
    __asm__ volatile("stlxp %w0, %1, %2, [%3]" : "=&r"(fail) : "r"((uint64_t)new.head), "r"((uint64_t)new.count), "r"(q) : "memory");
  } while (fail);
  return 1;
#else
  unsigned char ok;
  __asm__ volatile("lock; cmpxchg16b %1; sete %0"
                   : "=q"(ok), "+m"(*q), "+a"(old.head), "+d"(old.count)
                   : "b"(new.head), "c"(new.count) : "memory", "cc");
  return ok;
#endif
}

static inline qhead load128(qhead *q) {
  qhead v;
#if defined(__arm64__)
  uint64_t lo, hi;
  /* ldaxp allein ist atomar; stlxp wird nicht gebraucht, die Exklusivmarkierung verfällt */
  __asm__ volatile("ldaxp %0, %1, [%2]\n\tclrex" : "=&r"(lo), "=&r"(hi) : "r"(q) : "memory");
  v.head = (void *)lo; v.count = hi;
#else
  v.head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
  v.count = __atomic_load_n(&q->count, __ATOMIC_ACQUIRE);   /* ein gerissener Wert scheitert unten im Tausch */
#endif
  return v;
}

void OSAtomicEnqueue(void *list, void *newp, size_t offset) {
  qhead *q = list;
  for (;;) {
    qhead old = load128(q);
    *(void **)((char *)newp + offset) = old.head;
    qhead nw = {newp, old.count + 1};
    if (cas128(q, old, nw)) return;
  }
}

void *OSAtomicDequeue(void *list, size_t offset) {
  qhead *q = list;
  for (;;) {
    qhead old = load128(q);
    if (!old.head) return NULL;
    qhead nw = {*(void **)((char *)old.head + offset), old.count + 1};
    if (cas128(q, old, nw)) return old.head;
  }
}

/* FIFO (OSFifoQueueHead = {Anfang, Ende, int}): kurzer Spin-Lock im dritten Feld */
typedef struct { void *first; void *last; volatile int lock; } fifohead;

static void fifo_lock(fifohead *q) { while (__atomic_exchange_n(&q->lock, 1, __ATOMIC_ACQUIRE)) sched_yield(); }
static void fifo_unlock(fifohead *q) { __atomic_store_n(&q->lock, 0, __ATOMIC_RELEASE); }

void OSAtomicFifoEnqueue(void *list, void *newp, size_t offset) {
  fifohead *q = list;
  *(void **)((char *)newp + offset) = NULL;
  fifo_lock(q);
  if (q->last) *(void **)((char *)q->last + offset) = newp; else q->first = newp;
  q->last = newp;
  fifo_unlock(q);
}

void *OSAtomicFifoDequeue(void *list, size_t offset) {
  fifohead *q = list;
  fifo_lock(q);
  void *p = q->first;
  if (p) {
    q->first = *(void **)((char *)p + offset);
    if (!q->first) q->last = NULL;
  }
  fifo_unlock(q);
  return p;
}

/* ---- libatomic: die generischen Aufrufe, die clang für ungewöhnliche Größen erzeugt ----
 * Ein Spin-Lock je Adressgruppe (wie compiler-rt). */
static volatile int atomic_locks[64];

static volatile int *atomic_lock(const void *p) {
  volatile int *l = &atomic_locks[((uintptr_t)p >> 4) % 64];
  while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) sched_yield();
  return l;
}

void cl_atomic_load(size_t size, void *src, void *ret, int order) __asm__("___atomic_load");
void cl_atomic_store(size_t size, void *dst, void *val, int order) __asm__("___atomic_store");
void cl_atomic_exchange(size_t size, void *mem, void *val, void *ret, int order) __asm__("___atomic_exchange");
_Bool cl_atomic_compare_exchange(size_t size, void *mem, void *expected, void *desired, int ok, int fail) __asm__("___atomic_compare_exchange");

void cl_atomic_load(size_t size, void *src, void *ret, int order) {
  (void)order;
  volatile int *l = atomic_lock(src);
  memcpy(ret, src, size);
  __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

void cl_atomic_store(size_t size, void *dst, void *val, int order) {
  (void)order;
  volatile int *l = atomic_lock(dst);
  memcpy(dst, val, size);
  __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

void cl_atomic_exchange(size_t size, void *mem, void *val, void *ret, int order) {
  (void)order;
  volatile int *l = atomic_lock(mem);
  char tmp[256];
  if (size > sizeof tmp) abort();
  memcpy(tmp, mem, size); memcpy(mem, val, size); memcpy(ret, tmp, size);
  __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

_Bool cl_atomic_compare_exchange(size_t size, void *mem, void *expected, void *desired, int ok, int fail) {
  (void)ok; (void)fail;
  volatile int *l = atomic_lock(mem);
  _Bool eq = memcmp(mem, expected, size) == 0;
  if (eq) memcpy(mem, desired, size); else memcpy(expected, mem, size);
  __atomic_store_n(l, 0, __ATOMIC_RELEASE);
  return eq;
}

/* ---- BSD-Kleinkram ---- */

/* OpenBSD strtonum */
long long strtonum(const char *numstr, long long minval, long long maxval, const char **errstrp) {
  static const char *const msgs[] = {NULL, "invalid", "too small", "too large"};
  long long ll = 0;
  int err = 0;
  char *ep;
  if (minval > maxval) err = 1;
  else {
    int save = errno;
    errno = 0;
    ll = strtoll(numstr, &ep, 10);
    if (numstr == ep || *ep != '\0') err = 1;
    else if ((ll == LLONG_MIN && errno == ERANGE) || ll < minval) err = 2;
    else if ((ll == LLONG_MAX && errno == ERANGE) || ll > maxval) err = 3;
    if (err == 0) errno = save;
  }
  if (errstrp) *errstrp = msgs[err];
  if (err) { errno = err == 1 ? EINVAL : ERANGE; ll = 0; }
  return ll;
}

/* libuuid: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> 16 Bytes, 0 wenn gültig, sonst -1 */
int uuid_parse(const char *in, unsigned char uu[16]) {
  static const int dash[] = {8, 13, 18, 23};
  size_t i, n = 0;
  for (i = 0; i < 36; i++) {
    char c = in[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return -1; continue; }
    int v = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
    if (v < 0) return -1;
    if (n & 1) uu[n / 2] |= v; else uu[n / 2] = (unsigned char)(v << 4);
    n++;
  }
  (void)dash;
  return in[36] == '\0' ? 0 : -1;
}

/* rand48-Familie (POSIX: Xn+1 = (a Xn + c) mod 2^48, a = 0x5DEECE66D, c = 0xB) */
static unsigned short x48[3] = {0x330E, 0, 0};
static unsigned short a48[3] = {0xE66D, 0xDEEC, 0x0005};
static unsigned short c48 = 0xB;

static void next48(unsigned short x[3]) {
  uint64_t xv = (uint64_t)x[0] | (uint64_t)x[1] << 16 | (uint64_t)x[2] << 32;
  uint64_t av = (uint64_t)a48[0] | (uint64_t)a48[1] << 16 | (uint64_t)a48[2] << 32;
  xv = (av * xv + c48) & 0xFFFFFFFFFFFFULL;
  x[0] = xv & 0xFFFF; x[1] = (xv >> 16) & 0xFFFF; x[2] = xv >> 32;
}

double erand48(unsigned short x[3]) {
  next48(x);
  uint64_t xv = (uint64_t)x[0] | (uint64_t)x[1] << 16 | (uint64_t)x[2] << 32;
  return (double)xv / 281474976710656.0;
}
long nrand48(unsigned short x[3]) { next48(x); return (long)(((uint32_t)x[2] << 15) | (x[1] >> 1)); }
long jrand48(unsigned short x[3]) { next48(x); return (int32_t)(((uint32_t)x[2] << 16) | x[1]); }
double drand48(void) { return erand48(x48); }
long lrand48(void) { return nrand48(x48); }
long mrand48(void) { return jrand48(x48); }
void srand48(long seed) { x48[0] = 0x330E; x48[1] = (unsigned short)seed; x48[2] = (unsigned short)((unsigned long)seed >> 16); a48[0] = 0xE66D; a48[1] = 0xDEEC; a48[2] = 5; c48 = 0xB; }
unsigned short *seed48(unsigned short s[3]) {
  static unsigned short old[3];
  memcpy(old, x48, sizeof old); memcpy(x48, s, sizeof x48);
  a48[0] = 0xE66D; a48[1] = 0xDEEC; a48[2] = 5; c48 = 0xB;
  return old;
}
void lcong48(unsigned short p[7]) { memcpy(x48, p, 6); memcpy(a48, p + 3, 6); c48 = p[6]; }

void sranddev(void) {
  unsigned seed;
  arc4random_buf(&seed, sizeof seed);
  srand(seed);
}

int vsscanf_l(const char *str, void *locale, const char *fmt, va_list ap) {
  (void)locale;
  return vsscanf(str, fmt, ap);
}

float sqrtf(float x) {
  float r;
#if defined(__arm64__)
  __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
#else
  __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
#endif
  return r;
}

/* ---- Dateisystem- und Kernel-Systemaufrufe ohne Wrapper in PureDarwins libSystem ----
 * Die Aufrufe gehen unverändert an den Kernel (gemessen: kevent64 und getattrlistbulk laufen,
 * fsgetpath und searchfs liefern ENOTSUP, weil der ext4-Treiber sie nicht kennt). */
int kevent64(int kq, const struct kevent64_s *changelist, int nchanges, struct kevent64_s *eventlist, int nevents, unsigned int flags, const struct timespec *timeout) {
  return (int)syscall(SYS_kevent64, kq, changelist, nchanges, eventlist, nevents, flags, timeout);
}

ssize_t fsgetpath(char *buf, size_t bufsize, void *fsid, uint64_t objid) {
  return (ssize_t)syscall(SYS_fsgetpath, buf, bufsize, fsid, objid);
}

int getattrlistbulk(int dirfd, void *alist, void *attrbuf, size_t bufsize, uint64_t options) {
  return (int)syscall(SYS_getattrlistbulk, dirfd, alist, attrbuf, bufsize, options);
}

int searchfs(const char *path, struct fssearchblock *searchblock, unsigned long *nummatches, unsigned int scriptcode,
             unsigned int options, struct searchstate *state) {
  return (int)syscall(SYS_searchfs, path, searchblock, nummatches, scriptcode, options, state);
}

/* ---- removefile ---- */
#define RM_RECURSIVE (1u << 0)
#define RM_KEEP_PARENT (1u << 1)
#define RM_STOP_ON_ERROR (1u << 5)

static int rm_tree(const char *path, unsigned flags, int top) {
  struct stat st;
  if (lstat(path, &st) < 0) return -1;
  int rc = 0;
  if (S_ISDIR(st.st_mode)) {
    if (!(flags & RM_RECURSIVE)) return 0;   /* Apples removefile lässt Verzeichnisse ohne REMOVEFILE_RECURSIVE stehen und meldet 0 (am Mac gemessen) */
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char child[PATH_MAX];
      if ((size_t)snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= sizeof child) { errno = ENAMETOOLONG; rc = -1; }
      else if (rm_tree(child, flags, 0) < 0) rc = -1;
      if (rc < 0 && (flags & RM_STOP_ON_ERROR)) break;
    }
    int saved = errno;
    closedir(d);
    errno = saved;
    if (rc < 0 && (flags & RM_STOP_ON_ERROR)) return -1;
    if (top && (flags & RM_KEEP_PARENT)) return rc;
    if (rmdir(path) < 0) return -1;
    return rc;
  }
  return unlink(path);
}

void *removefile_state_alloc(void) { return calloc(1, 64); }
int removefile_state_free(void *state) { free(state); return 0; }

int removefile(const char *path, void *state, uint32_t flags) {
  (void)state;
  struct stat st;
  if (lstat(path, &st) < 0) return -1;
  return rm_tree(path, flags, 1);
}

/* ---- Mach: Semaphore, Parallelität, Thread-Richtlinien ---- */
kern_return_t semaphore_signal_all_trap(mach_port_name_t);
kern_return_t semaphore_signal_all(mach_port_t sem) { return semaphore_signal_all_trap(sem); }

/* vm_page_mask: Seitengröße minus 1 (arm64 16 KiB, x86_64 4 KiB, wie im Kernel; gemessen: getpagesize() = 16384).
 * libSystem hat das Symbol, exportiert es aber nicht. Kein Konstruktor: der könnte vor der
 * Initialisierung von libSystem laufen. */
#if defined(__arm64__)
unsigned long vm_page_mask = 0x3FFF;
#else
unsigned long vm_page_mask = 0xFFF;
#endif

/* pthread_qos_max_parallelism: Zahl der Prozessoren (Flag 1 = nur physische). PureDarwin kennt keine
 * Effizienzkerne, deshalb ist die Antwort für jede QoS-Klasse gleich. */
int pthread_qos_max_parallelism(int qos, unsigned long flags) {
  (void)qos;
  int n = 0;
  size_t len = sizeof n;
  if (sysctlbyname((flags & 1) ? "hw.physicalcpu" : "hw.activecpu", &n, &len, NULL, 0) < 0 || n < 1) n = 1;
  return n;
}

int pthread_time_constraint_max_parallelism(unsigned long flags) { return pthread_qos_max_parallelism(0, flags); }

/* thread_policy / thread_policy_get fehlen in libsystem_kernel: die MIG-Nachrichten (thread_act,
 * ids 3616 und 3618, gelesen aus dem thread_policy_set von PureDarwins libSystem) werden hier von
 * Hand gebaut. */
extern mach_port_t mach_reply_port(void);

#define MIG_THREAD_POLICY 3616
#define MIG_THREAD_POLICY_GET 3618

/* Schickt die Anfrage in m und wartet auf die Antwort im selben Puffer (wie MIG-Stubs). */
static kern_return_t mig_call(mach_msg_header_t *h, mach_port_t thread, int id, mach_msg_size_t send_size, mach_msg_size_t rcv_size) {
  mach_port_t reply = mach_reply_port();
  h->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
  h->msgh_remote_port = thread;
  h->msgh_local_port = reply;
  h->msgh_id = id;
  h->msgh_size = send_size;
  kern_return_t kr = mach_msg(h, MACH_SEND_MSG | MACH_RCV_MSG, send_size, rcv_size, reply, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
  if (kr != KERN_SUCCESS) {
    if (kr >= MACH_RCV_IN_PROGRESS) mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
    return kr;
  }
  if (h->msgh_id != id + 100) return MIG_REPLY_MISMATCH;
  return KERN_SUCCESS;
}

kern_return_t thread_policy(thread_act_t thread, policy_t policy, policy_base_t base, mach_msg_type_number_t count, boolean_t set_limit) {
  union {
    struct { mach_msg_header_t head; NDR_record_t ndr; int policy; mach_msg_type_number_t count; int base[5 + 1]; } req;
    struct { mach_msg_header_t head; NDR_record_t ndr; kern_return_t ret; char pad[64]; } rep;
  } m;
  if (count > 5) return MIG_ARRAY_TOO_LARGE;
  memset(&m, 0, sizeof m);
  m.req.ndr = NDR_record;
  m.req.policy = policy;
  m.req.count = count;
  memcpy(m.req.base, base, count * 4);
  m.req.base[count] = set_limit;   /* set_limit steht hinter den belegten Feldern */
  mach_msg_size_t size = (mach_msg_size_t)(offsetof(__typeof__(m.req), base) + count * 4 + 4);
  kern_return_t kr = mig_call(&m.req.head, thread, MIG_THREAD_POLICY, size, sizeof m);
  return kr != KERN_SUCCESS ? kr : m.rep.ret;
}

kern_return_t thread_policy_get(thread_inspect_t thread, thread_policy_flavor_t flavor, thread_policy_t policy_info_p, mach_msg_type_number_t *count, boolean_t *get_default) {
  int *policy_info = policy_info_p;
  union {
    struct { mach_msg_header_t head; NDR_record_t ndr; int flavor; mach_msg_type_number_t count; int get_default; } req;
    struct { mach_msg_header_t head; NDR_record_t ndr; kern_return_t ret; mach_msg_type_number_t count; int info[16 + 1]; } rep;
  } m;
  memset(&m, 0, sizeof m);
  m.req.ndr = NDR_record;
  m.req.flavor = flavor;
  m.req.count = *count > 16 ? 16 : *count;
  m.req.get_default = *get_default;
  kern_return_t kr = mig_call(&m.req.head, thread, MIG_THREAD_POLICY_GET, sizeof m.req, sizeof m);
  if (kr != KERN_SUCCESS) return kr;
  if (m.rep.ret != KERN_SUCCESS) return m.rep.ret;
  mach_msg_type_number_t n = m.rep.count;
  if (n > 16) return MIG_TYPE_ERROR;
  if (n > *count) { *count = n; return KERN_NO_SPACE; }
  memcpy(policy_info, m.rep.info, n * 4);
  *get_default = m.rep.info[n];
  *count = n;
  return KERN_SUCCESS;
}
