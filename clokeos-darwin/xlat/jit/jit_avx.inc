// jit_avx.inc – Erzeugung der AVX-Befehle (Erkennung und Tabellen: jit_avx_dec.inc)
// ------------------------------------------------------------------------------------------------ Erzeugung
static void zero_hi(int d) { E(0x6F00E400 | 16); stq_ctx(16, YH(d)); }
static int vex_is3(Insn *I) {
	switch (I->sop) {
	case S_FARITH: case S_FLOGIC: case S_N3: case S_PACK: case S_CMPF: case S_SHUFP: case S_PMADDWD: case S_PMULH: case S_PMULUDQ: case S_PSADBW:
	case S_PALIGNR: case S_PSHUFB: case S_BLEND: case S_PINSR: case S_SHIFTX: case S_MOVHLPS: case S_MOVLHPS: return 1;
	case S_MOVLP: case S_MOVHP: return !(I->fl & SF_ST);
	case S_MOVSS: case S_MOVSD: return !(I->fl & SF_ST) && I->s.k == 1;
	case S_CVT: return I->arg == CV_I2F || I->arg == CV_S2D || I->arg == CV_D2S;
	case S_ROUND: return I->fmt >= 2;
	default: return 0;
	}
}
static int vex_writes_xmm(Insn *I) {
	if (I->fl & SF_ST) return 0;
	switch (I->sop) {
	case S_COMIS: case S_MOVMSK: case S_PMOVMSKB: case S_PEXTRW: case S_PEXTR: case S_VMOVMSKP: case S_VPMOVMSK: return 0;
	case S_CVT: return !(I->arg == CV_F2I || I->arg == CV_F2I_T);
	default: return 1;
	}
}
// zweiter Quelloperand eines 256-Bit-Befehls (Register oder Speicher): untere Hälfte nach lo (oder das Register selbst), obere nach hi; liefert das Register der unteren Hälfte
static int vex_load256(Insn *I, int lo, int hi) {
	if (I->s.k == 1) { ldq_ctx(hi, YH(I->s.n)); return I->s.n; }
	Addr a = gen_addr(I); vldst(1, 16, lo, a);
	I->disp += 16; a = gen_addr(I); vldst(1, 16, hi, a); I->disp -= 16;
	return lo;
}
static void pmovmskb_to(int v, int rd) {
	E(0x4E20A800 | v << 5 | 16); E(0x4E201C00 | 31 << 16 | 16 << 5 | 16); E(0x6E000000 | 16 << 16 | 8 << 11 | 16 << 5 | 17);
	E(0x4E003800 | 17 << 16 | 16 << 5 | 16); E(0x4E71B800 | 16 << 5 | 16); E(0x0E023C00 | 16 << 5 | rd);
}
static void movmskp_to(int v, int rd, int pd) {
	if (!pd) { E(0x6F210400 | v << 5 | 16); E(0x6EA04400 | 30 << 16 | 16 << 5 | 16); E(0x4EB1B800 | 16 << 5 | 16); E(0x1E260000 | 16 << 5 | rd); }
	else { E(0x6F410400 | v << 5 | 17); vumov(16, 17, 0, 3); E(0x4E183C00 | 17 << 5 | 17); alu_rs(ADD_, rd, 16, 17, 1, 0, 1); }
}

