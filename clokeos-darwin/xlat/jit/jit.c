// jit.c – Block-JIT x86_64 -> arm64 (ganzzahliger Befehlssatz), Aufbau und Grenzen: docs/translation-study.md, Abschnitt 16.
//
// Überblick:
//  * Gastspeicher: Host-Adresse = Gast-Adresse + delta (ein fester Versatz, Register x20). Der Bereich [delta, delta+2^38) wird einmal
//    reserviert (PROT_NONE); jit_guest_alloc schaltet Seiten frei. Ein Zugriff kostet damit keinen Befehl mehr als [Reg, x20].
//  * Gastregister: rax..r15 liegen in x0..x15 (gleiche Nummer), nur beim Verlassen des JIT-Codes in JitCtx.
//  * Flags: lazy im Host-NZCV. Zustand beim Übersetzen: FS_CTX (in JitCtx), FS_ADD (C = CF), FS_SUB (C = !CF), FS_LOGIC (wie ADD, C=V=0).
//    Beim Verlassen eines Blocks wird NZCV (auf C = CF normiert) in JitCtx gesichert; ist das Ziel schon übersetzt und liest keine
//    Flags, wird das Sichern per Patch zu NOPs. PF/AF gibt es nicht (siehe Studie).
//  * Nicht unterstützte Befehle (SSE, x87, ...): Block endet, Ereignis JIT_FALLBACK; der Aufrufer lässt Unicorn die Befehle ausführen.
#include "jit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <sys/ucontext.h>
#include <setjmp.h>

// Der arm64-Kernel von PureDarwin im VM trägt nur 39 Bit Benutzeradresse (Zugriff ab 2^39 hängt, mmap geht): Fenster [2^38, 2^39)
static uint64_t DELTA, RESERVE;  // Fenster für den Gastspeicher, in reserve_guest gewählt
#define CODE_SIZE (48u << 20)
#define MAXI 48
#define T0 22
#define T1 23
#define T2 24
#define T3 25
#define XZR 31

static uint32_t *cp, *code_base, *code_end, *code_blocks;
static JitCtx *gctx;
static uint32_t *g_enter, *g_E1, *g_E2, *g_M, *g_Mi, *g_MP;
static uint64_t st_blocks, st_insns, st_fb;
static int trace_jit;

// ------------------------------------------------------------------------------------------------ Emitter
#define E(x) (*cp++ = (uint32_t)(x))
#define SF(is64) ((is64) ? 0x80000000u : 0)
enum { ADD_ = 0x0B000000, ADDS_ = 0x2B000000, SUB_ = 0x4B000000, SUBS_ = 0x6B000000,
       AND_ = 0x0A000000, ORR_ = 0x2A000000, EOR_ = 0x4A000000, ANDS_ = 0x6A000000 };
static void alu_r(uint32_t op, int rd, int rn, int rm, int is64) { E(op | SF(is64) | rm << 16 | rn << 5 | rd); }
static void alu_rs(uint32_t op, int rd, int rn, int rm, int is64, int shift, int amt) { E(op | SF(is64) | shift << 22 | rm << 16 | amt << 10 | rn << 5 | rd); }
static void addsub_i(int sub, int setf, int rd, int rn, uint32_t imm, int is64) { E(0x11000000 | SF(is64) | (sub ? 0x40000000 : 0) | (setf ? 0x20000000 : 0) | imm << 10 | rn << 5 | rd); }
static void mov_r(int rd, int rm, int is64) { E((is64 ? 0xAA0003E0u : 0x2A0003E0u) | rm << 16 | rd); }
static void movz_(int rd, uint32_t imm, int hw) { E(0xD2800000 | hw << 21 | imm << 5 | rd); }
static void movimm(int rd, uint64_t v) {
	int nz = 0, nf = 0; for (int i = 0; i < 4; i++) { uint32_t c = (v >> 16 * i) & 0xffff; nz += c != 0; nf += c != 0xffff; }
	if (nf < nz) {
		int first = 1; for (int i = 0; i < 4; i++) { uint32_t c = (v >> 16 * i) & 0xffff; if (c == 0xffff) continue;
			if (first) { E(0x92800000 | i << 21 | ((~c) & 0xffff) << 5 | rd); first = 0; } else E(0xF2800000 | i << 21 | c << 5 | rd); }
		if (first) E(0x92800000 | rd);
	} else {
		int first = 1; for (int i = 0; i < 4; i++) { uint32_t c = (v >> 16 * i) & 0xffff; if (!c) continue;
			if (first) { movz_(rd, c, i); first = 0; } else E(0xF2800000 | i << 21 | c << 5 | rd); }
		if (first) movz_(rd, 0, 0);
	}
}
// Bitfeld: ubfm/sbfm/bfm
static void bfm(uint32_t base32, int rd, int rn, int immr, int imms, int is64) { E(base32 | (is64 ? 0x80400000u : 0) | immr << 16 | imms << 10 | rn << 5 | rd); }
#define UBFM 0x53000000
#define SBFM 0x13000000
#define BFM 0x33000000
static void ubfx(int rd, int rn, int lsb, int w, int is64) { bfm(UBFM, rd, rn, lsb, lsb + w - 1, is64); }
static void sbfx(int rd, int rn, int lsb, int w, int is64) { bfm(SBFM, rd, rn, lsb, lsb + w - 1, is64); }
static void bfi(int rd, int rn, int lsb, int w, int is64) { bfm(BFM, rd, rn, (-lsb) & (is64 ? 63 : 31), w - 1, is64); }
static void lsl_i(int rd, int rn, int k, int is64) { int n = is64 ? 64 : 32; bfm(UBFM, rd, rn, (n - k) % n, n - 1 - k, is64); }
static void lsr_i(int rd, int rn, int k, int is64) { bfm(UBFM, rd, rn, k, is64 ? 63 : 31, is64); }
static void asr_i(int rd, int rn, int k, int is64) { bfm(SBFM, rd, rn, k, is64 ? 63 : 31, is64); }
static void shv(int kind, int rd, int rn, int rm, int is64) { E(0x1AC02000 | kind << 10 | SF(is64) | rm << 16 | rn << 5 | rd); }  // 0 lsl 1 lsr 2 asr
static void csel_(int rd, int rn, int rm, int cond, int is64) { E(0x1A800000 | SF(is64) | rm << 16 | cond << 12 | rn << 5 | rd); }
static void cset_(int rd, int cond, int is64) { E(0x1A9F07E0 | SF(is64) | (cond ^ 1) << 12 | rd); }
static void csetm_(int rd, int cond, int is64) { E(0x5A9F03E0 | SF(is64) | (cond ^ 1) << 12 | rd); }
static void madd_(int rd, int rn, int rm, int ra, int is64) { E(0x1B000000 | SF(is64) | rm << 16 | ra << 10 | rn << 5 | rd); }
static void msub_(int rd, int rn, int rm, int ra, int is64) { E(0x1B008000 | SF(is64) | rm << 16 | ra << 10 | rn << 5 | rd); }
static void div_(int sgn, int rd, int rn, int rm, int is64) { E((sgn ? 0x1AC00C00 : 0x1AC00800) | SF(is64) | rm << 16 | rn << 5 | rd); }
static void mrs_nzcv(int rt) { E(0xD53B4200 | rt); }
static void msr_nzcv(int rt) { E(0xD51B4200 | rt); }
#define NOP 0xD503201Fu
static void br_(int rn) { E(0xD61F0000 | rn << 5); }
static void adr_(int rd, void *target) { int64_t off = (uint8_t *)target - (uint8_t *)cp; E(0x10000000 | (off & 3) << 29 | ((off >> 2) & 0x7ffff) << 5 | rd); }
static void b_to(void *t) { int64_t off = ((uint8_t *)t - (uint8_t *)cp) / 4; E(0x14000000 | (off & 0x3ffffff)); }
static void ldr_ctx(int rt, int off) { E(0xF9400000 | (off / 8) << 10 | 19 << 5 | rt); }
static void str_ctx(int rt, int off) { E(0xF9000000 | (off / 8) << 10 | 19 << 5 | rt); }
#define C_RIP 128
#define C_NZCV 136
#define C_EFL 152
#define C_FS 160
#define C_GS 168
#define C_NEXT 176
#define C_SITE 184
#define C_STATUS 192
#define C_HSP 208
#define C_PEND 216
#define C_COUNT 224
#define C_DELTA 232
#define C_JC 240
#define C_FBEND 248
#define STP_SAVE (0xA9000000u | (C_NZCV / 8) << 15 | 31 << 10 | 19 << 5 | 16)

enum { EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE };

// Bitmasken-Immediate (Logikbefehle): liefert N<<12 | immr<<6 | imms oder -1
static int enc_bitmask(uint64_t v, int is64) {
	if (!is64) { v &= 0xffffffffULL; v |= v << 32; }
	if (v == 0 || v == ~0ULL) return -1;
	int e;
	for (e = 2; e <= 64; e <<= 1) {
		uint64_t m = e == 64 ? ~0ULL : ((1ULL << e) - 1), el = v & m; int ok = 1;
		for (int i = e; i < 64; i += e) if (((v >> i) & m) != el) { ok = 0; break; }
		if (ok) break;
	}
	uint64_t m = e == 64 ? ~0ULL : ((1ULL << e) - 1), el = v & m; int ones = __builtin_popcountll(el);
	uint64_t run = ones == 64 ? ~0ULL : ((1ULL << ones) - 1); int r;
	for (r = 0; r < e; r++) { uint64_t rot = r ? (((el >> r) | (el << (e - r))) & m) : el; if (rot == run) break; }
	if (r == e) return -1;
	int immr = (e - r) % e, imms = ((-(e << 1)) & 0x3f) | (ones - 1), N = e == 64;
	return N << 12 | immr << 6 | imms;
}
static int logic_imm(uint32_t op /*AND_ ...*/, int rd, int rn, uint64_t v, int is64) {
	int b = enc_bitmask(v, is64); if (b < 0) return 0;
	uint32_t base = op == AND_ ? 0x12000000 : op == ORR_ ? 0x32000000 : op == EOR_ ? 0x52000000 : 0x72000000;
	E(base | SF(is64) | (b >> 12) << 22 | ((b >> 6) & 63) << 16 | (b & 63) << 10 | rn << 5 | rd); return 1;
}

