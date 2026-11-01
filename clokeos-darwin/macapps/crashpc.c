/* libcrashpc: Absturzmelder für Programme auf ClokeOS, die keinen Debugger und keinen Absturzbericht haben.
 *
 *   DYLD_INSERT_LIBRARIES=/usr/lib/libcrashpc.dylib <programm>
 *
 * Bei SIGBUS/SIGSEGV/SIGILL/SIGTRAP/SIGABRT schreibt er auf stderr: Signal, Fehleradresse, pc, lr, fp und einen Rückverfolgungsweg über die
 * Rahmenzeiger (bis 24 Stufen), jede Adresse mit Bibliothek (dladdr) und, wo vorhanden, Symbol samt Versatz. Läuft auf einem eigenen Stapel
 * (sigaltstack), weil der Stapel selbst kaputt sein kann. Nur arm64. Baut macapps/crashpc.sh. */
#define _DARWIN_C_SOURCE 1
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <pthread.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static int logfd = -1;   /* Spiegel in /tmp/crashpc.log: manche Programme (Logic Pro) schließen oder ersetzen stderr */
static void out(const char *s) {
  write(2, s, strlen(s));
  int fd = open("/tmp/crashpc.log", O_WRONLY | O_CREAT | O_APPEND, 0644);   /* jedes Mal neu: Logic schließt fremde Dateikennungen */
  if (fd >= 0) { write(fd, s, strlen(s)); close(fd); }
}
static void hex(uint64_t v) {
  char b[19] = "0x0000000000000000";
  for (int i = 0; i < 16; i++) b[17 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
  out(b);
}
static int rd64(uint64_t a, uint64_t *v) {
  int p[2]; if (pipe(p)) return 0;
  int ok = write(p[1], (const void *)(uintptr_t)a, 8) == 8 && read(p[0], v, 8) == 8;
  close(p[0]); close(p[1]); return ok;
}
static void frame(const char *label, uint64_t a) {
  out("  "); out(label); out(" "); hex(a);
  Dl_info d;
  if (a && dladdr((void *)(uintptr_t)a, &d)) {
    out("  "); out(d.dli_fname ? d.dli_fname : "?");
    if (d.dli_sname) { out(" "); out(d.dli_sname); out(" +"); hex(a - (uint64_t)(uintptr_t)d.dli_saddr); }
    else if (d.dli_fbase) { out(" (Basis "); hex((uint64_t)(uintptr_t)d.dli_fbase); out(", +"); hex(a - (uint64_t)(uintptr_t)d.dli_fbase); out(")"); }
  }
  out("\n");
}
static void handler(int sig, siginfo_t *si, void *ctx) {
  ucontext_t *uc = ctx;
  out("\n[crashpc] Signal "); hex((uint64_t)sig); out(" Fehleradresse "); hex((uint64_t)(uintptr_t)si->si_addr); out("\n");
  uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr, fp = uc->uc_mcontext->__ss.__fp;
  frame("pc", pc); frame("lr", lr);
  /* Register x0..x3: bei einem Absturz in objc_msgSend sind x0 = Empfänger, x1 = Selektor */
  for (int i = 0; i < 29; i++) { out("  x"); hex((uint64_t)i); out(" "); hex(uc->uc_mcontext->__ss.__x[i]); out("\n"); }
  /* x3 (und x2): vielleicht Swift-Metadaten eines generischen Parameters; Typname über die Laufzeit, Fundstelle über dladdr */
  for (int i = 2; i <= 3; i++) {
    uint64_t m = uc->uc_mcontext->__ss.__x[i];
    typedef struct { const char *s; uint64_t n; } pair;
    pair (*gtn)(const void *, int) = (pair (*)(const void *, int))dlsym(RTLD_DEFAULT, "swift_getTypeName");
    if (m > 0x100000000ULL && gtn && !(m & 7)) {
      pair r = gtn((const void *)(uintptr_t)m, 1);
      if (r.s && r.n < 400) { out("  x"); hex((uint64_t)i); out(" als Metadaten: "); write(2, r.s, r.n); if (logfd >= 0) write(logfd, r.s, r.n); out("\n"); }
    }
    uint64_t vwt = 0, w = 0;
    if (m > 0x100000000ULL && !(m & 7) && rd64(m - 8, &vwt)) {
      out("  x"); hex((uint64_t)i); out(" [m-8]="); hex(vwt);
      if (rd64(m, &w)) { out(" [m]="); hex(w); }
      if (rd64(m + 8, &w)) { out(" [m+8]="); hex(w); }
      out("\n"); frame("m", m); frame("vwt", vwt);
      for (int k = 0; k < 12; k++) if (rd64(vwt + 8 * k, &w)) { out("  vwt["); hex((uint64_t)k); out("] "); hex(w); out("\n"); }
    }
    Dl_info d; if (m && dladdr((void *)(uintptr_t)m, &d) && d.dli_sname) { out("  x"); hex((uint64_t)i); out(" bei Symbol "); out(d.dli_sname); out("\n"); }
  }
  if (uc->uc_mcontext->__ss.__x[1] > 0x1000) { out("  sel "); out(sel_getName((SEL)(uintptr_t)uc->uc_mcontext->__ss.__x[1])); out("\n"); }
  for (int i = 0; i < 24 && fp && !(fp & 7); i++) {   /* [fp] = voriger fp, [fp+8] = Rücksprung */
    uint64_t *f = (uint64_t *)(uintptr_t)fp;
    if (fp < 0x1000) break;
    uint64_t ret = f[1];
    if (!ret) break;
    frame("bt", ret);
    uint64_t next = f[0];
    if (next <= fp) break;
    fp = next;
  }
  _exit(128 + sig);
}

/* Mach-Ausnahmeport für die ganze Aufgabe, bedient von einem eigenen Faden mit gutem Stapel: fängt auch Abstürze, bei denen der Kernel den
 * Signalrahmen nicht mehr auf den kaputten Stapel legen kann (er beendet den Prozess dann mit SIGILL, ohne dass ein Signalhandler läuft). */
typedef struct {
  mach_msg_header_t head; mach_msg_body_t body; mach_msg_port_descriptor_t thread, task;
  char ndr[8]; int exception; int codeCnt; int64_t code[2];
  char pad[256];
} exc_msg_t;
static void *exc_thread(void *arg) {
  mach_port_t port = (mach_port_t)(uintptr_t)arg;
  for (;;) {
    exc_msg_t m; memset(&m, 0, sizeof m);
    if (mach_msg(&m.head, MACH_RCV_MSG, 0, sizeof m, port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS) continue;
    out("\n[crashpc] Mach-Ausnahme "); hex((uint64_t)m.exception); out(" Code "); hex((uint64_t)m.code[0]); out(" "); hex((uint64_t)m.code[1]); out("\n");
    arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(m.thread.name, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
      frame("pc", st.__pc); frame("lr", st.__lr);
      out("  sp "); hex(st.__sp); out("  fp "); hex(st.__fp); out("\n");
      for (int i = 0; i < 29; i++) { out("  x"); hex((uint64_t)i); out(" "); hex(st.__x[i]); out("\n"); }
      out("  sel "); out(sel_getName((SEL)(uintptr_t)st.__x[1])); out("\n");
      out("  Empfänger-Klasse "); out(object_getClassName((id)(uintptr_t)st.__x[0])); out("\n");
      uint64_t fp = st.__fp;
      for (int i = 0; i < 400 && fp && !(fp & 7) && fp > 0x1000; i++) {
        uint64_t ret, next;
        if (!rd64(fp + 8, &ret) || !rd64(fp, &next) || !ret) break;
        frame("bt", ret);
        if (next <= fp) break;
        fp = next;
      }
    }
    _exit(139);
  }
  return 0;
}
static void install_exception_port(void) {
  mach_port_t port;
  if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) return;
  mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
  if (task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT,
                               port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64) != KERN_SUCCESS) return;
  pthread_t th; pthread_create(&th, 0, exc_thread, (void *)(uintptr_t)port);
}
__attribute__((constructor)) static void init(void) {
  install_exception_port();
  logfd = open("/tmp/crashpc.log", 0x0001 | 0x0200 | 0x0008, 0644);   /* O_WRONLY|O_CREAT|O_APPEND */
  if (logfd >= 0 && logfd < 100) { int hi = fcntl(logfd, 0 /*F_DUPFD*/, 100); if (hi >= 0) { close(logfd); logfd = hi; } }
  static char stk[1 << 16];
  stack_t ss = { .ss_sp = stk, .ss_size = sizeof stk, .ss_flags = 0 };
  sigaltstack(&ss, 0);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  int sigs[] = { SIGBUS, SIGSEGV, SIGILL, SIGTRAP, SIGABRT };
  for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, 0);
}

