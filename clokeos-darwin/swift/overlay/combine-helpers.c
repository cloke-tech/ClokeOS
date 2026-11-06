/* Ersatz für OpenCombines COpenCombineHelpers.cpp (das std::recursive_mutex aus libc++ braucht, das PureDarwin nicht hat): reines C mit pthread.
 * Gleiche Funktionsnamen und Typen wie in include/COpenCombineHelpers.h. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct OpenCombineUnfairLock { void *opaque; } OpenCombineUnfairLock;
typedef struct OpenCombineUnfairRecursiveLock { void *opaque; } OpenCombineUnfairRecursiveLock;

uint64_t opencombine_next_combine_identifier(void) {
  static uint64_t next = 1;
  return __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
}
OpenCombineUnfairLock opencombine_unfair_lock_alloc(void) {
  pthread_mutex_t *m = malloc(sizeof *m); pthread_mutex_init(m, 0);
  return (OpenCombineUnfairLock){ m };
}
OpenCombineUnfairRecursiveLock opencombine_unfair_recursive_lock_alloc(void) {
  pthread_mutex_t *m = malloc(sizeof *m);
  pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
  return (OpenCombineUnfairRecursiveLock){ m };
}
void opencombine_unfair_lock_lock(OpenCombineUnfairLock l) { pthread_mutex_lock(l.opaque); }
void opencombine_unfair_lock_unlock(OpenCombineUnfairLock l) { pthread_mutex_unlock(l.opaque); }
void opencombine_unfair_lock_assert_owner(OpenCombineUnfairLock l) { (void)l; }
void opencombine_unfair_recursive_lock_lock(OpenCombineUnfairRecursiveLock l) { pthread_mutex_lock(l.opaque); }
void opencombine_unfair_recursive_lock_unlock(OpenCombineUnfairRecursiveLock l) { pthread_mutex_unlock(l.opaque); }
void opencombine_unfair_lock_dealloc(OpenCombineUnfairLock l) { pthread_mutex_destroy(l.opaque); free(l.opaque); }
void opencombine_unfair_recursive_lock_dealloc(OpenCombineUnfairRecursiveLock l) { pthread_mutex_destroy(l.opaque); free(l.opaque); }
void opencombine_stop_in_debugger(void) { __builtin_trap(); }
