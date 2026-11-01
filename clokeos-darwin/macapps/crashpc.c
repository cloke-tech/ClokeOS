/* libcrashpc: Absturzmelder für Programme auf ClokeOS, die keinen Debugger und keinen Absturzbericht haben.
 *
 *   DYLD_INSERT_LIBRARIES=/usr/lib/libcrashpc.dylib <programm>
 *
 * Bei SIGBUS/SIGSEGV/SIGILL/SIGTRAP/SIGABRT schreibt er auf stderr: Signal, Fehleradresse, pc, lr, fp und einen Rückverfolgungsweg über die
 * Rahmenzeiger (bis 24 Stufen), jede Adresse mit Bibliothek (dladdr) und, wo vorhanden, Symbol samt Versatz. Läuft auf einem eigenen Stapel
 * (sigaltstack), weil der Stapel selbst kaputt sein kann. Nur arm64. Baut macapps/crashpc.sh. */
#define _DARWIN_C_SOURCE 1
#include <dlfcn.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

static void out(const char *s) { write(2, s, strlen(s)); }
static void hex(uint64_t v) {
  char b[19] = "0x0000000000000000";
  for (int i = 0; i < 16; i++) b[17 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
  write(2, b, 18);
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
  for (int i = 0; i < 4; i++) { out("  x"); hex((uint64_t)i); out(" "); hex(uc->uc_mcontext->__ss.__x[i]); out("\n"); }
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
__attribute__((constructor)) static void init(void) {
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
