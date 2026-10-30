// ucrun – x86_64-Mach-O auf arm64 ausführen, mit Unicorn (QEMU-TCG als Bibliothek) als CPU-Kern.
//
// Gastspeicher liegt in Unicorns eigenem Adressraum (4-KiB-Seiten, jede Gastadresse frei): Pagezero, PIE-Verschiebung
// und die 16-KiB-Seite des Hosts spielen keine Rolle. Die Systemaufrufe des Gastes (BSD-Klasse 2; Nummern und
// LP64-Strukturen sind auf Darwin für x86_64 und arm64 gleich) werden mit einer kleinen Tabelle (Zeiger rein/raus)
// an den Host weitergereicht, Fehler wie bei x86_64-Darwin (Carry-Flag + errno in rax).
//
//   ucrun <x86_64-Mach-O> [Argumente]     (UCRUN_TRACE=1: Systemaufrufe zeigen, UCRUN_STATS=1: Zeit/Zähler am Ende)
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unicorn/unicorn.h>
#include "jit/jit.h"     // JIT (UCRUN_JIT=1): siehe jit/ und Studie Abschnitt 16
#include "jit/ucsync.h"

#define STACK_TOP 0x7ffee0100000ULL
#define STACK_SIZE 0x800000ULL
#define MMAP_BASE 0x600000000000ULL
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

extern char **environ;
static uc_engine *uc;
static int jit_on;  // UCRUN_JIT=1 und statisches Programm: Hauptthread läuft im Block-JIT, Gastspeicher mit festem Versatz (jit/jit.c)
static int trace, warn;  // trace: UCRUN_TRACE; warn: UCRUN_WARN (nicht umgesetzte Systemaufrufe melden)

// --- Gastspeicher: Host-Speicher, in jede Unicorn-Maschine (ein Gast-Thread = eine Maschine) per uc_mem_map_ptr eingeblendet ---
typedef struct { uint64_t g, n; uint8_t *h; } Reg;
static Reg regs[1024]; static int nregs;
static uc_engine *engs[64]; static int neng;
static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static bool on_unmapped(uc_engine *u, uc_mem_type t, uint64_t addr, int size, int64_t val, void *ud);
static void do_syscall(uc_engine *u, void *ud);

static void unmap_all(uint64_t g, uint64_t n) {
	pthread_mutex_lock(&big);
	for (int i = 0; i < neng; i++) uc_mem_unmap(engs[i], g, n);  // Fehler (nichts abgebildet) sind egal
	for (int i = 0, cnt = nregs; i < cnt; i++) {
		Reg r = regs[i]; uint64_t b = r.g + r.n, ue = g + n;
		if (r.g >= ue || b <= g) continue;
			if (jit_on) { uint64_t fa = r.g > g ? r.g : g, fe = b < ue ? b : ue; jit_guest_free(fa, fe - fa); jit_invalidate(fa, fe - fa); }  // JIT
		if (r.g < g) { regs[i].n = g - r.g; if (b > ue && nregs < 1024) regs[nregs++] = (Reg){ue, b - ue, r.h + (ue - r.g)}; }
		else if (b > ue) { regs[i] = (Reg){ue, b - ue, r.h + (ue - r.g)}; }
		else regs[i].n = 0;
	}
	int k = 0; for (int i = 0; i < nregs; i++) if (regs[i].n) regs[k++] = regs[i]; nregs = k;
	pthread_mutex_unlock(&big);
}
static uc_err map_all(uint64_t g, uint64_t n) {
	pthread_mutex_lock(&big);
	uint8_t *h = jit_on ? jit_guest_alloc(g, n) : mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);  // JIT: Host-Adresse = Gast + Versatz
	uc_err e = (h == MAP_FAILED || !h) ? UC_ERR_NOMEM : UC_ERR_OK;
	for (int i = 0; !e && i < neng; i++) e = uc_mem_map_ptr(engs[i], g, n, UC_PROT_ALL, h);
	if (!e && nregs < 1024) regs[nregs++] = (Reg){g, n, h};
	pthread_mutex_unlock(&big);
	return e;
}
static uc_engine *new_engine(void) {
	uc_engine *e; if (uc_open(UC_ARCH_X86, UC_MODE_64, &e)) return 0;
	pthread_mutex_lock(&big);
	for (int i = 0; i < nregs; i++) uc_mem_map_ptr(e, regs[i].g, regs[i].n, UC_PROT_ALL, regs[i].h);
	engs[neng++] = e;
	pthread_mutex_unlock(&big);
	uc_hook hm, h; uc_hook_add(e, &hm, UC_HOOK_MEM_UNMAPPED, on_unmapped, 0, 1, 0); uc_hook_add(e, &h, UC_HOOK_INSN, do_syscall, 0, 1, 0, UC_X86_INS_SYSCALL);
	return e;
}
static uint32_t *g2h32(uint64_t a) {
	uint32_t *p = 0; pthread_mutex_lock(&big);
	for (int i = nregs - 1; i >= 0; i--) if (a >= regs[i].g && a + 4 <= regs[i].g + regs[i].n) { p = (uint32_t *)(regs[i].h + (a - regs[i].g)); break; }
	pthread_mutex_unlock(&big); return p;
}

// --- Threads (bsdthread_create) und ulock-Warteschlange ---
static uint64_t thread_entry, tsd_offset, mts_offset;  // von bsdthread_register: Einsprung neuer Threads (libpthread)
static pthread_mutex_t wmx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t wcv = PTHREAD_COND_INITIALIZER;
typedef struct { uc_engine *u; uint64_t pc; uint64_t ring[64]; unsigned n; volatile int done; uint64_t rsp, rdi, rsi, rdx, rcx, r8, r9, gs; struct Saved *ss; int sd; uint64_t fault_addr; volatile int resume; uint64_t resume_rip; uint64_t tid; } Th;
static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t gc = PTHREAD_COND_INITIALIZER; static unsigned g_next, g_serving;  // fairer Ticket-Sperre
static void gil_lock(void) { pthread_mutex_lock(&gm); unsigned my = g_next++; while (g_serving != my) pthread_cond_wait(&gc, &gm); pthread_mutex_unlock(&gm); }
static void gil_unlock(void) { pthread_mutex_lock(&gm); g_serving++; pthread_cond_broadcast(&gc); pthread_mutex_unlock(&gm); }  // immer nur eine Maschine rechnet (Unicorn-Speicherabbild ist nicht thread-sicher); Zeitscheiben + Blockieren geben ihn frei
static Th *eth[64];
static Th main_th;
static uint64_t next_tid = 2;  // thread_selfid je Gast-Thread (libpthread erkennt damit EDEADLK beim rwlock/Mutex-Besitzer); der Haupt-Thread hat 1
static uint64_t slice_us = 1000000;  // Zeitscheibe 1 s: Wechsel praktisch nur an blockierenden Aufrufen, damit Mutexe nie umkämpft sind (psynch ist nicht nachgebildet)
#define BLOCKING_BEGIN gil_unlock()
#define BLOCKING_END gil_lock()
// Läuft in Zeitscheiben (ca. 1 ms), bis der Thread endet; liefert den letzten Fehler

// --- Signale ---
// Der Gast kennt sigaction (46), sigprocmask (48/329), sigreturn (184), kill, Zeitgeber. Echte Signale (kill, alarm, SIGCHLD ...) fängt ein Host-Handler ab
// und merkt sie vor; zugestellt wird zwischen zwei Zeitscheiben im Gast-Thread (Rahmen auf dem Gast-Stapel, Einsprung in die __sigtramp des Gastes).
// Fehler im Gast (ungültiger Zugriff, Division durch 0, ud2) werden in SIGSEGV/SIGFPE/SIGILL umgesetzt, wenn der Gast einen Handler hat.
typedef struct Saved { uint64_t r[16], rip, eflags; uint32_t oldmask; uint8_t xmm[16][16]; } Saved;
static struct { uint64_t handler, tramp; uint32_t mask, flags; } gsa[32];
static volatile uint32_t sig_pending; static uint32_t gmask;
static const int gregs[16] = {UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_RBX, UC_X86_REG_RSP, UC_X86_REG_RBP, UC_X86_REG_RSI, UC_X86_REG_RDI,
	UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15};
static void hsig(int s);
static Th *th_of(uc_engine *u);
static void wake_engines(void);

