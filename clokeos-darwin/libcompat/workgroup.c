/* libclokeos_compat, Teil 5: os_workgroup (<os/workgroup.h>). PureDarwins libSystem enthält die
 * Klassen OS_os_workgroup*, exportiert aber keine einzige os_workgroup_*-Funktion. Logics
 * Audio-Engine (MAAudioEngine) bindet sieben davon, CoreAudio (docs/audio-stack.md) braucht außerdem
 * os_workgroup_interval_create für die I/O-Threads der Geräte.
 *
 * Die Objekte sind echte Objective-C-Objekte einer beim ersten Gebrauch angelegten Klasse (Unterklasse
 * von NSObject), damit os_retain/os_release/objc_release funktionieren. libobjc wird dafür erst
 * dann geladen und per dlsym angesprochen; libclokeos_compat hängt wegen libSystem (die Bibliothek
 * wird von dort re-exportiert) nicht von libobjc ab.
 *
 * Was fehlt: Der Kernel (work_interval_*) wird nicht benutzt. Echtzeitverhalten kommt allein aus
 * thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY) der Audio-Threads (gemessen in
 * docs/logic-libsystem.md); join/start/finish führen Buch und prüfen die Fehlerfälle wie die
 * Dokumentation in <os/workgroup_*.h>. Ports (copy_port/create_with_port) gehen nicht. */
#define OS_OBJECT_USE_OBJC 0
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <os/workgroup.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

#define WG_PLAIN 0
#define WG_INTERVAL 1
#define WG_PARALLEL 2
#define TOKEN_SIG 0x4F57474Au   /* "OWGJ" */

typedef struct {
  int type;
  char *name;
  volatile int cancelled;
  pthread_mutex_t lock;
  int started;
  uint64_t start, deadline;
  void *arena;
  uint32_t arena_max;
  os_workgroup_working_arena_destructor_t arena_destructor;
} wg_state;

typedef void *id;
typedef void *Class;
typedef void *SEL;
typedef void (*IMP)(void);

static struct {
  Class cls;
  size_t base;   /* Offset des wg_state hinter dem Objekt */
  id (*createInstance)(Class, size_t);
  size_t (*instanceSize)(Class);
  IMP super_dealloc;
} rt;

static pthread_once_t rt_once = PTHREAD_ONCE_INIT;
static pthread_key_t cur_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;

static wg_state *state(void *wg) { return (wg_state *)((char *)wg + rt.base); }

static void wg_dealloc(id self, SEL cmd) {
  wg_state *s = state(self);
  if (s->arena && s->arena_destructor) s->arena_destructor(s->arena);
  free(s->name);
  pthread_mutex_destroy(&s->lock);
  ((void (*)(id, SEL))rt.super_dealloc)(self, cmd);
}

static void rt_init(void) {
  void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
  if (!objc) return;
  Class (*getClass)(const char *) = dlsym(objc, "objc_getClass");
  Class (*allocPair)(Class, const char *, size_t) = dlsym(objc, "objc_allocateClassPair");
  void (*regPair)(Class) = dlsym(objc, "objc_registerClassPair");
  SEL (*selReg)(const char *) = dlsym(objc, "sel_registerName");
  int (*addMethod)(Class, SEL, IMP, const char *) = dlsym(objc, "class_addMethod");
  IMP (*getImp)(Class, SEL) = dlsym(objc, "class_getMethodImplementation");
  rt.createInstance = dlsym(objc, "class_createInstance");
  rt.instanceSize = dlsym(objc, "class_getInstanceSize");
  if (!getClass || !allocPair || !regPair || !selReg || !addMethod || !getImp || !rt.createInstance || !rt.instanceSize) return;
  Class nsobject = getClass("NSObject");
  Class cls = allocPair(nsobject, "OS_os_workgroup_clokeos", 0);
  if (!cls) return;
  SEL dealloc = selReg("dealloc");
  rt.super_dealloc = getImp(nsobject, dealloc);
  addMethod(cls, dealloc, (IMP)wg_dealloc, "v16@0:8");
  regPair(cls);
  rt.base = (rt.instanceSize(cls) + 15) & ~(size_t)15;
  rt.cls = cls;
}

static void *wg_new(int type, const char *name) {
  pthread_once(&rt_once, rt_init);
  if (!rt.cls) { errno = ENOMEM; return NULL; }
  id obj = rt.createInstance(rt.cls, rt.base + sizeof(wg_state));
  if (!obj) { errno = ENOMEM; return NULL; }
  wg_state *s = state(obj);
  memset(s, 0, sizeof *s);
  s->type = type;
  s->name = name ? strdup(name) : NULL;
  pthread_mutex_init(&s->lock, NULL);
  return obj;
}

static void key_init(void) { pthread_key_create(&cur_key, NULL); }

static void *current(void) {
  pthread_once(&key_once, key_init);
  return pthread_getspecific(cur_key);
}

os_workgroup_t os_workgroup_create(const char *name, os_workgroup_attr_t attr) {
  (void)attr;
  return wg_new(WG_PLAIN, name);
}