/* Programme wie Logic Pro setzen eigene Signalbehandlungen (und schlucken so den Absturzbericht); hier bleiben unsere für die
 * Absturzsignale. Außerdem melden exit/_exit/abort samt Rückverfolgung, wer das Programm beendet. */
static void bt_here(const char *why, int arg) {
  out("\n[crashpc] "); out(why); out(" "); hex((uint64_t)arg); out("\n");
  uint64_t fp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
  for (int i = 0; i < 24 && fp && !(fp & 7) && fp > 0x1000; i++) {
    uint64_t *f = (uint64_t *)(uintptr_t)fp; uint64_t ret = f[1], next = f[0];
    if (!ret) break;
    frame("bt", ret);
    if (next <= fp) break;
    fp = next;
  }
}
static int mysigaction(int sig, const struct sigaction *act, struct sigaction *old) {
  if (sig == SIGBUS || sig == SIGSEGV || sig == SIGILL || sig == SIGTRAP || sig == SIGABRT) {
    if (act) { out("[crashpc] sigaction("); hex((uint64_t)sig); out(") ignoriert\n"); }
    return sigaction(sig, 0, old);
  }
  return sigaction(sig, act, old);
}
static void (*mysignal(int sig, void (*h)(int)))(int) {
  if (sig == SIGBUS || sig == SIGSEGV || sig == SIGILL || sig == SIGTRAP || sig == SIGABRT) { out("[crashpc] signal() ignoriert\n"); return SIG_DFL; }
  return signal(sig, h);
}
static void myexit(int c) { bt_here("exit", c); exit(c); }
static void my_exit(int c) { bt_here("_exit", c); _exit(c); }
static void myabort(void) { bt_here("abort", 0); abort(); }