static int fault_signal(uc_engine *u, uc_err e, uint64_t rip) {
	switch (e) {
	case UC_ERR_READ_UNMAPPED: case UC_ERR_WRITE_UNMAPPED: case UC_ERR_FETCH_UNMAPPED: case UC_ERR_READ_PROT: case UC_ERR_WRITE_PROT: case UC_ERR_FETCH_PROT: return 11;
	case UC_ERR_INSN_INVALID: return 4;
	case UC_ERR_EXCEPTION: {
		uint8_t b[16] = {0}; uc_mem_read(u, rip, b, 16); int i = 0;
		while (i < 4 && ((b[i] & 0xf0) == 0x40 || b[i] == 0x66 || b[i] == 0xf2 || b[i] == 0xf3)) i++;
		if ((b[i] == 0xf6 || b[i] == 0xf7) && ((b[i + 1] >> 3) & 7) >= 6) return 8;  // div/idiv
		if (b[i] == 0xcc) return 5;
		return 11;
	}
	default: return 0;
	}
}
// Rahmen für Signal sig auf dem Gast-Stapel bauen und in die Trampolin-Funktion des Gastes springen
static void push_signal(Th *t, int sig, uint64_t addr) {
	uc_engine *u = t->u; uint64_t r[16], rip, fl;
	for (int i = 0; i < 16; i++) uc_reg_read(u, gregs[i], &r[i]);
	uc_reg_read(u, UC_X86_REG_RIP, &rip); uc_reg_read(u, UC_X86_REG_EFLAGS, &fl);
	if (!t->ss) t->ss = calloc(16, sizeof(Saved));
	Saved *sv = &t->ss[t->sd++ & 15]; memcpy(sv->r, r, sizeof r); sv->rip = rip; sv->eflags = fl; sv->oldmask = gmask;
	for (int i = 0; i < 16; i++) uc_reg_read(u, UC_X86_REG_XMM0 + i, sv->xmm[i]);
	uint64_t sp = r[4] - 128 - 1024; uint64_t uctx = sp; sp -= 128; uint64_t info = sp; sp &= ~15ULL; info = sp; sp -= 8;
	uint8_t z[1152] = {0}; uc_mem_write(u, info, z, 1152 > 128 + 1024 ? 128 + 1024 : 1152);
	uint32_t hdr[3] = {(uint32_t)sig, 0, 1}; uc_mem_write(u, info, hdr, 12); uc_mem_write(u, info + 24, &addr, 8);
	sp -= 0; uint64_t ret0 = 0; sp = (info - 16) & ~15ULL; sp -= 8; uc_mem_write(u, sp, &ret0, 8);
	uint64_t style = (gsa[sig].flags & 0x40) ? 30 : 1, h = gsa[sig].handler, sg = sig;
	uc_reg_write(u, UC_X86_REG_RDI, &h); uc_reg_write(u, UC_X86_REG_RSI, &style); uc_reg_write(u, UC_X86_REG_RDX, &sg);
	uc_reg_write(u, UC_X86_REG_RCX, &info); uc_reg_write(u, UC_X86_REG_R8, &uctx); uc_reg_write(u, UC_X86_REG_RSP, &sp);
	uc_reg_write(u, UC_X86_REG_RIP, &gsa[sig].tramp);
	if (trace) fprintf(stderr, "ucrun: Signal %d: gespeichert rip %#llx rsp %#llx rax %#llx, Rahmen rsp %#llx\n", sig, (unsigned long long)rip, (unsigned long long)r[4], (unsigned long long)r[0], (unsigned long long)sp);
	gmask |= gsa[sig].mask | ((gsa[sig].flags & 0x10) ? 0 : (1u << (sig - 1)));
}
// Zwischen Zeitscheiben: wartende, nicht gesperrte Signale zustellen. true = ein Rahmen wurde gebaut
static int deliver_pending(Th *t, uint64_t *pc) {
	uint32_t p = __atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1);
	for (int s = 1; s < 32; s++) if (p & (1u << s)) {
		__atomic_and_fetch(&sig_pending, ~(1u << s), __ATOMIC_SEQ_CST);
		if (gsa[s].handler > 1 && gsa[s].tramp) { push_signal(t, s, 0); uc_reg_read(t->u, UC_X86_REG_RIP, pc); return 1; }
	}
	return 0;
}
#include "jit/ucglue.inc"
static uc_err run_engine(Th *t) {
	if (jit_on && t == &main_th) return jit_run_loop(t);  // JIT
	uint64_t pc = t->pc; uc_err e = UC_ERR_OK;
	for (;;) {
		gil_lock();
		deliver_pending(t, &pc);
		e = uc_emu_start(t->u, pc, 0, slice_us, 0);
		uc_reg_read(t->u, UC_X86_REG_RIP, &pc);
		if (t->resume) { pc = t->resume_rip; t->resume = 0; uc_reg_write(t->u, UC_X86_REG_RIP, &pc); }
		if (e && !t->done) {  // Fehler im Gast -> Signal, wenn der Gast einen Handler dafür hat
			int sg = fault_signal(t->u, e, pc);
			if (sg && gsa[sg].handler > 1 && gsa[sg].tramp && !(gmask & (1u << (sg - 1)))) { push_signal(t, sg, t->fault_addr); uc_reg_read(t->u, UC_X86_REG_RIP, &pc); e = UC_ERR_OK; }
		}
		gil_unlock();
		if (e || t->done) return e;
		sched_yield();
	}
}
static void on_code(uc_engine *u, uint64_t addr, uint32_t size, void *ud) { Th *t = ud; t->ring[t->n++ & 63] = addr; }
static void *thread_main(void *p) {
	Th *t = p;
	// Maschine im neuen Host-Thread anlegen (Apple-JIT-Speicher ist an den Thread gebunden, der ihn anlegt)
	gil_lock(); t->u = new_engine();
	for (int q = 0; q < 64; q++) if (!eth[q]) { eth[q] = t; break; }
	if (getenv("UCRUN_RING")) { uc_hook ch; uc_hook_add(t->u, &ch, UC_HOOK_CODE, on_code, t, 1, 0); }
	{ uc_x86_msr m = {0xC0000101, t->gs}; uc_reg_write(t->u, UC_X86_REG_MSR, &m); }
	uc_reg_write(t->u, UC_X86_REG_RSP, &t->rsp); uc_reg_write(t->u, UC_X86_REG_RDI, &t->rdi); uc_reg_write(t->u, UC_X86_REG_RSI, &t->rsi);
	uc_reg_write(t->u, UC_X86_REG_RDX, &t->rdx); uc_reg_write(t->u, UC_X86_REG_RCX, &t->rcx); uc_reg_write(t->u, UC_X86_REG_R8, &t->r8); uc_reg_write(t->u, UC_X86_REG_R9, &t->r9);
	gil_unlock();
	uc_err e = run_engine(t);
	if (e) { uint64_t rip; uc_reg_read(t->u, UC_X86_REG_RIP, &rip); { uint8_t b[16] = {0}; uc_mem_read(t->u, rip, b, 16); fprintf(stderr, "ucrun: Thread: %s bei rip %#llx, Bytes:", uc_strerror(e), (unsigned long long)rip); for (int q = 0; q < 16; q++) fprintf(stderr, " %02x", b[q]); fputc(10, stderr); if (getenv("UCRUN_RING")) { fprintf(stderr, "  letzte Befehle:"); for (unsigned q = t->n > 40 ? t->n - 40 : 0; q < t->n; q++) fprintf(stderr, " %llx", (unsigned long long)t->ring[q & 63]); fputc(10, stderr); } uint64_t r1, r2, r3; uc_reg_read(t->u, UC_X86_REG_RDI, &r1); uc_reg_read(t->u, UC_X86_REG_RAX, &r2); uc_reg_read(t->u, UC_X86_REG_RSP, &r3); fprintf(stderr, "  rdi %#llx rax %#llx rsp %#llx\n", (unsigned long long)r1, (unsigned long long)r2, (unsigned long long)r3); uc_x86_msr m = {0xC0000101, 0}; uc_reg_read(t->u, UC_X86_REG_MSR, &m); fprintf(stderr, "  gs_base %#llx\n", (unsigned long long)m.value); uint64_t ra[3] = {0}; uc_mem_read(t->u, r3, ra, 24); fprintf(stderr, "  [rsp] %#llx %#llx %#llx\n", (unsigned long long)ra[0], (unsigned long long)ra[1], (unsigned long long)ra[2]); } exit(4); }
	// Maschine und Tabelleneintrag freigeben (sonst läuft engs[] nach 64 Threads über und eth[] ist voll)
	gil_lock();
	for (int q = 0; q < 64; q++) if (eth[q] == t) eth[q] = 0;
	pthread_mutex_lock(&big); for (int i = 0; i < neng; i++) if (engs[i] == t->u) { engs[i] = engs[--neng]; break; } pthread_mutex_unlock(&big);
	uc_close(t->u); free(t->ss);
	gil_unlock();
	free(t); return 0;
}
static uint32_t *dead; static unsigned dead_cap, ndead;  // Mach-Thread-Ports beendeter Gast-Threads (Eigner von UL_UNFAIR_LOCK-Wörtern)
// offene Hash-Menge (0 = leer): wächst mit der Zahl beendeter Threads, damit pthread_join nie auf einen längst toten Thread wartet
static int is_dead(uint32_t v) { if (!dead_cap) return 0; for (unsigned i = (v * 2654435761u) & (dead_cap - 1); dead[i]; i = (i + 1) & (dead_cap - 1)) if (dead[i] == v) return 1; return 0; }
static void add_dead(uint32_t v) {
	if (!v) return;
	if (ndead * 2 >= dead_cap) {
		uint32_t *old = dead; unsigned oc = dead_cap; dead_cap = oc ? oc * 2 : 1024; dead = calloc(dead_cap, 4); ndead = 0;
		for (unsigned i = 0; i < oc; i++) if (old[i]) { unsigned j = (old[i] * 2654435761u) & (dead_cap - 1); while (dead[j]) j = (j + 1) & (dead_cap - 1); dead[j] = old[i]; ndead++; }
		free(old);
	}
	unsigned j = (v * 2654435761u) & (dead_cap - 1); while (dead[j]) { if (dead[j] == v) return; j = (j + 1) & (dead_cap - 1); } dead[j] = v; ndead++;
}
// __ulock_wait/wake: Warteschlange je Adresse (FIFO). Wert wird nur beim Eintritt verglichen; geweckt wird durch ulock_wake, den Tod des Eigners (UL_UNFAIR_LOCK, pthread_join),
// ein Zeitlimit oder ein wartendes Signal. Das Ergebnis ist die Zahl der übrigen Wartenden (libpthread setzt daraus das WAITERS-Bit des Mutex).
typedef struct UlW { uint64_t addr, val; int woke; struct UlW *next; } UlW;
static UlW *ulq_head, *ulq_tail;
static int ulock_wait(uint64_t addr, int is64, uint64_t val, uint64_t timeout_us, int *remaining) {
	struct timespec ts; if (timeout_us) { clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += timeout_us / 1000000; ts.tv_nsec += (timeout_us % 1000000) * 1000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } }
	uint32_t *p = g2h32(addr); if (!p) return EFAULT;
	BLOCKING_BEGIN; pthread_mutex_lock(&wmx); int r = 0; *remaining = 0;
	uint64_t cur = is64 ? __atomic_load_n((uint64_t *)p, __ATOMIC_SEQ_CST) : __atomic_load_n(p, __ATOMIC_SEQ_CST);
	if (cur == val) {
		UlW w = {addr, val, 0, 0};
		if (ulq_tail) ulq_tail->next = &w; else ulq_head = &w; ulq_tail = &w;
		while (!w.woke && !is_dead((uint32_t)val & ~3u)) {
			if (__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1)) { r = EINTR; break; }
			struct timespec slice; clock_gettime(CLOCK_REALTIME, &slice); slice.tv_nsec += 20000000; if (slice.tv_nsec >= 1000000000) { slice.tv_sec++; slice.tv_nsec -= 1000000000; }
			if (timeout_us && (ts.tv_sec < slice.tv_sec || (ts.tv_sec == slice.tv_sec && ts.tv_nsec < slice.tv_nsec))) slice = ts;
			if (pthread_cond_timedwait(&wcv, &wmx, &slice) && timeout_us) { struct timespec n; clock_gettime(CLOCK_REALTIME, &n); if (n.tv_sec > ts.tv_sec || (n.tv_sec == ts.tv_sec && n.tv_nsec >= ts.tv_nsec)) { r = ETIMEDOUT; break; } }
		}
		UlW **pp = &ulq_head, *prev = 0; for (; *pp; prev = *pp, pp = &(*pp)->next) if (*pp == &w) { *pp = w.next; if (ulq_tail == &w) ulq_tail = prev; break; }
		for (UlW *q = ulq_head; q; q = q->next) if (q->addr == addr) (*remaining)++;
	}
	pthread_mutex_unlock(&wmx); BLOCKING_END; return r;
}
// 0, oder ENOENT wenn niemand wartete
static int ulock_wake_addr(uint64_t addr, int all) {
	int n = 0; pthread_mutex_lock(&wmx);
	for (UlW *w = ulq_head; w; w = w->next) if (w->addr == addr && !w->woke) { w->woke = 1; n++; if (!all) break; }
	pthread_cond_broadcast(&wcv); pthread_mutex_unlock(&wmx); return n ? 0 : ENOENT;
}
static void ulock_wake(void) { pthread_mutex_lock(&wmx); pthread_cond_broadcast(&wcv); pthread_mutex_unlock(&wmx); }

// --- Bedingungsvariablen (psynch_cv*) und Mach-Semaphore, nachgebildet mit einer globalen Warteschlange ---
// Threads wechseln praktisch nur an blockierenden Aufrufen (GIL), deshalb genügt eine einfache Nachbildung ohne die Zählerwörter (lgen/ugen/sgen) des Kernels.
typedef struct CvW { uint64_t addr; int woke; struct CvW *next; } CvW;
static CvW *cvq_head, *cvq_tail;
static struct { uint32_t port; int64_t value; } sems[256]; static int nsems;
static void abs_deadline(struct timespec *ts, int64_t sec, int64_t nsec) {  // sec > 1e8: absolute Echtzeit, sonst relativ
	if (sec > 100000000) { ts->tv_sec = sec; ts->tv_nsec = nsec; }
	else { clock_gettime(CLOCK_REALTIME, ts); ts->tv_sec += sec; ts->tv_nsec += nsec; if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; } }
}
// 0 = geweckt, ETIMEDOUT, EINTR (Signal wartet)
static int cv_wait(uint64_t addr, int has_timeout, int64_t sec, int64_t nsec) {
	struct timespec ts; if (has_timeout) abs_deadline(&ts, sec, nsec);
	CvW w = {addr, 0, 0}; int r = 0;
	BLOCKING_BEGIN; pthread_mutex_lock(&wmx);
	if (cvq_tail) cvq_tail->next = &w; else cvq_head = &w; cvq_tail = &w;
	while (!w.woke) {
		if (__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1)) { r = EINTR; break; }
		struct timespec slice; clock_gettime(CLOCK_REALTIME, &slice); slice.tv_nsec += 20000000; if (slice.tv_nsec >= 1000000000) { slice.tv_sec++; slice.tv_nsec -= 1000000000; }
		if (has_timeout && (ts.tv_sec < slice.tv_sec || (ts.tv_sec == slice.tv_sec && ts.tv_nsec < slice.tv_nsec))) slice = ts;
		int cr = pthread_cond_timedwait(&wcv, &wmx, &slice);
		if (cr && has_timeout) { struct timespec n; clock_gettime(CLOCK_REALTIME, &n); if (n.tv_sec > ts.tv_sec || (n.tv_sec == ts.tv_sec && n.tv_nsec >= ts.tv_nsec)) { r = ETIMEDOUT; break; } }
	}
	CvW **pp = &cvq_head, *prev = 0; for (; *pp; prev = *pp, pp = &(*pp)->next) if (*pp == &w) { *pp = w.next; if (cvq_tail == &w) cvq_tail = prev; break; }
	pthread_mutex_unlock(&wmx); BLOCKING_END; return r;
}
static void cv_wake(uint64_t addr, int all) {
	pthread_mutex_lock(&wmx);
	for (CvW *w = cvq_head; w; w = w->next) if (w->addr == addr && !w->woke) { w->woke = 1; if (!all) break; }
	pthread_cond_broadcast(&wcv); pthread_mutex_unlock(&wmx);
}
static int sem_index(uint32_t port) { for (int i = 0; i < nsems; i++) if (sems[i].port == port) return i; return -1; }
static int sem_wait(uint32_t port, int has_timeout, int64_t sec, int64_t nsec) {  // 0, 3 = KERN_INVALID_ARGUMENT, 49 = KERN_OPERATION_TIMED_OUT, 14 = KERN_ABORTED (Signal)
	struct timespec ts; if (has_timeout) abs_deadline(&ts, sec, nsec);
	int r = 0; BLOCKING_BEGIN; pthread_mutex_lock(&wmx);
	int i = sem_index(port); if (i < 0) r = 3;
	while (!r && sems[i].value <= 0) {
		if (__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1)) { r = 14; break; }
		struct timespec slice; clock_gettime(CLOCK_REALTIME, &slice); slice.tv_nsec += 20000000; if (slice.tv_nsec >= 1000000000) { slice.tv_sec++; slice.tv_nsec -= 1000000000; }
		if (has_timeout && (ts.tv_sec < slice.tv_sec || (ts.tv_sec == slice.tv_sec && ts.tv_nsec < slice.tv_nsec))) slice = ts;
		if (pthread_cond_timedwait(&wcv, &wmx, &slice) && has_timeout) { struct timespec n; clock_gettime(CLOCK_REALTIME, &n); if (n.tv_sec > ts.tv_sec || (n.tv_sec == ts.tv_sec && n.tv_nsec >= ts.tv_nsec)) { r = 49; break; } }
	}
	if (!r) sems[i].value--;
	pthread_mutex_unlock(&wmx); BLOCKING_END; return r;
}
static int sem_signal(uint32_t port, int all) {
	pthread_mutex_lock(&wmx); int i = sem_index(port), r = 0;
	if (i < 0) r = 3; else { sems[i].value += all ? 1000000 : 1; pthread_cond_broadcast(&wcv); }
	pthread_mutex_unlock(&wmx); return r;
}