// Laden/Speichern: Adresse = {rn, rm (-1: imm)}.
typedef struct { int rn, rm; int imm; } Addr;
static void ldst(int load, int size, int sx /*0 / 1 nach x / 2 nach w*/, int rt, Addr a) {
	int si = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3; uint32_t szb = (uint32_t)si << 30;
	uint32_t base;
	if (a.rm >= 0) {
		base = 0x38206800 | szb;                      // STR
		if (load) { base = 0x38606800 | szb; if (sx == 1) base = 0x38A06800 | szb; else if (sx == 2) base = 0x38E06800 | szb; }
		E(base | a.rm << 16 | a.rn << 5 | rt);
	} else {
		base = 0x38000000 | szb;
		if (load) { base = 0x38400000 | szb; if (sx == 1) base = 0x38800000 | szb; else if (sx == 2) base = 0x38C00000 | szb; }
		E(base | (a.imm & 0x1ff) << 12 | a.rn << 5 | rt);
	}
}

// ------------------------------------------------------------------------------------------------ Gastspeicher
static uint8_t *guest_reserved;
static int reserve_guest(void) {
	if (guest_reserved) return 0;
	// Das Fenster muss ganz unter 2^39 liegen: der arm64-Kernel von PureDarwin im VM trägt nur 39 Bit Benutzeradresse (mmap darüber geht,
	// der erste Zugriff hängt). Zuerst mit Adresshinweis ans obere Ende, sonst nimmt der Kernel den Platz, den er will; Größe absteigend.
	const uint64_t top = 1ULL << 39;
	for (int sh = 37; sh >= 33; sh--) {
		uint64_t n = 1ULL << sh, hints[] = {top - n - (1ULL << 33), top - n, 0};
		for (int h = 0; h < 3; h++) {
			void *p = mmap((void *)hints[h], n, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
			if (p == MAP_FAILED) continue;
			if ((uint64_t)p + n > top) { munmap(p, n); continue; }
			guest_reserved = p; DELTA = (uint64_t)p; RESERVE = n;
			if (getenv("UCRUN_JIT_TRACE")) fprintf(stderr, "jit: Gastfenster %p, %llu GiB\n", p, (unsigned long long)(n >> 30));
			return 0;
		}
	}
	return -1;
}
uint64_t jit_guest_limit(void) { return reserve_guest() ? 0 : RESERVE; }
uint8_t *jit_guest_alloc(uint64_t g, uint64_t n) {
	if (reserve_guest() || g + n > RESERVE) return 0;
	uint64_t hs = (g + DELTA) & ~16383ULL, he = (g + DELTA + n + 16383) & ~16383ULL;
	if (mprotect((void *)hs, he - hs, PROT_READ | PROT_WRITE)) return 0;
	return (uint8_t *)(g + DELTA);
}
void jit_guest_free(uint64_t g, uint64_t n) {
	if (!guest_reserved || !n) return;
	uint64_t hs = g + DELTA, he = hs + n, fs_ = (hs + 16383) & ~16383ULL, fe = he & ~16383ULL;
	if (fs_ < fe) {
		if (hs < fs_) memset((void *)hs, 0, fs_ - hs);
		if (he > fe) memset((void *)fe, 0, he - fe);
		mmap((void *)fs_, fe - fs_, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
	} else if ((hs & ~16383ULL) == ((he - 1) & ~16383ULL) || 1) {
		memset((void *)hs, 0, n);  // Teil einer Host-Seite: Inhalt nullen, Seite bleibt offen
	}
}

// ------------------------------------------------------------------------------------------------ Decoder
enum { OP_FB, OP_NOP, OP_MOV, OP_LEA, OP_ALU, OP_INC, OP_DEC, OP_NEG, OP_NOT, OP_MUL1, OP_IMUL1, OP_DIV, OP_IDIV, OP_IMUL2,
       OP_SHIFT, OP_PUSH, OP_POP, OP_CALL, OP_CALLI, OP_RET, OP_JMP, OP_JMPI, OP_JCC, OP_SETCC, OP_CMOV, OP_MOVZX, OP_MOVSX,
       OP_MOVSXD, OP_CDQ, OP_CWDE, OP_XCHG, OP_STRING, OP_SYSCALL, OP_LEAVE, OP_BSWAP };
typedef struct { uint8_t k, n, hi; } Opd;  // k: 0 -, 1 Register, 2 Speicher, 3 Immediate
typedef struct {
	int op, sz, ssz, cc, alu, len, rep, count, live, cl, imm3, strop, branchy, followed;
	uint64_t pc, target; int64_t imm; Opd d, s;
	int base, index, scale, seg, ripr; int64_t disp;
} Insn;

static Opd mkreg(int n, int sz, int rex) { Opd o = {1, (uint8_t)n, 0}; if (sz == 1 && !rex && n >= 4 && n <= 7) { o.n = n - 4; o.hi = 1; } return o; }
static int64_t rd32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

// Länge eines Befehls, den der JIT nicht übersetzt (0 = unbekannt); *branchy: kann den Ablauf ändern
static int flen(const uint8_t *p, int *branchy) {
	int i = 0, opsz16 = 0, rex = 0; *branchy = 0;
	for (;; i++) { uint8_t b = p[i]; if (b == 0x66) opsz16 = 1; else if (b == 0xf2 || b == 0xf3 || b == 0xf0 || b == 0x67 || b == 0x2e || b == 0x3e || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) ; else break; }
	if ((p[i] & 0xf0) == 0x40) rex = p[i++];
	uint8_t op = p[i++]; int modrm = 0, imm = 0;
	if (op == 0x0f) {
		uint8_t o2 = p[i++];
		if (o2 == 0x38) { i++; modrm = 1; }
		else if (o2 == 0x3a) { i++; modrm = 1; imm = 1; }
		else if (o2 == 0x05 || o2 == 0x0b || o2 == 0x31 || o2 == 0xa2 || o2 == 0x77 || o2 == 0x01 && 0 || (o2 >= 0xc8 && o2 <= 0xcf) || o2 == 0xa0 || o2 == 0xa1 || o2 == 0xa8 || o2 == 0xa9 || o2 == 0x06 || o2 == 0x08 || o2 == 0x09 || o2 == 0x30 || o2 == 0x32 || o2 == 0x33 || o2 == 0x34 || o2 == 0x35) { if (o2 == 0x05 || o2 == 0x0b) *branchy = 1; }
		else if (o2 >= 0x80 && o2 <= 0x8f) { *branchy = 1; return i + 4; }
		else { modrm = 1; if (o2 == 0x70 || o2 == 0x71 || o2 == 0x72 || o2 == 0x73 || o2 == 0xa4 || o2 == 0xac || o2 == 0xba || o2 == 0xc2 || o2 == 0xc4 || o2 == 0xc5 || o2 == 0xc6) imm = 1; }
	} else if (op == 0xc4 || op == 0xc5 || op == 0x62 || op == 0xc8 || op == 0x8f && 0) return 0;  // VEX/EVEX: nicht bestimmt
	else if (op < 0x40 && (op & 7) < 4) modrm = 1;
	else if (op < 0x40 && (op & 7) == 4) imm = 1;
	else if (op < 0x40 && (op & 7) == 5) imm = opsz16 ? 2 : 4;
	else if (op >= 0x50 && op <= 0x5f) ;
	else if (op == 0x63 || op == 0x62) modrm = 1;
	else if (op == 0x68) imm = 4; else if (op == 0x6a) imm = 1;
	else if (op == 0x69) { modrm = 1; imm = opsz16 ? 2 : 4; } else if (op == 0x6b) { modrm = 1; imm = 1; }
	else if (op >= 0x70 && op <= 0x7f) { *branchy = 1; return i + 1; }
	else if (op == 0x80 || op == 0x82 || op == 0x83) { modrm = 1; imm = 1; } else if (op == 0x81) { modrm = 1; imm = opsz16 ? 2 : 4; }
	else if (op >= 0x84 && op <= 0x8f) modrm = 1;
	else if (op >= 0x90 && op <= 0x99) ; else if (op >= 0x9b && op <= 0x9f) ;
	else if (op >= 0xa0 && op <= 0xa3) return i + 8;
	else if (op >= 0xa4 && op <= 0xa7) ; else if (op == 0xa8) imm = 1; else if (op == 0xa9) imm = opsz16 ? 2 : 4; else if (op >= 0xaa && op <= 0xaf) ;
	else if (op >= 0xb0 && op <= 0xb7) imm = 1; else if (op >= 0xb8 && op <= 0xbf) imm = (rex & 8) ? 8 : opsz16 ? 2 : 4;
	else if (op == 0xc0 || op == 0xc1) { modrm = 1; imm = 1; } else if (op == 0xc2) { *branchy = 1; return i + 2; } else if (op == 0xc3) { *branchy = 1; return i; }
	else if (op == 0xc6) { modrm = 1; imm = 1; } else if (op == 0xc7) { modrm = 1; imm = opsz16 ? 2 : 4; }
	else if (op == 0xc9 || op == 0xcc) { if (op == 0xcc) *branchy = 1; }
	else if (op == 0xcd) { *branchy = 1; imm = 1; }
	else if (op >= 0xd0 && op <= 0xd3) modrm = 1; else if (op >= 0xd8 && op <= 0xdf) modrm = 1;
	else if (op >= 0xe0 && op <= 0xe3) { *branchy = 1; return i + 1; } else if (op == 0xe8 || op == 0xe9) { *branchy = 1; return i + 4; } else if (op == 0xeb) { *branchy = 1; return i + 1; }
	else if (op == 0xf6 || op == 0xf7) { modrm = 1; if (((p[i] >> 3) & 7) < 2) imm = op == 0xf6 ? 1 : opsz16 ? 2 : 4; }
	else if (op == 0xfe || op == 0xff) { modrm = 1; if (op == 0xff && (((p[i] >> 3) & 7) >= 2 && ((p[i] >> 3) & 7) <= 5)) *branchy = 1; }
	else if (op == 0xf4 || op == 0xf5 || (op >= 0xf8 && op <= 0xfd)) { if (op == 0xf4) *branchy = 1; }
	else return 0;
	if (modrm) {
		uint8_t m = p[i++]; int mod = m >> 6, rm = m & 7;
		if (mod != 3) { if (rm == 4) { uint8_t sib = p[i++]; if ((sib & 7) == 5 && mod == 0) i += 4; } else if (rm == 5 && mod == 0) i += 4; if (mod == 1) i += 1; else if (mod == 2) i += 4; }
	}
	return i + imm;
}

// Befehl unter p (Host-Zeiger) mit Gast-Adresse pc; unbekannte Befehle -> OP_FB
static void decode(const uint8_t *p, uint64_t pc, Insn *I) {
	memset(I, 0, sizeof *I); I->pc = pc; I->op = OP_FB; I->base = I->index = -1;
	int i = 0, opsz16 = 0, rep = 0, seg = 0, lock = 0, addr32 = 0, rex = 0;
	for (;; i++) { uint8_t b = p[i]; if (b == 0x66) opsz16 = 1; else if (b == 0xf3) rep = 1; else if (b == 0xf2) rep = 2; else if (b == 0x64) seg = 1; else if (b == 0x65) seg = 2;
		else if (b == 0xf0) lock = 1; else if (b == 0x67) addr32 = 1; else if (b == 0x2e || b == 0x3e || b == 0x26 || b == 0x36) ; else break; }
	if ((p[i] & 0xf0) == 0x40) rex = p[i++];
	int W = (rex >> 3) & 1, R = ((rex >> 2) & 1) << 3, X = ((rex >> 1) & 1) << 3, B = (rex & 1) << 3;
	int osz = W ? 8 : opsz16 ? 2 : 4;
	int fl = flen(p, &I->branchy);
	#define FAIL do { I->op = OP_FB; I->len = fl; return; } while (0)
	if (lock || addr32) FAIL;
	uint8_t op = p[i++];
	int regf = 0; Opd rmo = {0};
	// ModRM lesen: setzt regf, rmo (Register oder Speicher) für Operandengröße msz
	#define MODRM(msz) do { uint8_t m = p[i++]; int mod = m >> 6, rm = m & 7; regf = ((m >> 3) & 7) | R; \
		if (mod == 3) rmo = mkreg(rm | B, msz, rex); \
		else { rmo.k = 2; I->seg = seg; I->base = rm | B; I->index = -1; I->scale = 0; I->disp = 0; I->ripr = 0; \
			if (rm == 4) { uint8_t sib = p[i++]; I->scale = sib >> 6; int ix = ((sib >> 3) & 7) | X; I->index = ix == 4 ? -1 : ix; \
				if ((sib & 7) == 5 && mod == 0) { I->base = -1; I->disp = rd32(p + i); i += 4; } else I->base = (sib & 7) | B; } \
			else if (rm == 5 && mod == 0) { I->ripr = 1; I->base = -1; I->disp = rd32(p + i); i += 4; } \
			if (mod == 1) { I->disp = (int8_t)p[i++]; } else if (mod == 2) { I->disp = rd32(p + i); i += 4; } } } while (0)
	#define IMM8 ((int64_t)(int8_t)p[i++])
	#define IMMZ(sz_) ((sz_) == 2 ? ({ int16_t v_; memcpy(&v_, p + i, 2); i += 2; (int64_t)v_; }) : ({ int64_t v_ = rd32(p + i); i += 4; v_; }))
	int sz = osz;
	if (op == 0x0f) {
		uint8_t o2 = p[i++];
		if (o2 == 0x05) { I->op = OP_SYSCALL; I->len = i; return; }
		if (o2 == 0x1f) { MODRM(osz); I->op = OP_NOP; I->len = i; return; }
		if (o2 == 0x1e && rep == 1 && p[i] == 0xfa) { I->op = OP_NOP; I->len = i + 1; return; }  // endbr64
		if (o2 >= 0x80 && o2 <= 0x8f) { int cc = o2 & 15; if (cc == 10 || cc == 11) FAIL; int64_t d = rd32(p + i); i += 4; I->op = OP_JCC; I->cc = cc; I->len = i; I->target = pc + i + d; return; }
		if (o2 >= 0x90 && o2 <= 0x9f) { int cc = o2 & 15; if (cc == 10 || cc == 11) FAIL; MODRM(1); I->op = OP_SETCC; I->cc = cc; I->sz = 1; I->d = rmo; I->len = i; return; }
		if (o2 >= 0x40 && o2 <= 0x4f) { int cc = o2 & 15; if (cc == 10 || cc == 11 || osz == 2) FAIL; MODRM(osz); I->op = OP_CMOV; I->cc = cc; I->sz = osz; I->d = mkreg(regf, osz, rex); I->s = rmo; I->len = i; return; }
		if (o2 == 0xaf) { if (osz == 2) FAIL; MODRM(osz); I->op = OP_IMUL2; I->sz = osz; I->d = mkreg(regf, osz, rex); I->s = rmo; I->len = i; return; }
		if (o2 == 0xb6 || o2 == 0xb7 || o2 == 0xbe || o2 == 0xbf) { int ss = (o2 & 1) ? 2 : 1; MODRM(ss); I->op = (o2 < 0xb8) ? OP_MOVZX : OP_MOVSX; I->sz = osz; I->ssz = ss; I->d = mkreg(regf, osz, rex); I->s = rmo; I->len = i; return; }
		if (o2 >= 0xc8 && o2 <= 0xcf) { if (osz == 2) FAIL; I->op = OP_BSWAP; I->sz = osz; I->d = mkreg((o2 & 7) | B, osz, rex); I->len = i; return; }
		FAIL;
	}
	if (op < 0x40 && (op & 7) <= 5) {  // add or adc sbb and sub xor cmp
		int alu = op >> 3, f = op & 7; if (op == 0x0f) FAIL;
		I->op = OP_ALU; I->alu = alu;
		if (f < 4) { int s8 = (f & 1) ? osz : 1; MODRM(s8); I->sz = s8; Opd r = mkreg(regf, s8, rex); if (f < 2) { I->d = rmo; I->s = r; } else { I->d = r; I->s = rmo; } }
		else if (f == 4) { I->sz = 1; I->d = mkreg(0, 1, rex); I->s.k = 3; I->imm = IMM8; }
		else { I->sz = osz; I->d = mkreg(0, osz, rex); I->s.k = 3; I->imm = IMMZ(osz); }
		if ((alu == 2 || alu == 3) && I->sz < 4) FAIL;
		I->len = i; return;
	}
	if (op >= 0x50 && op <= 0x57) { if (osz == 2) FAIL; I->op = OP_PUSH; I->d = mkreg((op & 7) | B, 8, 1); I->len = i; return; }
	if (op >= 0x58 && op <= 0x5f) { if (osz == 2) FAIL; I->op = OP_POP; I->d = mkreg((op & 7) | B, 8, 1); I->len = i; return; }
	switch (op) {
	case 0x63: if (!W) FAIL; MODRM(4); I->op = OP_MOVSXD; I->sz = 8; I->d = mkreg(regf, 8, rex); I->s = rmo; I->len = i; return;
	case 0x68: I->op = OP_PUSH; I->d.k = 3; I->imm = IMMZ(4); I->len = i; return;
	case 0x6a: I->op = OP_PUSH; I->d.k = 3; I->imm = IMM8; I->len = i; return;
	case 0x69: case 0x6b: if (osz == 2) FAIL; MODRM(osz); I->op = OP_IMUL2; I->sz = osz; I->d = mkreg(regf, osz, rex); I->s = rmo; I->imm3 = 1; I->imm = op == 0x6b ? IMM8 : IMMZ(osz); I->len = i; return;
	case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
		{ int64_t d = (int8_t)p[i++]; I->op = OP_JCC; I->cc = op & 15; I->len = i; I->target = pc + i + d; return; }
	case 0x80: case 0x81: case 0x83: {
		int s8 = op == 0x80 ? 1 : osz; MODRM(s8); I->op = OP_ALU; I->alu = regf & 7; I->sz = s8; I->d = rmo; I->s.k = 3;
		I->imm = op == 0x81 ? IMMZ(osz) : IMM8; if ((I->alu == 2 || I->alu == 3) && s8 < 4) FAIL; I->len = i; return; }
	case 0x84: case 0x85: { int s8 = op == 0x84 ? 1 : osz; MODRM(s8); I->op = OP_ALU; I->alu = 8; I->sz = s8; I->d = rmo; I->s = mkreg(regf, s8, rex); I->len = i; return; }
	case 0x86: case 0x87: { int s8 = op == 0x86 ? 1 : osz; if (s8 < 4) FAIL; MODRM(s8); I->op = OP_XCHG; I->sz = s8; I->d = rmo; I->s = mkreg(regf, s8, rex); I->len = i; return; }
	case 0x88: case 0x89: { int s8 = op == 0x88 ? 1 : osz; MODRM(s8); I->op = OP_MOV; I->sz = s8; I->d = rmo; I->s = mkreg(regf, s8, rex); I->len = i; return; }
	case 0x8a: case 0x8b: { int s8 = op == 0x8a ? 1 : osz; MODRM(s8); I->op = OP_MOV; I->sz = s8; I->d = mkreg(regf, s8, rex); I->s = rmo; I->len = i; return; }
	case 0x8d: MODRM(osz); if (rmo.k != 2 || osz == 2) FAIL; I->seg = 0; I->op = OP_LEA; I->sz = osz; I->d = mkreg(regf, osz, rex); I->len = i; return;
	case 0x8f: { MODRM(8); if ((regf & 7) != 0 || rmo.k != 1 || osz == 2) FAIL; I->op = OP_POP; I->d = rmo; I->len = i; return; }
	case 0x90: if (B) { I->op = OP_XCHG; I->sz = osz < 4 ? 4 : osz; if (osz == 2) FAIL; I->d = mkreg(8, I->sz, rex); I->s = mkreg(0, I->sz, rex); } else I->op = OP_NOP; I->len = i; return;
	case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: if (osz == 2) FAIL; I->op = OP_XCHG; I->sz = osz; I->d = mkreg((op & 7) | B, osz, rex); I->s = mkreg(0, osz, rex); I->len = i; return;
	case 0x98: if (osz == 2) FAIL; I->op = OP_CWDE; I->sz = osz; I->len = i; return;
	case 0x99: if (osz == 2) FAIL; I->op = OP_CDQ; I->sz = osz; I->len = i; return;
	case 0xa4: case 0xa5: case 0xaa: case 0xab:
		if (rep == 2) FAIL; I->op = OP_STRING; I->strop = (op >= 0xaa) ? 1 : 0; I->sz = (op & 1) ? osz : 1; I->rep = rep == 1; I->len = i; return;
	case 0xa8: I->op = OP_ALU; I->alu = 8; I->sz = 1; I->d = mkreg(0, 1, rex); I->s.k = 3; I->imm = IMM8; I->len = i; return;
	case 0xa9: I->op = OP_ALU; I->alu = 8; I->sz = osz; I->d = mkreg(0, osz, rex); I->s.k = 3; I->imm = IMMZ(osz); I->len = i; return;
	case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
		I->op = OP_MOV; I->sz = 1; I->d = mkreg((op & 7) | B, 1, rex); I->s.k = 3; I->imm = p[i++]; I->len = i; return;
	case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
		I->op = OP_MOV; I->sz = osz; I->d = mkreg((op & 7) | B, osz, rex); I->s.k = 3;
		if (W) { memcpy(&I->imm, p + i, 8); i += 8; } else if (osz == 2) { uint16_t v; memcpy(&v, p + i, 2); i += 2; I->imm = v; } else { uint32_t v; memcpy(&v, p + i, 4); i += 4; I->imm = v; }
		I->len = i; return;
	case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
		int s8 = (op & 1) ? osz : 1; MODRM(s8); int k = regf & 7;
		if (k == 0 || k == 1 || k == 2 || k == 3) FAIL;  // rol/ror/rcl/rcr: Unicorn
		I->op = OP_SHIFT; I->alu = k == 4 || k == 6 ? 4 : k == 5 ? 5 : 7; I->sz = s8; I->d = rmo;
		if (op == 0xc0 || op == 0xc1) { I->s.k = 3; I->imm = p[i++]; } else if (op == 0xd0 || op == 0xd1) { I->s.k = 3; I->imm = 1; } else I->cl = 1;
		I->imm &= (s8 == 8 ? 63 : 31);
		if (!I->cl && I->imm == 0) FAIL;
		I->len = i; return; }
	case 0xc2: case 0xc3: I->op = OP_RET; I->len = i; if (op == 0xc2) { uint16_t v; memcpy(&v, p + i, 2); I->imm = v; I->len += 2; } return;
	case 0xc6: case 0xc7: { int s8 = op == 0xc6 ? 1 : osz; MODRM(s8); if ((regf & 7) != 0) FAIL; I->op = OP_MOV; I->sz = s8; I->d = rmo; I->s.k = 3; I->imm = s8 == 1 ? IMM8 : IMMZ(osz); I->len = i; return; }
	case 0xc9: I->op = OP_LEAVE; I->len = i; return;
	case 0xe8: { int64_t d = rd32(p + i); i += 4; I->op = OP_CALL; I->len = i; I->target = pc + i + d; return; }
	case 0xe9: { int64_t d = rd32(p + i); i += 4; I->op = OP_JMP; I->len = i; I->target = pc + i + d; return; }
	case 0xeb: { int64_t d = (int8_t)p[i++]; I->op = OP_JMP; I->len = i; I->target = pc + i + d; return; }
	case 0xf6: case 0xf7: {
		int s8 = op == 0xf6 ? 1 : osz; MODRM(s8); int k = regf & 7; I->sz = s8; I->d = rmo;
		if (k == 0 || k == 1) { I->op = OP_ALU; I->alu = 8; I->s.k = 3; I->imm = s8 == 1 ? IMM8 : IMMZ(osz); I->len = i; return; }
		if (k == 2) { I->op = OP_NOT; I->len = i; return; }
		if (k == 3) { I->op = OP_NEG; I->len = i; return; }
		if (s8 < 4) FAIL;
		I->op = k == 4 ? OP_MUL1 : k == 5 ? OP_IMUL1 : k == 6 ? OP_DIV : OP_IDIV; I->len = i; return; }
	case 0xfe: case 0xff: {
		int s8 = op == 0xfe ? 1 : osz; MODRM(s8); int k = regf & 7;
		if (k == 0 || k == 1) { I->op = k ? OP_DEC : OP_INC; I->sz = s8; I->d = rmo; I->len = i; return; }
		if (op == 0xff && k == 2) { I->op = OP_CALLI; I->d = rmo; I->sz = 8; I->len = i; return; }
		if (op == 0xff && k == 4) { I->op = OP_JMPI; I->d = rmo; I->sz = 8; I->len = i; return; }
		if (op == 0xff && k == 6) { I->op = OP_PUSH; I->d = rmo; I->sz = 8; I->len = i; if (osz == 2) FAIL; return; }
		FAIL; }
	}
	FAIL;
}

