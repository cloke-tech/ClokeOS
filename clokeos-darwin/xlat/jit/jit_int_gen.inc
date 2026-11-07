// jit_int_gen.inc – weitere Ganzzahlbefehle (wird in jit.c vor gen_insn eingebunden): cmpxchg, xadd (atomar über LSE-Befehle, wie das lock-Präfix
// verlangt), rol/ror, bsf/bsr, popcnt, bt/bts/btr/btc. Alle nur für 32/64 Bit; der Rest läuft weiter über Unicorn.

// Host-Adresse des Speicheroperanden nach x16 (für Befehle, die [Xn] ohne Index brauchen)
static void host_addr(Insn *I) {
	Addr a = gen_addr(I);
	if (a.rm >= 0) alu_r(ADD_, 16, a.rn, a.rm, 1);
	else if (a.imm >= 0) addsub_i(0, 0, 16, a.rn, a.imm, 1);
	else addsub_i(1, 0, 16, a.rn, -a.imm, 1);
}

static int gen_cmpxchg(Insn *I) {
	int sz = I->sz, is64 = sz == 8, s = I->s.n;
	if (I->d.k == 1) {
		int dr = I->d.n;
		mov_r(T0, dr, is64);
		alu_r(SUBS_, XZR, 0, T0, is64);
		csel_(dr, s, T0, EQ, is64);          // gleich: Ziel = Quelle, sonst bleibt das Ziel (ein 32-Bit-Ziel wird so oder so null-erweitert)
		csel_(0, T0, 0, NE, is64);           // ungleich: Akkumulator = altes Ziel
	} else {
		host_addr(I);
		mov_r(T0, 0, is64);
		E((is64 ? 0xC8E0FC00u : 0x88E0FC00u) | T0 << 16 | 16 << 5 | s);   // casal T0, s, [x16]: T0 = alter Wert
		alu_r(SUBS_, XZR, 0, T0, is64);
		csel_(0, T0, 0, NE, is64);
	}
	fs = FS_SUB; return 1;
}
static int gen_xadd(Insn *I) {
	int sz = I->sz, is64 = sz == 8, s = I->s.n;
	if (I->d.k == 1) {
		int dr = I->d.n;
		mov_r(T0, dr, is64);
		alu_r(ADDS_, T1, dr, s, is64);
		mov_r(s, T0, is64); mov_r(dr, T1, is64);
	} else {
		host_addr(I);
		E((is64 ? 0xF8E00000u : 0xB8E00000u) | s << 16 | 16 << 5 | T0);   // ldaddal s, T0, [x16]: T0 = alter Wert
		alu_r(ADDS_, T1, T0, s, is64);
		mov_r(s, T0, is64);
	}
	fs = FS_ADD; return 1;
}

// rol/ror (alu 0 / 1) um imm oder cl
static int gen_rot(Insn *I) {
	int sz = I->sz, is64 = sz == 8, live = I->live, n = is64 ? 64 : 32, k = (int)I->imm, ror = I->alu == 1; Opd *d = &I->d;
	if (I->cl && live) return 0;
	int dr = d->k == 1 ? d->n : (ld_mem(I, T0, sz, 0), T0), rd = d->k == 1 ? d->n : T2;
	if (I->cl) {
		if (!ror) { alu_r(SUB_, T1, XZR, 1, 0); shv(3, rd, dr, T1, is64); } else shv(3, rd, dr, 1, is64);
		if (d->k == 2) st_mem(I, rd, sz);
		return 1;
	}
	int r = ror ? k : (n - k) % n;    // EXTR Rd, Rn, Rn, #r
	E((is64 ? 0x93C00000u : 0x13800000u) | dr << 16 | r << 10 | dr << 5 | rd);
	if (d->k == 2) st_mem(I, rd, sz);
	if (live) {
		to_add();
		if (!ror) { ubfx(16, rd, 0, 1, is64); ubfx(T3, rd, n - 1, 1, is64); alu_r(EOR_, T3, T3, 16, 0); }                       // CF = Bit 0, OF = MSB xor CF
		else { ubfx(16, rd, n - 1, 1, is64); ubfx(T3, rd, n - 1, 1, is64); ubfx(T1, rd, n - 2, 1, is64); alu_r(EOR_, T3, T3, T1, 0); }   // CF = MSB, OF = MSB xor das nächste Bit
		mrs_nzcv(17); bfi(17, 16, 29, 1, 1); bfi(17, T3, 28, 1, 1); msr_nzcv(17); fs = FS_ADD;
	}
	return 1;
}

// bsf / bsr (arg: 0 bsf, 1 bsr): Z gesetzt, wenn die Quelle null ist (dann bleibt das Ziel)
static int gen_bs(Insn *I) {
	int sz = I->sz, is64 = sz == 8, sr;
	if (I->s.k == 1) sr = I->s.n; else { ld_mem(I, T1, sz, 0); sr = T1; }
	alu_r(ANDS_, XZR, sr, sr, is64);
	if (I->alu == 0) { E((is64 ? 0xDAC00000u : 0x5AC00000u) | sr << 5 | T0); E((is64 ? 0xDAC01000u : 0x5AC01000u) | T0 << 5 | T0); }   // rbit; clz
	else { E((is64 ? 0xDAC01000u : 0x5AC01000u) | sr << 5 | T0); movimm(16, (is64 ? 64 : 32) - 1); alu_r(SUB_, T0, 16, T0, is64); }
	csel_(I->d.n, I->d.n, T0, EQ, is64);
	fs = FS_LOGIC; return 1;
}
static int gen_popcnt(Insn *I) {
	int sz = I->sz, is64 = sz == 8, sr;
	if (I->s.k == 1) sr = I->s.n; else { ld_mem(I, T1, sz, 0); sr = T1; }
	if (!is64) { mov_r(T2, sr, 0); sr = T2; }
	E(0x9E670000 | sr << 5 | 17); E(0x0E205800 | 17 << 5 | 17); E(0x0E31B800 | 17 << 5 | 17); E(0x0E013C00 | 17 << 5 | I->d.n);
	if (I->live) { alu_r(ANDS_, XZR, sr, sr, is64); mrs_nzcv(17); logic_imm(AND_, 17, 17, 0x60000000, 1); msr_nzcv(17); fs = FS_ADD; }
	return 1;
}
// bt/bts/btr/btc: alu 0..3; Bitnummer: imm (s.k == 3) oder Register
static int gen_bt(Insn *I) {
	int sz = I->sz, is64 = sz == 8, n = is64 ? 64 : 32, op = I->alu; Opd *d = &I->d;
	int dr = d->k == 1 ? d->n : (ld_mem(I, T0, sz, 0), T0);
	int idx;
	if (I->s.k == 3) { movimm(T1, I->imm & (n - 1)); idx = T1; } else { logic_imm(AND_, T1, I->s.n, n - 1, 0); idx = T1; }
	shv(1, T3, dr, idx, is64); logic_imm(AND_, T3, T3, 1, is64);   // CF
	if (op) {
		movimm(T2, 1); shv(0, T2, T2, idx, is64);
		int nv = d->k == 1 ? d->n : T0;
		if (op == 1) alu_r(ORR_, nv, dr, T2, is64); else if (op == 2) E((is64 ? 0x8A200000u : 0x0A200000u) | T2 << 16 | dr << 5 | nv); else alu_r(EOR_, nv, dr, T2, is64);
		if (d->k == 2) st_mem(I, nv, sz);
	}
	to_add(); mrs_nzcv(17); bfi(17, T3, 29, 1, 1); msr_nzcv(17); fs = FS_ADD;
	return 1;
}