// --- pthread_rwlock: __psynch_rw_rdlock/wrlock/unlock (306-308), Port des rw-Teils von kern_synch.c (PureDarwin, src/Kernel/Extensions/pthread) ---
// Wie im Kernel: je Sperre eine Warteschlange (kwq) mit einer Leser- und einer Schreiberliste, sortiert nach der Sequenz aus dem L-Wort, dazu Preposts (Entsperren vor dem
// Eintreffen der Wartenden), die Überlappungslogik (MBIT) und die Rückgabebits (updatebits), die libpthread in L/U/S einmischt. Der Kern der Logik läuft unter wmx;
// Wartende schlafen auf wcv (wie cv_wait) und werden mit "woke" und ihrem Rückgabewert geweckt. Weggelassen: Besitzer/Turnstile (Prioritätsvererbung), pshared-Objekte,
// kw_lowseq/kw_highseq (nur für Mutex/CV), die "unterbrochener Weckruf"-Zählung (ein schon geweckter Wartender behält seine Zuteilung, auch wenn ein Signal wartet).
#define RW_COUNT_SHIFT 8
#define RW_INC (1u << RW_COUNT_SHIFT)
#define RW_BIT_MASK (RW_INC - 1)
#define RW_COUNT_MASK ((uint32_t)~RW_BIT_MASK)
#define RW_MAX_READERS RW_COUNT_MASK
#define RWL_KBIT 0x01
#define RWL_EBIT 0x02
#define RWL_WBIT 0x04
#define RWL_MBIT 0x40
#define RWL_IBIT 0x80
#define RWL_INIT RWL_IBIT
#define RWS_SBIT 0x01
#define RWS_INIT RWS_SBIT
#define RWS_SAVEMASK 0x04
#define RW_TYPE_READ 0x01
#define RW_TYPE_WRITE 0x04
#define RW_SHFT_READ 0x0100
#define RW_SHFT_WRITE 0x0400
#define RW_SHFT_MASK 0xff00
#define KW_UNLOCK_PREPOST 0x01
#define KW_UNLOCK_PREPOST_READLOCK 0x08
#define KW_UNLOCK_PREPOST_WRLOCK 0x20
#define KWF_INITCLEARED 0x1
#define KWF_OVERLAP_GUARD 0x8
static int seq_lower(uint32_t x, uint32_t y) { x &= RW_COUNT_MASK; y &= RW_COUNT_MASK; return x < y ? (y - x) < RW_MAX_READERS / 2 : (x - y) > RW_MAX_READERS / 2; }
static int seq_lower_eq(uint32_t x, uint32_t y) { return (x & RW_COUNT_MASK) == (y & RW_COUNT_MASK) ? 1 : seq_lower(x, y); }
static int seq_higher(uint32_t x, uint32_t y) { x &= RW_COUNT_MASK; y &= RW_COUNT_MASK; return x > y ? (x - y) < RW_MAX_READERS / 2 : (y - x) > RW_MAX_READERS / 2; }
static int seq_higher_eq(uint32_t x, uint32_t y) { return (x & RW_COUNT_MASK) == (y & RW_COUNT_MASK) ? 1 : seq_higher(x, y); }
static int diff_genseq(uint32_t x, uint32_t y) { x &= RW_COUNT_MASK; y &= RW_COUNT_MASK; return x == y ? 0 : x > y ? (int)(x - y) : (int)((RW_MAX_READERS - y) + x + RW_INC); }
static int find_diff(uint32_t upto, uint32_t lowest) {
	if (upto == lowest) return 0;
	uint32_t d = seq_lower(upto, lowest) ? diff_genseq(lowest, upto) : diff_genseq(upto, lowest);
	return d >> RW_COUNT_SHIFT;
}
typedef struct Kwe { struct Kwe *prev, *next; uint32_t lockseq, retval; int woke; struct Kq *q; } Kwe;
typedef struct Kq { Kwe *head, *tail; uint32_t count, firstnum, lastnum; } Kq;
enum { KQ_READ = 0, KQ_WRITE, KQ_MAX };
typedef struct Kwq {
	struct Kwq *next; uint64_t addr; time_t used;
	uint32_t inqueue, lword, uword, sword, lastunlockseq, lastseqword, nextseqword; int kflags;
	struct { uint32_t count, lseq, sseq; } prepost;
	Kq q[KQ_MAX];
} Kwq;
static Kwq *kwqs;
static void kwq_clear_prepost(Kwq *k) { k->prepost.lseq = 0; k->prepost.sseq = RWS_INIT; k->prepost.count = 0; }
static void kwq_clear_reinit(Kwq *k) {  // CLEAR_REINIT_BITS
	k->nextseqword = RWS_INIT; k->kflags &= ~KWF_OVERLAP_GUARD; kwq_clear_prepost(k);
	k->lastunlockseq = RWL_INIT; k->lastseqword = RWS_INIT; k->lword = 0; k->uword = 0; k->sword = RWS_INIT;
}
static Kwq *kwq_find(uint64_t addr, uint32_t l, uint32_t u, uint32_t s) {  // ksyn_wqfind; unbenutzte Einträge werden nach 10 s wieder freigegeben (wie die Aufräumfrist im Kernel)
	time_t now = time(0); Kwq *k, **pp;
	for (pp = &kwqs; (k = *pp);) {
		if (k->addr != addr && k->inqueue == 0 && k->prepost.count == 0 && now - k->used > 10) { *pp = k->next; free(k); } else pp = &k->next;
	}
	for (k = kwqs; k; k = k->next) if (k->addr == addr) break;
	if (!k) {
		k = calloc(1, sizeof *k); k->addr = addr; kwq_clear_reinit(k); k->lword = l; k->uword = u; k->sword = s; k->next = kwqs; kwqs = k;
	}
	k->used = now; return k;
}
static int kwq_check_init(Kwq *k, uint32_t lgen) {  // _ksyn_check_init
	int res = (lgen & RWL_INIT) != 0;
	if (res && !(k->kflags & KWF_INITCLEARED)) { kwq_clear_reinit(k); k->kflags |= KWF_INITCLEARED; }
	return res;
}
static int kq_insert(Kwq *k, int qi, Kwe *e, uint32_t mgen, int firstfit) {  // ksyn_queue_insert (SEQFIT oder FIRSTFIT, ohne Abbruch-Sonderfall)
	Kq *q = &k->q[qi]; uint32_t seq = mgen & RW_COUNT_MASK; int res = 0;
	e->lockseq = seq; e->next = e->prev = 0;
	if (q->count == 0) { q->head = q->tail = e; q->firstnum = q->lastnum = seq; }
	else if (firstfit) {  // Ankunftsreihenfolge
		e->prev = q->tail; q->tail->next = e; q->tail = e;
		if (seq_lower(seq, q->firstnum)) q->firstnum = seq;
		if (seq_higher(seq, q->lastnum)) q->lastnum = seq;
	}
	else if (seq == q->firstnum || seq == q->lastnum) res = EBUSY;
	else if (seq_lower(q->lastnum, seq)) { e->prev = q->tail; q->tail->next = e; q->tail = e; q->lastnum = seq; }
	else if (seq_lower(seq, q->firstnum)) { e->next = q->head; q->head->prev = e; q->head = e; q->firstnum = seq; }
	else {
		res = ESRCH;
		for (Kwe *w = q->head; w; w = w->next) if (seq_higher(w->lockseq, seq)) { e->next = w; e->prev = w->prev; if (w->prev) w->prev->next = e; else q->head = e; w->prev = e; res = 0; break; }
	}
	if (res == 0) { e->q = q; q->count++; k->inqueue++; }
	return res;
}
static void kq_remove(Kwq *k, Kq *q, Kwe *e) {  // ksyn_queue_remove_item
	if (e->prev) e->prev->next = e->next; else q->head = e->next;
	if (e->next) e->next->prev = e->prev; else q->tail = e->prev;
	e->prev = e->next = 0; e->q = 0;
	if (--q->count > 0) { q->firstnum = q->head->lockseq & RW_COUNT_MASK; q->lastnum = q->tail->lockseq & RW_COUNT_MASK; } else q->firstnum = q->lastnum = 0;
	k->inqueue--;
}
static uint32_t kq_count_tolowest(Kq *q, uint32_t upto) {
	uint32_t i = 0;
	if (q->count == 0 || seq_higher(q->firstnum, upto)) return 0;
	if (upto == q->firstnum) return 1;
	for (Kwe *w = q->head; w; w = w->next) { uint32_t cur = w->lockseq & RW_COUNT_MASK; if (seq_higher(cur, upto)) break; ++i; if (upto == cur) break; }
	return i;
}
static int find_seq_till(Kwq *k, uint32_t upto, uint32_t nwaiters, uint32_t *countp) {
	uint32_t count = 0;
	for (int i = 0; i < KQ_MAX; i++) { count += kq_count_tolowest(&k->q[i], upto); if (count >= nwaiters) break; }
	*countp = count;
	return count != 0 && count >= nwaiters;
}
static void kwq_signal(Kwq *k, int qi, uint32_t updateval) {  // ksyn_signal: den ersten Wartenden der Liste wecken
	Kwe *e = k->q[qi].head; kq_remove(k, &k->q[qi], e); e->retval = updateval; e->woke = 1;
}
static void kwq_find_rw_lowest(Kwq *k, int flags, uint32_t premgen, int *typep, uint32_t lowest[]) {
	uint32_t numbers[2] = {0}; int typenum[2] = {0}, count = 0, type = 0;
	if (k->q[KQ_READ].count != 0 || (flags & KW_UNLOCK_PREPOST_READLOCK)) {
		uint32_t fr; type |= RW_SHFT_READ;
		if (k->q[KQ_READ].count != 0) { fr = k->q[KQ_READ].firstnum; if ((flags & KW_UNLOCK_PREPOST_READLOCK) && seq_lower(premgen, fr)) fr = premgen; } else fr = premgen;
		lowest[KQ_READ] = fr; numbers[count] = fr; typenum[count++] = RW_TYPE_READ;
	} else lowest[KQ_READ] = 0;
	if (k->q[KQ_WRITE].count != 0 || (flags & KW_UNLOCK_PREPOST_WRLOCK)) {
		uint32_t fw; type |= RW_SHFT_WRITE;
		if (k->q[KQ_WRITE].count != 0) { fw = k->q[KQ_WRITE].firstnum; if ((flags & KW_UNLOCK_PREPOST_WRLOCK) && seq_lower(premgen, fw)) fw = premgen; } else fw = premgen;
		lowest[KQ_WRITE] = fw; numbers[count] = fw; typenum[count++] = RW_TYPE_WRITE;
	} else lowest[KQ_WRITE] = 0;
	uint32_t low = numbers[0]; int lowtype = typenum[0];
	for (int i = 1; i < count; i++) if (seq_lower(numbers[i], low)) { low = numbers[i]; lowtype = typenum[i]; }
	*typep = type | lowtype;
}
// Zuteilung beim Entsperren (oder wenn alle vorgemerkten Wartenden da sind): setzt *updatep, *blockp = 0 wenn der aufrufende Wartende selbst die Sperre bekommt
static int kwq_handle_unlock(Kwq *k, uint32_t rw_wc, uint32_t *updatep, int flags, int *blockp, uint32_t premgen) {
	uint32_t lowest[KQ_MAX], updatebits = 0, numneeded = 0, limitrdnum; int rwtype, block = 1, allreaders = 0, curthreturns = 0;
	k->lastseqword = rw_wc; k->lastunlockseq = rw_wc & RW_COUNT_MASK; k->kflags &= ~KWF_OVERLAP_GUARD;
	kwq_find_rw_lowest(k, flags, premgen, &rwtype, lowest);
	uint32_t low_writer = lowest[KQ_WRITE];
	switch (rwtype & 0xff) {
	case RW_TYPE_READ: {
		Kq *kq = &k->q[KQ_READ];
		if ((rwtype & RW_SHFT_MASK) && (rwtype & RW_SHFT_WRITE)) updatebits |= RWL_WBIT | RWL_KBIT;
		limitrdnum = 0;
		if (rwtype & RW_SHFT_WRITE) {
			limitrdnum = low_writer; numneeded = kq_count_tolowest(kq, limitrdnum);
			if ((flags & KW_UNLOCK_PREPOST_READLOCK) && seq_lower(premgen, limitrdnum)) { curthreturns = 1; numneeded += 1; }
		} else {
			allreaders = 1; k->kflags |= KWF_OVERLAP_GUARD; numneeded += kq->count;
			if (flags & KW_UNLOCK_PREPOST_READLOCK) { curthreturns = 1; numneeded += 1; }
		}
		updatebits += numneeded << RW_COUNT_SHIFT;
		k->nextseqword = (rw_wc & RW_COUNT_MASK) + updatebits;
		if (curthreturns) block = 0;
		while (kq->count != 0 && (allreaders || seq_lower(kq->firstnum, limitrdnum))) kwq_signal(k, KQ_READ, updatebits);  // ksyn_wakeupreaders
		break; }
	case RW_TYPE_WRITE:
		updatebits |= RW_INC | RWL_KBIT | RWL_EBIT;
		if ((flags & KW_UNLOCK_PREPOST_WRLOCK) && low_writer == premgen) {  // der Vormerker selbst ist der niedrigste Schreiber
			block = 0; if (k->q[KQ_WRITE].count != 0) updatebits |= RWL_WBIT;
		} else {
			if (k->q[KQ_WRITE].count > 1 || (flags & KW_UNLOCK_PREPOST_WRLOCK)) updatebits |= RWL_WBIT;
			kwq_signal(k, KQ_WRITE, updatebits);
		}
		k->nextseqword = (rw_wc & RW_COUNT_MASK) + updatebits;
		break;
	}
	*updatep = updatebits; if (blockp) *blockp = block;
	return 0;
}
static int rw_lock(int type, uint64_t addr, uint32_t lgen, uint32_t ugen, uint32_t rw_wc, uint32_t *retval) {  // _psynch_rw_rdlock/wrlock
	uint32_t lockseq = lgen & RW_COUNT_MASK; int qi = type == RW_TYPE_READ ? KQ_READ : KQ_WRITE, prepost_type = type == RW_TYPE_READ ? KW_UNLOCK_PREPOST_READLOCK : KW_UNLOCK_PREPOST_WRLOCK;
	pthread_mutex_lock(&wmx);
	Kwq *k = kwq_find(addr, lgen, ugen, rw_wc);
	kwq_check_init(k, lgen);
	// Überlappung der Leser zuerst (zählt nicht gegen prepost.count): weitere Leser dürfen in dieselbe Zuteilung
	if (type == RW_TYPE_READ && (k->kflags & KWF_OVERLAP_GUARD) && !(rw_wc & RWS_SAVEMASK) && !(lgen & RWL_WBIT) &&
	    (seq_lower_eq(rw_wc, k->nextseqword) || seq_higher_eq(k->lastseqword, rw_wc))) {
		k->nextseqword += RW_INC; *retval = RW_INC | ((k->nextseqword & RW_BIT_MASK) | RWL_MBIT);
		pthread_mutex_unlock(&wmx); return 0;
	}
	// _kwq_handle_preposted_wakeup
	if (k->prepost.count != 0 && seq_lower_eq(lockseq, k->prepost.lseq)) {
		k->prepost.count--;
		if (k->prepost.count == 0) {
			uint32_t pp_l = k->prepost.lseq, pp_s = k->prepost.sseq, ub = 0; int block = 0;
			kwq_clear_prepost(k); k->kflags &= ~KWF_INITCLEARED;
			kwq_handle_unlock(k, pp_s, &ub, prepost_type | KW_UNLOCK_PREPOST, &block, lockseq); pthread_cond_broadcast(&wcv); (void)pp_l;
			if (!block) { *retval = ub; pthread_mutex_unlock(&wmx); return 0; }
		}
	}
	Kwe w; memset(&w, 0, sizeof w);
	int r = kq_insert(k, qi, &w, lgen, 0);
	if (r) { pthread_mutex_unlock(&wmx); return r; }
	BLOCKING_BEGIN;
	while (!w.woke) {
		if (__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1)) { kq_remove(k, &k->q[qi], &w); r = EINTR; break; }
		struct timespec slice; clock_gettime(CLOCK_REALTIME, &slice); slice.tv_nsec += 20000000; if (slice.tv_nsec >= 1000000000) { slice.tv_sec++; slice.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&wcv, &wmx, &slice);
	}
	if (!r) *retval = w.retval;
	pthread_mutex_unlock(&wmx); BLOCKING_END; return r;
}
// Mutex-Warten/Entsperren, nur Richtlinie FIRSTFIT (die Voreinstellung; rekursive und Fehlerprüf-Mutexe brauchen es, normale gehen über ulock): _psynch_mutexwait/_mutexdrop.
// Ohne Besitzerhinweis (tid) und Turnstile; die Wartenden liegen in der Schreiberliste der kwq, ein Entsperren ohne Wartende wird vorgemerkt.
static int mtx_wait(uint64_t addr, uint32_t mgen, uint32_t ugen, uint32_t *retval) {
	uint32_t lseq = mgen & RW_COUNT_MASK; int r = 0;
	pthread_mutex_lock(&wmx);
	Kwq *k = kwq_find(addr, mgen, ugen, 0);
	if (k->prepost.count) {  // vorgemerktes Entsperren: Sperre sofort übernehmen
		k->prepost.count--;
		*retval = ((k->inqueue == 0 ? lseq : (k->q[KQ_WRITE].lastnum & RW_COUNT_MASK)) | RWL_KBIT | RWL_EBIT) & ~0x20u;
		pthread_mutex_unlock(&wmx); return 0;
	}
	Kwe w; memset(&w, 0, sizeof w);
	if ((r = kq_insert(k, KQ_WRITE, &w, mgen, 1))) { pthread_mutex_unlock(&wmx); return r; }
	BLOCKING_BEGIN;
	while (!w.woke) {
		if (__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1)) { kq_remove(k, &k->q[KQ_WRITE], &w); r = EINTR; break; }
		struct timespec slice; clock_gettime(CLOCK_REALTIME, &slice); slice.tv_nsec += 20000000; if (slice.tv_nsec >= 1000000000) { slice.tv_sec++; slice.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&wcv, &wmx, &slice);
	}
	if (!r) *retval = w.retval & ~0x20u;
	pthread_mutex_unlock(&wmx); BLOCKING_END; return r;
}
static void mtx_drop(uint64_t addr, uint32_t mgen, uint32_t ugen) {
	pthread_mutex_lock(&wmx);
	Kwq *k = kwq_find(addr, mgen, ugen, 0);
	k->lastunlockseq = ugen & RW_COUNT_MASK;
	if (k->inqueue == 0) { if (k->prepost.count == 0) { k->prepost.lseq = mgen & RW_COUNT_MASK; } k->prepost.count++; }
	else kwq_signal(k, KQ_WRITE, (k->q[KQ_WRITE].lastnum & RW_COUNT_MASK) | RWL_EBIT | RWL_KBIT);
	pthread_cond_broadcast(&wcv); pthread_mutex_unlock(&wmx);
}
static int rw_unlock(uint64_t addr, uint32_t lgen, uint32_t ugen, uint32_t rw_wc, uint32_t *retval) {  // _psynch_rw_unlock
	uint32_t curgen = lgen & RW_COUNT_MASK, updatebits = 0, count = 0; int error = 0, clearedkflags = 0;
	pthread_mutex_lock(&wmx);
	Kwq *k = kwq_find(addr, lgen, ugen, rw_wc);
	int isinit = kwq_check_init(k, lgen);
	if (k->lastunlockseq != RWL_INIT && seq_lower(ugen, k->lastunlockseq)) goto out;  // veraltete Meldung
	int diff = find_diff(lgen, ugen);  // L - U = Zahl der Wartenden (und Besitzer), die bei uns ankommen müssen
	if (find_seq_till(k, curgen, diff, &count) == 0 && (count == 0 || count < (uint32_t)diff)) {  // noch nicht alle da: vormerken
		if ((k->prepost.sseq & RWS_SBIT) || seq_higher_eq(rw_wc, k->prepost.sseq)) {
			k->prepost.count = diff - count; k->prepost.lseq = curgen; k->prepost.sseq = rw_wc;
			updatebits = lgen;  // keine Zuteilung jetzt
		}
		goto out;
	}
	if (isinit && (k->kflags & KWF_INITCLEARED)) { k->kflags &= ~KWF_INITCLEARED; clearedkflags = 1; }
	kwq_clear_prepost(k);
	error = kwq_handle_unlock(k, rw_wc, &updatebits, 0, 0, 0);
out:
	(void)clearedkflags;
	if (!error) *retval = updatebits;
	pthread_cond_broadcast(&wcv); pthread_mutex_unlock(&wmx); return error;
}