// ------------------------------------------------------------------------------------------------ Übersetzer
enum { FS_CTX, FS_ADD, FS_SUB, FS_LOGIC };
static int fs, trunc_;
typedef struct { uint32_t *pos; int kind; } Fix;  // kind 0: imm19 (cbz/b.cond), 1: imm14 (tbz), 2: b (imm26)
typedef struct { uint64_t pc, end; int fs, count; Fix f[8]; int nf; } Slow;
static Slow slow[MAXI + 4]; static int nslow;
typedef struct { uint32_t *site, *pend; uint64_t target; int fs; } Exit;
static Exit exs[16]; static int nexs;
static int cur_live_in;

static void ensure_nzcv(void) { if (fs == FS_CTX) { ldr_ctx(16, C_NZCV); msr_nzcv(16); fs = FS_ADD; } }
static void flipc(void) { mrs_nzcv(16); logic_imm(EOR_, 16, 16, 0x20000000, 1); msr_nzcv(16); }
static void to_sub(void) { ensure_nzcv(); if (fs != FS_SUB) { flipc(); fs = FS_SUB; } }
static void to_add(void) { ensure_nzcv(); if (fs == FS_SUB) { flipc(); fs = FS_ADD; } }
static const int cc_tab[16] = {VS, VC, -1, -1, EQ, NE, -1, -1, MI, PL, -1, -1, LT, GE, LE, GT};
static int cond_arm(int cc) {
	ensure_nzcv();
	if (cc == 6 || cc == 7) { if (fs == FS_LOGIC) return cc == 6 ? EQ : NE; if (fs != FS_SUB) to_sub(); return cc == 6 ? LS : HI; }
	if (cc == 2 || cc == 3) { if (fs == FS_SUB) return cc == 2 ? CC : CS; return cc == 2 ? CS : CC; }
	return cc_tab[cc];
}
// Flags sichern (normiert auf C = CF); fest 3 Befehle, wenn patchbar
static void save_flags(int fsx, int patchable) {
	if (fsx == FS_CTX) return;
	mrs_nzcv(16);
	if (fsx == FS_SUB) logic_imm(EOR_, 16, 16, 0x20000000, 1); else if (patchable) E(NOP);
	E(STP_SAVE);
}
static void exit_status(int st) { movz_(16, st, 0); str_ctx(16, C_STATUS); b_to(g_E1); }

