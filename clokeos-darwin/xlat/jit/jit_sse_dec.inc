// jit_sse_dec.inc – SSE/SSE2-Befehle erkennen (wird in jit.c vor decode() eingebunden). Tabelle (0F-Opcode, Pflichtpräfix) -> Art.
// Die Erzeugung des arm64-Codes steht in jit_sse_gen.inc.
enum { S_NONE = 0, S_MOV128, S_MOVSS, S_MOVSD, S_MOVLP, S_MOVHP, S_MOVDUP, S_FARITH, S_FLOGIC, S_N3, S_PACK, S_MOVD, S_MOVQX, S_PSHUF,
       S_SHIFTI, S_SHIFTX, S_CVT, S_COMIS, S_MOVMSK, S_PMOVMSKB, S_SHUFP, S_PEXTRW, S_PINSRW, S_PMADDWD, S_PMULH, S_PMULUDQ, S_PSADBW, S_CMPF,
       S_MOVHLPS, S_MOVLHPS, S_MXCSR, S_PALIGNR, S_PSHUFB, S_N3U, S_PMOV, S_ROUND, S_PTEST, S_PEXTR, S_PINSR, S_BLEND, S_X87 };
#define SF_IMM 1      // Befehl hat ein imm8
#define SF_ST 2       // das r/m-Feld ist das Ziel
#define SF_GPR 4      // ein Operand ist ein Allzweckregister (reg- oder r/m-Feld je nach Art)
#define SF_REGONLY 8  // r/m muss ein Register sein
#define SF_REV 16     // Operanden vertauscht (pandn)
// fmt für S_FARITH/S_FLOGIC/S_CMPF: 0 ps, 1 pd, 2 ss, 3 sd
enum { CV_I2F, CV_F2I_T, CV_F2I, CV_PS2PD, CV_PD2PS, CV_S2D, CV_D2S, CV_DQ2PS, CV_PS2DQ, CV_PS2DQ_T, CV_DQ2PD };