static unsigned long nsys;
static uint64_t mmap_next = MMAP_BASE, stack_top = STACK_TOP;  // JIT-Modus verschiebt beides unter JIT_GUEST_LIMIT
static double t0;

static const char *xroot = "";
#define DYLD_BASE 0x7fff20000000ULL

// Lädt ein x86_64-Mach-O (dünn) in den Gastspeicher; slide wird auf alle Adressen addiert.
static int load_image(const uint8_t *file, uint64_t slide, uint64_t *entry, uint64_t *mh_addr, int *dynamic) {
	struct mach_header_64 *mh = (void *)file; uc_err e;
	struct load_command *lc = (void *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (void *)((char *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64 *s = (void *)lc;
			if (!strcmp(s->segname, "__PAGEZERO") || !s->vmsize) continue;
			uint64_t va = s->vmaddr + slide;
			uint64_t base = va & ~0xfffULL, sz = (va + s->vmsize - base + 0xfff) & ~0xfffULL;
			unmap_all(base, sz);
			if ((e = map_all(base, sz))) { fprintf(stderr, "map %s: %s\n", s->segname, uc_strerror(e)); return -1; }
			uc_mem_write(uc, va, file + s->fileoff, s->filesize);
			if (!strcmp(s->segname, "__TEXT")) *mh_addr = va;
		} else if (lc->cmd == LC_UNIXTHREAD) {
			*entry = *(uint64_t *)((uint32_t *)((char *)lc + 8) + 2 + 16 * 2) + slide;
		} else if (lc->cmd == LC_LOAD_DYLINKER || lc->cmd == LC_MAIN) {
			*dynamic = 1;
		}
	}
	return 0;
}

static bool on_unmapped(uc_engine *u, uc_mem_type t, uint64_t addr, int size, int64_t val, void *ud) {
	Th *th = th_of(u); if (th) th->fault_addr = addr;
	if (!(gsa[11].handler > 1)) fprintf(stderr, "ucrun: Zugriff auf nicht abgebildeten Gastspeicher: Art %d, Adresse %#llx, Größe %d\n", (int)t, (unsigned long long)addr, size);
	return false;
}
// Antwort auf eine MIG-Nachricht an den Host-Port in den Empfangspuffer schreiben
static void mig_reply(uc_engine *u, uint64_t addr, uint32_t id, uint32_t retcode, const void *payload, uint32_t plen) {
	uint8_t r[512] = {0}; uint32_t bits = 0x12, size = 24 + 8 + 4 + plen, ndr[2] = {0, 1};
	memcpy(r, &bits, 4); memcpy(r + 4, &size, 4); memcpy(r + 20, &id, 4); memcpy(r + 24, ndr, 8); memcpy(r + 32, &retcode, 4);
	if (plen) memcpy(r + 36, payload, plen);
	uc_mem_write(u, addr, r, size);
}
static void mig_reply_port(uc_engine *u, uint64_t addr, uint32_t id, uint32_t port) {
	uint8_t r[40] = {0}; uint32_t bits = 0x80000012, size = 40, cnt = 1;
	memcpy(r, &bits, 4); memcpy(r + 4, &size, 4); memcpy(r + 20, &id, 4); memcpy(r + 24, &cnt, 4); memcpy(r + 28, &port, 4);
	r[38] = 17; r[39] = 0;  // disposition MOVE_SEND, type PORT_DESCRIPTOR
	uc_mem_write(u, addr, r, 40);
}

// --- Prozesse: fork, execve, wait4, pipe ---
static int gstr(uint64_t a, char *b, size_t n);
static char self_exe[1024];
static int is_foreign_macho(const char *p) {
	uint32_t h[8]; int fd = open(p, O_RDONLY); if (fd < 0) return 0;
	ssize_t n = read(fd, h, sizeof h); close(fd); if (n < 8) return 0;
	return (h[0] == 0xfeedfacf && h[1] == 0x01000007) || h[0] == 0xbebafeca;  // x86_64-Mach-O oder fat (ucrun sucht die Scheibe)
}
static char *gdup(uc_engine *u, uint64_t a) { char b[4096]; if (gstr(a, b, sizeof b)) return 0; return strdup(b); }
static int do_execve(uc_engine *u, uint64_t pa, uint64_t av, uint64_t ev) {
	char *path = gdup(u, pa); if (!path) return EFAULT;
	char rp[1200]; strncpy(rp, path, sizeof rp - 1); rp[sizeof rp - 1] = 0;
	if (*xroot && path[0] == '/') { char alt[1200]; snprintf(alt, sizeof alt, "%s%s", xroot, path); if (access(alt, F_OK) == 0) strncpy(rp, alt, sizeof rp - 1); }
	char *argvv[256], *envv[1024]; int na = 0, ne = 0; uint64_t p;
	for (; na < 250 && !uc_mem_read(u, av + 8 * na, &p, 8) && p; na++) argvv[na + 1] = gdup(u, p);
	for (; ne < 1000 && ev && !uc_mem_read(u, ev + 8 * ne, &p, 8) && p; ne++) envv[ne] = gdup(u, p);
	envv[ne] = 0;
	if (access(rp, X_OK)) return errno;
	char *nv[260]; int k = 0;
	if (is_foreign_macho(rp)) {  // x86_64: ucrun <datei> <argv[1]...> (das Gast-argv[0] ist dort die Datei)
		nv[k++] = self_exe; nv[k++] = rp; for (int i = 2; i <= na; i++) nv[k++] = argvv[i]; nv[k] = 0;
		execve(self_exe, nv, envv);
	} else {  // Host-Programm (arm64) oder #!-Skript: der Kernel erledigt es
		for (int i = 1; i <= na; i++) nv[k++] = argvv[i]; nv[k] = 0;
		execve(rp, nv, envv);
	}
	return errno;
}


static Th *th_of(uc_engine *u) { for (int i = 0; i < 64; i++) if (eth[i] && eth[i]->u == u) return eth[i]; return 0; }
static void wake_engines(void) { for (int i = 0; i < neng; i++) uc_emu_stop(engs[i]); }
static int sig_wake = 1;
static void hsig(int s) { __atomic_or_fetch(&sig_pending, 1u << s, __ATOMIC_SEQ_CST); if (sig_wake) wake_engines(); }


// --- posix_spawn (Systemaufruf 244) ---
// args: pid_t *pid, path, struct _posix_spawn_args_desc *adesc {attr_size, attrp, file_actions_size, file_actions, ...}, argv, envp
static void spawn_apply_actions(uc_engine *u, const uint8_t *fa, size_t fa_size) {
	if (!fa || fa_size < 8) return;
	int count = *(const int *)(fa + 4); const size_t asz = 8 + 1032;  // _psfa_action: type, fd, {oflag, mode, path[1024]}
	for (int i = 0; i < count && 8 + (size_t)(i + 1) * asz <= fa_size; i++) {
		const uint8_t *a = fa + 8 + (size_t)i * asz; int type = *(const int *)a, fd = *(const int *)(a + 4);
		switch (type) {
		case 0: {  // OPEN
			int oflag = *(const int *)(a + 8); unsigned mode = *(const uint16_t *)(a + 12); char p[1100]; strncpy(p, (const char *)(a + 14), 1023); p[1023] = 0;
			if (*xroot && p[0] == '/') { char alt[1200]; snprintf(alt, sizeof alt, "%s%s", xroot, p); if (access(alt, F_OK) == 0) strncpy(p, alt, sizeof p - 1); }
			int nfd = open(p, oflag, mode); if (nfd < 0) _exit(127);
			if (nfd != fd) { dup2(nfd, fd); close(nfd); }
			break; }
		case 1: close(fd); break;
		case 2: { int nfd = *(const int *)(a + 8); if (fd == nfd) { int fl = fcntl(fd, F_GETFD); if (fl >= 0) fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC); } else dup2(fd, nfd); break; }
		case 5: { char p[1100]; strncpy(p, (const char *)(a + 8), 1023); p[1023] = 0; if (chdir(p)) _exit(127); break; }
		case 6: if (fchdir(fd)) _exit(127); break;
		default: break;  // INHERIT: nichts zu tun
		}
	}
}

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static int gstr(uint64_t a, char *b, size_t n) {  // C-String aus dem Gast
	for (size_t i = 0; i < n; i++) { if (uc_mem_read(uc, a + i, b + i, 1)) return -1; if (!b[i]) return 0; }
	return -1;
}

