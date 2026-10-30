// a64run – arm64-Mach-O auf x86_64 ausführen, mit Unicorn (QEMU-TCG, AArch64-Kern) als CPU.
//
// Gegenstück zu ucrun (x86_64 auf arm64). Gastspeicher liegt in Unicorns eigenem Adressraum: Seiten des Gastes sind 16 KiB
// (wie bei arm64-Darwin), die Host-Seite (4 KiB) spielt keine Rolle. Systemaufrufe des Gastes (svc #0x80, x16 = Nummer,
// Argumente x0-x7, Fehler als Carry-Flag + errno in x0; BSD-Nummern und LP64-Strukturen sind auf Darwin für x86_64 und arm64 gleich)
// werden mit einer kleinen Tabelle an den Host weitergereicht. Mach-Fallen haben negative x16, die Maschinenfalle
// (thread_set_tsd_base) ist x16 = 0x80000000 mit x3 = 2 und setzt TPIDRRO_EL0.
//
//   a64run <arm64-Mach-O> [Argumente]     (A64_TRACE=1: Systemaufrufe zeigen, A64_WARN=1: nicht umgesetzte melden,
//                                          A64_STATS=1: Zeit/Zähler am Ende, A64_ROOT=<Ordner mit usr/lib/dyld ...>)
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <unicorn/unicorn.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#define PAGE 0x4000ULL                  // Seitengröße des Gastes (arm64-Darwin)
#define STACK_TOP 0x16fe00000ULL        // wie beim arm64-Kernel: Stapel knapp unter 6 GiB
#define STACK_SIZE 0x800000ULL
#define MMAP_BASE 0x280000000ULL
#define DYLD_BASE 0x1f0000000ULL
#define COMMPAGE 0xFFFFFC000ULL         // _COMM_PAGE64_BASE_ADDRESS auf arm64
#define CPU_ARM64 0x0100000c

extern char **environ;
static __thread uc_engine *uc;   // Maschine des laufenden Host-Threads (jeder Gast-Thread hat eine eigene)
static int trace, warn;
static const char *aroot = "";
static unsigned long nsys;
static double t0;
static uint64_t mmap_next = MMAP_BASE;
static char self_exe[1024];

// --- Gastspeicher: Host-Speicher, in jede Unicorn-Maschine (ein Gast-Thread = eine Maschine) per uc_mem_map_ptr eingeblendet ---
typedef struct { uint64_t g, n; uint8_t *h; } Reg;
static Reg regs[2048]; static int nregs;
static uc_engine *engs[64]; static int neng;
static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static void unmap_locked(uint64_t g, uint64_t n) {
	uint64_t ue = g + n;
	for (int i = 0, cnt = nregs; i < cnt; i++) {
		Reg r = regs[i]; uint64_t b = r.g + r.n;
		if (r.g >= ue || b <= g) continue;
		uint64_t s = r.g > g ? r.g : g, e = b < ue ? b : ue;
		for (int k = 0; k < neng; k++) uc_mem_unmap(engs[k], s, e - s);
		if (r.g < s) { regs[i].n = s - r.g; if (b > e && nregs < 2048) regs[nregs++] = (Reg){e, b - e, r.h + (e - r.g)}; }
		else if (b > e) regs[i] = (Reg){e, b - e, r.h + (e - r.g)};
		else regs[i].n = 0;
	}
	int k = 0; for (int i = 0; i < nregs; i++) if (regs[i].n) regs[k++] = regs[i]; nregs = k;
}
static void unmap_range(uint64_t g, uint64_t n) { pthread_mutex_lock(&big); unmap_locked(g, n); pthread_mutex_unlock(&big); }
static uc_err map_range(uint64_t g, uint64_t n) {
	pthread_mutex_lock(&big);
	unmap_locked(g, n);
	uint8_t *h = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	uc_err e = h == MAP_FAILED ? UC_ERR_NOMEM : UC_ERR_OK;
	for (int i = 0; !e && i < neng; i++) e = uc_mem_map_ptr(engs[i], g, n, UC_PROT_ALL, h);  // Gastspeicher bleibt RWX (wie bei ucrun)
	if (!e && nregs < 2048) regs[nregs++] = (Reg){g, n, h};
	pthread_mutex_unlock(&big);
	return e;
}
static uint32_t *g2h32(uint64_t a) {
	uint32_t *p = 0; pthread_mutex_lock(&big);
	for (int i = nregs - 1; i >= 0; i--) if (a >= regs[i].g && a + 4 <= regs[i].g + regs[i].n) { p = (uint32_t *)(regs[i].h + (a - regs[i].g)); break; }
	pthread_mutex_unlock(&big); return p;
}
static uint64_t gmap(uint64_t addr, uint64_t len, int fixed) {
	len = (len + PAGE - 1) & ~(PAGE - 1);
	if (!fixed || !addr) { addr = mmap_next; mmap_next += len + PAGE; }
	else addr &= ~(PAGE - 1);
	if (map_range(addr, len)) return 0;
	return addr;
}
static int gstr(uint64_t a, char *b, size_t n) {
	for (size_t i = 0; i < n; i++) { if (uc_mem_read(uc, a + i, b + i, 1)) return -1; if (!b[i]) return 0; }
	return -1;
}
static char *gdup(uint64_t a) { char b[4096]; if (gstr(a, b, sizeof b)) return 0; return strdup(b); }
static uint64_t X(int i) { uint64_t v; uc_reg_read(uc, UC_ARM64_REG_X0 + i, &v); return v; }
static void setX(int i, uint64_t v) { uc_reg_write(uc, UC_ARM64_REG_X0 + i, &v); }
static void setcarry(int on) { uint64_t f; uc_reg_read(uc, UC_ARM64_REG_NZCV, &f); f = on ? (f | (1u << 29)) : (f & ~(1u << 29)); uc_reg_write(uc, UC_ARM64_REG_NZCV, &f); }
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

