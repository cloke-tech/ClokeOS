/* Test der neuen libSystem-Symbole; gleiche Ausgabe auf dem Mac (Referenz) und in der ClokeOS-VM */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <complex.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <os/workgroup.h>
#include <os/workgroup_interval.h>
#include <os/workgroup_parallel.h>
#include <sys/attr.h>
#include <sys/fsgetpath.h>
#include <os/signpost.h>
#include <libkern/OSAtomic.h>
#include <CommonCrypto/CommonDigest.h>
#include <removefile.h>
#include <simd/simd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <sys/stat.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <uuid/uuid.h>
#include <stdatomic.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#define P(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)
extern os_workgroup_interval_t os_workgroup_interval_create(const char *, os_clockid_t, os_workgroup_attr_t);
extern double __cospi(double);
extern simd_float4 _simd_fma_f4(simd_float4, simd_float4, simd_float4), _simd_pow_f4(simd_float4, simd_float4), _simd_round_f4(simd_float4);
extern int32_t __isPlatformVersionAtLeast(uint32_t, uint32_t, uint32_t, uint32_t);
extern int pthread_qos_max_parallelism(int, unsigned long);
extern int pthread_time_constraint_max_parallelism(unsigned long);
extern int semaphore_signal_all(semaphore_t);
typedef struct { uint32_t platform, version; } bv;
extern bool _availability_version_check(uint32_t, bv[]);

static void hex(const char *tag, const unsigned char *d, int n) { printf("%s: ", tag); for (int i = 0; i < n; i++) printf("%02x", d[i]); printf("\n"); }

typedef struct node { struct node *next; int v; } node;
static void *sem_waiter(void *a) { semaphore_wait(*(semaphore_t *)a); return NULL; }