// Gastzeiger-Beschreibung eines Aufrufs: arg index, Richtung ('i' rein, 'o' raus, 's' Pfad), Größe fest oder aus Argument
typedef struct { int nr; const char *name; struct { int idx; char dir; int fixed, size_arg; } p[3]; } Sc;
#define NONE {-1, 0, 0, 0}
static const Sc tab[] = {
	{3, "read",  {{1, 'o', 0, 2}, NONE}},      {4, "write", {{1, 'i', 0, 2}, NONE}},
	{5, "open",  {{0, 's', 0, 0}, NONE}},      {6, "close", {NONE, NONE}},
	{20, "getpid", {NONE, NONE}},              {24, "getuid", {NONE, NONE}},
	{43, "getegid", {NONE, NONE}},             {47, "getgid", {NONE, NONE}},
	{25, "geteuid", {NONE, NONE}},
	{33, "access", {{0, 's', 0, 0}, NONE}},    {41, "dup", {NONE, NONE}},
	{10, "unlink", {{0, 's', 0, 0}, NONE}},    {136, "mkdir", {{0, 's', 0, 0}, NONE}},
	{137, "rmdir", {{0, 's', 0, 0}, NONE}},    {199, "lseek", {NONE, NONE}},
	{92, "fcntl", {NONE, NONE}},               {116, "gettimeofday", {{0, 'o', 16, 0}, {1, 'o', 8, 0}}},
	{153, "pread", {{1, 'o', 0, 2}, NONE}},    {154, "pwrite", {{1, 'i', 0, 2}, NONE}},
	{338, "stat64", {{0, 's', 0, 0}, {1, 'o', 144, 0}}}, {340, "lstat64", {{0, 's', 0, 0}, {1, 'o', 144, 0}}},
	{339, "fstat64", {{1, 'o', 144, 0}, NONE}},
	{500, "getentropy", {{0, 'o', 0, 1}, NONE}},
	{327, "issetugid", {NONE, NONE}},
	{344, "getdirentries64", {{1, 'o', 0, 2}, {3, 'o', 8, 0}}},
	{463, "openat", {{1, 's', 0, 0}, NONE}},
	{470, "fstatat64", {{1, 's', 0, 0}, {2, 'o', 144, 0}}},
	{473, "readlinkat", {{1, 's', 0, 0}, {2, 'o', 0, 3}}},
	{58, "readlink", {{0, 's', 0, 0}, {1, 'o', 0, 2}}},
	{346, "fstatfs64", {{1, 'o', 2168, 0}, NONE}},
	{345, "statfs64", {{0, 's', 0, 0}, {1, 'o', 2168, 0}}},
	{60, "umask", {NONE, NONE}},
	{12, "chdir", {{0, 's', 0, 0}, NONE}},
	{13, "fchdir", {NONE, NONE}},
	{15, "chmod", {{0, 's', 0, 0}, NONE}},
	{128, "rename", {{0, 's', 0, 0}, {1, 's', 0, 0}}},
	{57, "symlink", {{0, 's', 0, 0}, {1, 's', 0, 0}}},
	{81, "getpgrp", {NONE, NONE}},
	{82, "setpgid", {NONE, NONE}},
	{83, "setitimer", {{1, 'i', 32, 0}, {2, 'o', 32, 0}}},
	{86, "getitimer", {{1, 'o', 32, 0}, NONE}},
	{27, "alarm", {NONE, NONE}},
	{194, "getrlimit", {{1, 'o', 16, 0}, NONE}},
	{220, "getattrlist", {{0, 's', 0, 0}, {1, 'i', 24, 0}, {2, 'o', 0, 3}}},
};

static uint64_t gmap(uint64_t addr, uint64_t len, int fixed) {
	len = (len + 0xfff) & ~0xfffULL;
	if (!fixed || !addr) { addr = mmap_next; mmap_next += len + 0x1000; }
	else unmap_all(addr, len);  // MAP_FIXED überschreibt
	if (map_all(addr, len)) return 0;
	return addr;
}