// Adresse eines Speicheroperanden: Ergebnis in x16/x17; Host-Adresse = [rn, x20]
static Addr gen_addr(Insn *I) {
	Addr a = {16, 20, 0};
	if (!I->ripr && I->base >= 0 && I->index < 0 && !I->seg) {
		int64_t d = I->disp; E(0x8B000000 | 20 << 16 | I->base << 5 | 16);  // add x16, xbase, x20
		a.rn = 16; a.rm = -1;
		if (d >= -256 && d <= 255) a.imm = (int)d;
		else if (d > 0 && d < 4096) addsub_i(0, 0, 16, 16, d, 1);
		else if (d < 0 && d > -4096) addsub_i(1, 0, 16, 16, -d, 1);
		else { movimm(17, d); alu_r(ADD_, 16, 16, 17, 1); }
		return a;
	}
	int cur = -1;
	if (I->ripr) { movimm(16, I->pc + I->len + I->disp); cur = 16; }
	else {
		int64_t d = I->disp;
		if (I->base >= 0 && I->index >= 0) { alu_rs(ADD_, 16, I->base, I->index, 1, 0, I->scale); cur = 16; }
		else if (I->base >= 0) cur = I->base;
		else if (I->index >= 0) { lsl_i(16, I->index, I->scale, 1); cur = 16; }
		if (cur < 0) { movimm(16, d); cur = 16; d = 0; }
		if (d) {
			if (d > 0 && d < 4096) addsub_i(0, 0, 16, cur, d, 1);
			else if (d < 0 && d > -4096) addsub_i(1, 0, 16, cur, -d, 1);
			else { movimm(17, d); alu_r(ADD_, 16, cur, 17, 1); }
			cur = 16;
		}
	}
	if (I->seg) { ldr_ctx(17, I->seg == 1 ? C_FS : C_GS); alu_r(ADD_, 16, cur, 17, 1); cur = 16; }
	a.rn = cur; return a;
}
static void ld_mem(Insn *I, int rt, int size, int sx) { Addr a = gen_addr(I); ldst(1, size, sx, rt, a); }
static void st_mem(Insn *I, int rt, int size) { Addr a = gen_addr(I); ldst(0, size, 0, rt, a); }

static int reg_val(Opd o, int tmp) { if (o.hi) { ubfx(tmp, o.n, 8, 8, 0); return tmp; } return o.n; }
static void wr_reg(Opd d, int sz, int src) {
	if (sz == 8) { if (src != d.n) mov_r(d.n, src, 1); }
	else if (sz == 4) mov_r(d.n, src, 0);
	else if (sz == 2) bfi(d.n, src, 0, 16, 1);
	else bfi(d.n, src, d.hi ? 8 : 0, 8, 1);
}
// Operandenwert (nur die unteren sz Bytes gültig) in ein Host-Register
static int get(Insn *I, Opd *o, int sz, int tmp) {
	if (o->k == 1) return reg_val(*o, tmp);
	if (o->k == 2) { ld_mem(I, tmp, sz, 0); return tmp; }
	movimm(tmp, I->imm); return tmp;
}
static void put(Insn *I, Opd *d, int sz, int src) { if (d->k == 1) wr_reg(*d, sz, src); else st_mem(I, src, sz); }