static int sse_class(int o2, int pfx, int *sop, int *arg, int *fl, int *msz, int *fmt) {
	*sop = S_NONE; *arg = 0; *fl = 0; *msz = 16; *fmt = pfx == 0xf3 ? 2 : pfx == 0xf2 ? 3 : pfx == 0x66 ? 1 : 0;
	int p0 = pfx == 0, p66 = pfx == 0x66, pf3 = pfx == 0xf3, pf2 = pfx == 0xf2;
	switch (o2) {
	case 0x10: case 0x11:
		if (o2 == 0x11) *fl |= SF_ST;
		if (p0 || p66) { *sop = S_MOV128; return 1; }
		if (pf3) { *sop = S_MOVSS; *msz = 4; return 1; }
		*sop = S_MOVSD; *msz = 8; return 1;
	case 0x12: case 0x13:
		if (o2 == 0x13) *fl |= SF_ST;
		if (p0 || p66) { *sop = S_MOVLP; *msz = 8; return 1; }   // reg-Form von 0x12 (movhlps) erledigt der Decoder
		if (o2 == 0x12 && pf2) { *sop = S_MOVDUP; *arg = 0; *msz = 8; return 1; }
		if (o2 == 0x12 && pf3) { *sop = S_MOVDUP; *arg = 1; return 1; }
		return 0;
	case 0x16: case 0x17:
		if (o2 == 0x17) *fl |= SF_ST;
		if (p0 || p66) { *sop = S_MOVHP; *msz = 8; return 1; }   // reg-Form von 0x16 (movlhps) erledigt der Decoder
		if (o2 == 0x16 && pf3) { *sop = S_MOVDUP; *arg = 2; return 1; }
		return 0;
	case 0x14: case 0x15: if (!(p0 || p66)) return 0; *sop = S_N3; *arg = (p66 ? 0x4EC03800 : 0x4E803800) | (o2 == 0x15 ? 0x4000 : 0); return 1;
	case 0x28: case 0x29: if (!(p0 || p66)) return 0; if (o2 == 0x29) *fl |= SF_ST; *sop = S_MOV128; return 1;
	case 0x2b: if (!(p0 || p66)) return 0; *fl |= SF_ST; *sop = S_MOV128; return 1;
	case 0x2a: if (!(pf3 || pf2)) return 0; *sop = S_CVT; *arg = CV_I2F; *fl |= SF_GPR; return 1;
	case 0x2c: case 0x2d: if (!(pf3 || pf2)) return 0; *sop = S_CVT; *arg = o2 == 0x2c ? CV_F2I_T : CV_F2I; *fl |= SF_GPR; *msz = pf3 ? 4 : 8; return 1;
	case 0x2e: case 0x2f: if (!(p0 || p66)) return 0; *sop = S_COMIS; *fmt = p66 ? 3 : 2; *msz = p66 ? 8 : 4; return 1;
	case 0x50: if (!(p0 || p66)) return 0; *sop = S_MOVMSK; *fl |= SF_GPR | SF_REGONLY; return 1;
	case 0x51: case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
		*sop = S_FARITH; *arg = o2 == 0x58 ? 0 : o2 == 0x5c ? 1 : o2 == 0x59 ? 2 : o2 == 0x5e ? 3 : o2 == 0x5d ? 4 : o2 == 0x5f ? 5 : 6;
		*msz = *fmt == 2 ? 4 : *fmt == 3 ? 8 : 16; return 1;
	case 0x54: case 0x55: case 0x56: case 0x57: if (!(p0 || p66)) return 0; *sop = S_FLOGIC; *arg = o2 - 0x54; return 1;
	case 0x5a: *sop = S_CVT; *arg = p0 ? CV_PS2PD : p66 ? CV_PD2PS : pf3 ? CV_S2D : CV_D2S; *msz = p0 ? 8 : pf3 ? 4 : pf2 ? 8 : 16; return 1;
	case 0x5b: *sop = S_CVT; *arg = p0 ? CV_DQ2PS : p66 ? CV_PS2DQ : pf3 ? CV_PS2DQ_T : -1; if (pf2) return 0; return 1;
	case 0xe6: if (!pf3) return 0; *sop = S_CVT; *arg = CV_DQ2PD; *msz = 8; return 1;
	case 0x60: case 0x61: case 0x62: case 0x68: case 0x69: case 0x6a: case 0x6c: case 0x6d:
		if (!p66) return 0; *sop = S_N3;
		*arg = (o2 >= 0x68 && o2 != 0x6c ? 0x4E007800 : 0x4E003800) | ((o2 == 0x6c || o2 == 0x6d) ? 3 : (o2 & 3)) << 22; return 1;
	case 0x63: case 0x67: case 0x6b: if (!p66) return 0; *sop = S_PACK; *arg = o2 == 0x63 ? 0 : o2 == 0x67 ? 1 : 2; return 1;
	case 0x64: case 0x65: case 0x66: if (!p66) return 0; *sop = S_N3; *arg = 0x4E203400 | (o2 - 0x64) << 22; return 1;
	case 0x74: case 0x75: case 0x76: if (!p66) return 0; *sop = S_N3; *arg = 0x6E208C00 | (o2 - 0x74) << 22; return 1;
	case 0x6e: if (!p66) return 0; *sop = S_MOVD; *fl |= SF_GPR; return 1;
	case 0x7e: if (p66) { *sop = S_MOVD; *fl |= SF_GPR | SF_ST; return 1; } if (pf3) { *sop = S_MOVQX; *msz = 8; return 1; } return 0;
	case 0xd6: if (!p66) return 0; *sop = S_MOVQX; *fl |= SF_ST; *msz = 8; return 1;
	case 0x6f: case 0x7f: if (!(p66 || pf3)) return 0; if (o2 == 0x7f) *fl |= SF_ST; *sop = S_MOV128; return 1;
	case 0xe7: if (!p66) return 0; *fl |= SF_ST; *sop = S_MOV128; return 1;
	case 0x70: if (!(p66 || pf3 || pf2)) return 0; *sop = S_PSHUF; *arg = p66 ? 0 : pf3 ? 1 : 2; *fl |= SF_IMM; return 1;
	case 0x71: case 0x72: case 0x73: if (!p66) return 0; *sop = S_SHIFTI; *arg = o2 - 0x71; *fl |= SF_IMM | SF_REGONLY; return 1;
	case 0xc2: *sop = S_CMPF; *fl |= SF_IMM; *msz = *fmt == 2 ? 4 : *fmt == 3 ? 8 : 16; return 1;
	case 0xc4: if (!p66) return 0; *sop = S_PINSRW; *fl |= SF_IMM | SF_GPR; *msz = 2; return 1;
	case 0xc5: if (!p66) return 0; *sop = S_PEXTRW; *fl |= SF_IMM | SF_GPR | SF_REGONLY; return 1;
	case 0xc6: if (!(p0 || p66)) return 0; *sop = S_SHUFP; *fl |= SF_IMM; return 1;
	case 0xd1: case 0xd2: case 0xd3: if (!p66) return 0; *sop = S_SHIFTX; *arg = 0; *fmt = o2 - 0xd0; return 1;       // fmt: 1 H, 2 S, 3 D
	case 0xe1: case 0xe2: if (!p66) return 0; *sop = S_SHIFTX; *arg = 1; *fmt = o2 - 0xe0; return 1;
	case 0xf1: case 0xf2: case 0xf3: if (!p66) return 0; *sop = S_SHIFTX; *arg = 2; *fmt = o2 - 0xf0; return 1;
	case 0xd7: if (!p66) return 0; *sop = S_PMOVMSKB; *fl |= SF_GPR | SF_REGONLY; return 1;
	case 0xf4: if (!p66) return 0; *sop = S_PMULUDQ; return 1;
	case 0xf5: if (!p66) return 0; *sop = S_PMADDWD; return 1;
	case 0xf6: if (!p66) return 0; *sop = S_PSADBW; return 1;
	case 0xe4: case 0xe5: if (!p66) return 0; *sop = S_PMULH; *arg = o2 == 0xe4; return 1;
	case 0xdf: if (!p66) return 0; *sop = S_N3; *arg = 0x4E601C00; *fl |= SF_REV; return 1;
	default: break;
	}
	if (!p66) return 0;
	static const struct { uint8_t op; uint32_t enc; } t[] = {
		{0xd4, 0x4EE08400}, {0xd5, 0x4E609C00}, {0xd8, 0x6E202C00}, {0xd9, 0x6E602C00}, {0xda, 0x6E206C00}, {0xdb, 0x4E201C00}, {0xdc, 0x6E200C00},
		{0xdd, 0x6E600C00}, {0xde, 0x6E206400}, {0xe0, 0x6E201400}, {0xe3, 0x6E601400}, {0xe8, 0x4E202C00}, {0xe9, 0x4E602C00}, {0xea, 0x4E606C00},
		{0xeb, 0x4EA01C00}, {0xec, 0x4E200C00}, {0xed, 0x4E600C00}, {0xee, 0x4E606400}, {0xef, 0x6E201C00}, {0xf8, 0x6E208400}, {0xf9, 0x6E608400},
		{0xfa, 0x6EA08400}, {0xfb, 0x6EE08400}, {0xfc, 0x4E208400}, {0xfd, 0x4E608400}, {0xfe, 0x4EA08400}};
	for (unsigned k = 0; k < sizeof t / sizeof *t; k++) if (t[k].op == o2) { *sop = S_N3; *arg = t[k].enc; return 1; }
	return 0;
}
