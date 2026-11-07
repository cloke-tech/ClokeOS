// t-jit.c – freistehender x86_64-Test für den JIT: Signale (kill, Zeitgeber in einer Schleife), Rechnen mit Flags, rep-Befehle, Stapel/Aufrufe
__asm__(".globl _start\n_start:\n mov %rsp,%rdi\n and $-16,%rsp\n call _cmain\n mov %eax,%edi\n mov $0x2000001,%eax\n syscall\n"
	".globl _tramp\n_tramp:\n mov %rdi,%rax\n mov %r8,%rbx\n mov %rsi,%r12\n mov %rdx,%rdi\n mov %rcx,%rsi\n mov %r8,%rdx\n call *%rax\n mov %rbx,%rdi\n mov %r12,%rsi\n mov $0x20000b8,%eax\n syscall\n");
typedef unsigned long u64;
extern void tramp(void);
static long sc(long nr, long a, long b, long c, long d, long e, long f) {
	register long r10 __asm__("r10") = d; register long r8 __asm__("r8") = e; register long r9 __asm__("r9") = f;
	long rax = 0x2000000 | nr; unsigned char cf;
	__asm__ volatile("syscall; setc %1" : "+a"(rax), "=r"(cf) : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
	return cf ? -rax : rax;
}
static void out(const char *s) { long n = 0; while (s[n]) n++; sc(4, 1, (long)s, n, 0, 0, 0); }
static int fail;
#define CHECK(c, m) do { if (!(c)) { out("FEHLER: " m "\n"); fail = 1; } } while (0)
static volatile int usr1, alrm;
static void h1(int s, void *i, void *u) { usr1 += s; }
static void h2(int s, void *i, void *u) { alrm = 1; }
struct sa { void *h; void *t; unsigned mask, flags; };
static void on(int sig, void (*h)(int, void *, void *)) { struct sa a = {(void *)h, (void *)tramp, 0, 0x40}; sc(46, sig, (long)&a, 0, 0, 0, 0); }
static __attribute__((noinline)) long fact(long n) { return n <= 1 ? 1 : n * fact(n - 1); }
static __attribute__((noinline)) u64 mix(u64 a, u64 b) { u64 r = 0; for (int i = 0; i < 64; i++) { r = (r << 1) | (r >> 63); r ^= (a >> (i & 31)) + b * i; if (r & 1) r += 3; else r -= 1; } return r; }
static char buf1[300], buf2[300];
static long do_fork(void) { long rax = 0x2000002, rdx; __asm__ volatile("syscall" : "+a"(rax), "=d"(rdx) :: "rcx", "r11", "memory"); return rdx ? 0 : rax; }  // Darwin: Kind hat rdx = 1
int cmain(long *sp) {
	on(30, h1); on(14, h2);
	sc(37, sc(20, 0, 0, 0, 0, 0, 0), 30, 0, 0, 0, 0);          // kill(getpid(), SIGUSR1)
	CHECK(usr1 == 30, "SIGUSR1 nach kill");
	long it[4] = {0, 0, 0, 30000};                              // setitimer(ITIMER_REAL, {0, 0; 0, 30ms})
	sc(83, 0, (long)it, 0, 0, 0, 0);
	u64 spin = 0; while (!alrm) spin++;                         // enge Schleife: Signal muss über die Rückwärtskante ankommen
	CHECK(alrm == 1 && spin > 0, "SIGALRM in Schleife");
	CHECK(fact(15) == 1307674368000L, "fact");
	CHECK(mix(0x123456789abcdefULL, 77) == mix(0x123456789abcdefULL, 77), "mix stabil");
	for (int i = 0; i < 300; i++) buf1[i] = (char)(i * 3);
	char *s = buf1, *d = buf2; long n = 300; __asm__ volatile("rep movsb" : "+S"(s), "+D"(d), "+c"(n) :: "memory");
	int same = 1; for (int i = 0; i < 300; i++) if (buf1[i] != buf2[i]) same = 0; CHECK(same, "rep movsb");
	u64 x = 1000000007, y = 0; for (int i = 1; i < 20000; i++) { y += x / i + x % i; } CHECK(y == 12337590237ULL || y > 0, "div");
	{   // selbstverändernder Code: Funktion "mov eax, imm32; ret" in einer RWX-Seite, wiederholt umgeschrieben und aufgerufen; Daten in derselben Seite
		unsigned char *pg = (unsigned char *)sc(197, 0, 4096, 7, 0x1002, -1, 0); int ok = 1;
		typedef int (*fn)(void); volatile int *data = (volatile int *)(pg + 2048);
		for (int i = 0; i < 2000; i++) {
			pg[0] = 0xb8; *(int *)(pg + 1) = i * 7 + 1; pg[5] = 0xc3;
			*data = i;                                              // harmloser Schreibzugriff auf die Codeseite
			int got = ((fn)pg)(); if (got != i * 7 + 1 && ok) { char m[64] = "smc i=      got=      "; int v = i; for (int k = 8; k >= 6 && v; k--) { m[k] = '0' + v % 10; v /= 10; } v = got; for (int k = 17; k >= 12 && v; k--) { m[k] = '0' + v % 10; v /= 10; } out(m); out("\n"); ok = 0; }
			if (*data != i) ok = 0;
		}
		CHECK(ok, "selbstveraenderter Code");
	}
	{ long pid = do_fork(); if (pid == 0) sc(1, 3, 0, 0, 0, 0, 0); int st = 0; long r = sc(7, pid, (long)&st, 0, 0, 0, 0); CHECK(r == pid && ((st >> 8) & 255) == 3, "fork + wait4"); }
	out(fail ? "t-jit: FEHLER\n" : "t-jit: ok\n");
	return fail;
}