static uint32_t cur_pc_hint;
static void add_slow_fix(int idx, uint32_t *pos, int kind) { Slow *s = &slow[idx]; if (s->nf < 8) { s->f[s->nf].pos = pos; s->f[s->nf].kind = kind; s->nf++; } }
static int new_slow(Insn *I, int count) { Slow *s = &slow[nslow]; s->pc = I->pc; s->end = I->pc + I->len; s->fs = fs; s->count = count; s->nf = 0; return nslow++; }
static void br_cbz(int idx, int nz, int rt, int is64) { add_slow_fix(idx, cp, 0); E((is64 ? 0xB4000000u : 0x34000000u) | (nz ? 0x01000000u : 0) | rt); }
static void br_bcond(int idx, int cond) { add_slow_fix(idx, cp, 0); E(0x54000000 | cond); }
static void br_tbnz(int idx, int bit, int rt) { add_slow_fix(idx, cp, 1); E(0x37000000 | (bit & 31) << 19 | rt); }
static void patch_fix(Fix *f, uint32_t *target) {
	int64_t off = target - f->pos;
	if (f->kind == 0) *f->pos |= (off & 0x7ffff) << 5; else if (f->kind == 1) *f->pos |= (off & 0x3fff) << 5; else *f->pos |= off & 0x3ffffff;
}

// Direkter Ausgang (Ziel bekannt): Flags sichern (patchbar), ggf. Signalprüfung bei Rücksprung, Sprung (wird beim Auflösen gepatcht)
static void exit_direct(uint64_t target, uint64_t from_pc) {
	Exit *e = &exs[nexs++]; e->target = target; e->fs = fs; e->pend = 0;
	if (target <= from_pc) {  // rückwärts: Signale prüfen
		ldr_ctx(16, C_PEND); E(0xB9400000 | 16 << 5 | 16); e->pend = cp; E(0x35000000 | 16);  // ldr w16,[x16]; cbnz w16, pending
	}
	save_flags(fs, 1);
	e->site = cp; E(0x14000000);
}
static void exit_indirect(int fsx) {  // Ziel (Gastadresse) in x17
	save_flags(fsx, 0);
	ldr_ctx(16, C_PEND); E(0xB9400000 | 16 << 5 | 16);
	{ int64_t off = (uint8_t *)g_MP - (uint8_t *)cp; E(0x35000000 | ((off / 4) & 0x7ffff) << 5 | 16); }
	ubfx(16, 17, 0, 12, 1); alu_rs(ADD_, 16, 21, 16, 1, 0, 4);
	E(0xA9400000 | 23 << 10 | 16 << 5 | T0);                          // ldp T0, T1, [x16]
	alu_r(SUB_, T0, T0, 17, 1);
	E(0xB5000000 | 2 << 5 | T0);                                       // cbnz T0, +2
	br_(T1);
	b_to(g_Mi);
}
static void exit_fb(uint64_t pc, int count, uint64_t end) {
	save_flags(fs, 0); movimm(16, pc); str_ctx(16, C_RIP); movimm(16, count); str_ctx(16, C_COUNT); movimm(16, end); str_ctx(16, C_FBEND); exit_status(JIT_FALLBACK);
}

#define ALL64 1
// Flags-Zustand nach Operation
static void set_fs_alu(int alu) { fs = (alu == 0 || alu == 2) ? FS_ADD : (alu == 3 || alu == 5 || alu == 7) ? FS_SUB : FS_LOGIC; }

static int gen_alu(Insn *I) {
	int alu = I->alu, sz = I->sz, live = I->live, cmp = alu == 7 || alu == 8, is64 = sz == 8;
	if (cmp && !live) return 1;
	if (alu == 2) to_add(); else if (alu == 3) to_sub();
	Opd *d = &I->d, *s = &I->s;
	if (sz >= 4) {
		int dr, sr = -1, fold = 0; uint64_t imm = (uint64_t)I->imm;
		if (!is64) imm &= 0xffffffffULL;
		if (d->k == 1) dr = d->n; else { ld_mem(I, T0, sz, 0); dr = T0; }
		int rd = cmp ? XZR : (d->k == 1 ? d->n : T0);
		if (s->k == 1) sr = s->n; else if (s->k == 2) { ld_mem(I, T1, sz, 0); sr = T1; }
		else if ((alu == 0 || alu == 5 || alu == 7) && imm < 4096 && (I->imm >= 0 || !is64) ) fold = 1;
		else if ((alu == 4 || alu == 8 || alu == 1 || alu == 6) && enc_bitmask(is64 ? (uint64_t)I->imm : imm, is64) >= 0) fold = 2;
		else { movimm(T1, imm); sr = T1; }
		switch (alu) {
		case 0: if (fold) addsub_i(0, live, rd, dr, imm, is64); else alu_r(live ? ADDS_ : ADD_, rd, dr, sr, is64); break;
		case 5: case 7: if (fold) addsub_i(1, live, rd, dr, imm, is64); else alu_r(live ? SUBS_ : SUB_, rd, dr, sr, is64); break;
		case 2: alu_r(live ? 0x3A000000 : 0x1A000000, rd, dr, sr, is64); break;
		case 3: alu_r(live ? 0x7A000000 : 0x5A000000, rd, dr, sr, is64); break;
		case 4: case 8: if (fold) logic_imm(live ? ANDS_ : AND_, rd, dr, is64 ? (uint64_t)I->imm : imm, is64); else alu_r(live ? ANDS_ : AND_, rd, dr, sr, is64); break;
		case 1: case 6: { uint32_t o = alu == 1 ? ORR_ : EOR_; if (fold) logic_imm(o, rd, dr, is64 ? (uint64_t)I->imm : imm, is64); else alu_r(o, rd, dr, sr, is64);
			if (live) alu_r(ANDS_, XZR, rd, rd, is64); break; }
		}
		if (d->k == 2 && !cmp) st_mem(I, rd, sz);
		if (live) set_fs_alu(alu);
		return 1;
	}
	// 8/16 Bit
	int sh = 32 - 8 * sz;
	int dr = d->k == 1 ? reg_val(*d, T0) : (ld_mem(I, T0, sz, 0), T0);
	int sr = s->k == 1 ? reg_val(*s, T1) : s->k == 2 ? (ld_mem(I, T1, sz, 0), T1) : (movimm(T1, (uint32_t)I->imm), T1);
	if (!live) {
		uint32_t o = alu == 0 ? ADD_ : alu == 5 ? SUB_ : alu == 4 ? AND_ : alu == 1 ? ORR_ : EOR_;
		alu_r(o, T2, dr, sr, 0); put(I, d, sz, T2);
	} else {
		lsl_i(T2, dr, sh, 0); lsl_i(T3, sr, sh, 0);
		switch (alu) {
		case 0: alu_r(ADDS_, T2, T2, T3, 0); break;
		case 5: alu_r(SUBS_, T2, T2, T3, 0); break;
		case 7: alu_r(SUBS_, XZR, T2, T3, 0); break;
		case 4: alu_r(ANDS_, T2, T2, T3, 0); break;
		case 8: alu_r(ANDS_, XZR, T2, T3, 0); break;
		case 1: alu_r(ORR_, T2, T2, T3, 0); alu_r(ANDS_, XZR, T2, T2, 0); break;
		case 6: alu_r(EOR_, T2, T2, T3, 0); alu_r(ANDS_, XZR, T2, T2, 0); break;
		}
		if (!cmp) { lsr_i(T2, T2, sh, 0); put(I, d, sz, T2); }
		set_fs_alu(alu);
	}
	return 1;
}

static int gen_unary(Insn *I) {
	int sz = I->sz, live = I->live, is64 = sz == 8, op = I->op; Opd *d = &I->d;
	int dr = d->k == 1 ? reg_val(*d, T0) : (ld_mem(I, T0, sz, 0), T0);
	if (op == OP_NOT) { E((is64 ? 0xAA2003E0u : 0x2A2003E0u) | dr << 16 | T2); put(I, d, sz, T2); return 1; }
	int sh = 32 - 8 * sz;
	if (op == OP_NEG) {
		if (!live) { alu_r(SUB_, T2, XZR, dr, is64 || 0); put(I, d, sz, T2); return 1; }
		if (sz >= 4) { alu_r(SUBS_, T2, XZR, dr, is64); } else { lsl_i(T2, dr, sh, 0); alu_r(SUBS_, T2, XZR, T2, 0); lsr_i(T2, T2, sh, 0); }
		put(I, d, sz, T2); fs = FS_SUB; return 1;
	}
	int inc = op == OP_INC;
	if (!live) { addsub_i(!inc, 0, T2, dr, 1, is64); put(I, d, sz, T2); return 1; }
	ensure_nzcv(); int oldsub = fs == FS_SUB; mrs_nzcv(17); ubfx(17, 17, 29, 1, 1);
	if (sz >= 4) addsub_i(!inc, 1, T2, dr, 1, is64);
	else { lsl_i(T2, dr, sh, 0); movimm(T3, 1u << sh); alu_r(inc ? ADDS_ : SUBS_, T2, T2, T3, 0); lsr_i(T2, T2, sh, 0); }
	mrs_nzcv(16);
	if (oldsub != !inc) logic_imm(EOR_, 17, 17, 1, 1);
	bfi(16, 17, 29, 1, 1); msr_nzcv(16); fs = inc ? FS_ADD : FS_SUB;
	put(I, d, sz, T2); return 1;
}