static int gen_vex(Insn *I) {
	int sop = I->sop, L = I->vex == 2, d = I->xd, vv = I->vv, imm = (int)(I->imm & 0xff), st = (I->fl & SF_ST) != 0;
	if (nlits > 16) return 0;
	switch (sop) {
	case S_VZERO:
		if (L) for (int i = 0; i < 16; i++) E(0x6F00E400 | i);
		E(0x6F00E400 | 16); for (int i = 0; i < 16; i++) stq_ctx(16, YH(i));
		return 1;
	case S_VBCAST: {
		int a = I->arg, s;
		if (I->s.k == 1) s = I->s.n; else { Addr ad = gen_addr(I); vldst(1, I->ssz, 16, ad); s = 16; }
		if (a == 2) vmov(d, s);
		else vdup(d, s, 0, a == 0 || a == 3 ? 2 : a == 1 || a == 4 ? 3 : a == 5 ? 0 : 1);
		if (L) stq_ctx(d, YH(d)); else zero_hi(d);
		return 1; }
	case S_VINS128: {
		int s; if (I->s.k == 1) s = I->s.n; else { Addr ad = gen_addr(I); vldst(1, 16, 16, ad); s = 16; }
		ldq_ctx(24, YH(vv));
		if (!(imm & 1)) { vmov(17, s); } else { vmov(17, vv); vmov(25, s); vmov(24, 25); }
		vmov(d, 17); stq_ctx(24, YH(d));
		return 1; }
	case S_VEXT128: {
		int val = d; if (imm & 1) { ldq_ctx(16, YH(d)); val = 16; }
		if (I->s.k == 1) { vmov(I->s.n, val); zero_hi(I->s.n); } else { Addr ad = gen_addr(I); vldst(0, 16, val, ad); }
		return 1; }
	case S_VPERM2: {
		ldq_ctx(24, YH(vv)); int s2lo = vex_load256(I, 26, 27);
		for (int h = 0; h < 2; h++) {
			int sel = (imm >> (4 * h)) & 15, dst = h ? 25 : 17;
			if (sel & 8) vzero(dst);
			else { int src = (sel & 3) == 0 ? vv : (sel & 3) == 1 ? 24 : (sel & 3) == 2 ? s2lo : 27; vmov(dst, src); }
		}
		vmov(d, 17); stq_ctx(25, YH(d)); return 1; }
	case S_VPERMQ: {
		int slo = vex_load256(I, 20, 21), hi = I->s.k == 1 ? 21 : 21; (void)hi;
		vmov(24, slo); vmov(25, 21);
		for (int h = 0; h < 2; h++) {
			uint8_t idx[16];
			for (int j = 0; j < 2; j++) { int e = (imm >> (2 * (2 * h + j))) & 3; for (int b = 0; b < 8; b++) idx[8 * j + b] = (uint8_t)(8 * e + b); }
			vlit(17, idx); E(0x4E002000 | 17 << 16 | 24 << 5 | (h ? 23 : 22));
		}
		vmov(d, 22); stq_ctx(23, YH(d)); return 1; }
	case S_VPERMD: {   // d = Tabelle rm (256 Bit) nach Indizes vvvv
		int slo = vex_load256(I, 20, 21); vmov(24, slo); vmov(25, 21);
		uint8_t c1[16], c2[16]; for (int i = 0; i < 16; i++) { c1[i] = 1; c2[i] = (uint8_t)(i & 3); }
		vlit(28, c1); vlit(29, c2);
		for (int h = 0; h < 2; h++) {
			int idx = vv; if (h) { ldq_ctx(20, YH(vv)); idx = 20; }
			E(0x4F0004E0 | 17); E(0x4E201C00 | 17 << 16 | idx << 5 | 18);         // v18 = idx & 7 (je Doppelwort)
			E(0x4F225400 | 18 << 5 | 18);                                          // << 2
			E(0x4EA09C00 | 28 << 16 | 18 << 5 | 18);                               // * 0x01010101 (Byte vervielfachen)
			E(0x4EA08400 | 29 << 16 | 18 << 5 | 18);                               // + 0x03020100 (Byte im Wort)
			E(0x4E002000 | 18 << 16 | 24 << 5 | (h ? 23 : 22));
		}
		vmov(d, 22); stq_ctx(23, YH(d)); return 1; }
	case S_VSHIFTV: {
		int a = I->arg;
		for (int h = 0; h < (L ? 2 : 1); h++) {
			int s1 = vv, s2;
			if (h) { ldq_ctx(24, YH(vv)); s1 = 24; if (I->s.k == 1) { ldq_ctx(26, YH(I->s.n)); s2 = 26; } else { Addr ad = gen_addr(I); I->disp += 16; ad = gen_addr(I); vldst(1, 16, 26, ad); I->disp -= 16; s2 = 26; } }
			else if (I->s.k == 1) s2 = I->s.n; else { Addr ad = gen_addr(I); vldst(1, 16, 16, ad); s2 = 16; }
			E(0x4F010400 | 18); E(0x6EA06C00 | 18 << 16 | s2 << 5 | 19);           // v19 = min(Zähler, 32)
			if (a != 2) E(0x6EA0B800 | 19 << 5 | 19);                              // rechts: negativer Zähler
			int dst = h ? 25 : 17;
			E((a == 1 ? 0x4EA04400u : 0x6EA04400u) | 19 << 16 | s1 << 5 | dst);
		}
		vmov(d, 17); if (L) stq_ctx(25, YH(d)); else zero_hi(d);
		return 1; }
	case S_VPMOVMSK: pmovmskb_to(I->s.n, T0); ldq_ctx(24, YH(I->s.n)); pmovmskb_to(24, T1); alu_rs(ORR_, d, T0, T1, 0, 0, 16); return 1;
	case S_VMOVMSKP: { int pd = I->arg; movmskp_to(I->s.n, T0, pd); ldq_ctx(24, YH(I->s.n)); movmskp_to(24, T1, pd); alu_rs(ORR_, d, T0, T1, 0, 0, pd ? 2 : 4); return 1; }
	case S_VCVTW: {
		int s; if (I->s.k == 1) s = I->s.n; else { Addr ad = gen_addr(I); vldst(1, 16, 16, ad); s = 16; }
		if (I->arg == 0) { E(0x0E617800 | s << 5 | 17); E(0x4E617800 | s << 5 | 18); }                    // fcvtl, fcvtl2
		else { E(0x0F20A400 | s << 5 | 17); E(0x4F20A400 | s << 5 | 18); E(0x4E61D800 | 17 << 5 | 17); E(0x4E61D800 | 18 << 5 | 18); }
		vmov(d, 17); stq_ctx(18, YH(d)); return 1; }
	case S_VCVTN: {
		int slo = vex_load256(I, 20, 21); E(0x0E616800 | slo << 5 | 17); E(0x4E616800 | 21 << 5 | 17); vmov(d, 17); zero_hi(d); return 1; }
	default: break;
	}
	if (sop == S_PMOV && L) {   // vpmovzx/sx ymm, xmm/m128: die zweite Hälfte kommt aus den nächsten Quellbytes
		int s; int k = I->arg & 7; static const int shiftb[6] = {8, 4, 2, 8, 4, 8};
		if (I->s.k == 1) s = I->s.n; else { Addr ad = gen_addr(I); vldst(1, I->ssz > 8 ? 16 : I->ssz * 2 > 16 ? 16 : I->ssz * 2, 16, ad); s = 16; }
		Insn t = *I; t.s.k = 1; t.s.n = s; t.vex = 0;
		E(0x6E000000 | s << 16 | shiftb[k] << 11 | s << 5 | 20);   // ext v20 = (s:s) >> shiftb
		t.xd = 24; t.s.n = 20; if (!gen_sse(&t)) return 0;
		t.xd = d; t.s.n = s; if (!gen_sse(&t)) return 0;
		stq_ctx(24, YH(d)); return 1;
	}
	// ---- allgemeiner Fall: der SSE-Befehl, einmal je Hälfte
	int three = vex_is3(I), wr = vex_writes_xmm(I), dst = sop == S_SHIFTI ? vv : d;
	Insn t = *I; t.vex = 0;
	if (sop == S_SHIFTI) { if (dst != I->s.n) vmov(dst, I->s.n); t.xd = dst; }
	else {
		if (three && vv != d) { if (!st && I->s.k == 1 && I->s.n == d) { vmov(28, d); t.s.n = 28; } vmov(d, vv); }
		t.xd = d;
	}
	if (sop == S_MOVSS || sop == S_MOVSD || sop == S_FARITH || sop == S_CVT) { }   // Skalare: obere Bits kamen schon von vvvv
	if (!gen_sse(&t)) return 0;
	if (!L) { if (wr) zero_hi(dst); return 1; }
	if (st && I->s.k == 1) {   // Register-Form eines Speicherbefehls (vmovaps ymm1, ymm2 mit Opcode 29): Hälfte kopieren
		ldq_ctx(24, YH(d)); stq_ctx(24, YH(I->s.n)); return 1;
	}
	Insn u = *I; u.vex = 0;
	if (sop == S_SHIFTI) ldq_ctx(24, YH(I->s.n)); else if (three) ldq_ctx(24, YH(vv)); else if (st) ldq_ctx(24, YH(d));
	if (!st && sop != S_SHIFTI && sop != S_SHIFTX) { if (I->s.k == 1) { ldq_ctx(26, YH(I->s.n)); u.s.n = 26; } else u.disp += 16; }   // (Schiebezähler: beide Hälften nehmen dieselbe Quelle)
	else if (st && I->s.k == 2) u.disp += 16;
	if (sop == S_BLEND) { if (I->arg == 0) u.imm = imm >> 4; else if (I->arg == 1) u.imm = imm >> 2; }
	u.xd = 24;
	if (!gen_sse(&u)) return 0;
	if (!st) stq_ctx(24, YH(dst));
	return 1;
}