// --- Laden eines arm64-Mach-O (dünn); slide wird auf alle Adressen addiert ---
static int load_image(const uint8_t *file, uint64_t slide, uint64_t *entry, uint64_t *mh_addr, int *dynamic) {
	struct mach_header_64 *mh = (void *)file; uc_err e;
	struct load_command *lc = (void *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (void *)((char *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64 *s = (void *)lc;
			if (!strcmp(s->segname, "__PAGEZERO") || !s->vmsize) continue;
			uint64_t va = s->vmaddr + slide, base = va & ~(PAGE - 1), sz = (va + s->vmsize - base + PAGE - 1) & ~(PAGE - 1);
			if ((e = map_range(base, sz))) { fprintf(stderr, "a64run: map %s %#llx: %s\n", s->segname, (unsigned long long)base, uc_strerror(e)); return -1; }
			if (s->filesize) uc_mem_write(uc, va, file + s->fileoff, s->filesize);
			if (!strcmp(s->segname, "__TEXT")) *mh_addr = va;
		} else if (lc->cmd == LC_UNIXTHREAD) {  // flavor, count, x0-x28, fp, lr, sp, pc, cpsr
			*entry = *(uint64_t *)((char *)lc + 16 + 8 * 32) + slide;
		} else if (lc->cmd == LC_LOAD_DYLINKER || lc->cmd == LC_MAIN) *dynamic = 1;
	}
	return 0;
}

// --- MIG: Antworten auf Nachrichten an den Host-Port (wie in ucrun, Layout ist architekturunabhängig) ---
static void mig_reply(uint64_t addr, uint32_t id, uint32_t retcode, const void *payload, uint32_t plen) {
	uint8_t r[512] = {0}; uint32_t bits = 0x12, size = 24 + 8 + 4 + plen, ndr[2] = {0, 1};
	memcpy(r, &bits, 4); memcpy(r + 4, &size, 4); memcpy(r + 20, &id, 4); memcpy(r + 24, ndr, 8); memcpy(r + 32, &retcode, 4);
	if (plen) memcpy(r + 36, payload, plen);
	uc_mem_write(uc, addr, r, size);
}
static void mig_reply_port(uint64_t addr, uint32_t id, uint32_t port) {
	uint8_t r[40] = {0}; uint32_t bits = 0x80000012, size = 40, cnt = 1;
	memcpy(r, &bits, 4); memcpy(r + 4, &size, 4); memcpy(r + 20, &id, 4); memcpy(r + 24, &cnt, 4); memcpy(r + 28, &port, 4);
	r[38] = 17; r[39] = 0;
	uc_mem_write(uc, addr, r, 40);
}

// --- Prozesse ---
static int is_arm64_macho(const char *p) {
	uint32_t h[8]; int fd = open(p, O_RDONLY); if (fd < 0) return 0;
	ssize_t n = read(fd, h, sizeof h); close(fd); if (n < 8) return 0;
	return (h[0] == 0xfeedfacf && h[1] == CPU_ARM64) || h[0] == 0xbebafeca;
}
static const char *guest_path(const char *p, char *buf, size_t n) {  // erst A64_ROOT (arm64-Systembibliotheken), sonst der Host
	if (*aroot && p[0] == '/') { snprintf(buf, n, "%s%s", aroot, p); if (access(buf, F_OK) == 0) return buf; }
	return p;
}
static int do_execve(uint64_t pa, uint64_t av, uint64_t ev) {
	char *path = gdup(pa); if (!path) return EFAULT;
	char rpb[1200]; const char *rp = guest_path(path, rpb, sizeof rpb);
	char *argvv[256], *envv[1024]; int na = 0, ne = 0; uint64_t p;
	for (; na < 250 && !uc_mem_read(uc, av + 8 * na, &p, 8) && p; na++) argvv[na + 1] = gdup(p);
	for (; ne < 1000 && ev && !uc_mem_read(uc, ev + 8 * ne, &p, 8) && p; ne++) envv[ne] = gdup(p);
	envv[ne] = 0;
	if (access(rp, X_OK)) return errno;
	char *nv[260]; int k = 0;
	if (is_arm64_macho(rp)) { nv[k++] = self_exe; nv[k++] = (char *)rp; for (int i = 2; i <= na; i++) nv[k++] = argvv[i]; nv[k] = 0; execve(self_exe, nv, envv); }
	else { for (int i = 1; i <= na; i++) nv[k++] = argvv[i]; nv[k] = 0; execve(rp, nv, envv); }  // x86_64-Programme und #!-Skripte: der Host
	return errno;
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
	{116, "gettimeofday", {{0, 'o', 16, 0}, {1, 'o', 8, 0}}},
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

// Eigenschaften des Gastes für sysctl: der Host ist x86_64, der Gast sieht arm64 mit 16-KiB-Seiten
static int fake_sysctl_name(const char *name, void *ob, size_t *ol) {
	static const struct { const char *n; uint64_t v; int sz; } v[] = {
		{"hw.pagesize", PAGE, 8}, {"vm.pagesize", PAGE, 8}, {"hw.cputype", CPU_ARM64, 4}, {"hw.cpusubtype", 2, 4}, {"hw.cpu64bit_capable", 1, 4},
		{"hw.cpufamily", 0x1b588bb3, 4}, {"hw.ncpu", 1, 4}, {"hw.activecpu", 1, 4}, {"hw.physicalcpu", 1, 4}, {"hw.logicalcpu", 1, 4},
		{"hw.optional.arm64", 1, 4}, {"hw.optional.floatingpoint", 1, 4}, {"hw.optional.neon", 1, 4}, {"hw.cachelinesize", 64, 8},
	};
	for (unsigned i = 0; i < sizeof v / sizeof *v; i++) if (!strcmp(name, v[i].n)) {
		if (ob && *ol >= (size_t)v[i].sz) memcpy(ob, &v[i].v, v[i].sz);
		*ol = v[i].sz; return 1;
	}
	if (!strcmp(name, "hw.machine") || !strcmp(name, "hw.targettype")) { if (ob && *ol >= 6) memcpy(ob, "arm64", 6); *ol = 6; return 1; }
	return 0;
}


// --- posix_spawn (Systemaufruf 244): Aufbau von struct _posix_spawn_args_desc und der Dateiaktionen wie in ucrun (XNU spawn_internal.h) ---
static void spawn_apply_actions(const uint8_t *fa, size_t fa_size) {
	if (!fa || fa_size < 8) return;
	int count = *(const int *)(fa + 4); const size_t asz = 8 + 1032;  // _psfa_action: type, fd, {oflag, mode, path[1024]}
	for (int i = 0; i < count && 8 + (size_t)(i + 1) * asz <= fa_size; i++) {
		const uint8_t *a = fa + 8 + (size_t)i * asz; int type = *(const int *)a, fd = *(const int *)(a + 4);
		switch (type) {
		case 0: {  // OPEN
			int oflag = *(const int *)(a + 8); unsigned mode = *(const uint16_t *)(a + 12); char p[1100]; strncpy(p, (const char *)(a + 14), 1023); p[1023] = 0;
			char pb[1200]; int nfd = open(guest_path(p, pb, sizeof pb), oflag, mode); if (nfd < 0) _exit(127);
			if (nfd != fd) { dup2(nfd, fd); close(nfd); }
			break; }
		case 1: close(fd); break;
		case 2: { int nfd = *(const int *)(a + 8); if (fd == nfd) { int fl = fcntl(fd, F_GETFD); if (fl >= 0) fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC); } else dup2(fd, nfd); break; }
		case 5: { char p[1100]; strncpy(p, (const char *)(a + 8), 1023); p[1023] = 0; if (chdir(p)) _exit(127); break; }
		case 6: if (fchdir(fd)) _exit(127); break;
		default: break;
		}
	}
}
// --- Threads (bsdthread_create), GIL, Signale, ulock/Bedingungsvariablen/Semaphore: das Konzept von ucrun, für AArch64 ---
// Jeder Gast-Thread hat eine eigene Unicorn-Maschine, angelegt im eigenen Host-Thread (Apples JIT-Speicher gehört dem anlegenden
// Thread). Nur eine Maschine rechnet gleichzeitig (Ticket-Sperre), weil Unicorns Speicherabbild nicht thread-sicher ist; die Sperre wird
// bei blockierenden Aufrufen und nach einer Zeitscheibe freigegeben.
static void do_syscall(uc_engine *u, uint32_t intno, void *ud);
static bool on_fault(uc_engine *u, uc_mem_type t, uint64_t addr, int size, int64_t val, void *ud);
typedef struct Saved { uint64_t x[31], sp, pc, nzcv, tpidr; uint32_t oldmask; uint8_t q[32][16]; } Saved;
typedef struct Th {
	uc_engine *u; uint64_t pc, sp, x[6], tpidr; volatile int done, resume, fault_sig; uint64_t resume_pc, fault_addr;
	Saved *ss; int sd; uint64_t tid;
} Th;
static __thread Th *cth;
static Th *eth[64];
static Th main_th;
static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t gc = PTHREAD_COND_INITIALIZER; static unsigned g_next, g_serving;
static void gil_lock(void) { pthread_mutex_lock(&gm); unsigned my = g_next++; while (g_serving != my) pthread_cond_wait(&gc, &gm); pthread_mutex_unlock(&gm); }
static void gil_unlock(void) { pthread_mutex_lock(&gm); g_serving++; pthread_cond_broadcast(&gc); pthread_mutex_unlock(&gm); }
#define BLOCKING_BEGIN gil_unlock()
#define BLOCKING_END gil_lock()
static uint64_t slice_us = 1000000;  // Zeitscheibe 1 s: Wechsel praktisch nur an blockierenden Aufrufen
static uint64_t thread_entry, tsd_offset, mts_offset;  // von bsdthread_register

// Anhalten: Unicorn prüft nach jedem Gast-Speicherzugriff, ob ein Halt angefordert ist, und setzt dann auf den Anfang DIESES Befehls zurück. Ein von
// außen (Zeitgeber, Signal-Handler) mitten im Block angeforderter Halt führt Ladebefehle wie "ldr x8, [x8]" deshalb zweimal aus (die Basis ist dann schon
// überschrieben). Darum fordert nur ein Block-Hook den Halt an: er läuft vor dem ersten Befehl des Blocks, dort ist der Zustand genau.
static volatile int stop_flag;
static void on_block(uc_engine *u, uint64_t addr, uint32_t size, void *ud) { (void)addr; (void)size; (void)ud; if (stop_flag) { stop_flag = 0; uc_emu_stop(u); } }
static void *slice_timer(void *p) { (void)p; for (;;) { usleep(slice_us); stop_flag = 1; } return 0; }
static uc_engine *new_engine(void) {
	uc_engine *e; if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &e)) return 0;
	uc_ctl_set_cpu_model(e, getenv("A64_CPU") && !strcmp(getenv("A64_CPU"), "a57") ? UC_CPU_ARM64_A57 : UC_CPU_ARM64_MAX);  // alle Befehlssatzerweiterungen (LSE-Atomics, Crypto ...): Apple-Programme setzen sie voraus
	uint64_t cpacr = 3 << 20; uc_reg_write(e, UC_ARM64_REG_CPACR_EL1, &cpacr);  // Gleitkomma/NEON im EL0 freigeben
	pthread_mutex_lock(&big);
	for (int i = 0; i < nregs; i++) uc_mem_map_ptr(e, regs[i].g, regs[i].n, UC_PROT_ALL, regs[i].h);
	engs[neng++] = e;
	pthread_mutex_unlock(&big);
	uc_hook h1, h2, h3; uc_hook_add(e, &h3, UC_HOOK_BLOCK, on_block, 0, 1, 0); uc_hook_add(e, &h1, UC_HOOK_INTR, do_syscall, 0, 1, 0); uc_hook_add(e, &h2, UC_HOOK_MEM_UNMAPPED, on_fault, 0, 1, 0);
	return e;
}
static Th *th_of(uc_engine *u) { for (int i = 0; i < 64; i++) if (eth[i] && eth[i]->u == u) return eth[i]; return 0; }

