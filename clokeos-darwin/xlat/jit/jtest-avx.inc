// jtest-avx.inc – Prüfung der AVX-Übersetzung (jtest <n> <seed> -a). Unicorn kennt kein AVX, deshalb vergleicht der Test den JIT mit sich selbst: ein VEX-Befehl muss
// dasselbe tun wie der gewöhnliche SSE-Befehl (der gegen Unicorn geprüft ist), je Hälfte angewandt: 128 Bit = "Ziel = vvvv, Befehl, obere Hälfte null", 256 Bit =
// derselbe Befehl auf den unteren und (mit vertauschten Registerhälften und Speicher + 16) den oberen Hälften. Für die Befehle, die es nur mit VEX gibt
// (Broadcast, Einfügen/Herausziehen, Permutationen, Masken), steht die Erwartung als C-Code da.
static uint8_t avx_xmm[16][16], avx_ymmh[16][16];
static int avx_run(JitCtx *c, uint64_t pc, uint64_t *r0, uint8_t (*xm)[16], uint8_t (*yh)[16], uint8_t (*outx)[16], uint8_t (*outy)[16], uint64_t *outr) {
	memcpy(c->r, r0, 16 * 8); memcpy(c->xmm, xm, 256); memcpy(c->ymmh, yh, 256); c->rip = pc; jit_set_eflags(c, 2);
	int st = jit_run(c);
	if (st != JIT_SYSCALL) return -1;
	memcpy(outx, c->xmm, 256); memcpy(outy, c->ymmh, 256); if (outr) memcpy(outr, c->r, 16 * 8); return 0;
}
// ModRM mit Basisregister rbx (kein RIP-relativ), 8-Bit-Verschiebung disp
static void avx_modrm(Buf *B, int reg, int mem, int rm, int disp) {
	if (!mem) { put8(B, 0xc0 | (reg & 7) << 3 | (rm & 7)); return; }
	put8(B, 0x40 | (reg & 7) << 3 | 3); put8(B, disp);
}
static const struct { uint8_t map, pp, op, three, imm, scalar, packed256; } AT[] = {
	{1,0,0x58,1,0,0,1},{1,1,0x58,1,0,0,1},{1,2,0x58,1,0,1,0},{1,3,0x58,1,0,1,0},{1,0,0x5c,1,0,0,1},{1,1,0x5c,1,0,0,1},{1,2,0x5c,1,0,1,0},{1,3,0x5c,1,0,1,0},
	{1,0,0x59,1,0,0,1},{1,1,0x59,1,0,0,1},{1,2,0x59,1,0,1,0},{1,3,0x59,1,0,1,0},{1,0,0x5e,1,0,0,1},{1,1,0x5e,1,0,0,1},{1,2,0x5e,1,0,1,0},{1,3,0x5e,1,0,1,0},
	{1,0,0x5d,1,0,0,1},{1,1,0x5d,1,0,0,1},{1,2,0x5d,1,0,1,0},{1,3,0x5d,1,0,1,0},{1,0,0x5f,1,0,0,1},{1,1,0x5f,1,0,0,1},{1,2,0x5f,1,0,1,0},{1,3,0x5f,1,0,1,0},
	{1,0,0x51,0,0,0,1},{1,1,0x51,0,0,0,1},{1,2,0x51,1,0,1,0},{1,3,0x51,1,0,1,0},
	{1,0,0x54,1,0,0,1},{1,1,0x54,1,0,0,1},{1,0,0x55,1,0,0,1},{1,1,0x56,1,0,0,1},{1,0,0x57,1,0,0,1},{1,0,0x14,1,0,0,1},{1,1,0x15,1,0,0,1},
	{1,0,0x5b,0,0,0,1},{1,1,0x5b,0,0,0,1},{1,2,0x5b,0,0,0,1},{1,2,0x5a,1,0,1,0},{1,3,0x5a,1,0,1,0},
	{1,1,0x60,1,0,0,1},{1,1,0x61,1,0,0,1},{1,1,0x62,1,0,0,1},{1,1,0x63,1,0,0,1},{1,1,0x64,1,0,0,1},{1,1,0x65,1,0,0,1},{1,1,0x66,1,0,0,1},{1,1,0x67,1,0,0,1},
	{1,1,0x68,1,0,0,1},{1,1,0x69,1,0,0,1},{1,1,0x6a,1,0,0,1},{1,1,0x6b,1,0,0,1},{1,1,0x6c,1,0,0,1},{1,1,0x6d,1,0,0,1},{1,1,0x74,1,0,0,1},{1,1,0x75,1,0,0,1},{1,1,0x76,1,0,0,1},
	{1,1,0xd4,1,0,0,1},{1,1,0xd5,1,0,0,1},{1,1,0xd8,1,0,0,1},{1,1,0xda,1,0,0,1},{1,1,0xdb,1,0,0,1},{1,1,0xdf,1,0,0,1},{1,1,0xe0,1,0,0,1},{1,1,0xe4,1,0,0,1},{1,1,0xe5,1,0,0,1},
	{1,1,0xeb,1,0,0,1},{1,1,0xef,1,0,0,1},{1,1,0xf4,1,0,0,1},{1,1,0xf5,1,0,0,1},{1,1,0xf6,1,0,0,1},{1,1,0xf8,1,0,0,1},{1,1,0xfb,1,0,0,1},{1,1,0xfc,1,0,0,1},{1,1,0xfe,1,0,0,1},
	{1,1,0xd1,1,0,0,1},{1,1,0xd2,1,0,0,1},{1,1,0xe2,1,0,0,1},{1,1,0xf3,1,0,0,1},
	{1,1,0x70,0,1,0,1},{1,2,0x70,0,1,0,1},{1,3,0x70,0,1,0,1},{1,0,0xc6,1,1,0,1},{1,1,0xc6,1,1,0,1},{1,0,0xc2,1,1,0,1},{1,1,0xc2,1,1,0,1},{1,2,0xc2,1,1,1,0},{1,3,0xc2,1,1,1,0},
	{2,1,0x00,1,0,0,1},{2,1,0x1c,0,0,0,1},{2,1,0x1e,0,0,0,1},{2,1,0x29,1,0,0,1},{2,1,0x37,1,0,0,1},{2,1,0x2b,1,0,0,1},{2,1,0x38,1,0,0,1},{2,1,0x3b,1,0,0,1},{2,1,0x40,1,0,0,1},
	{2,1,0x20,0,0,0,0},{2,1,0x32,0,0,0,0},{2,1,0x25,0,0,0,0},
	{3,1,0x0f,1,1,0,1},{3,1,0x0c,1,1,0,1},{3,1,0x0d,1,1,0,1},{3,1,0x0e,1,1,0,1},{3,1,0x02,1,1,0,1},{3,1,0x08,0,1,0,1},{3,1,0x09,0,1,0,1},{3,1,0x0a,1,1,1,0},{3,1,0x0b,1,1,1,0},
};
// Befehl mit VEX (vex = 1) oder in der gewöhnlichen Schreibweise; R/X/B/W wie im REX-Byte, reg/rm 3-Bit-Felder
static void avx_bytes(Buf *B, int vex, int map, int pp, int op, int R, int Bb, int W, int vvvv, int L, int reg, int mem, int rm, int disp, int imm_n, int imm) {
	if (vex) { put8(B, 0xc4); put8(B, (~R & 1) << 7 | 1 << 6 | (~Bb & 1) << 5 | map); put8(B, W << 7 | ((~vvvv) & 15) << 3 | L << 2 | pp); }
	else {
		if (pp) put8(B, pp == 1 ? 0x66 : pp == 2 ? 0xf3 : 0xf2);
		int rex = (R ? 4 : 0) | (Bb ? 1 : 0) | (W ? 8 : 0); if (rex) put8(B, 0x40 | rex);
		put8(B, 0x0f); if (map == 2) put8(B, 0x38); else if (map == 3) put8(B, 0x3a);
	}
	put8(B, op); avx_modrm(B, reg, mem, rm, disp); if (imm_n) put8(B, imm);
}
static const uint8_t *zero16(void) { static const uint8_t z[16] = {0}; return z; }
static void y_get(uint8_t (*xm)[16], uint8_t (*yh)[16], int i, uint8_t *o) { memcpy(o, xm[i], 16); memcpy(o + 16, yh[i], 16); }
static int avx_special(long N) {   // Befehle, die es nur mit VEX gibt: Erwartung als C-Code
	JitCtx c; if (jit_init(&c)) return 2; jit_set_avx(1);
	uint8_t *code_h = jit_guest_alloc(CODE, CODE_N), *data_h = jit_guest_alloc(DATA, DATA_N); (void)data_h;
	c.pend = (volatile uint32_t *)calloc(1, 4);
	jit_flush();
	long fails = 0, ok = 0, skipped = 0; uint64_t r0[16];
	static const char *names[] = {"vbroadcastss", "vbroadcastsd", "vbroadcastf128", "vpbroadcastb", "vpbroadcastw", "vpbroadcastd", "vpbroadcastq", "vinsertf128", "vextractf128",
		"vperm2f128", "vpermq", "vpermd", "vpmovmskb", "vmovmskps", "vpsllvd", "vpsrlvd", "vpsravd", "vpmovzxbw", "vpmovsxwd", "vpmovzxbd", "vpmovzxwq", "vcvtps2pd", "vcvtdq2pd", "vcvtpd2ps", "vzeroupper", "vzeroall", "vmovmskpd"};
	for (long it = 0; it < N; it++) {
		int slot = it % NSLOT; if (slot == 0 && it) jit_flush();
		uint8_t xm[16][16], yh[16][16], ox[16][16], oy[16][16]; uint64_t outr[16];
		for (int i = 0; i < 16; i++) { rnd_xmm(xm[i]); rnd_xmm(yh[i]); }
		for (int i = 0; i < 16; i++) r0[i] = pool64(); r0[3] = DATA + 0x4000; r0[4] = DATA + 0x8000;
		for (size_t i = 0; i < DATA_N; i += 8) { uint64_t v = rnd(); memcpy(data_h + i, &v, 8); }
		int k = rn(27), D = rn(14), V = rn(14), RM = rn(14), mem = rn(3) == 0, imm = rn(256), L = 1, W = 0, map = 2, pp = 1, op = 0, three = 0, imm_n = 0, gpr = 0, vzero = 0;
		switch (k) {
		case 0: op = 0x18; break; case 1: op = 0x19; break; case 2: op = 0x1a; mem = 1; break; case 3: op = 0x78; L = rn(2); break; case 4: op = 0x79; L = rn(2); break; case 5: op = 0x58; L = rn(2); break; case 6: op = 0x59; L = rn(2); break;
		case 7: map = 3; op = 0x18; three = 1; imm_n = 1; break; case 8: map = 3; op = 0x19; imm_n = 1; break; case 9: map = 3; op = 0x06; three = 1; imm_n = 1; break;
		case 10: map = 3; op = 0x00; W = 1; imm_n = 1; break; case 11: op = 0x36; three = 1; break;
		case 12: map = 1; op = 0xd7; gpr = 1; break; case 13: map = 1; pp = 0; op = 0x50; gpr = 1; break; case 26: map = 1; pp = 1; op = 0x50; gpr = 1; break;
		case 14: op = 0x47; three = 1; L = rn(2); break; case 15: op = 0x45; three = 1; L = rn(2); break; case 16: op = 0x46; three = 1; L = rn(2); break;
		case 17: op = 0x30; break; case 18: op = 0x23; break; case 19: op = 0x31; break; case 20: op = 0x34; break;
		case 21: map = 1; pp = 0; op = 0x5a; break; case 22: map = 1; pp = 2; op = 0xe6; break; case 23: map = 1; op = 0x5a; break;
		case 24: map = 1; pp = 0; op = 0x77; L = 0; vzero = 1; break; case 25: map = 1; pp = 0; op = 0x77; L = 1; vzero = 1; break;
		}
		if (k == 18) op = 0x23;
		if (k == 12 || k == 13 || k == 26) { D = rn(3) + 0; mem = 0; }
		if (k == 8) { mem = rn(2); }
		if (k == 7 && !mem && 0) ;
		int disp = 8 * rn(4);
		Buf A; memset(&A, 0, sizeof A); A.base = slot;
		if (vzero) { put8(&A, 0xc5); put8(&A, 0xf8 | (L << 2)); put8(&A, 0x77); }
		else {
			int rmreg = RM, regf = D;
			if (k == 8) { regf = V; rmreg = RM; }           // vextractf128 xmm/m128(rm), ymm(reg), imm
			if (gpr) { mem = 0; regf = D; rmreg = RM; }
			avx_bytes(&A, 1, map, pp, op, regf >> 3, mem ? 0 : rmreg >> 3, W, three ? V : 0, L, regf & 7, mem, rmreg & 7, disp, imm_n, imm);
		}
		put8(&A, 0x0f); put8(&A, 0x05);
		memcpy(code_h + slot * SLOT, A.b, A.n);
		uint64_t pc = CODE + (uint64_t)slot * SLOT;
		if (avx_run(&c, pc, r0, xm, yh, ox, oy, outr) < 0) { skipped++; if (getenv("JT_DBG")) printf("skip %s mem%d L%d D%d RM%d status %d\n", names[k], mem, L, D, RM, (int)c.status); continue; }
		uint8_t src[32], s1[32], dst[32], want[32], mem32[48]; memcpy(mem32, data_h + 0x4000 + disp, 48);
		if (!mem) y_get(xm, yh, RM, src); else { memcpy(src, mem32, 32); }
		y_get(xm, yh, V, s1); y_get(ox, oy, D, dst); memset(want, 0, 32);
		int same = 1, lanes = L ? 32 : 16; uint32_t gwant = 0; int check_gpr = 0;
		cur_fp = 0;
		switch (k) {
		case 0: { uint32_t v; memcpy(&v, mem ? mem32 : src, 4); for (int i = 0; i < lanes / 4; i++) memcpy(want + 4 * i, &v, 4); break; }
		case 1: { uint64_t v; memcpy(&v, mem ? mem32 : src, 8); for (int i = 0; i < 4; i++) memcpy(want + 8 * i, &v, 8); break; }
		case 2: memcpy(want, mem32, 16); memcpy(want + 16, mem32, 16); break;
		case 3: for (int i = 0; i < lanes; i++) want[i] = (mem ? mem32 : src)[0]; break;
		case 4: for (int i = 0; i < lanes / 2; i++) memcpy(want + 2 * i, mem ? mem32 : src, 2); break;
		case 5: for (int i = 0; i < lanes / 4; i++) memcpy(want + 4 * i, mem ? mem32 : src, 4); break;
		case 6: for (int i = 0; i < lanes / 8; i++) memcpy(want + 8 * i, mem ? mem32 : src, 8); break;
		case 7: { uint8_t in[16]; if (mem) memcpy(in, mem32, 16); else memcpy(in, src, 16); memcpy(want, s1, 32); memcpy(want + 16 * (imm & 1), in, 16); break; }
		case 8: { uint8_t sy[32]; y_get(xm, yh, V, sy); memcpy(want, sy + 16 * (imm & 1), 16);
			if (mem) { if (memcmp(data_h + 0x4000 + disp, want, 16)) same = 0; check_gpr = 2; } else { memset(want + 16, 0, 16); y_get(ox, oy, RM, dst); } break; }
		case 9: { uint8_t t4[4][16]; memcpy(t4[0], s1, 16); memcpy(t4[1], s1 + 16, 16); if (mem) { memcpy(t4[2], mem32, 16); memcpy(t4[3], mem32 + 16, 16); } else { memcpy(t4[2], src, 16); memcpy(t4[3], src + 16, 16); }
			for (int h = 0; h < 2; h++) { int sel = (imm >> (4 * h)) & 15; if (sel & 8) memset(want + 16 * h, 0, 16); else memcpy(want + 16 * h, t4[sel & 3], 16); } break; }
		case 10: for (int j = 0; j < 4; j++) { int e = (imm >> (2 * j)) & 3; memcpy(want + 8 * j, (mem ? mem32 : src) + 8 * e, 8); } break;
		case 11: { for (int j = 0; j < 8; j++) { uint32_t ix; memcpy(&ix, s1 + 4 * j, 4); ix &= 7; memcpy(want + 4 * j, (mem ? mem32 : src) + 4 * ix, 4); } break; }
		case 12: { uint32_t m = 0; for (int i = 0; i < 32; i++) if (src[i] & 0x80) m |= 1u << i; gwant = m; check_gpr = 1; break; }
		case 13: { uint32_t m = 0; for (int i = 0; i < 8; i++) if (src[4 * i + 3] & 0x80) m |= 1u << i; gwant = m; check_gpr = 1; break; }
		case 26: { uint32_t m = 0; for (int i = 0; i < 4; i++) if (src[8 * i + 7] & 0x80) m |= 1u << i; gwant = m; check_gpr = 1; break; }
		case 14: case 15: case 16: for (int i = 0; i < lanes / 4; i++) { uint32_t a, b, r; memcpy(&a, s1 + 4 * i, 4); memcpy(&b, (mem ? mem32 : src) + 4 * i, 4);
				if (k == 14) r = b > 31 ? 0 : a << b; else if (k == 15) r = b > 31 ? 0 : a >> b; else r = (uint32_t)((int32_t)a >> (b > 31 ? 31 : b)); memcpy(want + 4 * i, &r, 4); } break;
		case 17: for (int i = 0; i < 16; i++) { uint16_t v = (mem ? mem32 : src)[i]; memcpy(want + 2 * i, &v, 2); } break;
		case 18: for (int i = 0; i < 8; i++) { int32_t v; int16_t w; memcpy(&w, (mem ? mem32 : src) + 2 * i, 2); v = w; memcpy(want + 4 * i, &v, 4); } break;
		case 19: for (int i = 0; i < 8; i++) { uint32_t v = (mem ? mem32 : src)[i]; memcpy(want + 4 * i, &v, 4); } break;
		case 20: for (int i = 0; i < 4; i++) { uint64_t v; uint16_t w; memcpy(&w, (mem ? mem32 : src) + 2 * i, 2); v = w; memcpy(want + 8 * i, &v, 8); } break;
		case 21: for (int i = 0; i < 4; i++) { float f; memcpy(&f, (mem ? mem32 : src) + 4 * i, 4); double dd = f; memcpy(want + 8 * i, &dd, 8); } cur_fp = 1; break;
		case 22: for (int i = 0; i < 4; i++) { int32_t v; memcpy(&v, (mem ? mem32 : src) + 4 * i, 4); double dd = v; memcpy(want + 8 * i, &dd, 8); } break;
		case 23: for (int i = 0; i < 4; i++) { double dd; memcpy(&dd, (mem ? mem32 : src) + 8 * i, 8); float f = (float)dd; memcpy(want + 4 * i, &f, 4); } cur_fp = 1; break;
		case 24: break; case 25: break;
		}
		if (k == 24) { for (int i = 0; i < 16; i++) { if (memcmp(oy[i], zero16(), 16)) same = 0; if (memcmp(ox[i], xm[i], 16)) same = 0; } }
		else if (k == 25) { for (int i = 0; i < 16; i++) { if (memcmp(oy[i], zero16(), 16) || memcmp(ox[i], zero16(), 16)) same = 0; } }
		else if (check_gpr == 1) { if ((uint32_t)outr[D] != gwant) same = 0; }
		else if (check_gpr == 2) { }
		else { uint8_t got[32]; int rr = k == 8 ? RM : D; y_get(ox, oy, rr, got); if (k == 23 || k == 8) { memset(want + (k == 23 ? 16 : lanes > 16 ? 32 : 16), 0, 0); }
			int n = (k == 23) ? 16 : (k == 8 ? 32 : lanes); if (k == 23) { memset(want + 16, 0, 16); n = 32; } if (k == 8 && mem) n = 0;
			for (int h = 0; h < n; h += 16) { if (memcmp(got + h, want + h, 16) && !xmm_same(got + h, want + h)) same = 0; }
			if (!L && k != 8 && k != 23) { if (memcmp(got + 16, zero16(), 16)) same = 0; } }
		if (!same) { fails++; if (fails <= 20) { printf("FEHLER %ld: %s D%d V%d RM%d mem%d L%d imm%02x  code:", it, names[k], D, V, RM, mem, L, imm); for (int q = 0; q < A.n - 2; q++) printf(" %02x", A.b[q]); uint8_t got[32]; y_get(ox, oy, D, got); printf("\n   got "); for (int q = 0; q < 32; q++) printf("%02x", got[q]); printf("\n  want "); for (int q = 0; q < 32; q++) printf("%02x", want[q]); printf("\n   src "); for (int q = 0; q < 32; q++) printf("%02x", src[q]); printf("\n"); if (getenv("JT_DBG") && fails == 1) { jit_flush(); jit_set_trace(2); uint8_t ox2[16][16], oy2[16][16]; avx_run(&c, pc, r0, xm, yh, ox2, oy2, 0); jit_set_trace(0); uint8_t g2[32]; y_get(ox2, oy2, D, g2); printf("  got2 "); for (int q = 0; q < 32; q++) printf("%02x", g2[q]); printf("\n  init "); uint8_t g0[32]; y_get(xm, yh, D, g0); for (int q = 0; q < 32; q++) printf("%02x", g0[q]); printf("\n"); } } } else ok++;
		(void)dst;
	}
	printf("jtest -s2 (AVX-Sonderbefehle): %ld Tests, %ld gleich, %ld Fehler, %ld übersprungen\n", N, ok, fails, skipped);
	return fails != 0;
}
static int avx_main(long N) {
	JitCtx c; if (jit_init(&c)) return 2; jit_set_avx(1);
	uint8_t *code_h = jit_guest_alloc(CODE, CODE_N), *data_h = jit_guest_alloc(DATA, DATA_N);
	c.pend = (volatile uint32_t *)calloc(1, 4);
	jit_flush();
	long fails = 0, ok = 0, skipped = 0, n_special = 0;
	uint64_t r0[16];
	for (long it = 0; it < N; it++) {
		int slot = it % NSLOT; if (slot == 0 && it) jit_flush();
		uint8_t xm[16][16], yh[16][16], ox_a[16][16], oy_a[16][16], ox_b[16][16], oy_b[16][16], ox_c[16][16], oy_c[16][16];
		for (int i = 0; i < 16; i++) { rnd_xmm(xm[i]); rnd_xmm(yh[i]); }
		for (size_t i = 0; i < DATA_N; i += 8) { uint64_t v = rnd(); memcpy(data_h + i, &v, 8); }
		uint64_t pc = CODE + (uint64_t)slot * SLOT; for (int i = 0; i < 16; i++) r0[i] = pool64(); r0[3] = DATA + 0x4000; r0[4] = DATA + 0x8000;
		Buf A, Rf; memset(&A, 0, sizeof A); memset(&Rf, 0, sizeof Rf); A.base = Rf.base = slot;
		int t = rn(sizeof AT / sizeof *AT);
		int map = AT[t].map, pp = AT[t].pp, op = AT[t].op, three = AT[t].three, imm_n = AT[t].imm, scalar = AT[t].scalar, L = scalar ? 0 : (AT[t].packed256 ? rn(2) : 0);
		int D = rn(14), V = rn(14), RM = rn(14), mem = rn(3) == 0, imm = rn(256), W = 0;
		if (op == 0xc2) imm = rn(8);
		if (map == 3 && (op == 0x0a || op == 0x0b || op == 0x08 || op == 0x09)) imm = rn(4);
		if (map == 3 && op == 0x0f) imm = rn(40);
		if (map == 3 && op == 0x0c && 0) imm = 0;
		if (!three) V = 0;   // vvvv ungenutzt (1111b -> Register 0 im Feld: wird im Befehl nicht beachtet)
		int disp = 8 * rn(4);
		int shiftx = map == 1 && pp == 1 && (op == 0xd1 || op == 0xd2 || op == 0xd3 || op == 0xe1 || op == 0xe2 || op == 0xf1 || op == 0xf2 || op == 0xf3);
		if (shiftx && !mem && (RM == V || RM == D)) RM = (D + 5) % 14 == V ? (D + 6) % 14 : (D + 5) % 14;
		if (shiftx && !mem && (RM == V || RM == D)) { skipped++; continue; }
		// VEX-Befehl (Ziel D, vvvv = V, Quelle RM/Speicher)
		avx_bytes(&A, 1, map, pp, op, D >> 3, mem ? 0 : RM >> 3, W, three ? V : 0, L, D & 7, mem, RM & 7, disp, imm_n, imm);
		put8(&A, 0x0f); put8(&A, 0x05);
		// Referenz: movaps xmm14, V ; op xmm14, rm ; movaps D, xmm14   (bei zwei Operanden ohne das erste movaps)
		if (three) { put8(&Rf, 0x44 | (V >> 3)); put8(&Rf, 0x0f); put8(&Rf, 0x28); put8(&Rf, 0xc0 | 6 << 3 | (V & 7)); }
		avx_bytes(&Rf, 0, map, pp, op, 1, mem ? 0 : RM >> 3, W, 0, 0, 6, mem, RM & 7, disp, imm_n, imm);
		put8(&Rf, 0x40 | ((D >> 3) << 2) | 1); put8(&Rf, 0x0f); put8(&Rf, 0x28); put8(&Rf, 0xc0 | (D & 7) << 3 | 6);
		put8(&Rf, 0x0f); put8(&Rf, 0x05);
		memcpy(code_h + slot * SLOT, A.b, A.n); memcpy(code_h + (slot + 0) * SLOT + 0x80, Rf.b, Rf.n);
		uint64_t pcr = pc + 0x80;
		if (avx_run(&c, pc, r0, xm, yh, ox_a, oy_a, 0) < 0) { skipped++; continue; }
		// Referenz: untere Hälfte
		uint8_t zero_y[16][16]; memset(zero_y, 0, sizeof zero_y);
		if (avx_run(&c, pcr, r0, xm, yh, ox_b, oy_b, 0) < 0) { skipped++; continue; }
		int same = 1; char why[200] = "";
		for (int i = 0; i < 16; i++) { if (i == 14) continue; if (memcmp(ox_a[i], i == D ? ox_b[D] : xm[i], 16) && !(cur_fp = 1, xmm_same(ox_a[i], i == D ? ox_b[D] : xm[i]))) { same = 0; snprintf(why + strlen(why), 60, " lo xmm%d", i); } }
		// obere Hälfte
		if (L) {
			uint64_t rh[16]; memcpy(rh, r0, sizeof rh); rh[3] += 16;
			uint8_t sx[16][16]; memcpy(sx, yh, 256); if (shiftx && !mem) memcpy(sx[RM], xm[RM], 16);   // Schiebezähler: bei beiden Hälften derselbe (untere) xmm-Operand
			if (shiftx && mem) rh[3] = r0[3];
			int imm2 = imm; if (map == 3 && op == 0x0c) imm2 = imm >> 4; else if (map == 3 && op == 0x0d) imm2 = imm >> 2;
			if (imm2 != imm) { Buf R2; memset(&R2, 0, sizeof R2); R2.base = slot; if (three) { put8(&R2, 0x44 | (V >> 3)); put8(&R2, 0x0f); put8(&R2, 0x28); put8(&R2, 0xc0 | 6 << 3 | (V & 7)); }
				avx_bytes(&R2, 0, map, pp, op, 1, mem ? 0 : RM >> 3, W, 0, 0, 6, mem, RM & 7, disp, imm_n, imm2);
				put8(&R2, 0x40 | ((D >> 3) << 2) | 1); put8(&R2, 0x0f); put8(&R2, 0x28); put8(&R2, 0xc0 | (D & 7) << 3 | 6); put8(&R2, 0x0f); put8(&R2, 0x05);
				memcpy(code_h + slot * SLOT + 0xc0, R2.b, R2.n); }
			if (avx_run(&c, imm2 != imm ? pcr + 0x40 : pcr, rh, sx, xm, ox_c, oy_c, 0) < 0) { skipped++; continue; }   // Registerhälften vertauscht
			for (int i = 0; i < 16; i++) { if (i == 14) continue; const uint8_t *want = i == D ? ox_c[D] : yh[i]; if (memcmp(oy_a[i], want, 16) && !xmm_same(oy_a[i], want)) { same = 0; snprintf(why + strlen(why), 60, " hi ymm%d", i); } }
		} else {
			for (int i = 0; i < 16; i++) { if (i == 14) continue; const uint8_t *want = i == D ? zero_y[0] : yh[i]; if (memcmp(oy_a[i], want, 16)) { same = 0; snprintf(why + strlen(why), 60, " zero-hi ymm%d", i); } }
		}
		if (!same) { fails++; if (fails <= 20) { printf("FEHLER %ld: map%d pp%d op%02x L%d D%d V%d RM%d mem%d imm%02x |%s\n", it, map, pp, op, L, D, V, RM, mem, imm, why); } } else ok++;
	}
	(void)n_special; (void)data_h;
	printf("jtest -a: %ld Tests, %ld gleich, %ld Fehler, %ld übersprungen\n", N, ok, fails, skipped);
	return fails != 0;
}
