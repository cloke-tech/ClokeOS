// jit_sse_gen.inc – arm64-Code für SSE/SSE2 (wird in jit.c vor gen_insn eingebunden).
// XMM0..15 liegen in v0..v15, Hilfsregister v16..v19, v30/v31 sind Konstanten (movmskps / pmovmskb). Vergleich mit Unicorn: jtest.
// Unterschiede zu x86, die hier ausgeglichen werden: min/max (x86: bei NaN/gleich die Quelle), Zahl->Ganzzahl bei Überlauf/NaN (x86: 0x80..0),
// Schiebezähler >= Elementbreite, pack/pmovmskb ohne direktes Gegenstück. Nicht nachgebildet: MXCSR-Rundungsmodus (immer "nächste gerade"),
// Ausnahmemasken/Flags, NaN-Bitmuster erzeugter NaNs (x86: negativ, arm64: positiv).

typedef struct { uint32_t *pos; uint8_t b[16]; } Lit;
static Lit lits[32]; static int nlits;

static void vldst(int load, int size, int vt, Addr a) {
	uint32_t b;
	if (a.rm >= 0) {
		b = size == 16 ? (load ? 0x3CE06800u : 0x3CA06800u) : size == 8 ? (load ? 0xFC606800u : 0xFC206800u) : size == 4 ? (load ? 0xBC606800u : 0xBC206800u) :
		    size == 2 ? (load ? 0x7C606800u : 0x7C206800u) : (load ? 0x3C606800u : 0x3C206800u);
		E(b | a.rm << 16 | a.rn << 5 | vt);
	} else {
		b = size == 16 ? (load ? 0x3CC00000u : 0x3C800000u) : size == 8 ? (load ? 0xFC400000u : 0xFC000000u) : size == 4 ? (load ? 0xBC400000u : 0xBC000000u) :
		    size == 2 ? (load ? 0x7C400000u : 0x7C000000u) : (load ? 0x3C400000u : 0x3C000000u);
		E(b | (a.imm & 0x1ff) << 12 | a.rn << 5 | vt);
	}
}
static void vmov(int d, int s) { if (d != s) E(0x4EA01C00 | s << 16 | s << 5 | d); }
static void vins(int d, int dl, int s, int sl, int esz) { E(0x6E000400 | ((dl << (esz + 1)) | (1 << esz)) << 16 | (sl << esz) << 11 | s << 5 | d); }
static void vdup(int d, int s, int lane, int esz) { E(0x4E000400 | ((lane << (esz + 1)) | (1 << esz)) << 16 | s << 5 | d); }
static void vumov(int rd, int vn, int lane, int esz) { E((esz == 3 ? 0x4E003C00u : 0x0E003C00u) | ((lane << (esz + 1)) | (1 << esz)) << 16 | vn << 5 | rd); }
static void vzero(int d) { E(0x6F00E400 | d); }
static void vlit(int vt, const uint8_t *b) { Lit *l = &lits[nlits++]; l->pos = cp; memcpy(l->b, b, 16); E(0x9C000000 | vt); }
static void vtbl1(int d, int s, const uint8_t *idx) { vlit(17, idx); E(0x4E000000 | 17 << 16 | s << 5 | d); }
static void vtbl2(int d, int a, int b, const uint8_t *idx) { vmov(18, a); vmov(19, b); vlit(17, idx); E(0x4E002000 | 17 << 16 | 18 << 5 | d); }

// Quelle: Register oder (msz Bytes) aus dem Speicher nach v16
static int vsrc(Insn *I, int msz) {
	if (I->s.k == 1) return I->s.n;
	Addr a = gen_addr(I); vldst(1, msz, 16, a); return 16;
}

static const uint32_t SV_OP[7][2] = {{0x4E20D400, 0x4E60D400}, {0x4EA0D400, 0x4EE0D400}, {0x6E20DC00, 0x6E60DC00}, {0x6E20FC00, 0x6E60FC00}, {0, 0}, {0, 0}, {0x6EA1F800, 0x6EE1F800}};
static const uint32_t SS_OP[7][2] = {{0x1E202800, 0x1E602800}, {0x1E203800, 0x1E603800}, {0x1E200800, 0x1E600800}, {0x1E201800, 0x1E601800}, {0, 0}, {0, 0}, {0x1E21C000, 0x1E61C000}};