/* Beendigung mit Begründung (Kernel-"Corpse"): abort_with_reason/payload, terminate_with_reason/payload, exit_with_reason (libsystem_kernel) */
extern void abort_with_reason(uint32_t ns, uint64_t code, const char *reason, uint64_t flags);
static void rsn(const char *what, uint32_t ns, uint64_t code, const char *reason) {
  bt_here(what, (int)ns); out("  code "); hex(code); out("  Begründung: "); out(reason ? reason : "(keine)"); out("\n");
}
static void my_abort_reason(uint32_t ns, uint64_t code, const char *reason, uint64_t flags) { rsn("abort_with_reason ns", ns, code, reason); abort_with_reason(ns, code, reason, flags); }
static int mykill(int pid, int sig) { if (pid == getpid() || pid == 0) bt_here("kill(self) sig", sig); return kill(pid, sig); }
static int myraise(int sig) { bt_here("raise sig", sig); return raise(sig); }
static int mypthread_kill(pthread_t th, int sig) { bt_here("pthread_kill sig", sig); return pthread_kill(th, sig); }
#define INTERPOSE(r, o) { (const void *)(r), (const void *)(o) }
__attribute__((used)) static const struct { const void *r, *o; } crashpc_interpose[] __attribute__((section("__DATA,__interpose"))) = {
  INTERPOSE(mysigaction, sigaction), INTERPOSE(mysignal, signal), INTERPOSE(myexit, exit), INTERPOSE(my_exit, _exit), INTERPOSE(myabort, abort),
  INTERPOSE(my_abort_reason, abort_with_reason), INTERPOSE(mykill, kill), INTERPOSE(myraise, raise), INTERPOSE(mypthread_kill, pthread_kill) };
