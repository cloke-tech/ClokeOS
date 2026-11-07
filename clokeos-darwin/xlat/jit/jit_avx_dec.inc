// jit_avx.inc – AVX/AVX2 (wird in jit.c nach jit_sse_gen.inc eingebunden). VEX-Befehle werden auf die SSE-Erzeugung zurückgeführt: ein Befehl auf
// 128 Bit ist "Ziel = vvvv, dann der gewöhnliche SSE-Befehl" (obere YMM-Hälfte auf null), auf 256 Bit läuft derselbe SSE-Befehl ein zweites Mal auf den
// oberen Hälften (diese liegen in JitCtx.ymmh, nicht in Vektorregistern). Nur mit Umgebung RAVEN_JIT_AVX=1 / jit_set_avx(1); sonst bleiben VEX-Befehle
// ein Fall für Unicorn (das als Nehalem keine kennt: #UD, passend zu dem, was cpuid meldet).
// Eigene Befehle: vzeroupper/all, vbroadcast*, vpbroadcast*, vinsert/vextract(f|i)128, vperm2(f|i)128, vpermq/pd/d/ps, vpblendd, vpmovmskb/vmovmskps ymm,
// vpmovzx/sx ymm, vcvtps2pd/vcvtdq2pd ymm, vcvtpd2ps xmm,ymm, vpsllvd/vpsrlvd/vpsravd.
// Nicht: FMA (wird nicht gemeldet), BMI, gather, vmaskmov, vtest*, vpermilp*, vcvtpd2dq ymm.

static int jit_avx;
void jit_set_avx(int on) { jit_avx = on; }

static void ldq_ctx(int vt, int off) { E(0x3DC00000 | (off / 16) << 10 | 19 << 5 | vt); }
static void stq_ctx(int vt, int off) { E(0x3D800000 | (off / 16) << 10 | 19 << 5 | vt); }
#define YH(i) (C_YMMH + 16 * (i))

// cpuid mit eigener Tabelle: Nehalem-Merkmale wie Unicorn (SSE4.2, POPCNT) plus XSAVE/OSXSAVE/AVX/AVX2; kein FMA/BMI/F16C/PCLMUL/AES
static void jit_cpuid(uint64_t *r) {
	uint32_t leaf = (uint32_t)r[0], sub = (uint32_t)r[1], a = 0, b = 0, c = 0, d = 0;
	switch (leaf) {
	case 0: a = 0xd; b = 0x756e6547; d = 0x49656e69; c = 0x6c65746e; break;
	case 1: a = 0x000306a9; b = 0x00010800;
		c = 1u | 1u << 9 | 1u << 13 | 1u << 19 | 1u << 20 | 1u << 23 | 1u << 26 | 1u << 27 | 1u << 28;
		d = 1u | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 8 | 1u << 9 | 1u << 11 | 1u << 12 | 1u << 13 | 1u << 15 | 1u << 16 | 1u << 17 | 1u << 19 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26; break;
	case 7: if (sub == 0) b = 1u << 5; break;
	case 0xd: if (sub == 0) { a = 7; b = 0x340; c = 0x340; } else if (sub == 2) { a = 256; b = 576; } break;
	case 0x80000000: a = 0x80000008; break;
	case 0x80000001: c = 1; d = 1u << 11 | 1u << 20 | 1u << 29; break;
	case 0x80000002: case 0x80000003: case 0x80000004: {
		static const char brand[49] = "Raven x86-64 JIT on arm64                        ";
		const uint32_t *w = (const uint32_t *)(brand + 16 * (leaf - 0x80000002)); a = w[0]; b = w[1]; c = w[2]; d = w[3]; break; }
	case 0x80000008: a = 0x3027; break;
	default: break;
	}
	r[0] = a; r[3] = b; r[1] = c; r[2] = d;   // Gastregisterfeld: rax=0 rcx=1 rdx=2 rbx=3
}

