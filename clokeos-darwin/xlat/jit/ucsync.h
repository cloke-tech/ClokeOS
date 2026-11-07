// ucsync.h – Registerabgleich zwischen JitCtx und einer Unicorn-Maschine (für ucrun und den Differenztest)
#pragma once
#include <unicorn/unicorn.h>
#include "jit.h"

static const int jit_ucreg[16] = {UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_RBX, UC_X86_REG_RSP, UC_X86_REG_RBP, UC_X86_REG_RSI, UC_X86_REG_RDI,
	UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15};

// Gastzustand (Allzweckregister, Flags, XMM, MXCSR) in die Maschine schreiben
static void jit_to_uc(uc_engine *u, JitCtx *c) {
	int regs[37]; void *vals[37]; uint64_t ef = jit_eflags_of(c);
	for (int i = 0; i < 16; i++) { regs[i] = jit_ucreg[i]; vals[i] = &c->r[i]; }
	regs[16] = UC_X86_REG_RIP; vals[16] = &c->rip; regs[17] = UC_X86_REG_EFLAGS; vals[17] = &ef; regs[18] = UC_X86_REG_GS_BASE; vals[18] = &c->gs_base;
	for (int i = 0; i < 16; i++) { regs[19 + i] = UC_X86_REG_XMM0 + i; vals[19 + i] = c->xmm[i]; }
	regs[35] = UC_X86_REG_MXCSR; vals[35] = &c->mxcsr; regs[36] = UC_X86_REG_FS_BASE; vals[36] = &c->fs_base;
	uc_reg_write_batch(u, regs, vals, 37);
}
static void jit_from_uc(uc_engine *u, JitCtx *c) {
	int regs[37]; void *vals[37]; uint64_t ef = 0;
	for (int i = 0; i < 16; i++) { regs[i] = jit_ucreg[i]; vals[i] = &c->r[i]; }
	regs[16] = UC_X86_REG_RIP; vals[16] = &c->rip; regs[17] = UC_X86_REG_EFLAGS; vals[17] = &ef; regs[18] = UC_X86_REG_GS_BASE; vals[18] = &c->gs_base;
	for (int i = 0; i < 16; i++) { regs[19 + i] = UC_X86_REG_XMM0 + i; vals[19 + i] = c->xmm[i]; }
	regs[35] = UC_X86_REG_MXCSR; vals[35] = &c->mxcsr; regs[36] = UC_X86_REG_FS_BASE; vals[36] = &c->fs_base;
	uc_reg_read_batch(u, regs, vals, 37);
	jit_set_eflags(c, ef);
}

// Befehle, die der JIT nicht übersetzt, von Unicorn ausführen lassen: Gastzustand hinein, [rip, fb_end) laufen, Zustand zurück.
// Unicorn setzt die 'until'-Prüfung beim Übersetzen in den Block ein; ein früher für denselben Anfang übersetzter Block (anderes Ende)
// würde darüber hinauslaufen -> beim ersten Mal je Bereich die Unicorn-Blöcke dort verwerfen.
static struct { uint64_t a, b; } jit_seen[4096]; static uint64_t jit_seen_epoch = ~0ULL;
static unsigned long jit_nfb, jit_nrm;
static uc_err jit_uc_fallback(uc_engine *u, JitCtx *c) {
	jit_nfb++; if (getenv("UCRUN_JIT_DBG")) fprintf(stderr, "jit-fb #%lu rip=%llx end=%llx cnt=%llu\n", jit_nfb, (unsigned long long)c->rip, (unsigned long long)c->fb_end, (unsigned long long)c->count);
	if (jit_seen_epoch != jit_flush_epoch) { memset(jit_seen, 0, sizeof jit_seen); jit_seen_epoch = jit_flush_epoch; }
	uint64_t a = c->rip, b = c->fb_end; uc_err e;
	jit_to_uc(u, c);
	// Unicorn kann ohne Fehler zurückkehren, ohne vorangekommen zu sein (volle Übersetzungspuffer-Leerung u. ä.): einmal wiederholen; eine
	// echte Ausnahme (#DE, #UD) wiederholt sich und wird dann gemeldet
	for (int attempt = 0; attempt < 3; attempt++) {
		if (b) {
			unsigned h = (unsigned)((a * 0x9E3779B97F4A7C15ULL ^ b) >> 40) & 4095;
			if (jit_seen[h].a != a || jit_seen[h].b != b || attempt) { jit_nrm++; uc_ctl_remove_cache(u, a, b + 1); jit_seen[h].a = a; jit_seen[h].b = b; }
			e = uc_emu_start(u, a, b, 0, 0);
		} else e = uc_emu_start(u, a, 0, 0, c->count ? c->count : 1);
		if (e) break;
		uint64_t rp; uc_reg_read(u, UC_X86_REG_RIP, &rp); if (rp != a || (b && a == b)) break;
	}
	if (e) { if (getenv("UCRUN_JIT_DBG")) fprintf(stderr, "jit-fb: uc_emu_start -> %d\n", (int)e); return e; }
	jit_from_uc(u, c);
	if (c->rip == a && !(b && a == b)) return UC_ERR_EXCEPTION;  // kein Fortschritt: Unicorn hat eine Ausnahme (z. B. #DE) nur gemeldet
	return UC_ERR_OK;
}