// Flags aus (c, v) Registern in NZCV nach tst: CF-Bit in x16, OF-Bit in x17 (oder -1)
static int gen_shift(Insn *I) {
	int sz = I->sz, live = I->live, is64 = sz == 8, k = (int)I->imm, alu = I->alu; Opd *d = &I->d; int n = is64 ? 64 : 32;
	if (I->cl) {
		if (live || sz < 4) return 0;
		int dr = d->k == 1 ? d->n : (ld_mem(I, T0, sz, 0), T0);
		int rd = d->k == 1 ? d->n : T0;
		shv(alu == 4 ? 0 : alu == 5 ? 1 : 2, rd, dr, 1, is64); if (d->k == 2) st_mem(I, rd, sz); return 1;
	}
	int dr = d->k == 1 ? reg_val(*d, T0) : (ld_mem(I, T0, sz, 0), T0);
	if (sz < 4) {
		if (live) return 0;
		int src = dr;
		if (alu == 5) { ubfx(T3, dr, 0, 8 * sz, 0); src = T3; } else if (alu == 7) { sbfx(T3, dr, 0, 8 * sz, 0); src = T3; }
		if (alu == 4) lsl_i(T2, src, k, 0); else if (alu == 5) lsr_i(T2, src, k, 0); else asr_i(T2, src, k, 0);
		put(I, d, sz, T2); return 1;
	}
	int rd = d->k == 1 ? d->n : T2;
	if (!live) {
		if (alu == 4) lsl_i(rd, dr, k, is64); else if (alu == 5) lsr_i(rd, dr, k, is64); else asr_i(rd, dr, k, is64);
		if (d->k == 2) st_mem(I, rd, sz); return 1;
	}
	// Flags: CF = zuletzt hinausgeschobenes Bit; OF (für Zähler 1) wie beim echten Befehl
	if (alu == 4) ubfx(16, dr, n - k, 1, is64); else ubfx(16, dr, k - 1, 1, is64);
	if (alu == 5) ubfx(17, dr, n - 1, 1, is64);
	if (alu == 4) lsl_i(rd, dr, k, is64); else if (alu == 5) lsr_i(rd, dr, k, is64); else asr_i(rd, dr, k, is64);
	if (alu == 4) { ubfx(17, rd, n - 1, 1, is64); alu_r(EOR_, 17, 17, 16, 0); }
	alu_r(ANDS_, XZR, rd, rd, is64); mrs_nzcv(T3);
	alu_rs(ORR_, T3, T3, 16, 1, 0, 29);
	if (alu != 7) alu_rs(ORR_, T3, T3, 17, 1, 0, 28);
	msr_nzcv(T3); fs = FS_ADD;
	if (d->k == 2) st_mem(I, rd, sz);
	return 1;
}

static void gen_ovf_flags(int ovf_ne /*Z=0 heißt Überlauf, nach cmp*/) {
	csetm_(T3, NE, 1); logic_imm(AND_, T3, T3, 0x30000000, 1); msr_nzcv(T3); fs = FS_ADD; (void)ovf_ne;
}
static int gen_mul(Insn *I) {
	int sz = I->sz, is64 = sz == 8, live = I->live, op = I->op;
	if (op == OP_IMUL2) {
		Opd *d = &I->d;
		int dr = d->n;
		int sr; if (I->imm3) { movimm(T1, I->imm); if (I->s.k == 1) { dr = I->s.n; } else { ld_mem(I, T3, sz, 0); dr = T3; } sr = T1; }
		else { if (I->s.k == 1) sr = I->s.n; else { ld_mem(I, T1, sz, 0); sr = T1; } }
		if (is64) {
			madd_(T0, dr, sr, XZR, 1);
			if (live) { E(0x9B407C00 | sr << 16 | dr << 5 | T2); alu_rs(SUBS_, XZR, T2, T0, 1, 2, 63); gen_ovf_flags(1); }
			mov_r(d->n, T0, 1);
		} else {
			E(0x9B207C00 | sr << 16 | dr << 5 | T0);                // smull x22, w, w
			if (live) { E(0xEB20C000 | 22 << 16 | T0 << 5 | XZR); gen_ovf_flags(1); }  // cmp x22, w22, sxtw
			mov_r(d->n, T0, 0);
		}
		return 1;
	}
	int sr; if (I->d.k == 1) sr = I->d.n; else { ld_mem(I, T1, sz, 0); sr = T1; }
	int sgn = op == OP_IMUL1;
	if (is64) {
		E((sgn ? 0x9B407C00u : 0x9BC07C00u) | sr << 16 | 0 << 5 | T2);  // hi
		madd_(T0, 0, sr, XZR, 1);
		if (live) { if (sgn) alu_rs(SUBS_, XZR, T2, T0, 1, 2, 63); else addsub_i(1, 1, XZR, T2, 0, 1); gen_ovf_flags(1); }
		mov_r(0, T0, 1); mov_r(2, T2, 1);
	} else {
		E((sgn ? 0x9B207C00u : 0x9BA07C00u) | sr << 16 | 0 << 5 | T0);
		if (live) { if (sgn) E(0xEB20C000 | 22 << 16 | T0 << 5 | XZR); else { lsr_i(T2, T0, 32, 1); addsub_i(1, 1, XZR, T2, 0, 1); } gen_ovf_flags(1); }
		lsr_i(T2, T0, 32, 1); mov_r(0, T0, 0); mov_r(2, T2, 0);
	}
	return 1;
}
static int gen_div(Insn *I) {
	int sz = I->sz, is64 = sz == 8, sgn = I->op == OP_IDIV;
	int sr; if (I->d.k == 1) sr = I->d.n; else { ld_mem(I, T1, sz, 0); sr = T1; }
	int sl = new_slow(I, 1);
	if (sr == 0 || sr == 2) { mov_r(T1, sr, 1); sr = T1; }
	br_cbz(sl, 0, sr, is64);
	if (!sgn) { br_cbz(sl, 1, 2, is64); }
	else { asr_i(T2, 0, is64 ? 63 : 31, is64); alu_r(EOR_, T2, T2, 2, is64); br_cbz(sl, 1, T2, is64);
	       addsub_i(0, 0, T2, sr, 1, is64); br_cbz(sl, 0, T2, is64); }
	div_(sgn, T0, 0, sr, is64); msub_(2, T0, sr, 0, is64); mov_r(0, T0, is64);
	return 1;
}

static int gen_push_val(int reg) { addsub_i(1, 0, 16, 4, 8, 1); Addr a = {16, 20, 0}; ldst(0, 8, 0, reg, a); mov_r(4, 16, 1); return 1; }

// Übersetzt einen Befehl; 0 = nicht übersetzbar (Block endet davor)
static int gen_insn(Insn *I) {
	int sz = I->sz;
	switch (I->op) {
	case OP_NOP: return 1;
	case OP_MOV: {
		Opd *d = &I->d, *s = &I->s;
		if (s->k == 3) {
			if (d->k == 1) {
				uint64_t v = I->imm; if (sz == 4) v &= 0xffffffff;
				if (sz >= 4) movimm(d->n, v); else { movimm(T1, v & 0xffff); wr_reg(*d, sz, T1); }
			} else { movimm(T1, I->imm); st_mem(I, T1, sz); }
			return 1;
		}
		if (d->k == 1 && s->k == 2) {
			if (sz >= 4) { ld_mem(I, d->n, sz, 0); } else { ld_mem(I, T1, sz, 0); wr_reg(*d, sz, T1); }
			return 1;
		}
		int sr = reg_val(*s, T1); put(I, d, sz, sr); return 1; }
	case OP_LEA: {
		Insn t = *I; Addr a;
		(void)t;
		if (I->ripr || I->index >= 0 || I->base < 0 || 1) {
			// Adresse in x16 (Gastadresse): gen_addr liefert Host-Form; hier die Gast-Form selbst bilden
			int64_t dsp = I->disp; int rd = (sz >= 4) ? I->d.n : T0; int cur = -1;
			if (I->ripr) { movimm(rd, I->pc + I->len + dsp); cur = rd; dsp = 0; }
			else if (I->base >= 0 && I->index >= 0) { alu_rs(ADD_, rd, I->base, I->index, 1, 0, I->scale); cur = rd; }
			else if (I->base >= 0) cur = I->base;
			else if (I->index >= 0) { lsl_i(rd, I->index, I->scale, 1); cur = rd; }
			else { movimm(rd, dsp); cur = rd; dsp = 0; }
			if (dsp) {
				if (dsp > 0 && dsp < 4096) addsub_i(0, 0, rd, cur, dsp, 1);
				else if (dsp < 0 && dsp > -4096) addsub_i(1, 0, rd, cur, -dsp, 1);
				else { movimm(17, dsp); alu_r(ADD_, rd, cur, 17, 1); }
				cur = rd;
			}
			if (sz == 8) { if (cur != rd) mov_r(rd, cur, 1); }
			else if (sz == 4) mov_r(rd, cur, 0);
		}
		(void)a; return 1; }
	case OP_ALU: return gen_alu(I);
	case OP_INC: case OP_DEC: case OP_NEG: case OP_NOT: return gen_unary(I);
	case OP_SHIFT: return gen_shift(I);
	case OP_MUL1: case OP_IMUL1: case OP_IMUL2: return gen_mul(I);
	case OP_DIV: case OP_IDIV: return gen_div(I);
	case OP_PUSH: {
		int r;
		if (I->d.k == 3) { movimm(T1, I->imm); r = T1; } else if (I->d.k == 1) r = I->d.n; else { ld_mem(I, T1, 8, 0); r = T1; }
		return gen_push_val(r); }
	case OP_POP: {
		Addr a = {4, 20, 0}; ldst(1, 8, 0, T1, a); addsub_i(0, 0, 4, 4, 8, 1); mov_r(I->d.n, T1, 1); return 1; }
	case OP_LEAVE: { mov_r(4, 5, 1); Addr a = {4, 20, 0}; ldst(1, 8, 0, 5, a); addsub_i(0, 0, 4, 4, 8, 1); return 1; }
	case OP_SETCC: { int c = cond_arm(I->cc); cset_(T0, c, 0); put(I, &I->d, 1, T0); return 1; }
	case OP_CMOV: {
		int c = cond_arm(I->cc); int is64 = sz == 8, sr;
		if (I->s.k == 1) sr = I->s.n; else { ld_mem(I, T1, sz, 0); sr = T1; }
		csel_(I->d.n, sr, I->d.n, c, is64); return 1; }
	case OP_MOVZX: {
		int ss = I->ssz;
		if (sz == 2) { int v = I->s.k == 1 ? reg_val(I->s, T1) : (ld_mem(I, T1, ss, 0), T1); ubfx(T2, v, 0, 8 * ss, 0); wr_reg(I->d, 2, T2); return 1; }
		if (I->s.k == 2) { ld_mem(I, I->d.n, ss, 0); return 1; }
		if (I->s.hi) ubfx(I->d.n, I->s.n, 8, 8, 0); else ubfx(I->d.n, I->s.n, 0, 8 * ss, 0);
		return 1; }
	case OP_MOVSX: {
		int ss = I->ssz, is64 = sz == 8;
		if (sz == 2) { if (I->s.k == 1) { if (I->s.hi) sbfx(T2, I->s.n, 8, 8, 0); else sbfx(T2, I->s.n, 0, 8 * ss, 0); } else ld_mem(I, T2, ss, 2); wr_reg(I->d, 2, T2); return 1; }
		if (I->s.k == 2) { ld_mem(I, I->d.n, ss, is64 ? 1 : 2); return 1; }
		if (I->s.hi) sbfx(I->d.n, I->s.n, 8, 8, is64); else sbfx(I->d.n, I->s.n, 0, 8 * ss, is64);
		return 1; }
	case OP_MOVSXD:
		if (I->s.k == 2) { ld_mem(I, I->d.n, 4, 1); return 1; }
		sbfx(I->d.n, I->s.n, 0, 32, 1); return 1;
	case OP_CDQ: if (sz == 8) asr_i(2, 0, 63, 1); else asr_i(2, 0, 31, 0); return 1;
	case OP_CWDE: if (sz == 8) sbfx(0, 0, 0, 32, 1); else sbfx(0, 0, 0, 16, 0); return 1;
	case OP_BSWAP: E((sz == 8 ? 0xDAC00C00u : 0x5AC00800u) | I->d.n << 5 | I->d.n); return 1;
	case OP_XCHG: {
		int is64 = sz == 8;
		if (I->d.k == 1) { mov_r(T0, I->d.n, is64); mov_r(I->d.n, I->s.n, is64); mov_r(I->s.n, T0, is64); }
		else { ld_mem(I, T0, sz, 0); st_mem(I, I->s.n, sz); mov_r(I->s.n, T0, is64); }
		return 1; }
	}
	return 0;
}