// Gleitkomma -> Ganzzahl in x16 (is64: 64 Bit) mit x86-Verhalten bei Überlauf und NaN (0x80..0); Quelle Skalar sreg, dbl: double
static void cvt_f2i(int sreg, int dbl, int trunc, int is64) {
	uint32_t base = trunc ? (is64 ? 0x9E380000u : 0x1E380000u) : (is64 ? 0x9E200000u : 0x1E200000u);
	E(base | (dbl ? 0x00400000u : 0) | sreg << 5 | 16);
	if (is64) logic_imm(ORR_, 25, XZR, 0x7fffffffffffffffULL, 1); else logic_imm(ORR_, 25, XZR, 0x7fffffffULL, 0);
	alu_r(EOR_, 25, 25, 16, is64);
	E((is64 ? 0xB5000000u : 0x35000000u) | 2 << 5 | 25);                     // cbnz x25, +2
	logic_imm(ORR_, 16, XZR, is64 ? 0x8000000000000000ULL : 0x80000000ULL, is64);
	E((dbl ? 0x5E60E400u : 0x5E20E400u) | sreg << 16 | sreg << 5 | 17);       // fcmeq (nicht NaN: alles Einsen)
	E(0x9E660000 | 17 << 5 | 17);                                              // fmov x17, d17
	E(0xB5000000 | 2 << 5 | 17);                                               // cbnz x17, +2
	logic_imm(ORR_, 16, XZR, is64 ? 0x8000000000000000ULL : 0x80000000ULL, is64);
}

