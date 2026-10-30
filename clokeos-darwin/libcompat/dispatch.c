/* libclokeos_compat, Teil 7: libdispatch-Funktionen und os_signpost. PureDarwins libSystem enthält
 * libdispatch 1292, aber nur mit der Exportliste von dispatch_async, dispatch_sync, Gruppen,
 * Quellen usw.; dispatch_barrier_async, dispatch_*_specific und dispatch_block_cancel sind beim
 * Linken weggefallen. Hier der Ersatz:
 *   - dispatch_barrier_async nutzt libdispatchs internes _dispatch_barrier_async_detached_f; build.sh
 *     rechnet dessen Abstand zu dispatch_barrier_sync aus der geladenen libSystem aus
 *     (-DBARRIER_DETACHED_OFFSET), die Adresse ergibt sich zur Laufzeit. Ohne Offset
 *     (anderes libSystem) bleibt dispatch_async als Ersatz, das auf seriellen Queues dasselbe ist.
 *   - dispatch_queue_set_specific/dispatch_get_specific mit einer Tabelle außerhalb der Queue; der
 *     Destruktor läuft beim Ersetzen/Löschen, nicht beim Freigeben der Queue.
 *   - dispatch_block_cancel setzt DBF_CANCELED im privaten Datenblock, den dispatch_block_create
 *     hinter den Block-Kopf legt (Magic 0xD159B10C). */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <Block.h>
#include <pthread.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

typedef void (*detached_fn)(void *dq, void *ctxt, void (*func)(void *));
static detached_fn detached;
static pthread_once_t detached_once = PTHREAD_ONCE_INIT;

static void detached_init(void) {
#ifdef BARRIER_DETACHED_OFFSET
  Dl_info info;
  if (dladdr((void *)dispatch_barrier_sync, &info) && info.dli_saddr)
    detached = (detached_fn)((char *)info.dli_saddr + (long)BARRIER_DETACHED_OFFSET);
#endif
}

static void run_and_release(void *ctxt) {
  dispatch_block_t b = ctxt;
  b();
  Block_release(b);
}

void dispatch_barrier_async_f(dispatch_queue_t q, void *ctxt, dispatch_function_t fn) {
  pthread_once(&detached_once, detached_init);
  if (detached) detached(q, ctxt, fn);
  else dispatch_async_f(q, ctxt, fn);
}

void dispatch_barrier_async(dispatch_queue_t q, dispatch_block_t block) {
  dispatch_barrier_async_f(q, Block_copy(block), run_and_release);
}

void dispatch_barrier_sync_f(dispatch_queue_t q, void *ctxt, dispatch_function_t fn) {
  dispatch_barrier_sync(q, ^{ fn(ctxt); });
}

/* ---- aktuelle Queue: TSD-Platz 20 (dispatch_queue_key); Prüfung in docs/logic-libsystem.md ---- */
static void *current_queue(void) {
#if defined(__arm64__)
  void **tsd;
  __asm__("mrs %0, tpidrro_el0" : "=r"(tsd));
  return tsd[20];
#else
  void *q;
  __asm__("movq %%gs:%c1, %0" : "=r"(q) : "i"(20 * 8));
  return q;
#endif
}

dispatch_queue_t dispatch_get_current_queue(void) {
  void *q = current_queue();
  return q ? q : dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
}

/* ---- specific ---- */
typedef struct spec { struct spec *next; void *queue; const void *key; void *ctxt; dispatch_function_t dtor; } spec;
static spec *specs;
static pthread_mutex_t specs_lock = PTHREAD_MUTEX_INITIALIZER;

void dispatch_queue_set_specific(dispatch_queue_t q, const void *key, void *ctxt, dispatch_function_t dtor) {
  void *old_ctxt = NULL;
  dispatch_function_t old_dtor = NULL;
  pthread_mutex_lock(&specs_lock);
  spec **pp = &specs;
  while (*pp && !((*pp)->queue == (void *)q && (*pp)->key == key)) pp = &(*pp)->next;
  if (*pp) {
    spec *s = *pp;
    old_ctxt = s->ctxt; old_dtor = s->dtor;
    if (ctxt) { s->ctxt = ctxt; s->dtor = dtor; }
    else { *pp = s->next; free(s); }
  } else if (ctxt) {
    spec *s = malloc(sizeof *s);
    s->next = specs; s->queue = q; s->key = key; s->ctxt = ctxt; s->dtor = dtor;
    specs = s;
  }
  pthread_mutex_unlock(&specs_lock);
  if (old_dtor && old_ctxt) old_dtor(old_ctxt);
}

static void *lookup(void *q, const void *key) {
  void *r = NULL;
  pthread_mutex_lock(&specs_lock);
  for (spec *s = specs; s; s = s->next) if (s->queue == q && s->key == key) { r = s->ctxt; break; }
  pthread_mutex_unlock(&specs_lock);
  return r;
}

void *dispatch_queue_get_specific(dispatch_queue_t q, const void *key) { return lookup(q, key); }

/* sucht von der aktuellen Queue aus über deren Ziel-Queues (do_targetq, Offset 24 im Objekt) */
void *dispatch_get_specific(const void *key) {
  void *q = current_queue();
  for (int depth = 0; q && depth < 16; depth++) {
    void *r = lookup(q, key);
    if (r) return r;
    q = *(void **)((char *)q + 24);
  }
  return NULL;
}

/* ---- Blöcke ---- */
#define BLOCK_MAGIC 0xD159B10CUL
#define DBF_CANCELED 1u

/* dispatch_block_flags_t ist ein unsigned long: die Atomic-Flags liegen bei +16 (gelesen aus
 * _dispatch_block_async_invoke2 in PureDarwins libSystem) */
typedef struct { unsigned long magic; unsigned long flags; unsigned volatile atomic_flags; } block_data;

static block_data *block_data_of(dispatch_block_t b) {
  block_data *d = (block_data *)((char *)b + 32);   /* hinter struct Block_layout */
  return d->magic == BLOCK_MAGIC ? d : NULL;
}

void dispatch_block_cancel(dispatch_block_t b) {
  block_data *d = block_data_of(b);
  if (d) __atomic_or_fetch(&d->atomic_flags, DBF_CANCELED, __ATOMIC_RELAXED);
}

long dispatch_block_testcancel(dispatch_block_t b) {
  block_data *d = block_data_of(b);
  return d && (__atomic_load_n(&d->atomic_flags, __ATOMIC_RELAXED) & DBF_CANCELED);
}

/* QoS und relative Priorität werden nicht an Threads weitergegeben (PureDarwin hat keine QoS-Pools) */
dispatch_block_t dispatch_block_create_with_qos_class(dispatch_block_flags_t flags, qos_class_t qos, int rel, dispatch_block_t block) {
  (void)qos; (void)rel;
  return dispatch_block_create(flags, block);
}

/* ---- os_signpost: nie eingeschaltet, Aufrufe werden verworfen ---- */
void _os_signpost_emit_with_name_impl(void *dso, void *log, uint8_t type, uint64_t id, const char *name, const char *fmt, uint8_t *buf, uint32_t size) {
  (void)dso; (void)log; (void)type; (void)id; (void)name; (void)fmt; (void)buf; (void)size;
}
_Bool os_signpost_enabled(void *log) { (void)log; return 0; }