static void do_syscall(uc_engine *u, void *ud) {
	uint64_t rax, a[6], rdx = 0; int64_t res; int err = 0; nsys++;
	uc_reg_read(u, UC_X86_REG_RAX, &rax);
	uc_reg_read(u, UC_X86_REG_RDI, &a[0]); uc_reg_read(u, UC_X86_REG_RSI, &a[1]); uc_reg_read(u, UC_X86_REG_RDX, &a[2]);
	uc_reg_read(u, UC_X86_REG_R10, &a[3]); uc_reg_read(u, UC_X86_REG_R8, &a[4]); uc_reg_read(u, UC_X86_REG_R9, &a[5]);
	if ((rax >> 24) == 1) {  // Mach-Fallen: nur was dyld/libSystem beim Start brauchen, mit festen Port-Namen
		uint64_t tn = rax & 0xffffff;
		if (trace) fprintf(stderr, "ucrun: mach trap %llu(%#llx, %#llx, %#llx)\n", (unsigned long long)tn, (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2]);
		res = 0;
		switch (tn) {
		case 26: res = 0x303; break;   // mach_reply_port
		case 27: res = 0x203; break;   // thread_self_trap
		case 28: res = 0x103; break;   // task_self_trap
		case 29: res = 0x403; break;   // host_self_trap
		case 10: {                     // _kernelrpc_mach_vm_allocate_trap(task, *addr, size, flags)
			uint64_t want = 0; uc_mem_read(u, a[1], &want, 8);
			uint64_t g = gmap((a[3] & 1) ? 0 : want, a[2], !(a[3] & 1) && want);
			if (!g) { res = 3; break; }
			uc_mem_write(u, a[1], &g, 8); break; }
		case 89: { uint32_t tb[2] = {1, 1}; uc_mem_write(u, a[0], tb, 8); break; }  // mach_timebase_info_trap
		case 15: {                     // _kernelrpc_mach_vm_map_trap(task, *addr, size, mask, flags, prot)
			uint64_t al = a[3] + 1; if (al > 1) mmap_next = (mmap_next + al - 1) & ~(al - 1);
			uint64_t g = gmap(0, a[2], 0);
			if (!g) { res = 3; break; }
			uc_mem_write(u, a[1], &g, 8); break; }
		case 16: { uint32_t nm = 0x1003; uc_mem_write(u, a[2], &nm, 4); break; }  // mach_port_allocate_trap(task, right, *name)
		case 12: unmap_all(a[1], (a[2] + 0xfff) & ~0xfffULL); break;  // mach_vm_deallocate
		case 14: break;                // mach_vm_protect: Gastspeicher bleibt RWX
		case 18: case 19: break;       // mach_port_deallocate / mod_refs
		case 33: res = sem_signal((uint32_t)a[0], 0); break;        // semaphore_signal_trap
		case 34: res = sem_signal((uint32_t)a[0], 1); break;        // semaphore_signal_all_trap
		case 36: res = sem_wait((uint32_t)a[0], 0, 0, 0); break;    // semaphore_wait_trap
		case 37: sem_signal((uint32_t)a[0], 0); res = sem_wait((uint32_t)a[1], 0, 0, 0); break;  // semaphore_wait_signal_trap
		case 38: res = sem_wait((uint32_t)a[0], 1, (int64_t)a[1], (int64_t)a[2]); break;        // semaphore_timedwait_trap
		case 39: sem_signal((uint32_t)a[0], 0); res = sem_wait((uint32_t)a[1], 1, (int64_t)a[2], (int64_t)a[3]); break;
		case 31: {  // mach_msg_trap: nur Nachrichten an den Host-Port, als kleine MIG-Server
			uint8_t m[0x40]; uc_mem_read(u, a[0], m, 0x40); uint32_t id = *(uint32_t *)(m + 20);
			if (trace) fprintf(stderr, "ucrun: mach_msg id %u an Port %#x flavor %u count %u\n", id, *(uint32_t *)(m + 8), *(uint32_t *)(m + 32), *(uint32_t *)(m + 36));
			if (id == 200) {  // host_info(flavor, count)
				uint32_t flavor = *(uint32_t *)(m + 32);
				if (flavor == 1) {  // HOST_BASIC_INFO
					uint32_t p[1 + 12] = {12, 1, 1, 4096, 0x01000007, 3, 0, 1, 1, 1, 1, 0, 1};  // count, max/avail cpus, memory_size, type, subtype, ..., max_mem (64 bit)
					p[11] = 0; p[12] = 1;
					mig_reply(u, a[0], 300, 0, p, sizeof p);
				} else if (flavor == 5) {  // HOST_PRIORITY_INFO
					uint32_t p[1 + 8] = {8, 81, 80, 80, 31, 0, 0, 0, 127};
					mig_reply(u, a[0], 300, 0, p, sizeof p);
				} else mig_reply(u, a[0], 300, 4, 0, 0);
			} else if (id == 3409) { mig_reply_port(u, a[0], 3509, 0);   // task_get_special_port: kein Bootstrap-Server -> MACH_PORT_NULL
			} else if (id == 3418) {  // semaphore_create(task, policy, value): neuer Port-Name
				int32_t pol = *(int32_t *)(m + 32), val = *(int32_t *)(m + 36); (void)pol; uint32_t port = 0x1000005;
				pthread_mutex_lock(&wmx); if (nsems < 256) { port = 0x5003 + 4 * nsems; sems[nsems].port = port; sems[nsems].value = val; nsems++; } pthread_mutex_unlock(&wmx);
				mig_reply_port(u, a[0], 3518, port);
			} else if (id == 3419) { mig_reply(u, a[0], 3519, 0, 0, 0);  // semaphore_destroy
			} else if (id == 206) { mig_reply_port(u, a[0], 306, *(uint32_t *)(m + 32) == 1 ? 0x1005 : 0x1004); // host_get_clock_service (0 = System-, 1 = Kalenderuhr)
			} else if (id == 1000) {  // clock_get_time(clock): mach_timespec_t; Port 0x1005 = Kalenderuhr (Echtzeit), sonst Systemuhr (monoton)
				struct timespec ts; clock_gettime(*(uint32_t *)(m + 8) == 0x1005 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
				uint32_t mts[2] = {(uint32_t)ts.tv_sec, (uint32_t)ts.tv_nsec}; mig_reply(u, a[0], 1100, 0, mts, 8);

			} else { if (trace || warn) fprintf(stderr, "ucrun: mach_msg id %u nicht umgesetzt\n", id); mig_reply(u, a[0], id + 100, 5, 0, 0); }
			break; }
		default: if (trace || warn) fprintf(stderr, "ucrun: Mach-Falle %llu nicht umgesetzt\n", (unsigned long long)tn); res = 5;
		}
		goto mach_done;
	}
	if ((rax >> 24) == 3) {  // machdep: thread_set_tsd_base(3) = %gs-Basis
		if ((rax & 0xffffff) == 3) { uc_reg_write(u, UC_X86_REG_GS_BASE, &a[0]); res = 0; goto mach_done; }
	}
	if ((rax >> 24) != 2) { if (trace || warn) fprintf(stderr, "ucrun: Systemaufruf-Klasse %llu (Mach-Falle?) nicht umgesetzt: %#llx\n", (unsigned long long)(rax >> 24), (unsigned long long)rax); err = ENOSYS; res = ENOSYS; goto done; }
	uint64_t nr = rax & 0xffffff;
	{ static const struct { int from, to; } nc[] = {{423, 334}, {396, 3}, {397, 4}, {398, 5}, {399, 6}, {400, 7}, {406, 92}, {410, 111}, {414, 153}, {415, 154}};  // *_nocancel-Varianten (libc-Abbruchpunkte)
	  for (unsigned q = 0; q < sizeof nc / sizeof *nc; q++) if (nc[q].from == (int)nr) { nr = nc[q].to; break; } }
	if (trace) fprintf(stderr, "ucrun: sys %llu(%#llx, %#llx, %#llx)\n", (unsigned long long)nr, (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2]);
	if (nr == 1) {
		if (getenv("UCRUN_STATS")) { fprintf(stderr, "ucrun: %.3f s, %lu Systemaufrufe\n", now() - t0, nsys); jit_report(); }
		exit((int)a[0]);
	}
	if (nr == 197) {  // mmap(addr, len, prot, flags, fd, off): anonym oder Datei (Inhalt wird kopiert, kein Teilen)
		uint64_t g = gmap(a[0], a[1], (a[3] & MAP_FIXED) != 0);
		if (!g) { err = ENOMEM; res = ENOMEM; goto done; }
		if (!(a[3] & MAP_ANON)) {
			char *b = malloc(a[1]); ssize_t n = pread((int)a[4], b, a[1], (off_t)a[5]);
			if (n > 0) uc_mem_write(u, g, b, n);
			free(b);
		}
		res = g; goto done;
	}
	if (nr == 366) {  // bsdthread_register(threadstart, wqthread, pthsize, init_data, size, ...): tsd_offset steht bei init_data+24
		thread_entry = a[0]; if (trace) fprintf(stderr, "ucrun: bsdthread_register data %#llx size %llu a5 %#llx\n", (unsigned long long)a[3], (unsigned long long)a[4], (unsigned long long)a[5]); { uint32_t dmp[13]; uc_mem_read(u, a[3], dmp, 52); if (trace) for (int q = 0; q < 13; q++) fprintf(stderr, " %08x", dmp[q]); if (trace) fputc(10, stderr); } uint32_t to = 0; if (a[3] && a[4] >= 28) uc_mem_read(u, a[3] + 24, &to, 4); tsd_offset = to;
		uint32_t mo = 0; if (a[3] && a[4] >= 36) uc_mem_read(u, a[3] + 32, &mo, 4); mts_offset = mo; res = 0; goto done;
	}  // bsdthread_register(threadstart, wqthread, ...)
	if (nr == 360) {  // bsdthread_create(func, arg, stack, pthread, flags): neue Unicorn-Maschine + Host-Thread
		uint64_t port = 0x2000 + nsys;
		Th *t = calloc(1, sizeof *t); t->pc = thread_entry; t->tid = next_tid++;
		t->rsp = (a[2] - 128) & ~15ULL; t->rdi = a[3]; t->rsi = port; t->rdx = a[0]; t->rcx = a[1]; t->r8 = a[2]; t->r9 = a[4] | 0x10000000; t->gs = a[3] + tsd_offset;
		{ uint32_t p32 = (uint32_t)port; if (mts_offset) uc_mem_write(u, a[3] + tsd_offset + mts_offset, &p32, 4); }  // der Kernel trägt den Mach-Thread-Port ins pthread ein
		pthread_t pt; pthread_attr_t pa; pthread_attr_init(&pa); pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
		pthread_create(&pt, &pa, thread_main, t); res = a[3]; goto done;
	}
	if (nr == 361) { pthread_mutex_lock(&wmx); add_dead((uint32_t)a[2]); pthread_mutex_unlock(&wmx); ulock_wake(); for (int q = 0; q < 64; q++) if (eth[q] && eth[q]->u == u) eth[q]->done = 1; uc_emu_stop(u); res = 0; goto done; }  // bsdthread_terminate: Gast-Thread endet
	if (nr == 334) {  // __semwait_signal(cond_sem, mutex_sem, timeout, relative, sec, nsec): so schläft libc (usleep, nanosleep, sleep)
		if (!a[2]) { res = 0; goto done; }
		struct timespec ts; if (a[3]) { ts.tv_sec = (time_t)a[4]; ts.tv_nsec = (long)a[5]; }
		else { clock_gettime(CLOCK_REALTIME, &ts); struct timespec target = {(time_t)a[4], (long)a[5]}; ts.tv_sec = target.tv_sec - ts.tv_sec; ts.tv_nsec = target.tv_nsec - ts.tv_nsec; if (ts.tv_nsec < 0) { ts.tv_sec--; ts.tv_nsec += 1000000000; } if (ts.tv_sec < 0) ts.tv_sec = ts.tv_nsec = 0; }
		BLOCKING_BEGIN;
		struct timespec rem = ts; int intr = 0;
		while (rem.tv_sec > 0 || rem.tv_nsec > 0) {  // in 1-ms-Stücken, damit Signale das Schlafen unterbrechen
			struct timespec step = {0, 1000000}; if (rem.tv_sec == 0 && rem.tv_nsec < step.tv_nsec) step.tv_nsec = rem.tv_nsec;
			nanosleep(&step, 0);
			rem.tv_nsec -= step.tv_nsec; if (rem.tv_nsec < 0) { if (rem.tv_sec > 0) { rem.tv_sec--; rem.tv_nsec += 1000000000; } else rem.tv_nsec = 0; }
			if (rem.tv_sec > 0 && rem.tv_nsec >= 1000000000) { rem.tv_sec++; rem.tv_nsec -= 1000000000; }
			if (__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1)) { intr = 1; break; }
		}
		BLOCKING_END; err = intr ? EINTR : ETIMEDOUT; res = err; goto done;
	}
	if (nr == 305) {  // __psynch_cvwait(cv, cvlsgen, cvugen, mutex, mugen, flags, sec, nsec): sec/nsec liegen auf dem Stapel (7./8. Argument)
		uint64_t sp, st[3] = {0, 0, 0}; uc_reg_read(u, UC_X86_REG_RSP, &sp); uc_mem_read(u, sp, st, 24);
		uint32_t flags = (uint32_t)a[5]; int hasto = st[1] != 0 || st[2] != 0 || 0;  // sec==0 && nsec==0: kein Zeitlimit
		int r = cv_wait(a[0], hasto, (int64_t)st[1], (int64_t)st[2]); (void)flags;
		if (r) { err = r; res = r; } else res = 0; goto done;
	}
	if (nr == 304) { cv_wake(a[0], 0); res = 0; goto done; }  // __psynch_cvsignal
	if (nr == 303) { cv_wake(a[0], 1); res = 0; goto done; }  // __psynch_cvbroad
	if (nr == 312) { res = 0; goto done; }                    // __psynch_cvclrprepost
	if (nr == 306 || nr == 307 || nr == 308) {  // __psynch_rw_rdlock/wrlock/unlock(rwlock, lgenval, ugenval, rw_wc, flags): Rückgabe = updatebits
		uint32_t rv = 0; int r = nr == 308 ? rw_unlock(a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], &rv) : rw_lock(nr == 306 ? RW_TYPE_READ : RW_TYPE_WRITE, a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], &rv);
		if (r) { err = r; res = r; } else res = rv;
		goto done;
	}
	if (nr == 301) { uint32_t rv = 0; int r = mtx_wait(a[0], (uint32_t)a[1], (uint32_t)a[2], &rv); if (r) { err = r; res = r; } else res = rv; goto done; }  // __psynch_mutexwait(mutex, mgen, ugen, tid, flags)
	if (nr == 302) { mtx_drop(a[0], (uint32_t)a[1], (uint32_t)a[2]); res = 0; goto done; }                                                           // __psynch_mutexdrop
	if (nr == 297 || nr == 298 || nr == 299 || nr == 300 || nr == 309) { err = ESRCH; res = ESRCH; goto done; }  // longrdlock, yieldwrlock, downgrade, upgrade, unlock2: auch im PureDarwin-Kern nicht umgesetzt
	if (nr == 515 || nr == 544) {  // __ulock_wait(op, addr, value, timeout_us) / __ulock_wait2(op, addr, value, timeout_ns, value2)
		int op = (int)(a[0] & 0xff), noerr = (a[0] & 0x01000000) != 0, rem = 0;
		int r = ulock_wait(a[1], op == 5 || op == 6, op == 5 || op == 6 ? a[2] : (uint32_t)a[2], nr == 515 ? a[3] : a[3] / 1000, &rem);
		if (r) { if (noerr) res = -(int64_t)r; else { err = r; res = r; } } else res = rem;
		goto done;
	}
	if (nr == 516) {  // __ulock_wake(op, addr, wake_value)
		int noerr = (a[0] & 0x01000000) != 0; int r = ulock_wake_addr(a[1], (a[0] & 0x100) != 0);
		if (r) { if (noerr) res = -(int64_t)r; else { err = r; res = r; } } else res = 0;
		goto done;
	}
	if (nr == 46) {  // sigaction(sig, struct __sigaction *nsa {handler, tramp, mask, flags}, struct sigaction *osa {handler, mask, flags})
		int sg = (int)a[0]; if (sg <= 0 || sg >= 32 || sg == 9 || sg == 17) { err = EINVAL; res = EINVAL; goto done; }
		if (a[2]) { uint64_t o[2] = {gsa[sg].handler, 0}; uint32_t om[2] = {gsa[sg].mask, gsa[sg].flags}; memcpy((char *)o + 8, om, 0); uint8_t ob[16]; memcpy(ob, &o[0], 8); memcpy(ob + 8, om, 8); uc_mem_write(u, a[2], ob, 16); }
		if (a[1]) {
			uint8_t nb[24]; uc_mem_read(u, a[1], nb, 24); uint64_t hd, tr; uint32_t mk, fl; memcpy(&hd, nb, 8); memcpy(&tr, nb + 8, 8); memcpy(&mk, nb + 16, 4); memcpy(&fl, nb + 20, 4);
			gsa[sg].handler = hd; gsa[sg].tramp = tr; gsa[sg].mask = mk; gsa[sg].flags = fl;
			struct sigaction ha; memset(&ha, 0, sizeof ha);
			if (hd == 0) ha.sa_handler = SIG_DFL; else if (hd == 1) ha.sa_handler = SIG_IGN; else { ha.sa_handler = hsig; ha.sa_flags = (fl & 2) ? SA_RESTART : 0; }
			sigaction(sg, &ha, 0);
		}
		res = 0; goto done;
	}
	if (nr == 48 || nr == 329) {  // sigprocmask / __pthread_sigmask(how, set*, oset*): ein Prozess-weite Maske genügt hier
		uint32_t old = gmask, nm = 0; if (a[1]) uc_mem_read(u, a[1], &nm, 4);
		if (a[2]) uc_mem_write(u, a[2], &old, 4);
		if (a[1]) { nm &= ~((1u << 8) | (1u << 16)); gmask = a[0] == 1 ? old | nm : a[0] == 2 ? old & ~nm : nm; }
		res = 0; goto done;
	}
	if (nr == 52) { uint32_t p = (sig_pending >> 1) & gmask; uc_mem_write(u, a[0], &p, 4); res = 0; goto done; }   // sigpending
	if (nr == 53) { res = 0; goto done; }                                                                  // sigaltstack: ignoriert
	if (nr == 111 || nr == 429) {  // sigsuspend(mask) / pause: auf ein Signal warten, Maske für die Dauer ersetzen
		uint32_t nm = 0; if (nr == 111 && a[0]) uc_mem_read(u, a[0], &nm, 4); uint32_t old = gmask; gmask = nm;
		BLOCKING_BEGIN; while (!(__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1))) usleep(500); BLOCKING_END;
		Th *th = th_of(u); uint64_t pc0; uc_reg_read(u, UC_X86_REG_RIP, &pc0); (void)pc0; gmask = old | 0; (void)th;
		err = EINTR; res = EINTR; goto done;
	}
	if (nr == 184) {  // sigreturn(uctx, style): Register und Maske aus dem gemerkten Rahmen wiederherstellen
		Th *th = th_of(u);
		if (a[0] == 0) { res = 0; goto done; }  // _longjmp: sigreturn(NULL, UC_RESET_ALT_STACK) setzt nur den Alternativstapel zurück
		if (th && th->sd > 0) {
			Saved *sv = &th->ss[--th->sd & 15];
			for (int i = 0; i < 16; i++) uc_reg_write(u, gregs[i], &sv->r[i]);
			for (int i = 0; i < 16; i++) uc_reg_write(u, UC_X86_REG_XMM0 + i, sv->xmm[i]);
			uc_reg_write(u, UC_X86_REG_EFLAGS, &sv->eflags); th->resume_rip = sv->rip; th->resume = 1; gmask = sv->oldmask;  // rip erst nach dem Hook setzen (Unicorn rechnet nach dem syscall-Hook sonst weiter)
			if (trace) fprintf(stderr, "ucrun: sigreturn: rip %#llx rsp %#llx rax %#llx\n", (unsigned long long)sv->rip, (unsigned long long)sv->r[4], (unsigned long long)sv->r[0]);
			uc_emu_stop(u);  // aus dem Hook heraus neu aufsetzen (run_engine liest rip)
			return;
		}
		res = 0; goto done;
	}
	if (nr == 328) { if (a[1]) kill(getpid(), (int)a[1]); res = 0; goto done; }  // __pthread_kill(thread, sig): Prozess-weit genügt
	switch (nr) {  // Systemaufrufe, die nur beim Start nach dem Zustand des Systems fragen
	case 483: case 381: case 520: case 521: case 331: res = 0; goto done;                 // csrctl, bsdthread_register, __mac_syscall
	case 54: err = 25; res = 25; goto done;                              // ioctl: ENOTTY (Terminal-Abfragen der libc)
	case 274: err = 2; res = 2; goto done;                               // __sysctlbyname: ENOENT
	case 294: case 336: case 287: err = EINVAL; res = EINVAL; goto done; // shared_region_check_np (kein Cache), proc_info, ...
	}
	if (nr == 92) {  // fcntl: Code-Signatur-Befehle gibt es hier nicht (PureDarwin erzwingt keine), F_GETPATH braucht einen Puffer
		int cmd = (int)a[1];
		if (cmd == 98) { res = 0; goto done; }                                  // F_CHECK_LV
		if (cmd == 59 || cmd == 61 || cmd == 97 || cmd == 99) { err = 45; res = 45; goto done; }  // F_ADDFILESIGS*, F_ADDSIGS: ENOTSUP
		if (cmd == 50) { char p[1024] = {0}; res = fcntl((int)a[0], 50, p); if (res == -1) { err = errno; res = errno; } else { uc_mem_write(u, a[2], p, 1024); } goto done; }
		res = fcntl((int)a[0], cmd, a[2]); if (res == -1) { err = errno; res = errno; } goto done;
	}
	if (nr == 202) {  // sysctl(name, namelen, oldp, oldlenp, newp, newlen): Namen und Werte gelten für den Host (arm64) - siehe Studie
		int mib[16]; uint32_t nl = a[1] > 16 ? 16 : a[1]; uc_mem_read(u, a[0], mib, nl * 4);
		size_t ol = 0; if (a[3]) uc_mem_read(u, a[3], &ol, 8);
		char *ob = a[2] ? calloc(1, ol ? ol : 1) : 0; void *nb = 0;
		if (a[4]) { nb = malloc(a[5]); uc_mem_read(u, a[4], nb, a[5]); }
		if (nl >= 2 && mib[0] == 6 && mib[1] == 1 && ob && ol >= 7) { memcpy(ob, "x86_64", 7); ol = 7; res = 0; }  // hw.machine: der Gast ist x86_64 (wie unter Rosetta)
		else res = sysctl(mib, nl, ob, a[3] ? &ol : 0, nb, a[5]);
		if (res == -1) { err = errno; res = errno; }
		if (ob && (res == 0 || err == ENOMEM)) uc_mem_write(u, a[2], ob, ol);
		if (a[3] && (res == 0 || err == ENOMEM)) uc_mem_write(u, a[3], &ol, 8);
		free(ob); free(nb); goto done;
	}
	if (nr == 2 || nr == 66) {  // fork/vfork: Host-fork; der Gastspeicher (private anonyme Seiten) wird kopiert. Kind: rdx = 1 (Darwin-Konvention)
		fflush(0); pid_t pid = fork();
		if (pid < 0) { err = errno; res = errno; goto done; }
		res = pid; rdx = pid == 0 ? 1 : 0;
		if (pid == 0) { nsys = 0; }
		goto done;
	}
	if (nr == 244) {  // posix_spawn: fork + Dateiaktionen + execve (SETEXEC: ohne fork)
		uint64_t d[4] = {0, 0, 0, 0}; if (a[2]) uc_mem_read(u, a[2], d, 32);
		uint8_t attr[64] = {0}; if (d[1]) uc_mem_read(u, d[1], attr, d[0] < 64 ? d[0] : 64);
		short aflags = *(short *)attr; uint8_t *fa = 0; if (d[3] && d[2] > 0 && d[2] < (1u << 24)) { fa = malloc(d[2]); uc_mem_read(u, d[3], fa, d[2]); }
		char *pth = gdup(u, a[1]); if (!pth) { err = EFAULT; res = EFAULT; goto done; }
		char rp[1200]; strncpy(rp, pth, sizeof rp - 1); rp[sizeof rp - 1] = 0;
		if (*xroot && pth[0] == '/') { char alt[1200]; snprintf(alt, sizeof alt, "%s%s", xroot, pth); if (access(alt, F_OK) == 0) strncpy(rp, alt, sizeof rp - 1); }
		if (access(rp, X_OK)) { err = errno; res = errno; free(fa); goto done; }  // Fehler beim Starten melden wie der Kernel
		if (aflags & 0x40) { spawn_apply_actions(u, fa, d[2]); int r = do_execve(u, a[1], a[3], a[4]); err = r; res = r; goto done; }
		fflush(0); pid_t pid = fork();
		if (pid < 0) { err = errno; res = errno; free(fa); goto done; }
		if (pid == 0) {
			spawn_apply_actions(u, fa, d[2]);
			if (aflags & 2) setpgid(0, *(pid_t *)(attr + 12));
			if (aflags & 4) { uint32_t sd = *(uint32_t *)(attr + 4); for (int sg = 1; sg < 32; sg++) if (sd & (1u << (sg - 1))) signal(sg, SIG_DFL); }
			do_execve(u, a[1], a[3], a[4]); _exit(127);
		}
		free(fa);
		if (a[0]) { int32_t p32 = pid; uc_mem_write(u, a[0], &p32, 4); }
		res = 0; goto done;
	}
	if (nr == 59) { int r = do_execve(u, a[0], a[1], a[2]); err = r; res = r; goto done; }  // execve kehrt nur bei Fehler zurück
	if (nr == 7) {  // wait4(pid, *status, options, *rusage)
		int st = 0; struct rusage ru; BLOCKING_BEGIN; pid_t r = wait4((pid_t)a[0], &st, (int)a[2], a[3] ? &ru : 0); BLOCKING_END;
		if (r < 0) { err = errno; res = errno; goto done; }
		if (a[1]) uc_mem_write(u, a[1], &st, 4);
		if (a[3]) uc_mem_write(u, a[3], &ru, sizeof ru);
		res = r; goto done;
	}
	if (nr == 42) { int fds[2]; if (pipe(fds)) { err = errno; res = errno; goto done; } res = fds[0]; rdx = fds[1]; goto done; }  // pipe: rax = Lese-, rdx = Schreibende
	if (nr == 90) { res = dup2((int)a[0], (int)a[1]); if (res < 0) { err = errno; res = errno; } goto done; }
	if (nr == 39) { res = getppid(); goto done; }
	if (nr == 37) { res = kill((pid_t)a[0], (int)a[1]); if (res < 0) { err = errno; res = errno; } goto done; }
	if (nr == 372) { Th *th = th_of(u); res = th ? th->tid : 1; goto done; }  // thread_selfid
	if (nr == 73) { unmap_all(a[0], (a[1] + 0xfff) & ~0xfffULL); res = 0; goto done; }
	if (nr == 74) { res = 0; goto done; }  // mprotect: Gastspeicher bleibt RWX (Eigenschaft von ucrun, siehe Studie)
	const Sc *s = 0;
	for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++) if (tab[i].nr == (int)nr) s = &tab[i];
	if (!s) { if (trace || warn) fprintf(stderr, "ucrun: Systemaufruf %llu nicht umgesetzt\n", (unsigned long long)nr); err = ENOSYS; res = ENOSYS; goto done; }
	uint64_t h[6]; memcpy(h, a, sizeof h);
	char *bufs[3] = {0, 0, 0}; size_t sz[3] = {0, 0, 0}; char pathb[3][1024];
	for (int k = 0; k < 3; k++) {
		int i = s->p[k].idx; if (i < 0 || !s->p[k].dir) continue;
		if (s->p[k].dir == 's') { if (gstr(a[i], pathb[k], 1024)) { err = EFAULT; res = EFAULT; goto done; } if (*xroot && pathb[k][0] == '/') {  // Gast-Dateisystem: erst XLAT_ROOT (x86_64-Systembibliotheken), sonst der Host
				char alt[1100]; snprintf(alt, sizeof alt, "%s%s", xroot, pathb[k]);
				if (access(alt, F_OK) == 0) strncpy(pathb[k], alt, 1023);
			}
			h[i] = (uint64_t)pathb[k]; continue; }
		sz[k] = s->p[k].fixed ? (size_t)s->p[k].fixed : (size_t)a[s->p[k].size_arg];
		bufs[k] = calloc(1, sz[k] ? sz[k] : 1);
		if (s->p[k].dir == 'i') uc_mem_read(u, a[i], bufs[k], sz[k]);
		h[i] = (uint64_t)bufs[k];
	}
	if (nr == 339 && 0) {}
	if (nr == 338) res = stat((char *)h[0], (struct stat *)h[1]);        // libc statt SYS_stat64: auf arm64-Hosts nicht überall vorhanden
	else if (nr == 340) res = lstat((char *)h[0], (struct stat *)h[1]);
	else if (nr == 339) res = fstat((int)h[0], (struct stat *)h[1]);
	else if (nr == 470) res = fstatat((int)h[0], (char *)h[1], (struct stat *)h[2], (int)h[3]);
	else res = syscall((int)nr, h[0], h[1], h[2], h[3], h[4], h[5]);
	if (res == -1) { err = errno; res = errno; }
	else for (int k = 0; k < 3; k++) if (bufs[k] && s->p[k].dir == 'o') {
		size_t n = sz[k]; if ((nr == 3 || nr == 153 || nr == 344) && k == 0) n = res < 0 ? 0 : (size_t)res;
		else if ((nr == 473 || nr == 58) && res >= 0) n = (size_t)res;
		if (0) n = res < 0 ? 0 : (size_t)res;
		uc_mem_write(u, a[s->p[k].idx], bufs[k], n);
	}
	for (int k = 0; k < 3; k++) free(bufs[k]);
	goto done;