static int gen_sse(Insn *I) {
	int d = I->xd, fmt = I->fmt, arg = I->arg, sop = I->sop, st = (I->fl & SF_ST) != 0, imm = (int)(I->imm & 0xff);
	if (nlits > 20) return 0;
	switch (sop) {
	case S_MOV128:
		if (st) { if (I->s.k == 1) vmov(I->s.n, d); else { Addr a = gen_addr(I); vldst(0, 16, d, a); } }
		else { if (I->s.k == 1) vmov(d, I->s.n); else { Addr a = gen_addr(I); vldst(1, 16, d, a); } }
		return 1;
	case S_MOVSS: case S_MOVSD: {
		int sz = sop == S_MOVSS ? 4 : 8, esz = sop == S_MOVSS ? 2 : 3;
		if (st) { if (I->s.k == 1) vins(I->s.n, 0, d, 0, esz); else { Addr a = gen_addr(I); vldst(0, sz, d, a); } }
		else { if (I->s.k == 1) vins(d, 0, I->s.n, 0, esz); else { Addr a = gen_addr(I); vldst(1, sz, d, a); } }
		return 1; }
	case S_MOVLP:
		if (st) { Addr a = gen_addr(I); vldst(0, 8, d, a); }
		else { Addr a = gen_addr(I); vldst(1, 8, 16, a); vins(d, 0, 16, 0, 3); }
		return 1;
	case S_MOVHP:
		if (st) { vumov(T0, d, 1, 3); Addr a = gen_addr(I); ldst(0, 8, 0, T0, a); }
		else { Addr a = gen_addr(I); vldst(1, 8, 16, a); vins(d, 1, 16, 0, 3); }
		return 1;
	case S_MOVHLPS: vins(d, 0, I->s.n, 1, 3); return 1;
	case S_MOVLHPS: vins(d, 1, I->s.n, 0, 3); return 1;
	case S_MOVDUP: {
		int s = vsrc(I, arg == 0 ? 8 : 16);
		if (arg == 0) vdup(d, s, 0, 3); else E((arg == 1 ? 0x4E802800u : 0x4E806800u) | s << 16 | s << 5 | d);
		return 1; }
	case S_FARITH: {
		int s = vsrc(I, I->ssz), vec = fmt < 2, dbl = fmt & 1;
		if (arg == 4 || arg == 5) {
			uint32_t gt = vec ? (dbl ? 0x6EE0E400u : 0x6EA0E400u) : (dbl ? 0x7EE0E400u : 0x7EA0E400u);
			if (arg == 4) E(gt | d << 16 | s << 5 | 17); else E(gt | s << 16 | d << 5 | 17);   // min: s > d, max: d > s
			E(0x6E601C00 | s << 16 | d << 5 | 17);                                                // bsl: Maske ? d : s
			if (vec) vmov(d, 17); else vins(d, 0, 17, 0, dbl ? 3 : 2);
			return 1;
		}
		if (vec) { if (arg == 6) E(SV_OP[6][dbl] | s << 5 | d); else E(SV_OP[arg][dbl] | s << 16 | d << 5 | d); }
		else { if (arg == 6) E(SS_OP[6][dbl] | s << 5 | 17); else E(SS_OP[arg][dbl] | s << 16 | d << 5 | 17); vins(d, 0, 17, 0, dbl ? 3 : 2); }
		return 1; }
	case S_FLOGIC: {
		int s = vsrc(I, 16);
		static const uint32_t o[4] = {0x4E201C00, 0, 0x4EA01C00, 0x6E201C00};
		if (arg == 1) E(0x4E601C00 | d << 16 | s << 5 | d); else E(o[arg] | s << 16 | d << 5 | d);
		return 1; }
	case S_N3: {
		int s = vsrc(I, 16);
		if (I->fl & SF_REV) E(arg | d << 16 | s << 5 | d); else E(arg | s << 16 | d << 5 | d);
		return 1; }
	case S_PACK: {
		int s = vsrc(I, 16);
		static const uint32_t lo[3] = {0x0E214800, 0x2E212800, 0x0E614800}, hi[3] = {0x4E214800, 0x6E212800, 0x4E614800};
		E(lo[arg] | d << 5 | 17); E(hi[arg] | s << 5 | 17); vmov(d, 17); return 1; }
	case S_MOVD: {
		int w = I->sz == 8;
		if (!st) {
			if (I->s.k == 1) E((w ? 0x9E670000u : 0x1E270000u) | I->s.n << 5 | d);
			else { Addr a = gen_addr(I); vldst(1, w ? 8 : 4, d, a); }
		} else {
			if (I->s.k == 1) E((w ? 0x9E660000u : 0x1E260000u) | d << 5 | I->s.n);
			else { Addr a = gen_addr(I); vldst(0, w ? 8 : 4, d, a); }
		}
		return 1; }
	case S_MOVQX:
		if (!st) { if (I->s.k == 1) E(0x1E604000 | I->s.n << 5 | d); else { Addr a = gen_addr(I); vldst(1, 8, d, a); } }
		else { if (I->s.k == 1) E(0x1E604000 | d << 5 | I->s.n); else { Addr a = gen_addr(I); vldst(0, 8, d, a); } }
		return 1;
	case S_PSHUF: {
		int s = vsrc(I, 16); uint8_t idx[16];
		if (arg == 0) {
			int e0 = imm & 3, e1 = (imm >> 2) & 3, e2 = (imm >> 4) & 3, e3 = (imm >> 6) & 3;
			if (imm == 0xe4) vmov(d, s);
			else if (e0 == e1 && e1 == e2 && e2 == e3) vdup(d, s, e0, 2);
			else if (imm == 0x44) vdup(d, s, 0, 3);
			else if (imm == 0xee) vdup(d, s, 1, 3);
			else if (imm == 0x4e) E(0x6E000000 | s << 16 | 8 << 11 | s << 5 | d);
			else if (imm == 0xb1) E(0x4EA00800 | s << 5 | d);
			else { for (int j = 0; j < 4; j++) for (int b = 0; b < 4; b++) idx[4 * j + b] = (uint8_t)(4 * ((imm >> (2 * j)) & 3) + b); vtbl1(d, s, idx); }
		} else {
			for (int w = 0; w < 8; w++) {
				int e = w; if (arg == 1 && w >= 4) e = 4 + ((imm >> (2 * (w - 4))) & 3); else if (arg == 2 && w < 4) e = (imm >> (2 * w)) & 3;
				idx[2 * w] = (uint8_t)(2 * e); idx[2 * w + 1] = (uint8_t)(2 * e + 1);
			}
			vtbl1(d, s, idx);
		}
		return 1; }
	case S_SHIFTI: {
		int ext = I->cc, esz = 16 << arg, k = imm;     // d = r/m-Register
		if (arg == 2 && (ext == 3 || ext == 7)) {      // psrldq / pslldq (Bytes)
			if (k == 0) return 1;
			if (k > 15) { vzero(d); return 1; }
			vzero(16);
			if (ext == 3) E(0x6E000000 | 16 << 16 | k << 11 | d << 5 | d); else E(0x6E000000 | d << 16 | (16 - k) << 11 | 16 << 5 | d);
			return 1;
		}
		if (k == 0) return 1;
		if (ext == 2) { if (k >= esz) vzero(d); else E(0x6F000400 | (2 * esz - k) << 16 | d << 5 | d); }
		else if (ext == 4) { int kk = k > esz ? esz : k; E(0x4F000400 | (2 * esz - kk) << 16 | d << 5 | d); }
		else { if (k >= esz) vzero(d); else E(0x4F005400 | (esz + k) << 16 | d << 5 | d); }
		return 1; }
	case S_SHIFTX: {
		int s = vsrc(I, 16), z = fmt;                  // fmt: 1 H, 2 S, 3 D
		E(0x7EA14800 | s << 5 | 17);                   // uqxtn s17, d(s): Zähler auf 32 Bit sättigen
		E(0x4F020400 | 18); E(0x6EA06C00 | 18 << 16 | 17 << 5 | 17);   // movi v18.4s,#64; umin
		vdup(17, 17, 0, z);
		if (arg != 2) E(0x6E20B800 | z << 22 | 17 << 5 | 17);          // neg: Rechtsschieben
		E((arg == 1 ? 0x4E204400u : 0x6E204400u) | z << 22 | 17 << 16 | d << 5 | d);
		return 1; }
	case S_CVT: {
		int w = I->sz == 8;
		switch (arg) {
		case CV_I2F: {
			int sr;
			if (I->s.k == 1) sr = I->s.n; else { ld_mem(I, T0, w ? 8 : 4, 0); sr = T0; }
			E(((fmt == 3 ? 0x1E620000u : 0x1E220000u) | (w ? 0x80000000u : 0)) | sr << 5 | 17);
			vins(d, 0, 17, 0, fmt == 3 ? 3 : 2); return 1; }
		case CV_F2I_T: case CV_F2I: {
			int s = vsrc(I, I->ssz);
			cvt_f2i(s, fmt == 3, arg == CV_F2I_T, w); mov_r(d, 16, w); return 1; }
		case CV_PS2PD: { int s = vsrc(I, 8); E(0x0E617800 | s << 5 | d); return 1; }
		case CV_PD2PS: { int s = vsrc(I, 16); E(0x0E616800 | s << 5 | d); return 1; }
		case CV_S2D: { int s = vsrc(I, 4); E(0x1E22C000 | s << 5 | 17); vins(d, 0, 17, 0, 3); return 1; }
		case CV_D2S: { int s = vsrc(I, 8); E(0x1E624000 | s << 5 | 17); vins(d, 0, 17, 0, 2); return 1; }
		case CV_DQ2PS: { int s = vsrc(I, 16); E(0x4E21D800 | s << 5 | d); return 1; }
		case CV_DQ2PD: { int s = vsrc(I, 8); E(0x0F20A400 | s << 5 | 17); E(0x4E61D800 | 17 << 5 | d); return 1; }
		case CV_PS2DQ: case CV_PS2DQ_T: {
			int s = vsrc(I, 16);
			E((arg == CV_PS2DQ ? 0x4E21A800u : 0x4EA1B800u) | s << 5 | 17);
			E(0x4E20E400 | s << 16 | s << 5 | 18);                    // fcmeq: nicht NaN
			E(0x6F046400 | 19);                                       // mvni v19.4s, #0x80, lsl 24 = 0x7fffffff
			E(0x6EA08C00 | 19 << 16 | 17 << 5 | 19);                  // cmeq: Lanes mit 0x7fffffff
			E(0x6E201C00 | 19 << 16 | 17 << 5 | 17);                  // eor -> 0x80000000
			E(0x4F046400 | 19);                                       // movi v19.4s, #0x80, lsl 24
			E(0x6EE01C00 | 18 << 16 | 19 << 5 | 17);                  // bif: NaN-Lanes -> 0x80000000
			vmov(d, 17); return 1; }
		}
		return 0; }
	case S_COMIS: {
		int s = vsrc(I, I->ssz);
		if (!I->live) return 1;
		E((fmt == 3 ? 0x1E602000u : 0x1E202000u) | s << 16 | d << 5);
		fs = FS_FCMP; return 1; }
	case S_MOVMSK: {
		int s = I->s.n, rd = d;                   // reg-Feld = Ziel (Allzweck), r/m = xmm
		if (fmt == 0) {
			E(0x6F210400 | s << 5 | 16); E(0x6EA04400 | 30 << 16 | 16 << 5 | 16); E(0x4EB1B800 | 16 << 5 | 16); E(0x1E260000 | 16 << 5 | rd);
		} else {
			E(0x6F410400 | s << 5 | 17); vumov(16, 17, 0, 3); E(0x4E183C00 | 17 << 5 | 17);
			alu_rs(ADD_, rd, 16, 17, 1, 0, 1);
		}
		return 1; }
	case S_PMOVMSKB: {
		int s = I->s.n;
		E(0x4E20A800 | s << 5 | 16); E(0x4E201C00 | 31 << 16 | 16 << 5 | 16); E(0x6E000000 | 16 << 16 | 8 << 11 | 16 << 5 | 17);
		E(0x4E003800 | 17 << 16 | 16 << 5 | 16); E(0x4E71B800 | 16 << 5 | 16); E(0x0E023C00 | 16 << 5 | d);
		return 1; }
	case S_SHUFP: {
		int s = vsrc(I, 16); uint8_t idx[16];
		if (fmt == 0) {
			for (int j = 0; j < 4; j++) { int e = (imm >> (2 * j)) & 3; for (int b = 0; b < 4; b++) idx[4 * j + b] = (uint8_t)((j >= 2 ? 16 : 0) + 4 * e + b); }
		} else {
			for (int j = 0; j < 2; j++) { int e = (imm >> j) & 1; for (int b = 0; b < 8; b++) idx[8 * j + b] = (uint8_t)((j ? 16 : 0) + 8 * e + b); }
		}
		vtbl2(d, d, s, idx); return 1; }
	case S_PEXTRW: vumov(d, I->s.n, imm & 7, 1); return 1;
	case S_PINSRW: {
		int g; if (I->s.k == 1) g = I->s.n; else { ld_mem(I, T0, 2, 0); g = T0; }
		E(0x4E001C00 | (((imm & 7) << 2) | 2) << 16 | g << 5 | d); return 1; }
	case S_PMADDWD: {
		int s = vsrc(I, 16);
		E(0x0E60C000 | s << 16 | d << 5 | 17); E(0x4E60C000 | s << 16 | d << 5 | 18); E(0x4EA0BC00 | 18 << 16 | 17 << 5 | d); return 1; }
	case S_PMULH: {
		int s = vsrc(I, 16), u = arg;
		E((u ? 0x2E60C000u : 0x0E60C000u) | s << 16 | d << 5 | 17); E((u ? 0x6E60C000u : 0x4E60C000u) | s << 16 | d << 5 | 18); E(0x4E405800 | 18 << 16 | 17 << 5 | d); return 1; }
	case S_PMULUDQ: {
		int s = vsrc(I, 16);
		E(0x4E801800 | d << 16 | d << 5 | 17); E(0x4E801800 | s << 16 | s << 5 | 18); E(0x2EA0C000 | 18 << 16 | 17 << 5 | d); return 1; }
	case S_PSADBW: {
		int s = vsrc(I, 16);
		E(0x6E207400 | s << 16 | d << 5 | 17); E(0x6E202800 | 17 << 5 | 17); E(0x6E602800 | 17 << 5 | 17); E(0x6EA02800 | 17 << 5 | 17); vmov(d, 17); return 1; }
	case S_CMPF: {
		int s = vsrc(I, I->ssz), vec = fmt < 2, dbl = fmt & 1, pr = imm & 7;
		uint32_t eq = vec ? (dbl ? 0x4E60E400u : 0x4E20E400u) : (dbl ? 0x5E60E400u : 0x5E20E400u);
		uint32_t gt = vec ? (dbl ? 0x6EE0E400u : 0x6EA0E400u) : (dbl ? 0x7EE0E400u : 0x7EA0E400u);
		uint32_t ge = vec ? (dbl ? 0x6E60E400u : 0x6E20E400u) : (dbl ? 0x7E60E400u : 0x7E20E400u);
		int inv = 0;
		switch (pr) {
		case 0: case 4: E(eq | s << 16 | d << 5 | 17); inv = pr == 4; break;
		case 1: case 5: E(gt | d << 16 | s << 5 | 17); inv = pr == 5; break;          // a < b  <=>  b > a
		case 2: case 6: E(ge | d << 16 | s << 5 | 17); inv = pr == 6; break;
		default:                                                                        // 3 unordered, 7 ordered
			E(eq | d << 16 | d << 5 | 17); E(eq | s << 16 | s << 5 | 18); E(0x4E201C00 | 18 << 16 | 17 << 5 | 17); inv = pr == 3; break;
		}
		if (inv) E(0x6E205800 | 17 << 5 | 17);
		if (vec) vmov(d, 17); else vins(d, 0, 17, 0, dbl ? 3 : 2);
		return 1; }
	}
	return 0;
}
