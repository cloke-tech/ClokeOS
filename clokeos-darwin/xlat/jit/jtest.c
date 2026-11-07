// jtest.c – Differenztest des JIT gegen Unicorn: zufällige Befehlsfolgen des unterstützten Teilsatzes, Zustand danach vergleichen
//   jtest [Anzahl] [Seed] [-v]       (Host-Mac, arm64)
// Vergleich: 16 Register, rip, Flags CF/ZF/SF/OF (PF/AF nicht: siehe Studie), Datenbereich. Läuft erst Unicorn (Orakel), dann der JIT
// (mit Rückfall auf Unicorn für nicht unterstützte Befehle, wie in ucrun).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "jit.h"
#include "ucsync.h"

#define CODE 0x10000000ULL
#define CODE_N 0x10000ULL
#define DATA 0x20000000ULL
#define DATA_N 0x10000ULL
#define SLOT 0x100
#define NSLOT 256

static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static int rn(int n) { return (int)(rnd() % (uint64_t)n); }

typedef struct { uint8_t b[96]; int n; uint32_t ptr, idx; uint64_t base; uint32_t undef; int wf; } Buf;
static void put8(Buf *B, int v) { B->b[B->n++] = (uint8_t)v; }
static void put32(Buf *B, int64_t v) { int32_t x = (int32_t)v; memcpy(B->b + B->n, &x, 4); B->n += 4; }
static int64_t rimm(int bytes) {
	static const int64_t pool[] = {0, 1, -1, 2, 0x7f, -0x80, 0x80, 0xff, 0x7fff, -0x8000, 0x7fffffff, -0x7fffffff - 1, 31, 32, 63, 64};
	int64_t v = rn(3) ? pool[rn(16)] : (int64_t)rnd();
	if (bytes == 1) v = (int8_t)v; else if (bytes == 2) v = (int16_t)v; else if (bytes == 4) v = (int32_t)v;
	return v;
}
// Befehl mit ModRM: pfx66, rex.w, opcode (1-2 Bytes), reg-Feld (oder /Ziffer), mem (0: Register rm), imm-Bytes
static void emit_rm(Buf *B, int p66, int w, const uint8_t *op, int nop, int reg, int mem, int rmreg, int bytereg, int immb, int64_t imm) {
	int base = 0, index = -1, scale = 0, mod = 3, rip = 0, disp = 0, dispb = 0;
	if (mem) {
		int kind = rn(8);
		if (kind == 0) { rip = 1; mod = 0; dispb = 4; }
		else {
			base = rn(16); mod = rn(3); if (rn(4) == 0) index = rn(16); if (index == 4) index = -1; scale = rn(4);
			if ((base & 7) == 5 && mod == 0) mod = 1;
			disp = mod == 1 ? (int8_t)(rn(64) - 32) : mod == 2 ? rn(512) - 256 : 0; dispb = mod == 1 ? 1 : mod == 2 ? 4 : 0;
			B->ptr |= 1u << base; if (index >= 0) B->idx |= 1u << index;
		}
	}
	int rex = 0; if (w) rex |= 8; if (reg & 8) rex |= 4; if (index >= 0 && mem) rex |= 2;
	if (mem ? (!rip && (base & 8)) : (rmreg & 8)) rex |= 1;
	if (bytereg && ((reg & 7) >= 4 || (!mem && (rmreg & 7) >= 4)) && rn(2)) rex |= 0x40;   // spl/bpl/sil/dil statt ah..bh
	if (bytereg && (reg >= 4 || (!mem && rmreg >= 4)) && !rex) rex |= 0x40;               // Register >= 4 ohne REX wären ah..bh -> das ist erlaubt; hier zufällig REX
	if (p66) put8(B, 0x66);
	if (rex) put8(B, 0x40 | rex);
	for (int i = 0; i < nop; i++) put8(B, op[i]);
	if (!mem) put8(B, 0xc0 | (reg & 7) << 3 | (rmreg & 7));
	else if (rip) {
		put8(B, (reg & 7) << 3 | 5);
		uint64_t next = CODE + (uint64_t)B->base * SLOT + B->n + 4 + immb;
		put32(B, (int64_t)(DATA + 0x4100 + rn(0x100) - next));
	} else if (index >= 0 || (base & 7) == 4) {
		put8(B, mod << 6 | (reg & 7) << 3 | 4); put8(B, scale << 6 | ((index >= 0 ? index : 4) & 7) << 3 | (base & 7));
		if (dispb == 1) put8(B, disp); else if (dispb == 4) put32(B, disp);
	} else {
		put8(B, mod << 6 | (reg & 7) << 3 | (base & 7));
		if (dispb == 1) put8(B, disp); else if (dispb == 4) put32(B, disp);
	}
	if (immb == 1) put8(B, (int)imm); else if (immb == 2) { put8(B, (int)imm); put8(B, (int)(imm >> 8)); } else if (immb == 4) put32(B, imm);
	else if (immb == 8) { memcpy(B->b + B->n, &imm, 8); B->n += 8; }
}
static uint32_t cur_undef; static int cur_fp, sse_mode_;
static int cc_ok(int cc) { static const uint32_t rm[16] = {0x800, 0x800, 1, 1, 0x40, 0x40, 0x41, 0x41, 0x80, 0x80, 0, 0, 0x880, 0x880, 0x8c0, 0x8c0}; return !(rm[cc] & cur_undef); }
static int pick_cc(void) { for (int k = 0; k < 64; k++) { int cc = rn(16); if (cc != 10 && cc != 11 && cc_ok(cc)) return cc; } return -1; }
static int rsz(void) { static const int s[] = {1, 2, 4, 4, 8, 8}; return s[rn(6)]; }
#define CFOF 0x801u
#define ALLF 0x8c1u
static void gen_one(Buf *B) {
	int sz = rsz(), w = sz == 8, p66 = sz == 2, reg = rn(16), rm = rn(16), mem = rn(2), alu = rn(8);
	uint8_t op[3];
	switch (rn(30)) {
	case 0: case 1: case 2: case 3: {  // ALU r/m,r  /  r,r/m
		op[0] = alu * 8 + (rn(2) ? 2 : 0) + (sz > 1); emit_rm(B, p66, w, op, 1, reg, mem, rm, sz == 1, 0, 0); B->wf = 1; B->undef = 0; break; }
	case 4: case 5: {  // ALU imm
		int k = rn(3);
		if (k == 0 && sz == 1) { op[0] = alu * 8 + 4; put8(B, op[0]); put8(B, (int)rimm(1)); }
		else if (k == 0) { if (p66) put8(B, 0x66); if (w) put8(B, 0x48); put8(B, alu * 8 + 5); int ib = sz == 2 ? 2 : 4; int64_t v = rimm(ib); if (ib == 2) { put8(B, (int)v); put8(B, (int)(v >> 8)); } else put32(B, v); }
		else if (sz == 1) { op[0] = 0x80; emit_rm(B, 0, 0, op, 1, alu, mem, rm, 1, 1, rimm(1)); }
		else if (k == 1) { op[0] = 0x83; emit_rm(B, p66, w, op, 1, alu, mem, rm, 0, 1, rimm(1)); }
		else { op[0] = 0x81; int ib = sz == 2 ? 2 : 4; emit_rm(B, p66, w, op, 1, alu, mem, rm, 0, ib, rimm(ib)); }
		B->wf = 1; B->undef = 0; break; }
	case 6: {  // test
		if (rn(2)) { op[0] = 0x84 + (sz > 1); emit_rm(B, p66, w, op, 1, reg, mem, rm, sz == 1, 0, 0); }
		else { op[0] = sz == 1 ? 0xf6 : 0xf7; int ib = sz == 1 ? 1 : sz == 2 ? 2 : 4; emit_rm(B, p66, w, op, 1, 0, mem, rm, sz == 1, ib, rimm(ib)); }
		B->wf = 1; B->undef = 0; break; }
	case 7: {  // inc dec
		op[0] = sz == 1 ? 0xfe : 0xff; emit_rm(B, p66, w, op, 1, rn(2), mem, rm, sz == 1, 0, 0); B->wf = 2; break; }
	case 8: {  // not neg
		op[0] = sz == 1 ? 0xf6 : 0xf7; int k = 2 + rn(2); emit_rm(B, p66, w, op, 1, k, mem, rm, sz == 1, 0, 0); if (k == 3) { B->wf = 1; B->undef = 0; } break; }
	case 9: {  // mul imul div idiv (1 Operand)
		op[0] = sz == 1 ? 0xf6 : 0xf7; int k = 4 + rn(4);
		emit_rm(B, p66, w, op, 1, k, mem, rm, sz == 1, 0, 0);
		B->wf = 1; B->undef = k < 6 ? (0x80 | 0x40 | 0x04 | 0x10) : ALLF; break; }
	case 10: {  // imul 2/3 Operanden
		if (sz == 1) sz = 4, p66 = 0, w = 0;
		int k = rn(3);
		if (k == 0) { op[0] = 0x0f; op[1] = 0xaf; emit_rm(B, p66, w, op, 2, reg, mem, rm, 0, 0, 0); }
		else if (k == 1) { op[0] = 0x6b; emit_rm(B, p66, w, op, 1, reg, mem, rm, 0, 1, rimm(1)); }
		else { op[0] = 0x69; int ib = sz == 2 ? 2 : 4; emit_rm(B, p66, w, op, 1, reg, mem, rm, 0, ib, rimm(ib)); }
		B->wf = 1; B->undef = 0xc4 | 0x10; break; }
	case 11: case 12: {  // Schiebebefehle
		int k = rn(10) < 8 ? (int[]){4, 5, 7}[rn(3)] : rn(8), form = rn(3);
		if (form == 0) { op[0] = sz == 1 ? 0xc0 : 0xc1; int c = rn(sz == 8 ? 64 : 32); emit_rm(B, p66, w, op, 1, k, mem, rm, sz == 1, 1, c); B->undef = c == 1 ? 0x10 : (CFOF | 0x10) & ~1u | 0x800 | 0x10; if (c != 1) B->undef = 0x800 | 0x10; if (c == 0) B->undef = ALLF; }
		else if (form == 1) { op[0] = sz == 1 ? 0xd0 : 0xd1; emit_rm(B, p66, w, op, 1, k, mem, rm, sz == 1, 0, 0); B->undef = 0x10; }
		else { op[0] = sz == 1 ? 0xd2 : 0xd3; emit_rm(B, p66, w, op, 1, k, mem, rm, sz == 1, 0, 0); B->undef = ALLF; }
		if (k >= 0 && k <= 3) B->undef = ALLF;
		B->wf = 1; break; }
	case 13: case 14: case 15: {  // mov
		int k = rn(5);
		if (k == 0) { op[0] = 0x88 + (sz > 1); emit_rm(B, p66, w, op, 1, reg, mem, rm, sz == 1, 0, 0); }
		else if (k == 1) { op[0] = 0x8a + (sz > 1); emit_rm(B, p66, w, op, 1, reg, mem, rm, sz == 1, 0, 0); }
		else if (k == 2) { op[0] = sz == 1 ? 0xc6 : 0xc7; int ib = sz == 1 ? 1 : sz == 2 ? 2 : 4; emit_rm(B, p66, w, op, 1, 0, mem, rm, sz == 1, ib, rimm(ib)); }
		else if (sz == 1) { int r = rn(16); if (r >= 8 || rn(2)) put8(B, 0x40 | (r >> 3)); else r = rn(4); put8(B, 0xb0 + (r & 7)); put8(B, (int)rimm(1)); }
		else { int r = rn(16); if (p66) put8(B, 0x66); if (w || r >= 8) put8(B, 0x40 | (w ? 8 : 0) | (r >> 3)); put8(B, 0xb8 + (r & 7));
			if (w) { int64_t v = rimm(8); memcpy(B->b + B->n, &v, 8); B->n += 8; } else if (p66) { int64_t v = rimm(2); put8(B, (int)v); put8(B, (int)(v >> 8)); } else put32(B, rimm(4)); }
		break; }
	case 16: { op[0] = 0x8d; if (sz == 1) sz = 4, p66 = 0, w = 0; if (sz == 2) { p66 = 0; w = 0; } emit_rm(B, p66, w, op, 1, reg, 1, rm, 0, 0, 0); break; }
	case 17: {  // movzx / movsx
		int k = rn(4); op[0] = 0x0f; op[1] = k == 0 ? 0xb6 : k == 1 ? 0xb7 : k == 2 ? 0xbe : 0xbf; if (sz == 1) sz = 4, p66 = 0, w = 0;
		emit_rm(B, p66, w, op, 2, reg, mem, rm, (k & 1) == 0, 0, 0); break; }
	case 18: { op[0] = 0x63; emit_rm(B, 0, 1, op, 1, reg, mem, rm, 0, 0, 0); break; }
	case 19: { op[0] = 0x86 + (sz > 1); mem = 0; /* xchg mit Speicher: Unicorn rechnet bei unausgerichteten Adressen falsch (atomarer Pfad) */ emit_rm(B, p66, w, op, 1, reg, mem, rm, sz == 1, 0, 0); break; }
	case 20: { int k = rn(4); if (k == 0) { put8(B, 0x99); } else if (k == 1) { put8(B, 0x48); put8(B, 0x99); } else if (k == 2) put8(B, 0x98); else { put8(B, 0x48); put8(B, 0x98); } break; }
	case 21: { int cc = pick_cc(); if (cc < 0) { put8(B, 0x90); break; } op[0] = 0x0f; op[1] = 0x90 + cc; emit_rm(B, 0, 0, op, 2, 0, mem, rm, 1, 0, 0); break; }
	case 22: { int cc = pick_cc(); if (cc < 0) { put8(B, 0x90); break; } op[0] = 0x0f; op[1] = 0x40 + cc; if (sz == 1) sz = 4, p66 = 0, w = 0; emit_rm(B, p66, w, op, 2, reg, mem, rm, 0, 0, 0); break; }
	case 23: { int r = rn(16); if (sz < 4) sz = 4; if (r >= 8 || sz == 8) put8(B, 0x40 | (sz == 8 ? 8 : 0) | (r >> 3)); put8(B, 0x0f); put8(B, 0xc8 + (r & 7)); break; }
	case 24: {  // push/pop Paar
		int r = rn(16), r2 = rn(16); if (r != 4) { if (r >= 8) put8(B, 0x41); put8(B, 0x50 + (r & 7)); } else put8(B, 0x90);
		if (r2 != 4) { if (r2 >= 8) put8(B, 0x41); put8(B, 0x58 + (r2 & 7)); } break; }
	case 25: {  // push imm / push r/m / leave
		int k = rn(4);
		if (k == 0) { put8(B, 0x68); put32(B, rimm(4)); } else if (k == 1) { put8(B, 0x6a); put8(B, (int)rimm(1)); }
		else if (k == 2) { op[0] = 0xff; emit_rm(B, 0, 0, op, 1, 6, 1, rm, 0, 0, 0); } else { put8(B, 0xc9); B->ptr |= 1u << 5; }
		break; }
	case 26: {  // Zeichenketten
		int k = rn(4); B->ptr |= (1u << 6) | (1u << 7); if (rn(3)) put8(B, 0xf3);
		if (k == 0) put8(B, 0xa4); else if (k == 1) { put8(B, 0x48); put8(B, 0xa5); } else if (k == 2) put8(B, 0xaa); else { put8(B, 0x48); put8(B, 0xab); }
		B->idx |= 1u << 1; break; }
	case 27: { op[0] = 0x0f; op[1] = 0x1f; emit_rm(B, 0, 0, op, 2, 0, 1, 0, 0, 0, 0); break; }
	case 28: { // xchg eax,r / nop / x87-Befehl (Fallback) / SSE (Fallback)
		int k = rn(4);
		if (k == 0) { int r = 1 + rn(7); if (r == 4) r = 3; put8(B, 0x90 + r); }
		else if (k == 1) { put8(B, 0x66); put8(B, 0x90); }
		else if (k == 2) { op[0] = 0x0f; op[1] = 0xa2; put8(B, 0x0f); put8(B, 0xa2); B->undef = 0; }   // cpuid -> Unicorn
		else { put8(B, 0xfc); }  // cld
		break; }
	case 29: { op[0] = 0x0f; op[1] = 0xaf; emit_rm(B, p66, w, op, 2, reg, mem, rm, 0, 0, 0); B->wf = 1; B->undef = 0xc4 | 0x10; if (sz == 1 || sz == 2) { B->n = B->n; } break; }
	}
}