// --- Signale ---
// Echte Signale fängt ein Host-Handler ab und merkt sie vor; zugestellt wird zwischen zwei Zeitscheiben im Gast-Thread: der Zustand wird
// gesichert, ein Rahmen gebaut und in die __sigtramp des Gastes gesprungen (x0 = Handler, x1 = Art, x2 = Signal, x3 = siginfo, x4 = ucontext).
// sigreturn stellt den gesicherten Zustand wieder her (das ucontext des Gastes wird ignoriert).
static struct { uint64_t handler, tramp; uint32_t mask, flags; } gsa[32];
static volatile uint32_t sig_pending; static uint32_t gmask;
static int sig_wake = 1;
static void hsig(int s) { __atomic_or_fetch(&sig_pending, 1u << s, __ATOMIC_SEQ_CST); if (sig_wake) stop_flag = 1; }

static int sig_waiting(void) { return (__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1)) != 0; }

static void push_signal(Th *t, int sig, uint64_t addr) {
	uc_engine *u = t->u; if (!t->ss) t->ss = calloc(16, sizeof(Saved));
	Saved *sv = &t->ss[t->sd++ & 15];
	for (int i = 0; i < 29; i++) uc_reg_read(u, UC_ARM64_REG_X0 + i, &sv->x[i]);  // x29/x30 stehen in der Aufzählung vorn, daher einzeln
	uc_reg_read(u, UC_ARM64_REG_X29, &sv->x[29]); uc_reg_read(u, UC_ARM64_REG_X30, &sv->x[30]);
	uc_reg_read(u, UC_ARM64_REG_SP, &sv->sp); uc_reg_read(u, UC_ARM64_REG_PC, &sv->pc); uc_reg_read(u, UC_ARM64_REG_NZCV, &sv->nzcv);
	uc_reg_read(u, UC_ARM64_REG_TPIDRRO_EL0, &sv->tpidr);
	if (t->resume) sv->pc = t->resume_pc;
	sv->oldmask = gmask;
	for (int i = 0; i < 32; i++) uc_reg_read(u, UC_ARM64_REG_Q0 + i, sv->q[i]);
	uint64_t sp = (sv->sp - 128 - 1024 - 256) & ~15ULL, uctx = sp + 256, info = sp;   // Rotes-Zonen-Abstand, dann siginfo (128 Byte) + ucontext
	uint8_t z[256 + 1024] = {0}; uc_mem_write(u, sp, z, sizeof z);
	uint32_t hdr[3] = {(uint32_t)sig, 0, 1}; uc_mem_write(u, info, hdr, 12); uc_mem_write(u, info + 24, &addr, 8);
	uint64_t style = (gsa[sig].flags & 0x40) ? 30 : 1, h = gsa[sig].handler, sg = sig, zero = 0;
	uc_reg_write(u, UC_ARM64_REG_X0, &h); uc_reg_write(u, UC_ARM64_REG_X1, &style); uc_reg_write(u, UC_ARM64_REG_X2, &sg);
	uc_reg_write(u, UC_ARM64_REG_X3, &info); uc_reg_write(u, UC_ARM64_REG_X4, &uctx); uc_reg_write(u, UC_ARM64_REG_X5, &zero);
	uc_reg_write(u, UC_ARM64_REG_X30, &zero); uc_reg_write(u, UC_ARM64_REG_SP, &sp);
	uc_reg_write(u, UC_ARM64_REG_PC, &gsa[sig].tramp);
	t->resume = 0;
	if (trace) fprintf(stderr, "a64run: Signal %d: gesichert pc %#llx sp %#llx, Rahmen sp %#llx\n", sig, (unsigned long long)sv->pc, (unsigned long long)sv->sp, (unsigned long long)sp);
	gmask |= gsa[sig].mask | ((gsa[sig].flags & 0x10) ? 0 : (1u << (sig - 1)));
}
static void pop_signal(Th *t) {
	uc_engine *u = t->u; Saved *sv = &t->ss[--t->sd & 15];
	for (int i = 0; i < 29; i++) uc_reg_write(u, UC_ARM64_REG_X0 + i, &sv->x[i]);
	uc_reg_write(u, UC_ARM64_REG_X29, &sv->x[29]); uc_reg_write(u, UC_ARM64_REG_X30, &sv->x[30]);
	uc_reg_write(u, UC_ARM64_REG_SP, &sv->sp); uc_reg_write(u, UC_ARM64_REG_NZCV, &sv->nzcv);
	for (int i = 0; i < 32; i++) uc_reg_write(u, UC_ARM64_REG_Q0 + i, sv->q[i]);
	t->resume_pc = sv->pc; t->resume = 1; gmask = sv->oldmask;
}
static int fault_signal(uc_err e) {
	switch (e) {
	case UC_ERR_READ_UNMAPPED: case UC_ERR_WRITE_UNMAPPED: case UC_ERR_FETCH_UNMAPPED: case UC_ERR_READ_PROT: case UC_ERR_WRITE_PROT: case UC_ERR_FETCH_PROT: return 11;
	case UC_ERR_INSN_INVALID: return 4;
	default: return 0;
	}
}
static int deliver_pending(Th *t, uint64_t *pc) {
	uint32_t p = __atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1);
	for (int s = 1; s < 32; s++) if (p & (1u << s)) {
		__atomic_and_fetch(&sig_pending, ~(1u << s), __ATOMIC_SEQ_CST);
		if (gsa[s].handler > 1 && gsa[s].tramp) { uc_reg_write(t->u, UC_ARM64_REG_PC, pc); push_signal(t, s, 0); uc_reg_read(t->u, UC_ARM64_REG_PC, pc); return 1; }
	}
	return 0;
}
static uc_err run_engine(Th *t) {
	uint64_t pc = t->pc; uc_err e = UC_ERR_OK;
	for (;;) {
		gil_lock();
		deliver_pending(t, &pc);
		e = uc_emu_start(t->u, pc, 0, 0, 0);
		uc_reg_read(t->u, UC_ARM64_REG_PC, &pc);
		if (t->resume) { pc = t->resume_pc; t->resume = 0; }
		int sg = t->fault_sig; t->fault_sig = 0;
		if (!sg && e && !t->done) sg = fault_signal(e);
		if (sg && !t->done) {
			if (gsa[sg].handler > 1 && gsa[sg].tramp && !(gmask & (1u << (sg - 1)))) { uc_reg_write(t->u, UC_ARM64_REG_PC, &pc); push_signal(t, sg, t->fault_addr); uc_reg_read(t->u, UC_ARM64_REG_PC, &pc); e = UC_ERR_OK; }
			else if (!e) { fprintf(stderr, "a64run: Ausnahme (Signal %d) bei pc %#llx\n", sg, (unsigned long long)pc); gil_unlock(); return UC_ERR_EXCEPTION; }
		}
		gil_unlock();
		if (e || t->done) return e;
		sched_yield();
	}
}

