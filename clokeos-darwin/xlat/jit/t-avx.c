// t-avx.c – freistehender x86_64-Test für AVX/AVX2 im JIT (RAVEN_JIT_AVX=1): cpuid/xgetbv melden AVX, 256-Bit-Rechnung, Broadcast, Permutation, Maske
__asm__(".globl _start\n_start:\n mov %rsp,%rdi\n and $-16,%rsp\n call _cmain\n mov %eax,%edi\n mov $0x2000001,%eax\n syscall\n");
static long sc(long nr, long a, long b, long c) { long rax = 0x2000000 | nr; __asm__ volatile("syscall" : "+a"(rax) : "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return rax; }
static void out(const char *s) { long n = 0; while (s[n]) n++; sc(4, 1, (long)s, n); }
static int fail;
#define CHECK(c, m) do { if (!(c)) { out("FEHLER: " m "\n"); fail = 1; } } while (0)
int cmain(long *sp) {
	unsigned a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
	CHECK(c >> 28 & 1, "cpuid: AVX"); CHECK(c >> 27 & 1, "cpuid: OSXSAVE"); CHECK(c >> 19 & 1, "cpuid: SSE4.1");
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0)); CHECK(b >> 5 & 1, "cpuid: AVX2");
	unsigned lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0)); CHECK((lo & 6) == 6, "xgetbv: SSE+AVX");
	float x[8] = {1, 2, 3, 4, 5, 6, 7, 8}, y[8] = {10, 20, 30, 40, 50, 60, 70, 80}, z[8], w[8];
	__asm__ volatile("vmovups (%0), %%ymm0\n vmovups (%1), %%ymm1\n vaddps %%ymm1, %%ymm0, %%ymm2\n vmulps %%ymm0, %%ymm2, %%ymm3\n vmovups %%ymm3, (%2)\n"
		"vperm2f128 $1, %%ymm3, %%ymm3, %%ymm4\n vmovups %%ymm4, (%3)\n vzeroupper" :: "r"(x), "r"(y), "r"(z), "r"(w) : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4");
	for (int i = 0; i < 8; i++) CHECK(z[i] == x[i] * (x[i] + y[i]), "vaddps/vmulps ymm");
	for (int i = 0; i < 4; i++) CHECK(w[i] == z[i + 4] && w[i + 4] == z[i], "vperm2f128");
	float bs = 3.5f, bo[8]; __asm__ volatile("vbroadcastss (%0), %%ymm0\n vmovups %%ymm0, (%1)" :: "r"(&bs), "r"(bo) : "memory", "xmm0");
	for (int i = 0; i < 8; i++) CHECK(bo[i] == 3.5f, "vbroadcastss");
	int iv[8] = {1, 2, 3, 4, 5, 6, 7, 8}, ix[8] = {7, 6, 5, 4, 3, 2, 1, 0}, io[8]; int mask;
	__asm__ volatile("vmovdqu (%2), %%ymm1\n vmovdqu (%3), %%ymm0\n vpermd %%ymm0, %%ymm1, %%ymm2\n vmovdqu %%ymm2, (%1)\n vpcmpeqd %%ymm2, %%ymm2, %%ymm3\n vpmovmskb %%ymm3, %0\n vzeroupper"
		: "=r"(mask) : "r"(io), "r"(ix), "r"(iv) : "memory", "xmm0", "xmm1", "xmm2", "xmm3");
	for (int i = 0; i < 8; i++) CHECK(io[i] == 8 - i, "vpermd"); CHECK(mask == -1, "vpmovmskb ymm");
	out(fail ? "t-avx: FEHLER\n" : "t-avx: ok\n");
	return fail;
}