typedef struct { char a[24]; } big;

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  unsigned char md[20];
  hex("md5(abc)", CC_MD5("abc", 3, md), 16);
  hex("sha1(abc)", CC_SHA1("abc", 3, md), 20);

  OSQueueHead lh = OS_ATOMIC_QUEUE_INIT; node n[3] = {{0, 1}, {0, 2}, {0, 3}};
  for (int i = 0; i < 3; i++) OSAtomicEnqueue(&lh, &n[i], offsetof(node, next));
  node *d; char s[32] = ""; while ((d = OSAtomicDequeue(&lh, offsetof(node, next)))) sprintf(s + strlen(s), "%d", d->v);
  P("lifo: %s", s);
  OSFifoQueueHead fh = OS_ATOMIC_FIFO_QUEUE_INIT; s[0] = 0;
  for (int i = 0; i < 3; i++) OSAtomicFifoEnqueue(&fh, &n[i], offsetof(node, next));
  while ((d = OSAtomicFifoDequeue(&fh, offsetof(node, next)))) sprintf(s + strlen(s), "%d", d->v);
  P("fifo: %s", s);

  _Atomic big ab; big in = {"hello libatomic"}; atomic_store(&ab, in); big out = atomic_load(&ab); P("atomic24: %s", out.a);

  const char *err; long long v = strtonum("42", 0, 100, &err); P("strtonum: %lld %s", v, err ? err : "ok");
  v = strtonum("420", 0, 100, &err); P("strtonum big: %lld %s", v, err ? err : "ok");
  v = strtonum("-1", 0, 100, &err); P("strtonum small: %lld %s", v, err ? err : "ok");
  uuid_t u; int r = uuid_parse("E621E1F8-C36C-495A-93FC-0C247A3E6E5F", u); hex("uuid", u, 16); P("uuid_parse=%d bad=%d", r, uuid_parse("zz", u));
  srand48(1); P("drand48: %.12f %.12f lrand48: %ld mrand48: %ld", drand48(), drand48(), lrand48(), mrand48());
  unsigned short xs[3] = {1, 2, 3}; P("erand48: %.12f nrand48: %ld jrand48: %ld", erand48(xs), nrand48(xs), jrand48(xs));
  P("sqrtf: %.7f %.7f", sqrtf(2.0f), sqrtf(0.25f));

  double complex z = (INFINITY + 1.0 * I) * (2.0 + 3.0 * I); P("muldc3: %g %g", creal(z), cimag(z));
  float complex zf = (3.0f + 4.0f * I) * (1.0f - 2.0f * I); P("mulsc3: %g %g", crealf(zf), cimagf(zf));
  volatile int e = -3; P("powi: %g %g", __builtin_powi(2.0, e), (double)__builtin_powif(3.0f, 4));
  P("cospi: %g %g %g %g %g", __cospi(0.5), __cospi(1.0), __cospi(0.25), __cospi(-1.5), __cospi(2.0 / 3.0));
  simd_double3x3 m = {{{2, 0, 1}, {1, 3, 0}, {0, 1, 4}}}; simd_double3x3 mi = simd_inverse(m);
  P("inv: %.9f %.9f %.9f | %.9f %.9f %.9f | %.9f %.9f %.9f", mi.columns[0][0], mi.columns[0][1], mi.columns[0][2], mi.columns[1][0], mi.columns[1][1], mi.columns[1][2], mi.columns[2][0], mi.columns[2][1], mi.columns[2][2]);
  simd_float4 f = _simd_fma_f4((simd_float4){1, 2, 3, 4}, (simd_float4){2, 2, 2, 2}, (simd_float4){1, 1, 1, 1}); P("fma: %g %g %g %g", f[0], f[1], f[2], f[3]);
  simd_float4 pw = _simd_pow_f4((simd_float4){2, 3, 4, 5}, (simd_float4){2, 2, 0.5, 0}); P("pow: %g %g %g %g", pw[0], pw[1], pw[2], pw[3]);
  simd_float4 rd = _simd_round_f4((simd_float4){2.5, -2.5, 1.4, 0}); P("round: %g %g %g", rd[0], rd[1], rd[2]);
  P("identity: %g %g", matrix_identity_double3x3.columns[1][1], matrix_identity_double3x3.columns[0][1]);
  P("isinf/isnan: %d %d %d", __builtin_isinf(INFINITY), __builtin_isnan(NAN), __builtin_isinf(1.0f));

  /* dispatch */
  dispatch_queue_t q = dispatch_queue_create("t2.concurrent", DISPATCH_QUEUE_CONCURRENT);
  __block int order = 0, a1 = -1, a2 = -1, a3 = -1;
  dispatch_group_t g = dispatch_group_create();
  dispatch_group_async(g, q, ^{ usleep(100000); a1 = order++; });
  dispatch_group_async(g, q, ^{ usleep(100000); a2 = order++; });
  dispatch_barrier_async(q, ^{ a3 = order++; });
  __block int after = -1;
  dispatch_group_async(g, q, ^{ after = order++; });
  dispatch_group_notify(g, dispatch_get_main_queue(), ^{ exit(0); }); /* wird unten ersetzt */
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  dispatch_barrier_async(q, ^{ dispatch_semaphore_signal(done); });
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
  P("barrier: first two before=%d barrier=%d after=%d", a1 < 2 && a2 < 2, a3, after);
  static char keyA, keyB; dispatch_queue_t sq = dispatch_queue_create("t2.serial", NULL);
  dispatch_queue_set_specific(sq, &keyA, (void *)0x1234, NULL);
  P("get_specific(queue)=%p other=%p", dispatch_queue_get_specific(sq, &keyA), dispatch_queue_get_specific(sq, &keyB));
  dispatch_queue_t tq = dispatch_queue_create_with_target("t2.target", NULL, sq);
  __block void *in_sq = NULL, *in_tq = NULL, *none = (void *)1; __block void *cur = NULL;
  dispatch_sync(sq, ^{ in_sq = dispatch_get_specific(&keyA); cur = dispatch_get_current_queue(); });
  dispatch_sync(tq, ^{ in_tq = dispatch_get_specific(&keyA); none = dispatch_get_specific(&keyB); });
  P("get_specific: in_sq=%p in_target=%p none=%p current_is_sq=%d current_outside=%d", in_sq, in_tq, none, cur == (void *)sq, dispatch_get_current_queue() != NULL);
  __block int ran = 0; dispatch_block_t blk = dispatch_block_create(0, ^{ ran++; });
  dispatch_block_cancel(blk); dispatch_async(sq, blk); dispatch_sync(sq, ^{}); P("block_cancel: ran=%d testcancel=%ld", ran, dispatch_block_testcancel(blk));
  dispatch_block_t b2 = dispatch_block_create_with_qos_class(0, QOS_CLASS_USER_INTERACTIVE, 0, ^{ ran += 10; }); dispatch_async(sq, b2); dispatch_sync(sq, ^{}); P("block_qos: ran=%d", ran);
  P("signpost_enabled: %d", (int)os_signpost_enabled(OS_LOG_DEFAULT));

  /* os_workgroup */
  os_workgroup_t w = os_workgroup_parallel_create("t2.wg", NULL); P("wg create: %d", w != NULL);
  if (w) {
    os_workgroup_join_token_s tok, tok2;
    P("join=%d again=%d testcancel=%d", os_workgroup_join(w, &tok), os_workgroup_join(w, &tok2), os_workgroup_testcancel(w));
    os_workgroup_leave(w, &tok);
    os_workgroup_cancel(w); P("after cancel: testcancel=%d join=%d", os_workgroup_testcancel(w), os_workgroup_join(w, &tok));
    P("max_parallel>=1: %d", os_workgroup_max_parallel_threads(w, NULL) >= 1);
  }
  os_workgroup_interval_t iv = os_workgroup_interval_create("t2.iv", OS_CLOCK_MACH_ABSOLUTE_TIME, NULL); P("iv create: %d", iv != NULL);
  if (iv) {
    uint64_t t = mach_absolute_time(); os_workgroup_join_token_s tok;
    P("join iv=%d", os_workgroup_join(iv, &tok));
    P("start=%d start again=%d", os_workgroup_interval_start(iv, t, t + 1000000, NULL), os_workgroup_interval_start(iv, t, t + 1000000, NULL));
    P("update=%d finish=%d finish again=%d update after finish=%d", os_workgroup_interval_update(iv, t + 2000000, NULL), os_workgroup_interval_finish(iv, NULL), os_workgroup_interval_finish(iv, NULL), os_workgroup_interval_update(iv, t + 3000000, NULL));
    P("bad range start=%d", os_workgroup_interval_start(iv, t + 10, t, NULL));
    os_workgroup_leave(iv, &tok);
  }
  P("bad clock create: %d", os_workgroup_interval_create("x", 99, NULL) != NULL);

  /* Versionen */
  uint32_t vs[][3] = {{11,7,0},{12,0,0},{15,6,0},{15,6,1},{26,0,0}};
  for (int i = 0; i < 5; i++) P("atLeast(macos %u.%u.%u)=%d", vs[i][0], vs[i][1], vs[i][2], __isPlatformVersionAtLeast(1, vs[i][0], vs[i][1], vs[i][2]));
  bv x = {2, (17 << 16)}; P("ios17=%d", _availability_version_check(1, &x));

  /* Kernel-Aufrufe */
  int kq = kqueue(); struct kevent64_s kev; struct timespec ts = {0, 0};
  P("kevent64: %d", kevent64(kq, NULL, 0, &kev, 1, 0, &ts));
  char path[1024]; fsid_t fsid = {{0, 0}}; ssize_t fr = fsgetpath(path, sizeof path, &fsid, 2); P("fsgetpath: %s", fr < 0 ? "err" : "ok");
  int dfd = open("/usr", O_RDONLY | O_DIRECTORY); struct attrlist al = {0}; al.bitmapcount = ATTR_BIT_MAP_COUNT; al.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME;
  char abuf[8192]; int br = getattrlistbulk(dfd, &al, abuf, sizeof abuf, 0); P("getattrlistbulk: %s", br > 0 ? "entries" : br == 0 ? "none" : "err");
  char tmpl[] = "/tmp/t2rmXXXXXX"; mkdtemp(tmpl); char sub[256]; snprintf(sub, sizeof sub, "%s/a/b", tmpl); char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p %s && touch %s/f %s/../g", sub, sub, sub); system(cmd);
  P("removefile nonrec: %d errno=%d", removefile(tmpl, NULL, 0), errno);
  P("removefile keep parent: %d", removefile(tmpl, NULL, REMOVEFILE_RECURSIVE | REMOVEFILE_KEEP_PARENT)); struct stat st; P("  parent exists=%d", stat(tmpl, &st) == 0);
  P("removefile rec: %d, gone=%d", removefile(tmpl, NULL, REMOVEFILE_RECURSIVE), stat(tmpl, &st) != 0);
  P("pagemask: %lu", (unsigned long)vm_page_size - 1);
  extern unsigned long vm_page_mask; (void)vm_page_mask;
  int np = pthread_time_constraint_max_parallelism(0); P("parallelism>=1: %d qos==tc: %d", np >= 1, pthread_qos_max_parallelism(0x21, 0) == np);

  semaphore_t sem; semaphore_create(mach_task_self(), &sem, SYNC_POLICY_FIFO, 0); pthread_t th[3];
  for (int i = 0; i < 3; i++) pthread_create(&th[i], NULL, sem_waiter, &sem);
  usleep(200000); P("signal_all=%d", semaphore_signal_all(sem)); for (int i = 0; i < 3; i++) pthread_join(th[i], NULL); P("waiters released");

  /* Thread-Richtlinien */
  mach_timebase_info_data_t tb; mach_timebase_info(&tb);
  thread_time_constraint_policy_data_t tc; tc.period = (uint32_t)(10000000ULL * tb.denom / tb.numer); tc.computation = tc.period / 10; tc.constraint = tc.period / 2; tc.preemptible = 1;
  kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  thread_time_constraint_policy_data_t got = {0}; mach_msg_type_number_t cnt = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t def = 0;
  kern_return_t kg = thread_policy_get(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&got, &cnt, &def);
  P("tc set=%d get=%d same=%d count=%u default=%d", kr, kg, got.period == tc.period && got.computation == tc.computation && got.constraint == tc.constraint, cnt, def);
  thread_extended_policy_data_t ext; cnt = THREAD_EXTENDED_POLICY_COUNT; def = 0;
  kg = thread_policy_get(mach_thread_self(), THREAD_EXTENDED_POLICY, (thread_policy_t)&ext, &cnt, &def); P("extended get=%d timeshare=%d count=%u", kg, ext.timeshare, cnt);
  extern kern_return_t thread_policy(thread_act_t, policy_t, policy_base_t, mach_msg_type_number_t, boolean_t);
  policy_timeshare_base_data_t base = {31}; kern_return_t kt = thread_policy(mach_thread_self(), POLICY_TIMESHARE, (policy_base_t)&base, POLICY_TIMESHARE_BASE_COUNT, 0); P("thread_policy(TIMESHARE)=%d", kt);
  return 0;
}