// --- __ulock_wait/wake: Warteschlange je Adresse (FIFO); Ergebnis = Zahl der übrigen Wartenden ---
static pthread_mutex_t wmx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t wcv = PTHREAD_COND_INITIALIZER;
static uint32_t dead[256]; static int ndead;
static int is_dead(uint32_t v) { for (int i = 0; i < ndead; i++) if (dead[i] == v) return 1; return 0; }
typedef struct UlW { uint64_t addr, val; int woke; struct UlW *next; } UlW;
static UlW *ulq_head, *ulq_tail;
static void deadline_after(struct timespec *ts, uint64_t us) { clock_gettime(CLOCK_REALTIME, ts); ts->tv_sec += us / 1000000; ts->tv_nsec += (us % 1000000) * 1000; if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; } }
static int ts_before(const struct timespec *a, const struct timespec *b) { return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec); }
// wartet in Stücken von 20 ms (Signale, Tod des Eigners); 0 = geweckt, sonst errno
static int ulock_wait(uint64_t addr, int is64, uint64_t val, uint64_t timeout_us, int *remaining) {
	struct timespec ts; if (timeout_us) deadline_after(&ts, timeout_us);
	uint32_t *p = g2h32(addr); if (!p) return EFAULT;
	BLOCKING_BEGIN; pthread_mutex_lock(&wmx); int r = 0; *remaining = 0;
	uint64_t cur = is64 ? __atomic_load_n((uint64_t *)p, __ATOMIC_SEQ_CST) : __atomic_load_n(p, __ATOMIC_SEQ_CST);
	if (cur == val) {
		UlW w = {addr, val, 0, 0};
		if (ulq_tail) ulq_tail->next = &w; else ulq_head = &w; ulq_tail = &w;
		while (!w.woke && !is_dead((uint32_t)val & ~3u)) {
			if (sig_waiting()) { r = EINTR; break; }
			struct timespec slice; deadline_after(&slice, 20000);
			if (timeout_us && ts_before(&ts, &slice)) slice = ts;
			if (pthread_cond_timedwait(&wcv, &wmx, &slice) && timeout_us) { struct timespec n; clock_gettime(CLOCK_REALTIME, &n); if (!ts_before(&n, &ts)) { r = ETIMEDOUT; break; } }
		}
		UlW **pp = &ulq_head, *prev = 0; for (; *pp; prev = *pp, pp = &(*pp)->next) if (*pp == &w) { *pp = w.next; if (ulq_tail == &w) ulq_tail = prev; break; }
		for (UlW *q = ulq_head; q; q = q->next) if (q->addr == addr) (*remaining)++;
	}
	pthread_mutex_unlock(&wmx); BLOCKING_END; return r;
}
static int ulock_wake_addr(uint64_t addr, int all) {
	int n = 0; pthread_mutex_lock(&wmx);
	for (UlW *w = ulq_head; w; w = w->next) if (w->addr == addr && !w->woke) { w->woke = 1; n++; if (!all) break; }
	pthread_cond_broadcast(&wcv); pthread_mutex_unlock(&wmx); return n ? 0 : ENOENT;
}
static void ulock_wake_all_waiters(void) { pthread_mutex_lock(&wmx); pthread_cond_broadcast(&wcv); pthread_mutex_unlock(&wmx); }

