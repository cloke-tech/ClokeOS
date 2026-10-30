// jit.h – Block-JIT x86_64 -> arm64 für ucrun (siehe docs/translation-study.md, Abschnitt 16)
//
// Gastregister liegen beim Ablauf in den Host-Registern x0..x15 (gleiche Nummer wie bei x86: rax=0 rcx=1 rdx=2 rbx=3 rsp=4
// rbp=5 rsi=6 rdi=7 r8..r15); in JitCtx stehen sie nur, wenn der JIT-Code verlassen wird.
#pragma once
#include <stdint.h>

#define JIT_MISS 1      // intern: Sprungziel noch nicht übersetzt
#define JIT_SYSCALL 2   // rip = Adresse hinter dem syscall-Befehl
#define JIT_FALLBACK 3  // rip = erster Befehl, den Unicorn ausführen soll; count = Anzahl Befehle
#define JIT_PENDING 4   // Host-Signal vorgemerkt; rip = Fortsetzung
#define JIT_FAULT 5     // Host-Zugriffsfehler im Gast-Befehl rip; aux = Adresse

typedef struct JitCtx {
	uint64_t r[16];            // 0
	uint64_t rip;              // 128
	uint64_t nzcv;             // 136  x86-Flags CF/ZF/SF/OF in NZCV-Schreibweise (C = CF)
	uint64_t expl;             // 144  1: eflags ist maßgeblich (kommt von Unicorn), 0: nzcv ist maßgeblich
	uint64_t eflags;           // 152
	uint64_t fs_base, gs_base; // 160, 168
	uint64_t next_pc;          // 176
	uint64_t exit_site;        // 184
	uint64_t status;           // 192
	uint64_t aux;              // 200
	uint64_t host_sp;          // 208
	volatile uint32_t *pend;   // 216  Zeiger auf Wort, das bei wartendem Signal != 0 ist
	uint64_t count;            // 224  Befehle für JIT_FALLBACK
	uint64_t delta;            // 232  Host-Adresse = Gast-Adresse + delta
	uint64_t jcache;           // 240
	uint64_t fb_end;           // 248  JIT_FALLBACK/FAULT: Adresse hinter dem letzten Befehl, den Unicorn ausführen soll (uc_emu_start 'until')
} JitCtx;

uint64_t jit_guest_limit(void);                   // Gastadressen müssen darunter liegen (Größe des Fensters, 16 GiB bis 256 GiB; 0 = nicht verfügbar)

int jit_init(JitCtx *c);                          // 0 = ok
uint8_t *jit_guest_alloc(uint64_t g, uint64_t n); // Host-Speicher für Gastbereich [g, g+n), genullt; 0 bei Fehler
void jit_guest_free(uint64_t g, uint64_t n);
void jit_invalidate(uint64_t g, uint64_t n);      // übersetzte Blöcke verwerfen, falls [g, g+n) Code enthielt
int jit_run(JitCtx *c);                           // läuft ab c->rip bis zu einem Ereignis (JIT_SYSCALL/FALLBACK/PENDING/FAULT)
void jit_flush(void);                             // alle übersetzten Blöcke verwerfen (Code im Gast geändert/entladen)
void jit_stats(uint64_t *blocks, uint64_t *translated_insns, uint64_t *fallbacks);
// Hilfen für den Unicorn-Abgleich
uint64_t jit_eflags_of(const JitCtx *c);          // eflags aus nzcv/eflags zusammensetzen
void jit_set_eflags(JitCtx *c, uint64_t ef);      // eflags von Unicorn übernehmen
extern uint64_t jit_flush_epoch;                // zählt jit_flush-Aufrufe