// Maschinencode der String-Befehle (eigene Funktion, weil der DF-Test x16 vorher laden muss)
static int gen_string(Insn *I) {
	int sz = I->sz; int sl = new_slow(I, 1);
	ldr_ctx(16, C_EFL); br_tbnz(sl, 10, 16);
	Addr as = {6, 20, 0}, ad = {7, 20, 0};
	uint32_t *loop = 0, *cz = 0;
	if (I->rep) { cz = cp; E(0xB4000000 | 1); }  // cbz x1, done (später)
	loop = cp;
	if (I->strop == 0) { ldst(1, sz, 0, T0, as); ldst(0, sz, 0, T0, ad); addsub_i(0, 0, 6, 6, sz, 1); addsub_i(0, 0, 7, 7, sz, 1); }
	else { ldst(0, sz, 0, 0, ad); addsub_i(0, 0, 7, 7, sz, 1); }
	if (I->rep) { addsub_i(1, 0, 1, 1, 1, 1); { int64_t off = loop - cp; E(0xB5000000 | (off & 0x7ffff) << 5 | 1); } *cz |= (cp - cz) << 5; }
	return 1;
}

typedef struct { uint64_t pc; uint32_t off; uint8_t fs, len; } IMap;
typedef struct { uint64_t pc; uint32_t *code; int nf; } Blk;
#define BTAB (1u << 16)
static Blk btab[BTAB]; static int nblk;
typedef struct { uint32_t *start; uint32_t i0, i1; } BRange;
static BRange *branges; static uint32_t nbr, cbr; static IMap *imap; static uint32_t nim, cim;
static int flushed;
static uint64_t code_lo = ~0ULL, code_hi;
uint64_t jit_flush_epoch;

static Blk *blk_lookup(uint64_t pc) {
	uint32_t h = (uint32_t)((pc ^ (pc >> 16)) * 2654435761u) & (BTAB - 1);
	for (;;) { Blk *b = &btab[h]; if (!b->code) return 0; if (b->pc == pc) return b; h = (h + 1) & (BTAB - 1); }
}
static Blk *blk_add(uint64_t pc, uint32_t *code, int nf) {
	uint32_t h = (uint32_t)((pc ^ (pc >> 16)) * 2654435761u) & (BTAB - 1);
	while (btab[h].code) h = (h + 1) & (BTAB - 1);
	btab[h].pc = pc; btab[h].code = code; btab[h].nf = nf; nblk++; return &btab[h];
}

void jit_flush(void) {
	code_lo = ~0ULL; code_hi = 0; jit_flush_epoch++; memset(btab, 0, sizeof btab); nblk = 0; cp = code_blocks; nbr = nim = 0; flushed = 1;
	if (gctx) memset((void *)gctx->jcache, 0, 4096 * 16);
}

// Befehlsbytes lesen kann an der Grenze des abgebildeten Gastspeichers fehlschlagen: dann Befehl wie unbekannt behandeln (Unicorn meldet den Fehler)
static jmp_buf decode_jb; static volatile int in_decode;
static void safe_decode(uint64_t pc, Insn *I) {
	in_decode = 1;
	if (_setjmp(decode_jb)) { memset(I, 0, sizeof *I); I->pc = pc; I->op = OP_FB; I->len = 0; I->base = I->index = -1; in_decode = 0; return; }
	decode((const uint8_t *)(pc + DELTA), pc, I);
	in_decode = 0;
}
static Blk *translate(uint64_t pc) {
	if (cp + 16384 > code_end || nblk > 40000) jit_flush();
	pthread_jit_write_protect_np(0);
	static Insn ins[MAXI + 2]; int n = 0; uint64_t cur = pc; int follows = 0, term = 0;
	while (n < MAXI && !term) {
		Insn *I = &ins[n];
		safe_decode(cur, I);
		if (I->op == OP_FB) {
			I->count = 1;
			if (I->len > 0 && !I->branchy) {
				uint64_t c2 = cur + I->len;
				for (int k = 0; k < 160; k++) {
					Insn t; safe_decode(c2, &t);
					if (t.op != OP_FB || t.len <= 0) break;
					I->len += t.len; I->count++; c2 += t.len; if (t.branchy) break;
				}
			}
			n++; term = 1; break;
		}
		n++; cur += I->len;
		if (I->op == OP_JMP && follows < 4 && n < MAXI / 2) { cur = I->target; I->followed = 1; follows++; continue; }
		if (I->op == OP_JMP || I->op == OP_JCC || I->op == OP_CALL || I->op == OP_CALLI || I->op == OP_RET || I->op == OP_JMPI || I->op == OP_SYSCALL) term = 1;
	}
	// Flag-Lebendigkeit rückwärts
	int L = 1;
	for (int i = n - 1; i >= 0; i--) {
		Insn *I = &ins[i]; I->live = L; int rd = 0, wr = 0;
		switch (I->op) {
		case OP_ALU: wr = 1; if (I->alu == 2 || I->alu == 3) rd = 1; break;
		case OP_NEG: case OP_MUL1: case OP_IMUL1: case OP_IMUL2: wr = 1; break;
		case OP_SHIFT: if (!I->cl) wr = 1; else rd = 1; break;
		case OP_JCC: case OP_SETCC: case OP_CMOV: case OP_FB: case OP_SYSCALL: rd = 1; break;
		case OP_DIV: case OP_IDIV: rd = 1; break;
		default: break;
		}
		if (rd) L = 1; else if (wr) L = 0;
	}
	cur_live_in = L;
	if (pc < code_lo) code_lo = pc; if (cur + 16 > code_hi) code_hi = cur + 16; if (pc + 1 > code_hi) code_hi = pc + 1; for (int i = 0; i < n; i++) { if (ins[i].pc < code_lo) code_lo = ins[i].pc; if (ins[i].pc + 16 > code_hi) code_hi = ins[i].pc + 16; }
	if (nbr == cbr) { cbr = cbr ? cbr * 2 : 4096; branges = realloc(branges, cbr * sizeof *branges); }
	uint32_t *start = cp; branges[nbr].start = start; branges[nbr].i0 = nim;
	fs = FS_CTX; nslow = 0; nexs = 0; trunc_ = 0;
	int ended = 0; uint64_t next_pc = pc;
	for (int i = 0; i < n && !ended; i++) {
		Insn *I = &ins[i]; next_pc = I->pc + I->len;
		if (nim == cim) { cim = cim ? cim * 2 : 65536; imap = realloc(imap, cim * sizeof *imap); }
		imap[nim].pc = I->pc; imap[nim].off = (uint32_t)(cp - start); imap[nim].fs = fs; imap[nim].len = (uint8_t)I->len; nim++;
		st_insns++;
		switch (I->op) {
		case OP_FB: exit_fb(I->pc, I->count, I->len > 0 ? I->pc + I->len : 0); st_fb++; ended = 1; break;
		case OP_JMP: if (!I->followed) { exit_direct(I->target, I->pc); ended = 1; } break;
		case OP_JCC: {
			int c = cond_arm(I->cc); uint32_t *bc = cp; E(0x54000000 | (c ^ 1));
			exit_direct(I->target, I->pc);
			*bc |= ((cp - bc) & 0x7ffff) << 5;
			exit_direct(I->pc + I->len, I->pc); ended = 1; break; }
		case OP_CALL: movimm(T1, I->pc + I->len); gen_push_val(T1); exit_direct(I->target, I->pc); ended = 1; break;
		case OP_CALLI: {
			if (I->d.k == 1) mov_r(17, I->d.n, 1); else ld_mem(I, 17, 8, 0);
			movimm(T1, I->pc + I->len); gen_push_val(T1);   // gen_push_val benutzt x16, nicht x17
			exit_indirect(fs); ended = 1; break; }
		case OP_JMPI: if (I->d.k == 1) mov_r(17, I->d.n, 1); else ld_mem(I, 17, 8, 0); exit_indirect(fs); ended = 1; break;
		case OP_RET: {
			Addr a = {4, 20, 0}; ldst(1, 8, 0, 17, a); addsub_i(0, 0, 4, 4, 8 + (uint32_t)I->imm, 1); exit_indirect(fs); ended = 1; break; }
		case OP_SYSCALL: save_flags(fs, 0); movimm(16, I->pc + I->len); str_ctx(16, C_RIP); exit_status(JIT_SYSCALL); ended = 1; break;
		case OP_STRING: gen_string(I); break;
		default:
			if (!gen_insn(I)) { exit_fb(I->pc, 1, I->pc + I->len); st_fb++; ended = 1; trunc_ = 1; }
		}
	}
	if (!ended) exit_direct(next_pc, 0);
	// langsame Ausgänge
	for (int s = 0; s < nslow; s++) {
		for (int f = 0; f < slow[s].nf; f++) patch_fix(&slow[s].f[f], cp);
		int save_fs = slow[s].fs; save_flags(save_fs, 0);
		movimm(16, slow[s].pc); str_ctx(16, C_RIP); movimm(16, slow[s].count); str_ctx(16, C_COUNT); movimm(16, slow[s].end); str_ctx(16, C_FBEND); exit_status(JIT_FALLBACK);
	}
	// Ausgangs-Stubs
	for (int e = 0; e < nexs; e++) {
		Exit *x = &exs[e]; uint32_t *stub = cp;
		movimm(16, x->target); adr_(17, x->site); b_to(g_M);
		*x->site |= (stub - x->site) & 0x3ffffff;
		if (x->pend) {
			uint32_t *ps = cp; save_flags(x->fs, 0); movimm(16, x->target); str_ctx(16, C_RIP); exit_status(JIT_PENDING);
			*x->pend |= ((ps - x->pend) & 0x7ffff) << 5;
		}
	}
	branges[nbr].i1 = nim; nbr++;
	Blk *b = blk_add(pc, start, trunc_ ? 1 : cur_live_in);
	sys_icache_invalidate(start, (cp - start) * 4);
	pthread_jit_write_protect_np(1);
	st_blocks++;
	if (trace_jit) fprintf(stderr, "jit: Block %#llx: %d Befehle, %d Wörter, nf=%d\n", (unsigned long long)pc, n, (int)(cp - start), b->nf);
	if (trace_jit > 1) { fprintf(stderr, "jit: Code"); for (uint32_t *w = start; w < cp; w++) fprintf(stderr, " %08x", *w); fputc(10, stderr); }
	return b;
}