// --- Bedingungsvariablen (psynch_cv*) und Mach-Semaphore: eine globale FIFO-Warteschlange ---
typedef struct CvW { uint64_t addr; int woke; struct CvW *next; } CvW;
static CvW *cvq_head, *cvq_tail;
static struct { uint32_t port; int64_t value; } sems[256]; static int nsems;
static void abs_deadline(struct timespec *ts, int64_t sec, int64_t nsec) {  // sec > 1e8: absolute Echtzeit, sonst relativ
	if (sec > 100000000) { ts->tv_sec = sec; ts->tv_nsec = nsec; }
	else { clock_gettime(CLOCK_REALTIME, ts); ts->tv_sec += sec; ts->tv_nsec += nsec; if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; } }
}
static int cv_wait(uint64_t addr, int has_timeout, int64_t sec, int64_t nsec) {  // 0, ETIMEDOUT, EINTR
	struct timespec ts; if (has_timeout) abs_deadline(&ts, sec, nsec);
	CvW w = {addr, 0, 0}; int r = 0;
	BLOCKING_BEGIN; pthread_mutex_lock(&wmx);
	if (cvq_tail) cvq_tail->next = &w; else cvq_head = &w; cvq_tail = &w;
	while (!w.woke) {
		if (sig_waiting()) { r = EINTR; break; }
		struct timespec slice; deadline_after(&slice, 20000);
		if (has_timeout && ts_before(&ts, &slice)) slice = ts;
		int cr = pthread_cond_timedwait(&wcv, &wmx, &slice);
		if (cr && has_timeout) { struct timespec n; clock_gettime(CLOCK_REALTIME, &n); if (!ts_before(&n, &ts)) { r = ETIMEDOUT; break; } }
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
static int sem_wait(uint32_t port, int has_timeout, int64_t sec, int64_t nsec) {  // 0, 3 = KERN_INVALID_ARGUMENT, 49 = KERN_OPERATION_TIMED_OUT, 14 = KERN_ABORTED
	struct timespec ts; if (has_timeout) abs_deadline(&ts, sec, nsec);
	int r = 0; BLOCKING_BEGIN; pthread_mutex_lock(&wmx);
	int i = sem_index(port); if (i < 0) r = 3;
	while (!r && sems[i].value <= 0) {
		if (sig_waiting()) { r = 14; break; }
		struct timespec slice; deadline_after(&slice, 20000);
		if (has_timeout && ts_before(&ts, &slice)) slice = ts;
		if (pthread_cond_timedwait(&wcv, &wmx, &slice) && has_timeout) { struct timespec n; clock_gettime(CLOCK_REALTIME, &n); if (!ts_before(&n, &ts)) { r = 49; break; } }
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


static void *thread_main(void *p) {
	Th *t = p;
	gil_lock(); t->u = new_engine(); uc = t->u; cth = t;
	for (int q = 0; q < 64; q++) if (!eth[q]) { eth[q] = t; break; }
	uc_reg_write(t->u, UC_ARM64_REG_TPIDRRO_EL0, &t->tpidr); uc_reg_write(t->u, UC_ARM64_REG_SP, &t->sp);
	for (int i = 0; i < 6; i++) uc_reg_write(t->u, UC_ARM64_REG_X0 + i, &t->x[i]);
	gil_unlock();
	uc_err e = run_engine(t);
	if (e) { uint64_t pc; uc_reg_read(t->u, UC_ARM64_REG_PC, &pc); fprintf(stderr, "a64run: Thread: %s bei pc %#llx\n", uc_strerror(e), (unsigned long long)pc); exit(4); }
	return 0;
}

static uint64_t ring[64]; static unsigned ringn; static int use_ring;
static void on_code(uc_engine *u, uint64_t addr, uint32_t size, void *ud) { (void)u; (void)size; (void)ud; ring[ringn++ & 63] = addr; }
static void dump_ring(void) {
	if (!use_ring) return;
	fprintf(stderr, "  letzte Befehle (alt -> neu):");
	for (unsigned q = ringn > 40 ? ringn - 40 : 0; q < ringn; q++) { uint32_t w = 0; uc_mem_read(uc, ring[q & 63], &w, 4); fprintf(stderr, " %llx:%08x", (unsigned long long)ring[q & 63], w); }
	fputc(10, stderr);
}
static void do_syscall(uc_engine *u, uint32_t intno, void *ud) {
	(void)u; (void)ud;
	if (intno != 2) {  // Ausnahme im Gast: BRK -> SIGTRAP, undefinierter Befehl -> SIGILL, sonst SIGSEGV; run_engine entscheidet (Handler oder Ende)
		uint64_t pc; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
		uint32_t insn = 0; uc_mem_read(uc, pc, &insn, 4);
		if (!(gsa[intno == 7 ? 5 : intno == 1 ? 4 : 11].handler > 1)) {
			fprintf(stderr, "a64run: Ausnahme %u bei pc %#llx, Befehl %08x, x0-x3 %#llx %#llx %#llx %#llx\n", intno, (unsigned long long)pc, insn,
				(unsigned long long)X(0), (unsigned long long)X(1), (unsigned long long)X(2), (unsigned long long)X(3));
			dump_ring();
		}
		if (cth) { cth->fault_sig = intno == 7 ? 5 : intno == 1 ? 4 : 11; cth->resume = 1; cth->resume_pc = pc; }
		uc_emu_stop(uc); return;
	}
	uint64_t x16 = X(16); if (x16 != 0x80000000ULL && (x16 >> 32) == 0 && (int32_t)x16 < 0) x16 = (uint64_t)(int64_t)(int32_t)x16;  // mov w16, #-3 füllt die obere Hälfte mit 0
	uint64_t a[8], x1v = 0; int64_t res = 0; int err = 0; nsys++;
	for (int i = 0; i < 8; i++) a[i] = X(i);
	if ((int64_t)x16 < 0) {  // Mach-Fallen
		int64_t tn = -(int64_t)x16;
		if (trace) fprintf(stderr, "a64run: mach trap %lld(%#llx, %#llx, %#llx, %#llx, %#llx, %#llx)\n", (long long)tn, (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3], (unsigned long long)a[4], (unsigned long long)a[5]);
		switch (tn) {
		case 3: { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); res = ts.tv_sec * 1000000000ULL + ts.tv_nsec; break; }  // mach_absolute_time (Commpage ohne Zeitbasis)
		case 4: { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); res = ts.tv_sec * 1000000000ULL + ts.tv_nsec; break; }  // mach_continuous_time
		case 26: res = 0x303; break;   // mach_reply_port
		case 27: res = 0x203; break;   // thread_self_trap
		case 28: res = 0x103; break;   // task_self_trap
		case 29: res = 0x403; break;   // host_self_trap
		case 10: {                     // _kernelrpc_mach_vm_allocate_trap(task, *addr, size, flags)
			uint64_t want = 0; uc_mem_read(uc, a[1], &want, 8);
			uint64_t g = gmap((a[3] & 1) ? 0 : want, a[2], !(a[3] & 1) && want);
			if (!g) { res = 3; break; }
			uc_mem_write(uc, a[1], &g, 8); break; }
		case 89: { uint32_t tb[2] = {1, 1}; uc_mem_write(uc, a[0], tb, 8); break; }  // mach_timebase_info_trap
		case 15: {                     // _kernelrpc_mach_vm_map_trap(task, *addr, size, mask, flags, prot)
			uint64_t al = a[3] + 1; if (al > 1) mmap_next = (mmap_next + al - 1) & ~(al - 1);
			uint64_t g = gmap(0, a[2], 0);
			if (!g) { res = 3; break; }
			uc_mem_write(uc, a[1], &g, 8); break; }
		case 16: { uint32_t nm = 0x1003; uc_mem_write(uc, a[2], &nm, 4); break; }  // mach_port_allocate_trap(task, right, *name)
		case 12: unmap_range(a[1] & ~(PAGE - 1), (a[2] + PAGE - 1) & ~(PAGE - 1)); break;  // mach_vm_deallocate
		case 14: break;                // mach_vm_protect: Gastspeicher bleibt RWX
		case 18: case 19: break;       // mach_port_deallocate / mod_refs
		case 33: res = sem_signal((uint32_t)a[0], 0); break;        // semaphore_signal_trap
		case 34: res = sem_signal((uint32_t)a[0], 1); break;        // semaphore_signal_all_trap
		case 36: res = sem_wait((uint32_t)a[0], 0, 0, 0); break;    // semaphore_wait_trap
		case 37: sem_signal((uint32_t)a[0], 0); res = sem_wait((uint32_t)a[1], 0, 0, 0); break;  // semaphore_wait_signal_trap
		case 38: res = sem_wait((uint32_t)a[0], 1, (int64_t)a[1], (int64_t)a[2]); break;        // semaphore_timedwait_trap
		case 39: sem_signal((uint32_t)a[0], 0); res = sem_wait((uint32_t)a[1], 1, (int64_t)a[2], (int64_t)a[3]); break;
		case 31: {                     // mach_msg_trap: nur Nachrichten an den Host-Port, als kleine MIG-Server
			uint8_t m[0x40]; uc_mem_read(uc, a[0], m, 0x40); uint32_t id = *(uint32_t *)(m + 20);
			if (trace) fprintf(stderr, "a64run: mach_msg id %u an Port %#x flavor %u count %u\n", id, *(uint32_t *)(m + 8), *(uint32_t *)(m + 32), *(uint32_t *)(m + 36));
			if (id == 200) {  // host_info(flavor, count)
				uint32_t flavor = *(uint32_t *)(m + 32);
				if (flavor == 1) {  // HOST_BASIC_INFO
					uint32_t p[1 + 12] = {12, 1, 1, 4096, CPU_ARM64, 2, 0, 1, 1, 1, 1, 0, 1};
					p[11] = 0; p[12] = 1;
					mig_reply(a[0], 300, 0, p, sizeof p);
				} else if (flavor == 5) {
					uint32_t p[1 + 8] = {8, 81, 80, 80, 31, 0, 0, 0, 127};
					mig_reply(a[0], 300, 0, p, sizeof p);
				} else mig_reply(a[0], 300, 4, 0, 0);
			} else if (id == 3409) { mig_reply_port(a[0], 3509, 0);   // task_get_special_port: kein Bootstrap-Server -> MACH_PORT_NULL
			} else if (id == 3418) {  // semaphore_create(task, policy, value): neuer Port-Name
				int32_t val = *(int32_t *)(m + 36); uint32_t port = 0x1000005;
				pthread_mutex_lock(&wmx); if (nsems < 256) { port = 0x5003 + 4 * nsems; sems[nsems].port = port; sems[nsems].value = val; nsems++; } pthread_mutex_unlock(&wmx);
				mig_reply_port(a[0], 3518, port);
			} else if (id == 3419) { mig_reply(a[0], 3519, 0, 0, 0);  // semaphore_destroy
			} else if (id == 206) { mig_reply_port(a[0], 306, 0x1004); // host_get_clock_service
			} else { if (trace || warn) fprintf(stderr, "a64run: mach_msg id %u nicht umgesetzt\n", id); mig_reply(a[0], id + 100, 5, 0, 0); }
			break; }
		default: if (trace || warn) fprintf(stderr, "a64run: Mach-Falle %lld nicht umgesetzt\n", (long long)tn); res = 5;
		}
		setX(0, res); return;
	}
	if (x16 == 0x80000000ULL) {  // Maschinenfalle: x3 = 2 thread_set_tsd_base(x0)
		if (X(3) == 2) { uc_reg_write(uc, UC_ARM64_REG_TPIDRRO_EL0, &a[0]); res = 0; }
		else { if (trace || warn) fprintf(stderr, "a64run: Maschinenfalle %llu nicht umgesetzt\n", (unsigned long long)X(3)); res = 0; }
		setX(0, res); return;
	}
	uint64_t nr = x16;
	{ static const struct { int from, to; } nc[] = {{423, 334}, {396, 3}, {397, 4}, {398, 5}, {399, 6}, {400, 7}, {406, 92}, {410, 111}, {414, 153}, {415, 154}};  // *_nocancel-Varianten
	  for (unsigned q = 0; q < sizeof nc / sizeof *nc; q++) if (nc[q].from == (int)nr) { nr = nc[q].to; break; } }
	if (trace) fprintf(stderr, "a64run: sys %llu(%#llx, %#llx, %#llx)\n", (unsigned long long)nr, (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2]);
	if (nr == 1) {
		if (getenv("A64_STATS")) fprintf(stderr, "a64run: %.3f s, %lu Systemaufrufe\n", now() - t0, nsys);
		exit((int)a[0]);
	}
	if (nr == 197) {  // mmap(addr, len, prot, flags, fd, off): anonym oder Datei (Inhalt wird kopiert)
		uint64_t g = gmap(a[0], a[1], (a[3] & MAP_FIXED) != 0);
		if (!g) { err = ENOMEM; res = ENOMEM; goto done; }
		if (!(a[3] & MAP_ANON)) {
			char *b = malloc(a[1]); ssize_t n = pread((int)a[4], b, a[1], (off_t)a[5]);
			if (n > 0) uc_mem_write(uc, g, b, n);
			free(b);
		}
		res = g; goto done;
	}
	switch (nr) {  // Aufrufe, die nur beim Start nach dem Zustand des Systems fragen
	case 483: case 381: case 520: case 521: case 331: res = 0; goto done;                 // csrctl, __mac_syscall ...
	case 54: err = 25; res = 25; goto done;                              // ioctl: ENOTTY
	case 294: case 336: case 287: err = EINVAL; res = EINVAL; goto done; // shared_region_check_np (kein Cache), proc_info
	case 53: res = 0; goto done;                                         // sigaltstack: ignoriert
	}
	if (nr == 366) {  // bsdthread_register(threadstart, wqthread, pthsize, init_data, size, ...): tsd_offset steht bei init_data+24, mach-thread-self-Platz bei +32
		thread_entry = a[0]; uint32_t to = 0, mo = 0;
		if (a[3] && a[4] >= 28) uc_mem_read(uc, a[3] + 24, &to, 4);
		if (a[3] && a[4] >= 36) uc_mem_read(uc, a[3] + 32, &mo, 4);
		tsd_offset = to; mts_offset = mo; res = 0; goto done;
	}
	if (nr == 360) {  // bsdthread_create(func, arg, stack, pthread, flags): neue Unicorn-Maschine + Host-Thread, Startzustand wie vom Kernel
		uint64_t port = 0x2000 + nsys;
		static uint64_t next_tid = 2; Th *t = calloc(1, sizeof *t); t->pc = thread_entry; t->tid = __atomic_fetch_add(&next_tid, 1, __ATOMIC_SEQ_CST);
		t->sp = (a[2] - 128) & ~15ULL; t->x[0] = a[3]; t->x[1] = port; t->x[2] = a[0]; t->x[3] = a[1]; t->x[4] = a[2]; t->x[5] = a[4] | 0x10000000;
		t->tpidr = a[3] + tsd_offset;
		{ uint32_t p32 = (uint32_t)port; if (mts_offset) uc_mem_write(uc, a[3] + tsd_offset + mts_offset, &p32, 4); }  // der Kernel trägt den Mach-Thread-Port ins pthread ein
		pthread_t pt; pthread_attr_t pa; pthread_attr_init(&pa); pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
		pthread_create(&pt, &pa, thread_main, t); res = a[3]; goto done;
	}
	if (nr == 361) {  // bsdthread_terminate(stackaddr, freesize, port, joinsem): Gast-Thread endet, pthread_join wird über ulock geweckt
		pthread_mutex_lock(&wmx); if (ndead < 256) dead[ndead++] = (uint32_t)a[2]; pthread_mutex_unlock(&wmx); ulock_wake_all_waiters();
		if (cth) cth->done = 1; uc_emu_stop(uc); res = 0; goto done;
	}
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
	if (nr == 305) {  // __psynch_cvwait(cv, cvlsgen, cvugen, mutex, mugen, flags, sec, nsec): auf arm64 stehen sec/nsec in x6/x7
		int hasto = a[6] != 0 || a[7] != 0;
		int r = cv_wait(a[0], hasto, (int64_t)a[6], (int64_t)a[7]);
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
			if (sig_waiting()) { intr = 1; break; }
		}
		BLOCKING_END; err = intr ? EINTR : ETIMEDOUT; res = err; goto done;
	}
	if (nr == 46) {  // sigaction(sig, struct __sigaction *nsa {handler, tramp, mask, flags}, struct sigaction *osa {handler, mask, flags})
		int sg = (int)a[0]; if (sg <= 0 || sg >= 32 || sg == 9 || sg == 17) { err = EINVAL; res = EINVAL; goto done; }
		if (a[2]) { uint8_t ob[16]; memcpy(ob, &gsa[sg].handler, 8); memcpy(ob + 8, &gsa[sg].mask, 4); memcpy(ob + 12, &gsa[sg].flags, 4); uc_mem_write(uc, a[2], ob, 16); }
		if (a[1]) {
			uint8_t nb[24]; uc_mem_read(uc, a[1], nb, 24); uint64_t hd, tr; uint32_t mk, fl; memcpy(&hd, nb, 8); memcpy(&tr, nb + 8, 8); memcpy(&mk, nb + 16, 4); memcpy(&fl, nb + 20, 4);
			gsa[sg].handler = hd; gsa[sg].tramp = tr; gsa[sg].mask = mk; gsa[sg].flags = fl;
			struct sigaction ha; memset(&ha, 0, sizeof ha);
			if (hd == 0) ha.sa_handler = SIG_DFL; else if (hd == 1) ha.sa_handler = SIG_IGN; else { ha.sa_handler = hsig; ha.sa_flags = (fl & 2) ? SA_RESTART : 0; }
			sigaction(sg, &ha, 0);
		}
		res = 0; goto done;
	}
	if (nr == 48 || nr == 329) {  // sigprocmask / __pthread_sigmask(how, set*, oset*): eine prozessweite Maske genügt hier
		uint32_t old = gmask, nm = 0; if (a[1]) uc_mem_read(uc, a[1], &nm, 4);
		if (a[2]) uc_mem_write(uc, a[2], &old, 4);
		if (a[1]) { nm &= ~((1u << 8) | (1u << 16)); gmask = a[0] == 1 ? old | nm : a[0] == 2 ? old & ~nm : nm; }
		res = 0; goto done;
	}
	if (nr == 52) { uint32_t pnd = (sig_pending >> 1) & gmask; uc_mem_write(uc, a[0], &pnd, 4); res = 0; goto done; }   // sigpending
	if (nr == 111 || nr == 429) {  // sigsuspend(mask) / pause: auf ein Signal warten, Maske für die Dauer ersetzen
		uint32_t nm = 0; if (nr == 111 && a[0]) uc_mem_read(uc, a[0], &nm, 4); uint32_t old = gmask; gmask = nm;
		BLOCKING_BEGIN; while (!sig_waiting()) usleep(500); BLOCKING_END;
		gmask = old; err = EINTR; res = EINTR; goto done;
	}
	if (nr == 184) {  // sigreturn(uctx, style, token): gesicherten Zustand wiederherstellen
		if (a[0] == 0 || !cth || cth->sd <= 0) { res = 0; goto done; }  // _longjmp: sigreturn(NULL, ...) setzt nur den Alternativstapel zurück
		pop_signal(cth);
		if (trace) fprintf(stderr, "a64run: sigreturn: pc %#llx\n", (unsigned long long)cth->resume_pc);
		uc_emu_stop(uc); return;  // run_engine setzt den Zähler neu auf (resume_pc), x0 darf nicht überschrieben werden
	}
	if (nr == 328) { if (a[1]) kill(getpid(), (int)a[1]); res = 0; goto done; }  // __pthread_kill(thread, sig): prozessweit genügt
	if (nr == 274) {  // __sysctlbyname(name, namelen, oldp, oldlenp, newp, newlen)
		char nm[256] = {0}; uint64_t nl = a[1] < 255 ? a[1] : 255; uc_mem_read(uc, a[0], nm, nl);
		size_t ol = 0; if (a[3]) uc_mem_read(uc, a[3], &ol, 8);
		char ob[512] = {0}; size_t cap = ol < sizeof ob ? ol : sizeof ob; size_t l2 = cap;
		if (a[4]) { err = EPERM; res = EPERM; goto done; }
		if (fake_sysctl_name(nm, ob, &l2)) { res = 0; if (a[2] && l2 > ol) { err = ENOMEM; res = ENOMEM; } }
		else { l2 = cap; res = sysctlbyname(nm, a[2] ? ob : 0, &l2, 0, 0); if (res) { err = errno; res = errno; } }
		if (!err && a[2]) uc_mem_write(uc, a[2], ob, l2 < sizeof ob ? l2 : sizeof ob);
		if (a[3] && (!err || err == ENOMEM)) uc_mem_write(uc, a[3], &l2, 8);
		goto done;
	}
	if (nr == 92) {  // fcntl: Code-Signatur-Befehle gibt es hier nicht
		int cmd = (int)a[1];
		if (cmd == 98) { res = 0; goto done; }                                  // F_CHECK_LV
		if (cmd == 59 || cmd == 61 || cmd == 97 || cmd == 99) { err = 45; res = 45; goto done; }  // F_ADDFILESIGS*: ENOTSUP
		if (cmd == 50) { char p[1024] = {0}; res = fcntl((int)a[0], 50, p); if (res == -1) { err = errno; res = errno; } else uc_mem_write(uc, a[2], p, 1024); goto done; }
		res = fcntl((int)a[0], cmd, a[2]); if (res == -1) { err = errno; res = errno; } goto done;
	}
	if (nr == 202) {  // sysctl(name, namelen, oldp, oldlenp, newp, newlen)
		int mib[16]; uint32_t nl = a[1] > 16 ? 16 : a[1]; uc_mem_read(uc, a[0], mib, nl * 4);
		size_t ol = 0; if (a[3]) uc_mem_read(uc, a[3], &ol, 8);
		if (trace) fprintf(stderr, "a64run: sysctl mib %d.%d.%d len %u oldlen %zu\n", mib[0], nl > 1 ? mib[1] : 0, nl > 2 ? mib[2] : 0, nl, ol);
		char *ob = a[2] ? calloc(1, ol ? ol : 1) : 0; void *nb = 0;
		if (a[4]) { nb = malloc(a[5]); uc_mem_read(uc, a[4], nb, a[5]); }
		if (nl >= 2 && mib[0] == 6 && mib[1] == 1 && ob && ol >= 6) { memcpy(ob, "arm64", 6); ol = 6; res = 0; }          // hw.machine
		else if (nl >= 2 && mib[0] == 6 && mib[1] == 7 && ob && ol >= 8) { uint64_t ps = PAGE; if (ol >= 8) memcpy(ob, &ps, 8); ol = 8; res = 0; }  // hw.pagesize
		else res = sysctl(mib, nl, ob, a[3] ? &ol : 0, nb, a[5]);
		if (res == -1) { err = errno; res = errno; }
		if (ob && (res == 0 || err == ENOMEM)) uc_mem_write(uc, a[2], ob, ol);
		if (a[3] && (res == 0 || err == ENOMEM)) uc_mem_write(uc, a[3], &ol, 8);
		free(ob); free(nb); goto done;
	}
	if (nr == 2 || nr == 66) {  // fork/vfork: Host-fork; Kind: x1 = 1 (Darwin-Konvention)
		fflush(0); pid_t pid = fork();
		if (pid < 0) { err = errno; res = errno; goto done; }
		res = pid; x1v = pid == 0 ? 1 : 0;
		if (pid == 0) { nsys = 0; pthread_t tt; pthread_attr_t ta; pthread_attr_init(&ta); pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED); pthread_create(&tt, &ta, slice_timer, 0); }  // Zeitgeber-Thread gibt es im Kind nicht mehr
		goto done;
	}
	if (nr == 244) {  // posix_spawn(pid_t *pid, path, adesc, argv, envp): fork + Dateiaktionen + execve (SETEXEC: ohne fork)
		uint64_t d[4] = {0, 0, 0, 0}; if (a[2]) uc_mem_read(uc, a[2], d, 32);
		uint8_t attr[64] = {0}; if (d[1]) uc_mem_read(uc, d[1], attr, d[0] < 64 ? d[0] : 64);
		short aflags = *(short *)attr; uint8_t *fa = 0; if (d[3] && d[2] > 0 && d[2] < (1u << 24)) { fa = malloc(d[2]); uc_mem_read(uc, d[3], fa, d[2]); }
		char *pth = gdup(a[1]); if (!pth) { err = EFAULT; res = EFAULT; goto done; }
		char rpb[1200]; const char *rp = guest_path(pth, rpb, sizeof rpb);
		if (access(rp, X_OK)) { err = errno; res = errno; free(fa); goto done; }  // Fehler beim Starten melden wie der Kernel
		if (aflags & 0x40) { spawn_apply_actions(fa, d[2]); int r = do_execve(a[1], a[3], a[4]); err = r; res = r; goto done; }
		fflush(0); pid_t pid = fork();
		if (pid < 0) { err = errno; res = errno; free(fa); goto done; }
		if (pid == 0) {
			spawn_apply_actions(fa, d[2]);
			if (aflags & 2) setpgid(0, *(pid_t *)(attr + 12));
			if (aflags & 4) { uint32_t sd = *(uint32_t *)(attr + 4); for (int sg = 1; sg < 32; sg++) if (sd & (1u << (sg - 1))) signal(sg, SIG_DFL); }
			do_execve(a[1], a[3], a[4]); _exit(127);
		}
		free(fa);
		if (a[0]) { int32_t p32 = pid; uc_mem_write(uc, a[0], &p32, 4); }
		res = 0; goto done;
	}
	if (nr == 59) { int r = do_execve(a[0], a[1], a[2]); err = r; res = r; goto done; }  // execve kehrt nur bei Fehler zurück
	if (nr == 7) {  // wait4(pid, *status, options, *rusage)
		int st = 0; struct rusage ru; BLOCKING_BEGIN; pid_t r = wait4((pid_t)a[0], &st, (int)a[2], a[3] ? &ru : 0); BLOCKING_END;
		if (r < 0) { err = errno; res = errno; goto done; }
		if (a[1]) uc_mem_write(uc, a[1], &st, 4);
		if (a[3]) uc_mem_write(uc, a[3], &ru, sizeof ru);
		res = r; goto done;
	}
	if (nr == 42) { int fds[2]; if (pipe(fds)) { err = errno; res = errno; goto done; } res = fds[0]; x1v = fds[1]; goto done; }  // pipe: x0 = Lese-, x1 = Schreibende
	if (nr == 90) { res = dup2((int)a[0], (int)a[1]); if (res < 0) { err = errno; res = errno; } goto done; }
	if (nr == 39) { res = getppid(); goto done; }
	if (nr == 37) { res = kill((pid_t)a[0], (int)a[1]); if (res < 0) { err = errno; res = errno; } goto done; }
	if (nr == 372) { res = cth ? cth->tid : 1; goto done; }  // thread_selfid: je Gast-Thread eine eigene Nummer (rekursive Mutexe vergleichen sie)
	if (nr == 73) { unmap_range(a[0] & ~(PAGE - 1), (a[1] + PAGE - 1) & ~(PAGE - 1)); res = 0; goto done; }
	if (nr == 74) { res = 0; goto done; }   // mprotect: Gastspeicher bleibt RWX
	const Sc *s = 0;
	for (unsigned i = 0; i < sizeof tab / sizeof *tab; i++) if (tab[i].nr == (int)nr) s = &tab[i];
	if (!s) { if (trace || warn) fprintf(stderr, "a64run: Systemaufruf %llu nicht umgesetzt\n", (unsigned long long)nr); err = ENOSYS; res = ENOSYS; goto done; }
	uint64_t h[6]; memcpy(h, a, sizeof h);
	char *bufs[3] = {0, 0, 0}; size_t sz[3] = {0, 0, 0}; char pathb[3][1100];
	for (int k = 0; k < 3; k++) {
		int i = s->p[k].idx; if (i < 0 || !s->p[k].dir) continue;
		if (s->p[k].dir == 's') {
			char raw[1024]; if (gstr(a[i], raw, sizeof raw)) { err = EFAULT; res = EFAULT; goto done; }
			strcpy(pathb[k], guest_path(raw, pathb[k], sizeof pathb[k]));
			h[i] = (uint64_t)pathb[k]; continue; }
		sz[k] = s->p[k].fixed ? (size_t)s->p[k].fixed : (size_t)a[s->p[k].size_arg];
		bufs[k] = calloc(1, sz[k] ? sz[k] : 1);
		if (s->p[k].dir == 'i') uc_mem_read(uc, a[i], bufs[k], sz[k]);
		h[i] = (uint64_t)bufs[k];
	}
	if (nr == 338) res = stat((char *)h[0], (struct stat *)h[1]);
	else if (nr == 340) res = lstat((char *)h[0], (struct stat *)h[1]);
	else if (nr == 339) res = fstat((int)h[0], (struct stat *)h[1]);
	else if (nr == 470) res = fstatat((int)h[0], (char *)h[1], (struct stat *)h[2], (int)h[3]);
	else if (nr == 3) { BLOCKING_BEGIN; res = syscall(3, h[0], h[1], h[2]); int se = errno; BLOCKING_END; errno = se; }  // read kann blockieren (Rohr, Terminal)
	else res = syscall((int)nr, h[0], h[1], h[2], h[3], h[4], h[5]);
	if (res == -1) { err = errno; res = errno; }
	else for (int k = 0; k < 3; k++) if (bufs[k] && s->p[k].dir == 'o') {
		size_t n = sz[k]; if ((nr == 3 || nr == 153 || nr == 344) && k == 0) n = res < 0 ? 0 : (size_t)res;
		else if ((nr == 473 || nr == 58) && res >= 0) n = (size_t)res;
		uc_mem_write(uc, a[s->p[k].idx], bufs[k], n);
	}
	for (int k = 0; k < 3; k++) free(bufs[k]);
done:
	setcarry(err != 0);
	setX(0, res); if (nr == 2 || nr == 66 || nr == 42) setX(1, x1v);
	if (sig_waiting()) uc_emu_stop(uc);  // Signal wartet: Zeitscheibe beenden, Zustellung in run_engine
}