os_workgroup_t os_workgroup_create_with_workgroup(const char *name, os_workgroup_t wg) {
  if (!wg) { errno = EINVAL; return NULL; }
  return wg_new(state(wg)->type, name);
}

os_workgroup_interval_t os_workgroup_interval_create(const char *name, os_clockid_t clock, os_workgroup_attr_t attr) {
  (void)attr;
  (void)clock;   /* am Mac gemessen: jede Uhr-Kennung wird angenommen */
  return wg_new(WG_INTERVAL, name);
}

os_workgroup_parallel_t os_workgroup_parallel_create(const char *name, os_workgroup_attr_t attr) {
  (void)attr;
  return wg_new(WG_PARALLEL, name);
}

int os_workgroup_copy_port(os_workgroup_t wg, mach_port_t *port) { (void)wg; (void)port; return ENOTSUP; }
os_workgroup_t os_workgroup_create_with_port(const char *name, mach_port_t port) { (void)name; (void)port; errno = ENOTSUP; return NULL; }

int os_workgroup_join(os_workgroup_t wg, os_workgroup_join_token_t token) {
  wg_state *s = state(wg);
  if (s->cancelled) return EINVAL;
  if (current()) return EALREADY;
  memset(token, 0, sizeof(*token));
  ((uint32_t *)token)[0] = TOKEN_SIG;
  memcpy((char *)token + 8, &wg, sizeof wg);
  pthread_setspecific(cur_key, wg);
  return 0;
}

void os_workgroup_leave(os_workgroup_t wg, os_workgroup_join_token_t token) {
  void *w = NULL;
  memcpy(&w, (char *)token + 8, sizeof w);
  if (((uint32_t *)token)[0] != TOKEN_SIG || w != wg || current() != wg) abort();   /* wie Apple: fehlerhaftes Token */
  pthread_setspecific(cur_key, NULL);
  ((uint32_t *)token)[0] = 0;
}

int os_workgroup_set_working_arena(os_workgroup_t wg, void *arena, uint32_t max_workers, os_workgroup_working_arena_destructor_t destructor) {
  wg_state *s = state(wg);
  pthread_mutex_lock(&s->lock);
  if (s->arena && s->arena_destructor) s->arena_destructor(s->arena);
  s->arena = arena; s->arena_max = max_workers; s->arena_destructor = destructor;
  pthread_mutex_unlock(&s->lock);
  return 0;
}

void *os_workgroup_get_working_arena(os_workgroup_t wg, os_workgroup_index *index_out) {
  wg_state *s = state(wg);
  if (!s->arena) return NULL;
  if (index_out) *index_out = 0;
  return s->arena;
}

void os_workgroup_cancel(os_workgroup_t wg) { state(wg)->cancelled = 1; }
bool os_workgroup_testcancel(os_workgroup_t wg) { return state(wg)->cancelled != 0; }

int os_workgroup_max_parallel_threads(os_workgroup_t wg, os_workgroup_mpt_attr_t attr) {
  (void)wg; (void)attr;
  extern int pthread_time_constraint_max_parallelism(unsigned long);
  return pthread_time_constraint_max_parallelism(0);
}

/* Intervall: Fehler wie in <os/workgroup_interval.h> beschrieben */
static int interval_check(os_workgroup_interval_t wg) {
  wg_state *s = state(wg);
  if (s->type != WG_INTERVAL) return EINVAL;
  if (current() != (void *)wg) return EPERM;
  if (s->cancelled) return EINVAL;
  return 0;
}

int os_workgroup_interval_start(os_workgroup_interval_t wg, uint64_t start, uint64_t deadline, os_workgroup_interval_data_t data) {
  (void)data;
  int e = interval_check(wg);
  if (e) return e;
  wg_state *s = state(wg);
  if (deadline <= start) return EINVAL;
  pthread_mutex_lock(&s->lock);
  if (s->started) e = EINVAL;
  else { s->started = 1; s->start = start; s->deadline = deadline; }
  pthread_mutex_unlock(&s->lock);
  return e;
}

int os_workgroup_interval_update(os_workgroup_interval_t wg, uint64_t deadline, os_workgroup_interval_data_t data) {
  (void)data;
  int e = interval_check(wg);
  if (e) return e;
  wg_state *s = state(wg);
  pthread_mutex_lock(&s->lock);
  if (!s->started) e = EINVAL;
  else if (deadline <= s->start) e = EINVAL;
  else s->deadline = deadline;
  pthread_mutex_unlock(&s->lock);
  return e;
}

int os_workgroup_interval_finish(os_workgroup_interval_t wg, os_workgroup_interval_data_t data) {
  (void)data;
  int e = interval_check(wg);
  if (e) return e;
  wg_state *s = state(wg);
  pthread_mutex_lock(&s->lock);
  if (!s->started) e = EINVAL; else s->started = 0;
  pthread_mutex_unlock(&s->lock);
  return e;
}