// Sprung an der Stelle site direkt auf den übersetzten Block legen
static void patch_site(uint32_t *site, Blk *t) {
	pthread_jit_write_protect_np(0);
	int64_t off = t->code - site; uint32_t *lo = site;
	*site = 0x14000000 | (off & 0x3ffffff);
	if (!t->nf && site[-1] == STP_SAVE) { site[-1] = site[-2] = site[-3] = NOP; lo = site - 3; }
	pthread_jit_write_protect_np(1);
	sys_icache_invalidate(lo, (site + 1 - lo) * 4);
}

// ------------------------------------------------------------------------------------------------ Laufzeit
static void emit_common(void) {
	g_enter = cp;
	E(0xA9BA7BFD);                                   // stp x29,x30,[sp,#-96]!
	{ int regs[5][2] = {{19, 20}, {21, 22}, {23, 24}, {25, 26}, {27, 28}}; for (int i = 0; i < 5; i++) E(0xA9000000 | ((16 + 16 * i) / 8) << 15 | regs[i][1] << 10 | 31 << 5 | regs[i][0]); }
	mov_r(19, 0, 1); mov_r(16, 1, 1);
	E(0x910003E0 | 17); /* mov x17, sp */ str_ctx(17, C_HSP);
	ldr_ctx(20, C_DELTA); ldr_ctx(21, C_JC);
	for (int i = 0; i < 16; i += 2) E(0xA9400000 | (i / 2 * 2) << 15 | (i + 1) << 10 | 19 << 5 | i);
	br_(16);
	g_E1 = cp;
	for (int i = 0; i < 16; i += 2) E(0xA9000000 | (i / 2 * 2) << 15 | (i + 1) << 10 | 19 << 5 | i);
	g_E2 = cp;
	ldr_ctx(16, C_HSP); E(0x91000000 | 16 << 5 | 31);  // mov sp, x16
	{ int regs[5][2] = {{19, 20}, {21, 22}, {23, 24}, {25, 26}, {27, 28}}; for (int i = 0; i < 5; i++) E(0xA9400000 | ((16 + 16 * i) / 8) << 15 | regs[i][1] << 10 | 31 << 5 | regs[i][0]); }
	E(0xA8C67BFD);                                   // ldp x29,x30,[sp],#96
	E(0xD65F03C0);                                   // ret
	g_M = cp; str_ctx(16, C_NEXT); str_ctx(17, C_SITE); movz_(16, JIT_MISS, 0); str_ctx(16, C_STATUS); b_to(g_E1);
	g_Mi = cp; str_ctx(17, C_NEXT); str_ctx(XZR, C_SITE); movz_(16, JIT_MISS, 0); str_ctx(16, C_STATUS); b_to(g_E1);
	g_MP = cp; str_ctx(17, C_RIP); movz_(16, JIT_PENDING, 0); str_ctx(16, C_STATUS); b_to(g_E1);
}

static void fault_handler(int sig, siginfo_t *si, void *uc_) {
	ucontext_t *uc = uc_; uint64_t pcv = uc->uc_mcontext->__ss.__pc; if (trace_jit) fprintf(stderr, "jit: Signal %d pc %llx addr %p\n", sig, (unsigned long long)pcv, si->si_addr);
	if (in_decode) { in_decode = 0; _longjmp(decode_jb, 1); }
	if (!gctx || pcv < (uint64_t)code_blocks || pcv >= (uint64_t)code_end) {
		signal(sig, SIG_DFL); return;  // kein JIT-Code: Standardverhalten beim erneuten Fehler
	}
	// Block suchen (branges ist nach Adresse sortiert)
	uint32_t lo = 0, hi = nbr;
	while (lo + 1 < hi) { uint32_t mid = (lo + hi) / 2; if ((uint64_t)branges[mid].start <= pcv) lo = mid; else hi = mid; }
	BRange *br = &branges[lo]; uint32_t off = (uint32_t)((pcv - (uint64_t)br->start) / 4); IMap *m = &imap[br->i0];
	for (uint32_t i = br->i0; i < br->i1; i++) if (imap[i].off <= off) m = &imap[i];
	for (int i = 0; i < 16; i++) gctx->r[i] = uc->uc_mcontext->__ss.__x[i];
	uint64_t nz = uc->uc_mcontext->__ss.__cpsr & 0xf0000000ULL;
	if (m->fs == FS_SUB) nz ^= 0x20000000ULL;
	if (m->fs != FS_CTX) { gctx->nzcv = nz; gctx->expl = 0; }
	gctx->rip = m->pc; gctx->fb_end = m->len ? m->pc + m->len : 0; gctx->aux = (uint64_t)si->si_addr; gctx->count = 1; gctx->status = JIT_FAULT;
	uc->uc_mcontext->__ss.__pc = (uint64_t)g_E2;
	uc->uc_mcontext->__ss.__x[19] = (uint64_t)gctx;
}

int jit_init(JitCtx *c) {
	if (!code_base) {
		trace_jit = getenv("UCRUN_JIT_TRACE") ? atoi(getenv("UCRUN_JIT_TRACE")) : 0;
		if (trace_jit) fprintf(stderr, "jit: init: Codepuffer\n");
		void *p = mmap(0, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
		if (p == MAP_FAILED) return -1;
		code_base = p; code_end = code_base + CODE_SIZE / 4; cp = code_base;
		if (trace_jit) fprintf(stderr, "jit: init: Puffer %p, emit_common\n", p);
		pthread_jit_write_protect_np(0);
		emit_common(); code_blocks = cp;
		sys_icache_invalidate(code_base, (cp - code_base) * 4);
		pthread_jit_write_protect_np(1);
		if (trace_jit) fprintf(stderr, "jit: init: Handler\n");
		struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = fault_handler; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
		sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
	}
	if (trace_jit) fprintf(stderr, "jit: init: Gastspeicher\n");
	if (reserve_guest()) return -2;
	memset(c, 0, sizeof *c); c->delta = DELTA;
	c->jcache = (uint64_t)calloc(4096, 16);
	gctx = c;
	return 0;
}

int jit_run(JitCtx *c) {
	typedef uint64_t (*enter_fn)(JitCtx *, void *);
	Blk *b = blk_lookup(c->rip); if (!b) b = translate(c->rip);
	pthread_jit_write_protect_np(1);
	for (;;) {
		((enter_fn)(void *)g_enter)(c, b->code);
		if (c->status != JIT_MISS) return (int)c->status;
		uint64_t pc = c->next_pc; uint32_t *site = (uint32_t *)c->exit_site;
		flushed = 0;
		b = blk_lookup(pc); if (!b) b = translate(pc);
		if (site && !flushed) patch_site(site, b);
		else if (!site) { uint64_t *e = (uint64_t *)c->jcache + 2 * (pc & 4095); e[0] = pc; e[1] = (uint64_t)b->code; }
	}
}
void jit_invalidate(uint64_t g, uint64_t n) { if (code_hi && g < code_hi && g + n > code_lo) jit_flush(); }
void jit_stats(uint64_t *blocks, uint64_t *insns, uint64_t *fbs) { *blocks = st_blocks; *insns = st_insns; *fbs = st_fb; }

uint64_t jit_eflags_of(const JitCtx *c) {
	if (c->expl) return c->eflags;
	uint64_t n = c->nzcv, ef = (c->eflags & ~0x8d5ULL) | 2;
	if (n & (1ULL << 29)) ef |= 1; if (n & (1ULL << 30)) ef |= 0x40; if (n & (1ULL << 31)) ef |= 0x80; if (n & (1ULL << 28)) ef |= 0x800;
	return ef;
}
void jit_set_eflags(JitCtx *c, uint64_t ef) {
	c->eflags = ef; c->expl = 1; uint64_t n = 0;
	if (ef & 1) n |= 1ULL << 29; if (ef & 0x40) n |= 1ULL << 30; if (ef & 0x80) n |= 1ULL << 31; if (ef & 0x800) n |= 1ULL << 28;
	c->nzcv = n;
}