// ------------------------------------------------------------------------------------------------ Erkennung (VEX-Präfix)
enum { S_VZERO = 100, S_VBCAST, S_VINS128, S_VEXT128, S_VPERM2, S_VPERMQ, S_VPERMD, S_VBLENDD, S_VPMOVMSK, S_VCVTW, S_VCVTN, S_VSHIFTV, S_VMOVMSKP };

// Befehle, die es nur mit VEX gibt (oder auf 256 Bit eigene Behandlung brauchen); map: 1 = 0F, 2 = 0F38, 3 = 0F3A
static int avx_class(int map, int o2, int pfx, int W, int L, int *sop, int *arg, int *fl, int *msz) {
	*sop = S_NONE; *arg = 0; *fl = 0; *msz = 16;
	if (pfx != 0x66 && !(map == 1 && (o2 == 0x50 || o2 == 0xd7 || o2 == 0x5a || o2 == 0xe6 || o2 == 0x5b))) return 0;
	if (map == 1) {
		if (o2 == 0x50) { if (pfx != 0 && pfx != 0x66) return 0; *sop = S_VMOVMSKP; *arg = pfx == 0x66; *fl = SF_GPR | SF_REGONLY; return L; }
		if (o2 == 0xd7) { *sop = S_VPMOVMSK; *fl = SF_GPR | SF_REGONLY; return L; }
		if (L && o2 == 0x5a && (pfx == 0 || pfx == 0x66)) { *sop = pfx == 0 ? S_VCVTW : S_VCVTN; *arg = 0; *msz = pfx == 0 ? 16 : 32; return 1; }
		if (L && o2 == 0xe6 && pfx == 0xf3) { *sop = S_VCVTW; *arg = 1; *msz = 16; return 1; }
		return 0;
	}
	if (map == 2) {
		switch (o2) {
		case 0x18: *sop = S_VBCAST; *arg = 0; *msz = 4; return 1;
		case 0x19: if (!L) return 0; *sop = S_VBCAST; *arg = 1; *msz = 8; return 1;
		case 0x1a: case 0x5a: if (!L) return 0; *sop = S_VBCAST; *arg = 2; *msz = 16; return 1;
		case 0x58: *sop = S_VBCAST; *arg = 3; *msz = 4; return 1;      // vpbroadcastd
		case 0x59: *sop = S_VBCAST; *arg = 4; *msz = 8; return 1;      // vpbroadcastq
		case 0x78: *sop = S_VBCAST; *arg = 5; *msz = 1; return 1;      // vpbroadcastb
		case 0x79: *sop = S_VBCAST; *arg = 6; *msz = 2; return 1;      // vpbroadcastw
		case 0x16: case 0x36: if (!L) return 0; *sop = S_VPERMD; *arg = 0; return 1;
		case 0x45: case 0x46: case 0x47: if (W) return 0; *sop = S_VSHIFTV; *arg = o2 - 0x45; return 1;
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
			if (!L) return 0; *sop = S_PMOV; *arg = (o2 >= 0x30 ? o2 - 0x30 : (o2 - 0x20) | 8); return 2;   // 256-Bit-Form: Rückgabe 2, Quelle 128 Bit
		default: return 0;
		}
	}
	switch (o2) {   // map 3
	case 0x00: case 0x01: if (!L || !W) return 0; *sop = S_VPERMQ; *fl = SF_IMM; return 1;
	case 0x02: *sop = S_VBLENDD; *fl = SF_IMM; return 1;
	case 0x06: case 0x46: if (!L) return 0; *sop = S_VPERM2; *fl = SF_IMM; return 1;
	case 0x18: case 0x38: if (!L) return 0; *sop = S_VINS128; *fl = SF_IMM; *msz = 16; return 1;
	case 0x19: case 0x39: if (!L) return 0; *sop = S_VEXT128; *fl = SF_IMM | SF_ST; *msz = 16; return 1;
	default: return 0;
	}
}

