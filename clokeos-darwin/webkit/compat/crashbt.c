/* Diagnose: per DYLD_INSERT_LIBRARIES laden; gibt bei SIGTRAP/SIGSEGV/SIGBUS/SIGABRT einen Backtrace aus. */
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#include <sys/ucontext.h>
#include <dlfcn.h>

/* Alle anderen Threads: anhalten, Register lesen, Frame-Zeiger-Kette ablaufen */
static void dump_threads(void)
{
    thread_act_array_t ths; mach_msg_type_number_t n;
    if (task_threads(mach_task_self(), &ths, &n) != KERN_SUCCESS) return;
    mach_port_t me = mach_thread_self();
    char buf[256]; Dl_info di;
    { int k = snprintf(buf, sizeof buf, "=== threads=%u\n", n); write(2, buf, k); }
    for (unsigned t = 0; t < n; t++) {
        if (ths[t] == me) continue;
        thread_suspend(ths[t]);
        arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(ths[t], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
            int k = snprintf(buf, sizeof buf, "=== thread %u\n", t); write(2, buf, k);
            uintptr_t pc = arm_thread_state64_get_pc(st), fp = arm_thread_state64_get_fp(st);
            for (int i = 0; i < 24 && pc; i++) {
                if (dladdr((void*)pc, &di) && di.dli_fname)
                    k = snprintf(buf, sizeof buf, "  T%u#%d %p base=%p %s (%s)\n", t, i, (void*)pc, di.dli_fbase, di.dli_sname ? di.dli_sname : "?", di.dli_fname);
                else k = snprintf(buf, sizeof buf, "  T%u#%d %p\n", t, i, (void*)pc);
                write(2, buf, k);
                if (!fp || (fp & 7)) break;
                pc = ((uintptr_t*)fp)[1]; fp = ((uintptr_t*)fp)[0];
            }
        }
        thread_resume(ths[t]);
    }
}

static void handler(int sig, siginfo_t* info, void* uc)
{
    int saved_errno = errno;
    void* frames[64];
    char buf[256];
    int n = snprintf(buf, sizeof buf, "\n*** crashbt: signal %d at %p (pid %d) errno=%d\n", sig, info->si_addr, getpid(), saved_errno);
    write(2, buf, n);
    ucontext_t* c = uc;
    void* pc = (void*)c->uc_mcontext->__ss.__pc;
    void* lr = (void*)c->uc_mcontext->__ss.__lr;
    Dl_info di;
    frames[0] = pc;
    frames[1] = lr;
    int cnt = backtrace(frames + 2, 60) + 2;
    for (int i = 0; i < cnt; i++) {
        if (dladdr(frames[i], &di) && di.dli_sname)
            n = snprintf(buf, sizeof buf, "  #%d %p base=%p %s+%ld (%s)\n", i, frames[i], di.dli_fbase, di.dli_sname, (long)((char*)frames[i] - (char*)di.dli_saddr), di.dli_fname ? di.dli_fname : "?");
        else
            n = snprintf(buf, sizeof buf, "  #%d %p base=%p (%s)\n", i, frames[i], di.dli_fbase, di.dli_fname ? di.dli_fname : "?");
        write(2, buf, n);
    }
    if (sig == SIGINFO) {
        dump_threads();
        return;
    }
    _exit(128 + sig);
}

/* Wachhund: CLOKEOS_WATCHDOG=<Sekunden> -> nach dieser Zeit (und dann alle 10 s, dreimal) alle Threads ausgeben, auch wenn der Prozess im Kernel hängt */
static void* watchdog(void* arg)
{
    unsigned secs = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < 3; i++) {
        sleep(i ? 10 : secs);
        const char m[] = "\n*** crashbt watchdog\n";
        write(2, m, sizeof m - 1);
        dump_threads();
    }
    return NULL;
}

__attribute__((constructor)) static void install(void)
{
    const char* w = getenv("CLOKEOS_WATCHDOG");
    if (w && atoi(w) > 0) {
        pthread_t t;
        pthread_create(&t, NULL, watchdog, (void*)(uintptr_t)atoi(w));
        pthread_detach(t);
    }
    struct sigaction sa = { 0 };
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    signal(SIGSYS, SIG_IGN); // kein SysV-shm auf PureDarwin: shmget soll mit ENOSYS scheitern statt zu trappen
    int sigs[] = { SIGINFO, SIGTRAP, SIGSEGV, SIGBUS, SIGABRT, SIGILL };
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++)
        sigaction(sigs[i], &sa, NULL);
}
