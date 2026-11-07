// ucglue.inc – wird in ucrun.c eingebunden (vor run_engine): JIT-Hauptschleife des Hauptthreads. Nur mit UCRUN_JIT=1 und statischem Programm.
//
// Aufteilung: jit.c führt Gastcode aus und liefert Ereignisse; hier werden syscall (do_syscall wie beim Unicorn-Hook), nicht übersetzte
// Befehle (Unicorn, Zustand hin und zurück), Signale (deliver_pending/push_signal) und Gastfehler (fault_signal) bedient.
static JitCtx jctx;
static uint64_t mmap_next, stack_top;  // (unten mit Anfangswerten definiert)

// Entscheidung vor dem Laden: JIT nur bei UCRUN_JIT=1 und Programmen ohne dyld (LC_LOAD_DYLINKER/LC_MAIN)
static void jit_choose(const uint8_t *file, const char *name) {
	const char *e = getenv("UCRUN_JIT"); if (!e || *e != '1') return;
	struct mach_header_64 *mh = (void *)file; struct load_command *lc = (void *)(mh + 1); int dyn = 0;
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (void *)((char *)lc + lc->cmdsize)) if (lc->cmd == LC_LOAD_DYLINKER || lc->cmd == LC_MAIN) dyn = 1;
	if (dyn) { fprintf(stderr, "ucrun: UCRUN_JIT: %s ist dynamisch gelinkt, läuft auf Unicorn (der JIT kann bisher nur statische Programme)\n", name); return; }
	if (jit_init(&jctx) || jit_guest_limit() < (1ULL << 34)) { fprintf(stderr, "ucrun: UCRUN_JIT: JIT nicht verfügbar (Speicherreservierung/MAP_JIT), läuft auf Unicorn\n"); return; }
	jit_on = 1; { uint64_t L = jit_guest_limit(); stack_top = L - 0x2000000; mmap_next = L / 2; }  // alles unter dem Fenster des JIT-Gastspeichers
}
static void jit_start(uint64_t entry, uint64_t sp) {
	if (!jit_on) return;
	jctx.rip = entry; jctx.r[4] = sp; jctx.eflags = 2; jctx.expl = 1; jctx.pend = &sig_pending;
}
static void jit_report(void) {
	if (!jit_on) return;
	uint64_t b, i, f; jit_stats(&b, &i, &f);
	fprintf(stderr, "ucrun: JIT Unicorn-Aufrufe %lu (Bereiche neu %lu)\n", jit_nfb, jit_nrm);
	fprintf(stderr, "ucrun: JIT %llu Blöcke, %llu Befehle übersetzt, %llu Rückfälle auf Unicorn\n", (unsigned long long)b, (unsigned long long)i, (unsigned long long)f);
}

static uc_err jit_run_loop(Th *t) {
	uc_engine *u = t->u; uc_err e = UC_ERR_OK;
	for (;;) {
		gil_lock();
		if (__atomic_load_n(&sig_pending, __ATOMIC_SEQ_CST) & ~(gmask << 1)) {  // wartendes Signal zustellen: Rahmen auf dem Gaststapel (Zustand über Unicorn)
			uint64_t pc; jit_to_uc(u, &jctx);
			if (deliver_pending(t, &pc)) jit_from_uc(u, &jctx);
		}
		int st = jit_run(&jctx);
		switch (st) {
		case JIT_SYSCALL:
			jit_to_uc(u, &jctx); do_syscall(u, 0); jit_from_uc(u, &jctx);
			if (t->resume) { jctx.rip = t->resume_rip; t->resume = 0; }  // sigreturn
			break;
		case JIT_FALLBACK: case JIT_FAULT:
			e = jit_uc_fallback(u, &jctx);
			if (e && getenv("UCRUN_JIT_DBG")) fprintf(stderr, "jit-fb: st=%d rip=%llx end=%llx count=%llu e=%d (%s) after-rip=%llx\n", st, (unsigned long long)jctx.rip, (unsigned long long)jctx.fb_end, (unsigned long long)jctx.count, (int)e, uc_strerror(e), (unsigned long long)jctx.rip);
			if (e) {  // Fehler im Gast: Zustand steht in Unicorn (Anfang des Befehls)
				uint64_t pc; uc_reg_read(u, UC_X86_REG_RIP, &pc);
				int sg = fault_signal(u, e, pc);
				if (sg && gsa[sg].handler > 1 && gsa[sg].tramp && !(gmask & (1u << (sg - 1)))) { push_signal(t, sg, t->fault_addr); jit_from_uc(u, &jctx); e = UC_ERR_OK; }
			}
			break;
		default: break;  // JIT_PENDING: oben zustellen
		}
		gil_unlock();
		if (e) return e;
		sched_yield();
	}
}
