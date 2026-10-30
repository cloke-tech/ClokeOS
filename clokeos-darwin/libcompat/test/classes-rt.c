/* Klassen-Identität, Versionen mit geladenem 15.6-Code, Echtzeit-Latenz */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <dispatch/dispatch.h>

extern int32_t __isPlatformVersionAtLeast(uint32_t, uint32_t, uint32_t, uint32_t);
static mach_timebase_info_data_t tb;
static volatile int stop;
static void *hog(void *a) { volatile uint64_t x = 0; while (!stop) x++; return NULL; }

static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }

static void *meter(void *arg) {
  int rt = *(int *)arg;
  if (rt) {
    thread_time_constraint_policy_data_t tc;
    tc.period = (uint32_t)(2000000ULL * tb.denom / tb.numer); tc.computation = tc.period / 10; tc.constraint = tc.period / 2; tc.preemptible = 1;
    kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    printf("policy_set(TC) = %d\n", kr);
  }
  printf("meter rt=%d gestartet\n", rt);
  enum { N = 300 }; static double late[2][N]; double *l = late[rt];
  uint64_t t = mach_absolute_time();
  uint64_t step = 2000000ULL * tb.denom / tb.numer;
  for (int i = 0; i < N; i++) {
    t += step;
    mach_wait_until(t);
    uint64_t now = mach_absolute_time();
    if (i % 50 == 0) fprintf(stderr, "i=%d\n", i);
    l[i] = (double)(now - t) * tb.numer / tb.denom / 1000.0;   /* µs zu spät */
  }
  fprintf(stderr, "meter %d: Messung fertig\n", rt);
  qsort(l, N, sizeof(double), cmp);
  printf("%s: Weckverzug Median %.0f us, 95%% %.0f us, max %.0f us\n", rt ? "Zeitkritisch" : "Normal     ", l[N / 2], l[N * 95 / 100], l[N - 1]);
  return NULL;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  mach_timebase_info(&tb);
  void *sys = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW);
  void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
  void *(*getClass)(const char *) = dlsym(objc, "objc_getClass");
  void *(*getCls)(void *) = dlsym(objc, "object_getClass");
  void *(*superclass)(void *) = dlsym(objc, "class_getSuperclass");
  const char *(*cname)(void *) = dlsym(objc, "class_getName");
  const char *names[] = {"OS_dispatch_queue", "OS_dispatch_source", "OS_object"};
  for (int i = 0; i < 3; i++) {
    char sym[128]; snprintf(sym, sizeof sym, "OBJC_CLASS_$_%s", names[i]);
    void *s = dlsym(sys, sym), *c = getClass(names[i]);
    printf("%s: Symbol %s Klasse (objc_getClass %s) gleich=%d\n", names[i], s ? "da" : "FEHLT", c ? "da" : "-", s && s == c);
  }
  dispatch_queue_t q = dispatch_queue_create("t5", NULL);
  void *cls = getCls(q); int in_chain = 0;
  for (void *k = cls; k; k = superclass(k)) if (k == dlsym(sys, "OBJC_CLASS_$_OS_dispatch_queue")) in_chain = 1;
  printf("Klasse einer Queue: %s, OS_dispatch_queue in der Kette: %d\n", cname(cls), in_chain);
  void *osl = dlsym(sys, "OBJC_CLASS_$_OS_os_log"); printf("OS_os_log Symbol -> %s\n", osl ? cname(osl) : "FEHLT");

  printf("vor dem Laden: atLeast(15.6)=%d\n", __isPlatformVersionAtLeast(1, 15, 6, 0));
  for (int i = 1; i < argc; i++) {
    void *h = dlopen(argv[i], RTLD_NOW);
    printf("dlopen %s: %s -> atLeast(15.6)=%d (26.0)=%d\n", argv[i], h ? "ok" : dlerror(), __isPlatformVersionAtLeast(1, 15, 6, 0), __isPlatformVersionAtLeast(1, 26, 0, 0));
  }
  if (getenv("RT")) {
    pthread_t h1, m0, m1; int zero = 0, one = 1;
    pthread_create(&h1, NULL, hog, NULL);
    pthread_create(&m0, NULL, meter, &zero); fprintf(stderr, "main: warte auf m0\n"); int jr = pthread_join(m0, NULL); fprintf(stderr, "main: join=%d\n", jr);
    int pr = pthread_create(&m1, NULL, meter, &one); printf("create rc=%d\n", pr); pthread_join(m1, NULL);
    stop = 1; pthread_join(h1, NULL);
  }
  return 0;
}