mach_done:;
	uc_reg_write(u, UC_X86_REG_RAX, &res);
	return;
done:;
	uint64_t fl; uc_reg_read(u, UC_X86_REG_EFLAGS, &fl);
	fl = err ? (fl | 1) : (fl & ~1ULL);
	uc_reg_write(u, UC_X86_REG_EFLAGS, &fl);
	uc_reg_write(u, UC_X86_REG_RAX, &res);
	uc_reg_write(u, UC_X86_REG_RDX, &rdx);
	if (__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1)) uc_emu_stop(u);  // Signal wartet: Zeitscheibe beenden, Zustellung in run_engine
}


int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: ucrun <x86_64 Mach-O> [args]\n"); return 2; }
	trace = getenv("UCRUN_TRACE") != 0; warn = getenv("UCRUN_WARN") != 0;
	if (getenv("UCRUN_SLICE_MS")) { slice_us = atoll(getenv("UCRUN_SLICE_MS")) * 1000; sig_wake = 0; }
	{ uint32_t sz = sizeof self_exe; if (_NSGetExecutablePath(self_exe, &sz)) strcpy(self_exe, argv[0]); }
	if (getenv("XLAT_ROOT")) xroot = getenv("XLAT_ROOT");
	else if (access("/Library/ClokeOS/x86/usr/lib/dyld", F_OK) == 0) xroot = "/Library/ClokeOS/x86";  // Standard im ClokeOS-Image
	FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 127; }
	fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *file = malloc(fl); if (fread(file, 1, fl, f) != (size_t)fl) return 2;
	if (*(uint32_t *)file == 0xbebafeca) {  // fat: x86_64-Scheibe suchen (Kopf big endian)
		uint32_t na = __builtin_bswap32(*(uint32_t *)(file + 4)), off = 0;
		for (uint32_t i = 0; i < na; i++) if (__builtin_bswap32(*(uint32_t *)(file + 8 + 20 * i)) == 0x01000007) off = __builtin_bswap32(*(uint32_t *)(file + 16 + 20 * i));
		if (off) file += off;
	}
	struct mach_header_64 *mh = (void *)file;
	if (mh->magic != MH_MAGIC_64 || mh->cputype != 0x01000007) { fprintf(stderr, "ucrun: %s ist kein x86_64-Mach-O\n", argv[1]); return 126; }
	jit_choose(file, argv[1]);  // JIT: UCRUN_JIT=1 und kein dyld nötig -> JIT-Speichermodell
	uc_err e = UC_ERR_OK; uc = new_engine(); if (!uc) { fprintf(stderr, "uc_open fehlgeschlagen\n"); return 3; }
	uint64_t entry = 0, mh_addr = 0; int dynamic = 0;
	if (load_image(file, 0, &entry, &mh_addr, &dynamic)) return 3;
	uint64_t unix_entry = entry;
	if (dynamic) {  // wie der Kernel: dyld des Gastes laden, es lädt Programm und Bibliotheken selbst
		char dp[1024]; snprintf(dp, sizeof dp, "%s/usr/lib/dyld", xroot);
		FILE *df = fopen(dp, "rb"); if (!df) { fprintf(stderr, "ucrun: %s fehlt (XLAT_ROOT mit x86_64-Systembibliotheken)\n", dp); return 125; }
		fseek(df, 0, SEEK_END); long dl = ftell(df); fseek(df, 0, SEEK_SET);
		uint8_t *dfile = malloc(dl); if (fread(dfile, 1, dl, df) != (size_t)dl) return 2;
		uint64_t dmh = 0, dentry = 0; int dd = 0;
		if (load_image(dfile, DYLD_BASE, &dentry, &dmh, &dd)) return 3;
		entry = dentry;
	}
	// Commpage von x86_64-Darwin (Adresse im Host nicht abbildbar, in Unicorn schon): nur die Felder, die libSystem liest
	{
		uint8_t cp[0x4000] = {0}; uint64_t caps = 0x1fULL; uint16_t ver = 14; uint8_t ncpu = 1; uint64_t mem = 4ULL << 30; uint32_t fam = 0x1ee2d34e;
		memcpy(cp, "commpage 64-bit", 16); memcpy(cp + 0x10, &caps, 8); memcpy(cp + 0x1e, &ver, 2); cp[0x22] = ncpu;
		cp[0x34] = cp[0x35] = cp[0x36] = ncpu; memcpy(cp + 0x38, &mem, 8); memcpy(cp + 0x40, &fam, 4);
		{ uint32_t scale = 0xffffffffu, gen = 1; memcpy(cp + 0x58, &scale, 4); memcpy(cp + 0x68, &gen, 4); }  // nanotime: 1 ns je TSC-Takt
		map_all(0x7fffffe00000ULL, sizeof cp);
		uc_mem_write(uc, 0x7fffffe00000ULL, cp, sizeof cp);
	}
	map_all(stack_top - STACK_SIZE, STACK_SIZE);
	// Stapel wie ihn der Kernel für ein Programm mit LC_UNIXTHREAD baut: argc, argv[], 0, envp[], 0, apple[], 0, Strings
	// Umgebung des Gastes: ohne DYLD_INSERT_LIBRARIES des Hosts (die arm64-Bibliothek passt nicht zum x86_64-dyld)
	static char *genv[1024]; int ne = 0;
	for (int i = 0; environ[i] && ne < 1000; i++) if (strncmp(environ[i], "DYLD_INSERT_LIBRARIES=", 22)) genv[ne++] = environ[i];
	// XLAT_INSERT=<x86_64-dylib>: wird dem Gast als DYLD_INSERT_LIBRARIES gegeben (die Variable selbst gilt dem arm64-dyld von ucrun)
	if (getenv("XLAT_INSERT") && ne < 1000) { static char ins[1100]; snprintf(ins, sizeof ins, "DYLD_INSERT_LIBRARIES=%s", getenv("XLAT_INSERT")); genv[ne++] = ins; }
	// libpthread soll Mutexe über __ulock (os_unfair_lock-Stil) statt über psynch machen: die Nachbildung von psynch_mutexwait/drop (Kernel-Warteschlange mit Sequenzzählern) entfällt
	{ int have = 0; for (int i = 0; i < ne; i++) if (!strncmp(genv[i], "PTHREAD_MUTEX_USE_ULOCK=", 24)) have = 1; if (!have) genv[ne++] = "PTHREAD_MUTEX_USE_ULOCK=1"; }
	uint64_t sp = stack_top - 0x1000, ptrs[512]; int np = 0;
	int ng = argc - 1; char **gv = argv + 1;
	uint64_t pstr[600]; int ns = 0;
	for (int i = 0; i < ng + ne + 1; i++) {
		const char *s = i < ng ? gv[i] : i < ng + ne ? genv[i - ng] : gv[0];
		sp -= strlen(s) + 1; uc_mem_write(uc, sp, s, strlen(s) + 1); pstr[ns++] = sp;
		if (ns >= 590) break;
	}
	sp &= ~15ULL; np = 0;
	if (dynamic) ptrs[np++] = mh_addr;
	ptrs[np++] = ng;
	for (int i = 0; i < ng; i++) ptrs[np++] = pstr[i];
	ptrs[np++] = 0;
	for (int i = 0; i < ne && ng + i < ns - 1; i++) ptrs[np++] = pstr[ng + i];
	ptrs[np++] = 0; ptrs[np++] = pstr[ns - 1]; ptrs[np++] = 0;
	sp -= np * 8; sp &= ~15ULL; uc_mem_write(uc, sp, ptrs, np * 8);
	uc_reg_write(uc, UC_X86_REG_RSP, &sp);
	jit_start(entry, sp);  // JIT
	t0 = now();
	main_th.u = uc; main_th.pc = entry; main_th.tid = 1; eth[63] = &main_th;
	if (getenv("UCRUN_RING")) { uc_hook ch; uc_hook_add(uc, &ch, UC_HOOK_CODE, on_code, &main_th, 1, 0); }
	e = run_engine(&main_th);
	if (e) { uint64_t rip; uc_reg_read(uc, UC_X86_REG_RIP, &rip); { uint8_t b[16] = {0}; uc_mem_read(uc, rip, b, 16); fprintf(stderr, "ucrun: %s bei rip %#llx, Bytes:", uc_strerror(e), (unsigned long long)rip); for (int q = 0; q < 16; q++) fprintf(stderr, " %02x", b[q]); fputc(10, stderr); } if (getenv("UCRUN_RING")) { fprintf(stderr, "  letzte Befehle:"); for (unsigned q = main_th.n > 48 ? main_th.n - 48 : 0; q < main_th.n; q++) fprintf(stderr, " %llx", (unsigned long long)main_th.ring[q & 63]); fputc(10, stderr); } return 4; }
	return 0;
}