// ---------------------------------------------------------------------------------------------- SSE/SSE2
static int sse_mode;           // jtest <n> <seed> -v|-s : -s erzeugt überwiegend SSE-Befehle
static int want_pjcc;          // direkt hinter comis: jp/jnp/je/jne/jb/ja/... hinten anhängen
static void put_sse(Buf *B, int pfx, int op, int reg, int mem, int rmreg, int w, int immb, int64_t imm) {
	uint8_t o[2] = {0x0f, (uint8_t)op};
	if (pfx == 0x66) emit_rm(B, 1, w, o, 2, reg, mem, rmreg, 0, immb, imm);
	else { if (pfx) put8(B, pfx); emit_rm(B, 0, w, o, 2, reg, mem, rmreg, 0, immb, imm); }
}
static void gen_sse_one(Buf *B) {
	static const struct { uint8_t pfx, op, form; int8_t fp; } T[] = {
		// form 0: xmm, xmm/m
		{0,0x58,0,32},{0x66,0x58,0,64},{0xf3,0x58,0,32},{0xf2,0x58,0,64},{0,0x5c,0,32},{0x66,0x5c,0,64},{0xf3,0x5c,0,32},{0xf2,0x5c,0,64},
		{0,0x59,0,32},{0x66,0x59,0,64},{0xf3,0x59,0,32},{0xf2,0x59,0,64},{0,0x5e,0,32},{0x66,0x5e,0,64},{0xf3,0x5e,0,32},{0xf2,0x5e,0,64},
		{0,0x5d,0,32},{0x66,0x5d,0,64},{0xf3,0x5d,0,32},{0xf2,0x5d,0,64},{0,0x5f,0,32},{0x66,0x5f,0,64},{0xf3,0x5f,0,32},{0xf2,0x5f,0,64},
		{0,0x51,0,32},{0x66,0x51,0,64},{0xf3,0x51,0,32},{0xf2,0x51,0,64},
		{0,0x54,0,0},{0x66,0x54,0,0},{0,0x55,0,0},{0x66,0x55,0,0},{0,0x56,0,0},{0x66,0x56,0,0},{0,0x57,0,0},{0x66,0x57,0,0},
		{0,0x14,0,0},{0x66,0x14,0,0},{0,0x15,0,0},{0x66,0x15,0,0},
		{0,0x5a,0,32},{0x66,0x5a,0,32},{0xf3,0x5a,0,64},{0xf2,0x5a,0,32},{0,0x5b,0,32},{0x66,0x5b,0,32},{0xf3,0x5b,0,32},{0xf3,0xe6,0,64},
		{0x66,0x60,0,0},{0x66,0x61,0,0},{0x66,0x62,0,0},{0x66,0x63,0,0},{0x66,0x64,0,0},{0x66,0x65,0,0},{0x66,0x66,0,0},{0x66,0x67,0,0},
		{0x66,0x68,0,0},{0x66,0x69,0,0},{0x66,0x6a,0,0},{0x66,0x6b,0,0},{0x66,0x6c,0,0},{0x66,0x6d,0,0},{0x66,0x74,0,0},{0x66,0x75,0,0},{0x66,0x76,0,0},
		{0x66,0xd1,0,0},{0x66,0xd2,0,0},{0x66,0xd3,0,0},{0x66,0xe1,0,0},{0x66,0xe2,0,0},{0x66,0xf1,0,0},{0x66,0xf2,0,0},{0x66,0xf3,0,0},
		{0x66,0xd4,0,0},{0x66,0xd5,0,0},{0x66,0xd8,0,0},{0x66,0xd9,0,0},{0x66,0xda,0,0},{0x66,0xdb,0,0},{0x66,0xdc,0,0},{0x66,0xdd,0,0},{0x66,0xde,0,0},{0x66,0xdf,0,0},
		{0x66,0xe0,0,0},{0x66,0xe3,0,0},{0x66,0xe4,0,0},{0x66,0xe5,0,0},{0x66,0xe8,0,0},{0x66,0xe9,0,0},{0x66,0xea,0,0},{0x66,0xeb,0,0},{0x66,0xec,0,0},{0x66,0xed,0,0},{0x66,0xee,0,0},{0x66,0xef,0,0},
		{0x66,0xf4,0,0},{0x66,0xf5,0,0},{0x66,0xf6,0,0},{0x66,0xf8,0,0},{0x66,0xf9,0,0},{0x66,0xfa,0,0},{0x66,0xfb,0,0},{0x66,0xfc,0,0},{0x66,0xfd,0,0},{0x66,0xfe,0,0},
		{0,0x10,0,0},{0x66,0x10,0,0},{0xf3,0x10,0,0},{0xf2,0x10,0,0},{0,0x28,0,0},{0x66,0x28,0,0},{0x66,0x6f,0,0},{0xf3,0x6f,0,0},
		{0,0x12,0,0},{0x66,0x12,0,0},{0,0x16,0,0},{0x66,0x16,0,0},{0xf2,0x12,0,0},{0xf3,0x12,0,0},{0xf3,0x16,0,0},{0xf3,0x7e,0,0},
		// form 10: Speichern (r/m ist Ziel)
		{0,0x11,10,0},{0x66,0x11,10,0},{0xf3,0x11,10,0},{0xf2,0x11,10,0},{0,0x29,10,0},{0x66,0x29,10,0},{0x66,0x7f,10,0},{0xf3,0x7f,10,0},{0x66,0xd6,10,0},
		{0,0x13,11,0},{0x66,0x13,11,0},{0,0x17,11,0},{0x66,0x17,11,0},{0x66,0xe7,11,0},{0,0x2b,11,0},
		// form 2: imm8
		{0x66,0x70,2,0},{0xf3,0x70,2,0},{0xf2,0x70,2,0},{0,0xc2,2,32},{0x66,0xc2,2,64},{0xf3,0xc2,2,32},{0xf2,0xc2,2,64},{0,0xc6,2,0},{0x66,0xc6,2,0},{0x66,0xc4,9,0},
		// form 5: Gruppe 71/72/73
		{0x66,0x71,5,0},{0x66,0x72,5,0},{0x66,0x73,5,0},
		// Formen mit Allzweckregister
		{0,0x50,1,0},{0x66,0x50,1,0},{0x66,0xd7,1,0},{0x66,0xc5,8,0},{0xf3,0x2a,3,32},{0xf2,0x2a,3,64},{0xf3,0x2c,4,0},{0xf2,0x2c,4,0},{0xf3,0x2d,4,0},{0xf2,0x2d,4,0},
		{0x66,0x6e,6,0},{0x66,0x7e,7,0},
		// comis
		{0,0x2e,12,0},{0x66,0x2e,12,0},{0,0x2f,12,0},{0x66,0x2f,12,0},
		{0x66,0x20,20,0},{0x66,0x21,20,0},{0x66,0x22,20,0},{0x66,0x23,20,0},{0x66,0x24,20,0},{0x66,0x25,20,0},{0x66,0x30,20,0},{0x66,0x31,20,0},{0x66,0x32,20,0},{0x66,0x33,20,0},{0x66,0x34,20,0},{0x66,0x35,20,0},
		{0x66,0x00,20,0},{0x66,0x1c,20,0},{0x66,0x1d,20,0},{0x66,0x1e,20,0},{0x66,0x29,20,0},{0x66,0x37,20,0},{0x66,0x2b,20,0},{0x66,0x38,20,0},{0x66,0x39,20,0},{0x66,0x3a,20,0},{0x66,0x3b,20,0},
		{0x66,0x3c,20,0},{0x66,0x3d,20,0},{0x66,0x3e,20,0},{0x66,0x3f,20,0},{0x66,0x40,20,0},{0x66,0x10,20,0},{0x66,0x14,20,0},{0x66,0x15,20,0},
		{0x66,0x0f,21,0},{0x66,0x0a,21,32},{0x66,0x0b,21,64},{0x66,0x08,21,32},{0x66,0x09,21,64},{0x66,0x0c,21,0},{0x66,0x0d,21,0},{0x66,0x0e,21,0},
		{0x66,0x14,21,0},{0x66,0x15,21,0},{0x66,0x16,21,0},{0x66,0x20,21,0},{0x66,0x22,21,0},
	};
	int n = (int)(sizeof T / sizeof *T), k = rn(n);
	int reg = rn(16), rm = rn(16), mem = rn(3) == 0, op = T[k].op, pfx = T[k].pfx, form = T[k].form, w = 0;
	if (T[k].fp) cur_fp = T[k].fp;
	switch (form) {
	case 0: if ((op == 0x12 || op == 0x16) && pfx == 0x66) mem = 1; put_sse(B, pfx, op, reg, mem, rm, 0, 0, 0); break;
	case 10: put_sse(B, pfx, op, reg, mem, rm, 0, 0, 0); break;
	case 11: put_sse(B, pfx, op, reg, 1, rm, 0, 0, 0); break;
	case 2: { int ib = 1; int64_t im = rn(256); if (op == 0xc2) im = rn(8); put_sse(B, pfx, op, reg, mem, rm, 0, ib, im); break; }
	case 9: put_sse(B, pfx, op, reg, mem, rm, 0, 1, rn(8)); break;   // pinsrw xmm, r32/m16, imm
	case 5: {
		int ext, im = rn(3) ? rn(20) : rn(256);
		if (op == 0x73) { static const int e[4] = {2, 3, 6, 7}; ext = e[rn(4)]; } else { static const int e[3] = {2, 4, 6}; ext = e[rn(3)]; }
		put_sse(B, pfx, op, ext, 0, rm, 0, 1, im); break; }
	case 1: put_sse(B, pfx, op, rn(16), 0, rm, 0, 0, 0); break;   // movmsk / pmovmskb: Ziel Allzweckregister (reg), Quelle xmm (rm)
	case 8: put_sse(B, pfx, op, rn(16), 0, rm, 0, 1, rn(8)); break;
	case 3: w = rn(2); put_sse(B, pfx, op, reg, mem, rm, w, 0, 0); break;    // cvtsi2sd xmm, r/m
	case 4: w = rn(2); put_sse(B, pfx, op, rn(16), mem, rm, w, 0, 0); break; // cvttsd2si r, xmm/m
	case 6: w = rn(2); put_sse(B, pfx, op, reg, mem, rm, w, 0, 0); break;
	case 7: w = rn(2); put_sse(B, pfx, op, reg, mem, rm, w, 0, 0); break;
	case 12: put_sse(B, pfx, op, reg, mem, rm, 0, 0, 0); want_pjcc = 1; B->wf = 1; B->undef = 0; break;
	case 20: { uint8_t o[3] = {0x0f, 0x38, (uint8_t)op}; emit_rm(B, 1, 0, o, 3, reg, mem, rm, 0, 0, 0); break; }   // SSSE3/SSE4.1
	case 21: { uint8_t o[3] = {0x0f, 0x3a, (uint8_t)op}; int im = rn(256); if (op == 0x0a || op == 0x0b || op == 0x08 || op == 0x09) im = rn(4) | (rn(2) ? 8 : 0); if (op == 0x0f) im = rn(40); if (op == 0x16 || op == 0x22 || op == 0x14 || op == 0x20) { w = rn(2) && (op == 0x16 || op == 0x22); if (op == 0x14 || op == 0x20) w = 0; mem = rn(3) == 0; } emit_rm(B, 1, w, o, 3, reg, mem, rm, 0, 1, im); break; }
	}
}
static int gen_seq(Buf *B, uint64_t slot) {
	memset(B, 0, sizeof *B); B->base = slot;
	int n = 1 + rn(4); uint32_t um = 0; int have = 0; cur_undef = 0; cur_fp = 0;
	for (int i = 0; i < n; i++) {
		if (B->n > 60) break;
		if (rn(6) == 0 && i > 0) {  // jcc über einen Befehl
			int cc = pick_cc(); if (cc < 0) { put8(B, 0x90); continue; } put8(B, 0x70 + cc); put8(B, 3); put8(B, 0x83); put8(B, 0xc0 + rn(2) * 0); put8(B, 0x01 * 0 + 1);
			B->undef = 0; B->wf = 0; continue; }
		B->wf = 0; B->undef = 0; want_pjcc = 0; if (sse_mode && rn(4)) gen_sse_one(B); else gen_one(B);
		if (want_pjcc) {   // hinter comis: bedingte Sprünge über einen Befehl, auch jp/jnp
			static const int ccs[] = {2, 3, 4, 5, 6, 7, 10, 11}; for (int q = 0; q < 1; q++) { put8(B, 0x70 + ccs[rn(8)]); put8(B, 3); put8(B, 0x83); put8(B, 0xc0); put8(B, 1); }
		}
		if (B->wf == 1) { um = B->undef; cur_undef = um; have = 1; } else if (B->wf == 2) { /* inc/dec: CF bleibt */ }
	}
	put8(B, 0x0f); put8(B, 0x05);
	return (int)um;
}
// "add eax,1" im jcc-Rumpf verändert Flags auf beiden Seiten gleich