static bool on_fault(uc_engine *u, uc_mem_type t, uint64_t addr, int size, int64_t val, void *ud) {
	(void)u; (void)val; (void)ud;
	if (cth) cth->fault_addr = addr;
	if (!(gsa[11].handler > 1)) fprintf(stderr, "a64run: Zugriff auf nicht abgebildeten Gastspeicher: Art %d, Adresse %#llx, Größe %d\n", (int)t, (unsigned long long)addr, size);
	return false;
}
static bool on_insn_sys(uc_engine *u, uint32_t reg, const uc_arm64_cp_reg *cp, void *ud) {  // mrs/msr auf Systemregister, die Unicorn nicht kennt
	(void)u; (void)ud;
	fprintf(stderr, "a64run: unbekanntes Systemregister op0=%u op1=%u crn=%u crm=%u op2=%u\n", cp->op0, cp->op1, cp->crn, cp->crm, cp->op2);
	(void)reg; return false;
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: a64run <arm64 Mach-O> [args]\n"); return 2; }
	trace = getenv("A64_TRACE") != 0; warn = getenv("A64_WARN") != 0;
	{ uint32_t sz = sizeof self_exe; if (_NSGetExecutablePath(self_exe, &sz)) strcpy(self_exe, argv[0]); }
	if (getenv("A64_ROOT")) aroot = getenv("A64_ROOT");
	else if (access("/Library/ClokeOS/arm64/usr/lib/dyld", F_OK) == 0) aroot = "/Library/ClokeOS/arm64";
	FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 127; }
	fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *file = malloc(fl); if (fread(file, 1, fl, f) != (size_t)fl) return 2;
	if (*(uint32_t *)file == 0xbebafeca) {  // fat: arm64-Scheibe suchen (Kopf big endian), Unterart 0 (nicht arm64e)
		uint32_t na = __builtin_bswap32(*(uint32_t *)(file + 4)), off = 0;
		for (uint32_t i = 0; i < na; i++) if (__builtin_bswap32(*(uint32_t *)(file + 8 + 20 * i)) == CPU_ARM64 && !off) off = __builtin_bswap32(*(uint32_t *)(file + 16 + 20 * i));
		if (off) file += off;
	}
	struct mach_header_64 *mh = (void *)file;
	if (mh->magic != MH_MAGIC_64 || mh->cputype != (int)CPU_ARM64) { fprintf(stderr, "a64run: %s ist kein arm64-Mach-O\n", argv[1]); return 126; }
	uc_err e = UC_ERR_OK; uc = new_engine(); if (!uc) { fprintf(stderr, "uc_open fehlgeschlagen\n"); return 3; }
	if (getenv("A64_RING")) { use_ring = 1; uc_hook ch; uc_hook_add(uc, &ch, UC_HOOK_CODE, on_code, 0, 1, 0); }
	if (getenv("A64_SLICE_MS")) slice_us = atoll(getenv("A64_SLICE_MS")) * 1000;
	{ pthread_t tt; pthread_attr_t ta; pthread_attr_init(&ta); pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED); pthread_create(&tt, &ta, slice_timer, 0); }
	uint64_t entry = 0, mh_addr = 0; int dynamic = 0;
	if (load_image(file, 0, &entry, &mh_addr, &dynamic)) return 3;
	if (dynamic) {  // wie der Kernel: dyld des Gastes laden, es lädt Programm und Bibliotheken selbst
		char dp[1100]; snprintf(dp, sizeof dp, "%s/usr/lib/dyld", aroot);
		FILE *df = fopen(dp, "rb"); if (!df) { fprintf(stderr, "a64run: %s fehlt (A64_ROOT mit arm64-Systembibliotheken)\n", dp); return 125; }
		fseek(df, 0, SEEK_END); long dl = ftell(df); fseek(df, 0, SEEK_SET);
		uint8_t *dfile = malloc(dl); if (fread(dfile, 1, dl, df) != (size_t)dl) return 2;
		uint64_t dmh = 0, dentry = 0; int dd = 0;
		if (load_image(dfile, DYLD_BASE, &dentry, &dmh, &dd)) return 3;
		entry = dentry;
	}
	{  // Commpage von arm64-Darwin (0xFFFFFC000): nur die Felder, die libSystem liest
		uint8_t cp[PAGE]; memset(cp, 0, sizeof cp); uint64_t caps = 0x1fULL; uint16_t ver = 3; uint8_t ncpu = 1; uint64_t mem = 4ULL << 30;
		memcpy(cp, "commpage 64-bit", 16); memcpy(cp + 0x10, &caps, 8); memcpy(cp + 0x1e, &ver, 2); cp[0x22] = ncpu;
		cp[0x34] = cp[0x35] = cp[0x36] = ncpu; memcpy(cp + 0x38, &mem, 8);
		cp[0x24] = cp[0x25] = cp[0x37] = 14;  // Seitengröße 16 KiB: Benutzer-Seitenverschiebung (32/64 Bit) und Kern-Seitenverschiebung; libSystem leitet vm_page_size daraus ab
		// +0x90 = user_timebase: 0 -> libSystem fragt den Kern (Falle -3), +0xC120.. gettimeofday-Daten bleiben 0 (Rückfall auf den Systemaufruf)
		map_range(COMMPAGE, PAGE); uc_mem_write(uc, COMMPAGE, cp, sizeof cp);
	}
	map_range(STACK_TOP - STACK_SIZE, STACK_SIZE);
	// Stapel wie der Kernel: [mach_header*], argc, argv[], 0, envp[], 0, apple[], 0, Strings
	static char *genv[1024]; int ne = 0;
	for (int i = 0; environ[i] && ne < 1000; i++) if (strncmp(environ[i], "DYLD_INSERT_LIBRARIES=", 22)) genv[ne++] = environ[i];
	// libpthread soll Mutexe über __ulock (os_unfair_lock-Stil) statt über psynch machen
	{ int have = 0; for (int i = 0; i < ne; i++) if (!strncmp(genv[i], "PTHREAD_MUTEX_USE_ULOCK=", 24)) have = 1; if (!have) genv[ne++] = "PTHREAD_MUTEX_USE_ULOCK=1"; }
	uint64_t sp = STACK_TOP - 0x1000, ptrs[512]; int np = 0;
	int ng = argc - 1; char **gv = argv + 1;
	uint64_t pstr[600]; int ns = 0;
	for (int i = 0; i < ng + ne + 1; i++) {
		const char *s = i < ng ? gv[i] : i < ng + ne ? genv[i - ng] : gv[0];
		sp -= strlen(s) + 1; uc_mem_write(uc, sp, s, strlen(s) + 1); pstr[ns++] = sp;
		if (ns >= 590) break;
	}
	sp &= ~15ULL;
	if (dynamic) ptrs[np++] = mh_addr;
	ptrs[np++] = ng;
	for (int i = 0; i < ng; i++) ptrs[np++] = pstr[i];
	ptrs[np++] = 0;
	for (int i = 0; i < ne && ng + i < ns - 1; i++) ptrs[np++] = pstr[ng + i];
	ptrs[np++] = 0; ptrs[np++] = pstr[ns - 1]; ptrs[np++] = 0;
	sp -= np * 8; sp &= ~15ULL; uc_mem_write(uc, sp, ptrs, np * 8);
	uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
	t0 = now();
	main_th.u = uc; main_th.tid = 1; main_th.pc = entry; cth = &main_th; eth[63] = &main_th;
	e = run_engine(&main_th);
	uint64_t pc; uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
	fprintf(stderr, "a64run: %s bei pc %#llx\n", uc_strerror(e), (unsigned long long)pc);
	return 4;
}
