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
#define JIT_INT3 6      // int3: rip = Adresse hinter dem Befehl
#define JIT_UD 7        // ud2: rip = Adresse des Befehls
#define JIT_RETRY 8     // intern: Schreibzugriff auf eine Codeseite

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
	uint8_t xmm[16][16];       // 256  XMM0..15 (im JIT-Code in v0..v15)
	uint32_t mxcsr, pad0[3];   // 512
	uint64_t k_w[2];           // 528  Konstante für pmovmskb: Bytegewichte 1,2,4,..,128,1,..,128 (v31)
	uint32_t k_i[4];           // 544  Konstante für movmskps: 0,1,2,3 (v30)
	uint64_t x87_top, pad1;    // 560  x87: Stapelspitze (0..7)
	double st[8];              // 576  x87-Register (physisch, nicht relativ zur Spitze)
	uint16_t fcw, fsw, ftw;    // 640  x87: Steuer-, Statuswort (ohne TOP), Tag-Wort (0 = leer .. 1 = belegt, je Register ein Bit)
	uint16_t pad2;
} __attribute__((aligned(16))) JitCtx;
#define C_XMM 256
#define C_MXCSR 512
#define C_KW 528
#define C_KI 544
#define C_X87TOP 560
#define C_ST 576

uint64_t jit_guest_limit(void);                   // Gastadressen müssen darunter liegen (Größe des Fensters, 16 GiB bis 256 GiB; 0 = nicht verfügbar)

int jit_init(JitCtx *c);                          // 0 = ok
uint8_t *jit_guest_alloc(uint64_t g, uint64_t n); // Host-Speicher für Gastbereich [g, g+n), genullt; 0 bei Fehler
uint64_t jit_guest_delta(void);                   // Host-Adresse = Gastadresse + delta (0 vor der Reservierung)
int jit_guest_map(uint64_t g, uint64_t n, int prot);   // anonym, Gastschutz prot (PROT_*), genullt
int jit_guest_map_fd(uint64_t g, uint64_t n, int prot, int fd, uint64_t off);  // gemeinsames Speicherobjekt; g und off gleich ausgerichtet (16 KiB)
void jit_guest_unmap(uint64_t g, uint64_t n);
int jit_guest_protect(uint64_t g, uint64_t n, int prot);
int jit_guest_prot(uint64_t g);                   // Schutz der Gastseite (255 = nicht abgebildet)
int jit_guest_access(uint64_t g, uint64_t n);     // 0: alle Seiten abgebildet und nicht PROT_NONE
int jit_guest_copy(uint64_t g, void *buf, uint64_t n, int write);  // Host liest/schreibt Gastspeicher (ohne Rücksicht auf den Gastschutz); -1 wenn nicht abgebildet
void jit_guest_free(uint64_t g, uint64_t n);
void jit_invalidate(uint64_t g, uint64_t n);      // übersetzte Blöcke verwerfen, falls [g, g+n) Code enthielt
int jit_run(JitCtx *c);                           // läuft ab c->rip bis zu einem Ereignis (JIT_SYSCALL/FALLBACK/PENDING/FAULT)
void jit_flush(void);                             // alle übersetzten Blöcke verwerfen (Code im Gast geändert/entladen)
void jit_stats(uint64_t *blocks, uint64_t *translated_insns, uint64_t *fallbacks);
// Hilfen für den Unicorn-Abgleich
uint64_t jit_eflags_of(const JitCtx *c);          // eflags aus nzcv/eflags zusammensetzen
void jit_set_eflags(JitCtx *c, uint64_t ef);      // eflags von Unicorn übernehmen
extern uint64_t jit_flush_epoch;                // zählt jit_flush-Aufrufe