static uc_engine *uc; static uint8_t *code_h, *data_h;
static uint8_t code_init[CODE_N], data_init[DATA_N], data_jit[DATA_N], data_uc[DATA_N];
static uint64_t pool64(void) {
	static const uint64_t p[] = {0, 1, ~0ULL, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x7fffffffULL, 0x80000000ULL, 0xffffffffULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL, 2, 3, 31, 63};
	return rn(3) ? p[rn(19)] : rn(2) ? rnd() : (uint64_t)rn(1000);
}


// zufällige XMM-Werte: Bitmuster, Gleitkommazahlen (float/double) mit Sonderwerten, kleine ganze Zahlen
static void rnd_xmm(uint8_t *o) {
	static const float fp[] = {0.f, -0.f, 1.f, -1.f, 0.5f, 2.f, 3.5f, -7.25f, 100.f, 1e10f, -1e-10f, 3e38f, 1e-40f, 1.f/0, -1.f/0, 16777216.f, 2147483648.f, 4294967296.f, -2147483904.f, 0.1f, 123456.789f};
	static const double dp[] = {0., -0., 1., -1., 0.5, 2., 3.5, -7.25, 100., 1e10, -1e-10, 1e300, 1e-310, 1./0, -1./0, 9007199254740992., 2147483648., 4294967296., 9.3e18, -9.3e18, 0.1, 1e-5};
	int m = rn(8);
	if (m == 0) { for (int i = 0; i < 2; i++) { uint64_t v = rnd(); memcpy(o + 8 * i, &v, 8); } }
	else if (m <= 3) { for (int i = 0; i < 4; i++) { float f = fp[rn(sizeof fp / sizeof *fp)]; if (rn(3) == 0) f = (float)(int)(rnd() % 2000 - 1000); memcpy(o + 4 * i, &f, 4); } }
	else if (m <= 6) { for (int i = 0; i < 2; i++) { double f = dp[rn(sizeof dp / sizeof *dp)]; if (rn(3) == 0) f = (double)(int)(rnd() % 2000 - 1000); memcpy(o + 8 * i, &f, 8); } }
	else { for (int i = 0; i < 16; i++) o[i] = (uint8_t)(rn(2) ? rnd() : (uint8_t[]){0, 1, 0x7f, 0x80, 0xff}[rn(5)]); }
}
// gleich, oder (bei Gleitkomma-Befehlen) beide Seiten NaN: x86 erzeugt negative, arm64 positive Standard-NaNs
static int isnan32(uint32_t v) { return (v & 0x7f800000) == 0x7f800000 && (v & 0x7fffff); }
static int isnan64(uint64_t v) { return (v & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL && (v & 0xfffffffffffffULL); }
static int xmm_same(const uint8_t *a, const uint8_t *b) {
	if (!memcmp(a, b, 16)) return 1;
	if (!cur_fp) return 0;
	uint32_t a4[4], b4[4]; uint64_t a8[2], b8[2]; memcpy(a4, a, 16); memcpy(b4, b, 16); memcpy(a8, a, 16); memcpy(b8, b, 16);
	int ok4 = 1, ok8 = 1;
	for (int i = 0; i < 4; i++) if (a4[i] != b4[i] && !(isnan32(a4[i]) && isnan32(b4[i]))) ok4 = 0;
	for (int i = 0; i < 2; i++) if (a8[i] != b8[i] && !(isnan64(a8[i]) && isnan64(b8[i]))) ok8 = 0;
	return ok4 || ok8;
}

int main(int argc, char **argv) {
	long N = argc > 1 ? atol(argv[1]) : 20000; if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ULL; int verbose = argc > 3 && strcmp(argv[3], "-s"); sse_mode = argc > 3 && !strcmp(argv[3], "-s");
	JitCtx c; if (jit_init(&c)) { fprintf(stderr, "jit_init fehlgeschlagen\n"); return 2; }
	code_h = jit_guest_alloc(CODE, CODE_N); data_h = jit_guest_alloc(DATA, DATA_N);
	if (!code_h || !data_h) { fprintf(stderr, "jit_guest_alloc fehlgeschlagen\n"); return 2; }
	c.pend = (volatile uint32_t *)calloc(1, 4);
	if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc)) return 2;
	{ uint64_t cr = 0; uc_reg_read(uc, UC_X86_REG_CR0, &cr); cr = (cr & ~4ULL) | 2; uc_reg_write(uc, UC_X86_REG_CR0, &cr); uc_reg_read(uc, UC_X86_REG_CR4, &cr); cr |= 0x600; uc_reg_write(uc, UC_X86_REG_CR4, &cr); }   // SSE einschalten wie ein Betriebssystem
	uc_mem_map_ptr(uc, CODE, CODE_N, UC_PROT_ALL, code_h); uc_mem_map_ptr(uc, DATA, DATA_N, UC_PROT_ALL, data_h);
	setvbuf(stdout, NULL, _IOLBF, 0);
	long nanskip = 0, fails = 0, skipped = 0, fbtests = 0, ok = 0, pfdiff = 0;
	for (long it = 0; it < N; it++) {
		int slot = it % NSLOT; if (getenv("JT_TRACE")) printf("it %ld\n", it);
		if (slot == 0 && it) { jit_flush(); uc_ctl_flush_tb(uc); }
		Buf B; int um = gen_seq(&B, slot); uint64_t pc = CODE + (uint64_t)slot * SLOT; uint64_t end = pc + B.n - 2;
		memcpy(code_h + slot * SLOT, B.b, B.n);
		if (getenv("JT_TRACE")) { printf("  code:"); for (int q = 0; q < B.n; q++) printf(" %02x", B.b[q]); printf("\n"); }
		// Anfangszustand
		uint64_t r0[16], ef0 = 2 | (rn(2) ? 1 : 0) | (rn(2) ? 4 : 0) | (rn(2) ? 0x10 : 0) | (rn(2) ? 0x40 : 0) | (rn(2) ? 0x80 : 0) | (rn(2) ? 0x800 : 0);
		for (int i = 0; i < 16; i++) r0[i] = pool64();
		for (int i = 0; i < 16; i++) if (B.ptr & (1u << i)) r0[i] = DATA + 0x4000 + (rnd() % 0x100);
		for (int i = 0; i < 16; i++) if (B.idx & (1u << i)) r0[i] = rn(8);
		r0[4] = DATA + 0x8000 + (rn(4) * 8); if (B.ptr & (1u << 4)) r0[4] = DATA + 0x8000;
		if (B.ptr & (1u << 6)) r0[6] = DATA + 0x2000 + rn(64);
		if (B.ptr & (1u << 7)) r0[7] = DATA + 0x3000 + rn(64);
		if (B.idx & 2) r0[1] = rn(20);
		for (size_t i = 0; i < DATA_N; i += 8) { uint64_t v = rn(5) ? rnd() : 0; if (i >= 0x3f00 && i < 0x4200 && rn(2)) v = 0; memcpy(data_h + i, &v, 8); }
		memcpy(data_init, data_h, DATA_N); memcpy(code_init, code_h, CODE_N);
		uint8_t x0[16][16], xu[16][16]; for (int i = 0; i < 16; i++) rnd_xmm(x0[i]);
		for (int i = 0; i < 16; i++) uc_reg_write(uc, UC_X86_REG_XMM0 + i, x0[i]);
		// 1. Unicorn allein
		for (int i = 0; i < 16; i++) uc_reg_write(uc, jit_ucreg[i], &r0[i]);
		uc_reg_write(uc, UC_X86_REG_EFLAGS, &ef0);
		uc_err e;
		int nangen = 0;
		if (cur_fp) {   // Gleitkomma-Folge: Schritt für Schritt, ob x86 eine Standard-NaN (negativ) erzeugt, die arm64 anders bildet (Vorzeichen)
			uint64_t cur = pc; e = UC_ERR_OK;
			for (int step = 0; step < 24 && cur != end && !e; step++) {
				e = uc_emu_start(uc, cur, end, 0, 1); uc_reg_read(uc, UC_X86_REG_RIP, &cur);
				for (int i = 0; i < 16; i++) { uint8_t xx[16]; uc_reg_read(uc, UC_X86_REG_XMM0 + i, xx); uint32_t q[4]; uint64_t o[2]; memcpy(q, xx, 16); memcpy(o, xx, 16);
					for (int j = 0; j < 4; j++) if (q[j] == 0xffc00000u) nangen = 1; for (int j = 0; j < 2; j++) if (o[j] == 0xfff8000000000000ULL) nangen = 1; }
			}
		} else e = uc_emu_start(uc, pc, end, 0, 0);
		if (e) { skipped++; if (verbose) printf("skip %ld: %s\n", it, uc_strerror(e)); continue; }
		{ uint64_t rp; uc_reg_read(uc, UC_X86_REG_RIP, &rp); if (rp != end) { skipped++; continue; } }
		if (memcmp(code_h, code_init, CODE_N)) { skipped++; memcpy(code_h, code_init, CODE_N); continue; }  // Befehl hat den eigenen Code überschrieben (SMC wird nicht verglichen)
		uint64_t ru[16], efu; for (int i = 0; i < 16; i++) uc_reg_read(uc, jit_ucreg[i], &ru[i]);
		for (int i = 0; i < 16; i++) uc_reg_read(uc, UC_X86_REG_XMM0 + i, xu[i]);
		uc_reg_read(uc, UC_X86_REG_EFLAGS, &efu); memcpy(data_uc, data_h, DATA_N);
		if (getenv("JT_ONLY") && atol(getenv("JT_ONLY")) == it) setenv("UCRUN_JIT_TRACE", "2", 1);
		// 2. JIT (mit Rückfall)
		memcpy(data_h, data_init, DATA_N);
		memcpy(c.r, r0, sizeof r0); memcpy(c.xmm, x0, sizeof x0); c.rip = pc; jit_set_eflags(&c, ef0); int fb = 0, bad = 0;
		for (int guard = 0; guard < 200; guard++) {
			int st = jit_run(&c);
			if (st == JIT_SYSCALL) break;
			if (st == JIT_FALLBACK || st == JIT_FAULT) {
				fb++; uc_err e2 = jit_uc_fallback(uc, &c); if (e2) { bad = 2; break; }
				if (c.rip == end) { st = JIT_SYSCALL; break; } continue;
			}
			bad = 1; break;
		}
		if (bad == 2) { skipped++; continue; }
		if (bad) { fails++; printf("FEHLER %ld: unerwartetes Ereignis %d\n", it, (int)c.status); continue; }
		if (fb) fbtests++;
		memcpy(data_jit, data_h, DATA_N);
		uint64_t efj = jit_eflags_of(&c); uint64_t mask = ALLF & ~(uint64_t)um;
		int same = 1; char why[200] = "";
		for (int i = 0; i < 16; i++) if (c.r[i] != ru[i]) { same = 0; snprintf(why + strlen(why), 100, " r%d %llx!=%llx", i, (unsigned long long)c.r[i], (unsigned long long)ru[i]); }
		if ((efj & mask) != (efu & mask)) { same = 0; snprintf(why + strlen(why), 80, " fl %llx!=%llx(mask %llx)", (unsigned long long)(efj & ALLF), (unsigned long long)(efu & ALLF), (unsigned long long)mask); }
		for (int i = 0; i < 16; i++) if (!xmm_same(c.xmm[i], xu[i])) { same = 0; snprintf(why + strlen(why), 80, " xmm%d %016llx%016llx!=%016llx%016llx", i, (unsigned long long)((uint64_t *)c.xmm[i])[1], (unsigned long long)((uint64_t *)c.xmm[i])[0], (unsigned long long)((uint64_t *)xu[i])[1], (unsigned long long)((uint64_t *)xu[i])[0]); }
		if (c.rip != end + 2 && c.rip != end) { same = 0; snprintf(why + strlen(why), 60, " rip %llx", (unsigned long long)c.rip); }
		if (memcmp(data_jit, data_uc, DATA_N)) { same = 0; size_t k = 0; while (data_jit[k] == data_uc[k]) k++; snprintf(why + strlen(why), 60, " mem@%zx", k); }
		if (!same && nangen) { same = 1; nanskip++; }
		if (!same) {
			fails++;
			if (fails <= 25) { printf("FEHLER %ld:", it); for (int i = 0; i < B.n - 2; i++) printf(" %02x", B.b[i]); printf("  |%s\n  init:", why); for (int i = 0; i < 16; i++) printf(" %llx", (unsigned long long)r0[i]); printf(" ef=%llx\n", (unsigned long long)ef0); }
		} else ok++;
		(void)pfdiff;
	}
	uint64_t b, i, f; jit_stats(&b, &i, &f);
	printf("jtest: %ld Tests, %ld gleich (davon %ld mit NaN-Vorzeichen-Abweichung), %ld Fehler, %ld übersprungen (Unicorn-Ausnahme), %ld mit Rückfall; JIT: %llu Blöcke, %llu Befehle, %llu Rückfälle\n", N, ok, nanskip, fails, skipped, fbtests, (unsigned long long)b, (unsigned long long)i, (unsigned long long)f);
	return fails != 0;
}
