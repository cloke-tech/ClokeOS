/* libclokeos_carbon: die wenigen Carbon-Funktionen aus CoreServices, die Logic Pros OMF.framework
 * bindet und PureDarwins CoreServices-Attrappe nicht hat. macapps/carbon.sh trägt die Bibliothek
 * als LC_REEXPORT_DYLIB in CoreServices und in das Carbon-Framework (eine leere Hülle) ein.
 *
 * Nur das Nötige, mit dem Verhalten der Mac-Fassung, soweit es aus der Beschreibung folgt:
 *   NewPtr / DisposePtr           malloc / free (NewPtr(0) liefert einen gültigen Zeiger)
 *   TickCount                     60-Hz-Ticks seit dem Start
 *   UCConvertCFAbsoluteTimeToSeconds
 *                                 CFAbsoluteTime (Sekunden seit 2001) -> Sekunden seit 1904
 *   PBGetVolumeInfoSync           meldet nsvErr (-35): es gibt keine Volume-Verwaltung des Classic-Mac-OS
 *   InsTime/InsXTime/PrimeTime/RmvTime  Time Manager mit einem Faden je Aufgabe (Logics Sequenzer-Takt) */
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdlib.h>

void *NewPtr(long size) { return malloc(size > 0 ? (size_t)size : 1); }
void DisposePtr(void *p) { free(p); }

uint32_t TickCount(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  uint64_t ns = mach_absolute_time() * tb.numer / tb.denom;
  return (uint32_t)(ns / (1000000000ull / 60));
}

int32_t UCConvertCFAbsoluteTimeToSeconds(double abs, uint32_t *secs) {
  double s = abs + 3061152000.0;   /* kCFAbsoluteTimeIntervalSince1904 */
  if (s < 0 || s > 4294967295.0) return -50;   /* paramErr */
  *secs = (uint32_t)s;
  return 0;
}

int16_t PBGetVolumeInfoSync(void *pb) { (void)pb; return -35; }

/* Text Input Services (HIToolbox): Logic Pro fragt beim Start die aktuelle Tastaturbelegung ab und gibt das Ergebnis ohne Prüfung mit CFRelease frei
 * (bei Apple nie NULL). Hier: ein leeres CFDictionary als Eingabequelle (CoreFoundation wird erst zur Laufzeit gesucht, die Bibliothek bindet sie nicht),
 * Eigenschaften gibt es außer kTISPropertyUnicodeKeyLayoutData (unten) keine (NULL). */
#include <dlfcn.h>
static void *ck_empty_dict(void) {
  typedef void *(*Create)(void *alloc, const void **keys, const void **vals, long n, const void *kcb, const void *vcb);
  Create f = (Create)dlsym(RTLD_DEFAULT, "CFDictionaryCreate");
  return f ? f(0, 0, 0, 0, 0, 0) : 0;
}
void *TISCopyCurrentKeyboardInputSource(void) { return ck_empty_dict(); }
void *TISCopyCurrentKeyboardLayoutInputSource(void) { return ck_empty_dict(); }
void *TISCopyCurrentASCIICapableKeyboardInputSource(void) { return ck_empty_dict(); }
/* kTISPropertyUnicodeKeyLayoutData: ein eigener, von 0 verschiedener Schlüssel (nur die Adresse zählt). Die Eigenschaft ist ein CFData mit
 * einem Platzhalter statt Apples 'uchr'-Tabelle; UCKeyTranslate unten wertet ihn nicht aus, sondern nimmt immer die US-Belegung (ANSI).
 * Logic Pro baut damit beim Start seine Tabelle Tastencode -> Zeichen (ECKeyCodeConverter); mit NULL warf das eine Ausnahme. */
static const char ck_uchr_tag[] = "clokeos-uchr-us";
const void *const kTISPropertyUnicodeKeyLayoutData = ck_uchr_tag;
void *TISGetInputSourceProperty(void *source, void *key) {
  (void)source;
  static void *layout;
  if (key != (void *)kTISPropertyUnicodeKeyLayoutData) return 0;
  if (!layout) {
    typedef void *(*Create)(void *alloc, const void *bytes, long n);
    Create f = (Create)dlsym(RTLD_DEFAULT, "CFDataCreate");
    if (f) layout = f(0, ck_uchr_tag, sizeof ck_uchr_tag);
  }
  return layout;
}
unsigned char LMGetKbdType(void) { return 40; }   /* ANSI */

/* US-Belegung: je Tastencode (0…127) das Zeichen ohne und mit Umschalt; Funktionstasten mit Apples Steuerzeichen (F-Tasten 0x10) */
static const unsigned short ck_us[128][2] = {
  [0] = {'a', 'A'}, [1] = {'s', 'S'}, [2] = {'d', 'D'}, [3] = {'f', 'F'}, [4] = {'h', 'H'}, [5] = {'g', 'G'}, [6] = {'z', 'Z'}, [7] = {'x', 'X'},
  [8] = {'c', 'C'}, [9] = {'v', 'V'}, [10] = {0xa7, 0xb1}, [11] = {'b', 'B'}, [12] = {'q', 'Q'}, [13] = {'w', 'W'}, [14] = {'e', 'E'},
  [15] = {'r', 'R'}, [16] = {'y', 'Y'}, [17] = {'t', 'T'}, [18] = {'1', '!'}, [19] = {'2', '@'}, [20] = {'3', '#'}, [21] = {'4', '$'},
  [22] = {'6', '^'}, [23] = {'5', '%'}, [24] = {'=', '+'}, [25] = {'9', '('}, [26] = {'7', '&'}, [27] = {'-', '_'}, [28] = {'8', '*'},
  [29] = {'0', ')'}, [30] = {']', '}'}, [31] = {'o', 'O'}, [32] = {'u', 'U'}, [33] = {'[', '{'}, [34] = {'i', 'I'}, [35] = {'p', 'P'},
  [36] = {'\r', '\r'}, [37] = {'l', 'L'}, [38] = {'j', 'J'}, [39] = {'\'', '"'}, [40] = {'k', 'K'}, [41] = {';', ':'}, [42] = {'\\', '|'},
  [43] = {',', '<'}, [44] = {'/', '?'}, [45] = {'n', 'N'}, [46] = {'m', 'M'}, [47] = {'.', '>'}, [48] = {'\t', '\t'}, [49] = {' ', ' '},
  [50] = {'`', '~'}, [51] = {8, 8}, [52] = {3, 3}, [53] = {27, 27}, [65] = {'.', '.'}, [67] = {'*', '*'}, [69] = {'+', '+'}, [71] = {27, 27},
  [75] = {'/', '/'}, [76] = {3, 3}, [78] = {'-', '-'}, [81] = {'=', '='}, [82] = {'0', '0'}, [83] = {'1', '1'}, [84] = {'2', '2'},
  [85] = {'3', '3'}, [86] = {'4', '4'}, [87] = {'5', '5'}, [88] = {'6', '6'}, [89] = {'7', '7'}, [91] = {'8', '8'}, [92] = {'9', '9'},
  [64] = {16, 16}, [79] = {16, 16}, [80] = {16, 16}, [90] = {16, 16}, [96] = {16, 16}, [97] = {16, 16}, [98] = {16, 16}, [99] = {16, 16},
  [100] = {16, 16}, [101] = {16, 16}, [103] = {16, 16}, [105] = {16, 16}, [106] = {16, 16}, [107] = {16, 16}, [109] = {16, 16},
  [111] = {16, 16}, [113] = {16, 16}, [114] = {5, 5}, [115] = {1, 1}, [116] = {11, 11}, [117] = {127, 127}, [118] = {16, 16},
  [119] = {4, 4}, [120] = {16, 16}, [121] = {12, 12}, [122] = {16, 16}, [123] = {28, 28}, [124] = {29, 29}, [125] = {31, 31}, [126] = {30, 30},
};
/* modifierKeyState = (Modifier >> 8) & 0xff: 0x02 Umschalt, 0x04 Feststell, 0x10 Control; Wahl (0x08) ändert hier nichts */
int32_t UCKeyTranslate(const void *layout, uint16_t code, uint16_t action, uint32_t mods, uint32_t kbdType, uint32_t options,
                       uint32_t *deadKeyState, unsigned long maxLen, unsigned long *actualLen, uint16_t *out) {
  (void)layout; (void)kbdType; (void)options;
  if (deadKeyState) *deadKeyState = 0;
  if (actualLen) *actualLen = 0;
  if (action == 1) return 0;   /* kUCKeyActionUp: kein Zeichen */
  uint16_t c = code < 128 ? ck_us[code][(mods & 0x02) ? 1 : 0] : 0;
  if ((mods & 0x04) && c >= 'a' && c <= 'z') c -= 32;
  if ((mods & 0x10) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1f;
  if (c && maxLen > 0 && out) { out[0] = c; if (actualLen) *actualLen = 1; }
  return 0;
}

/* Time Manager (CoreServices, Carbon): InsTime/InsXTime/PrimeTime/RmvTime. Logic Pro treibt damit seinen Sequenzer
 * ("SeqTimer", "SeqTimer 2"): ohne laufende Aufgaben ruft es nach PrepareForStart nie MDCA::Start, die Wiedergabe beginnt nicht.
 * TMTask ist wie bei Apple auf 2 Bytes gepackt (tmAddr bei 0xa, tmCount 0x12, tmWakeUp 0x1a, tmReserved 0x22).
 * Jede installierte Aufgabe bekommt einen eigenen Faden; PrimeTime(t, n) feuert einmal nach n ms (n > 0) bzw. -n µs (n < 0),
 * bei InsXTime-Aufgaben ohne Drift relativ zur vorigen Frist. Der Rückruf läuft auf dem Faden der Aufgabe und darf PrimeTime/RmvTime rufen. */
#include <pthread.h>
#include <string.h>
#include <time.h>
#pragma pack(push, 2)
typedef struct TMTask { void *qLink; short qType; void (*tmAddr)(struct TMTask *); long tmCount, tmWakeUp, tmReserved; } TMTask;
#pragma pack(pop)
enum { kTMTaskActive = 0x8000, kTMTaskExtended = 0x4000, TM_MAX = 32 };
typedef struct { TMTask *t; pthread_t th; pthread_cond_t c; uint64_t deadline, last; int armed, quit, extended, alive; } tm_entry;
static pthread_mutex_t tm_lk = PTHREAD_MUTEX_INITIALIZER;
static tm_entry tm_tab[TM_MAX];
static mach_timebase_info_data_t tm_tb;
static uint64_t tm_ns2abs(uint64_t ns) { return ns * tm_tb.denom / tm_tb.numer; }
static uint64_t tm_abs2ns(uint64_t a) { return a * tm_tb.numer / tm_tb.denom; }
static tm_entry *tm_find(TMTask *t) { for (int i = 0; i < TM_MAX; i++) if (tm_tab[i].t == t && !tm_tab[i].quit) return &tm_tab[i]; return NULL; }
static void *tm_thread(void *arg) {
  tm_entry *e = arg;
  pthread_mutex_lock(&tm_lk);
  while (!e->quit) {
    if (!e->armed) { pthread_cond_wait(&e->c, &tm_lk); continue; }
    uint64_t now = mach_absolute_time();
    if (now < e->deadline) {
      uint64_t ns = tm_abs2ns(e->deadline - now);
      struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
      pthread_cond_timedwait_relative_np(&e->c, &tm_lk, &ts);
      continue;
    }
    e->armed = 0; e->last = e->deadline;
    TMTask *t = e->t; t->qType &= ~kTMTaskActive;
    pthread_mutex_unlock(&tm_lk);
    if (t->tmAddr) t->tmAddr(t);
    pthread_mutex_lock(&tm_lk);
  }
  e->alive = 0;   /* erst jetzt darf der Eintrag neu vergeben werden */
  pthread_mutex_unlock(&tm_lk);
  return NULL;
}
static short tm_install(TMTask *t, int extended) {
  if (!t) return -50;   /* paramErr */
  if (!tm_tb.denom) mach_timebase_info(&tm_tb);
  pthread_mutex_lock(&tm_lk);
  tm_entry *e = tm_find(t);
  if (!e) for (int i = 0; i < TM_MAX && !e; i++) if (!tm_tab[i].t && !tm_tab[i].alive) e = &tm_tab[i];
  if (!e) { pthread_mutex_unlock(&tm_lk); return -108; }   /* memFullErr */
  if (!e->t) {
    memset(e, 0, sizeof *e); e->t = t; e->alive = 1; pthread_cond_init(&e->c, NULL);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&e->th, &a, tm_thread, e); pthread_attr_destroy(&a);
  }
  e->extended = extended; e->armed = 0; e->last = 0;
  t->qType = (short)(extended ? kTMTaskExtended : 0); t->tmWakeUp = 0;
  pthread_mutex_unlock(&tm_lk);
  return 0;
}
short InsTime(TMTask *t) { return tm_install(t, 0); }
short InsXTime(TMTask *t) { return tm_install(t, 1); }
short PrimeTime(TMTask *t, long count) {
  pthread_mutex_lock(&tm_lk);
  tm_entry *e = tm_find(t);
  if (!e) { pthread_mutex_unlock(&tm_lk); return -50; }
  uint64_t delay = tm_ns2abs(count >= 0 ? (uint64_t)count * 1000000ull : (uint64_t)(-count) * 1000ull);
  uint64_t now = mach_absolute_time();
  /* erweiterte Aufgabe, schon einmal gelaufen: Frist ab der vorigen Frist (kein Aufsummieren der Verspätung) */
  uint64_t base = (e->extended && e->last && t->tmWakeUp) ? e->last : now;
  e->deadline = base + delay; e->armed = 1;
  t->tmWakeUp = (long)(e->deadline ? e->deadline : 1); t->qType |= kTMTaskActive;
  pthread_cond_signal(&e->c);
  pthread_mutex_unlock(&tm_lk);
  return 0;
}
short RmvTime(TMTask *t) {
  pthread_mutex_lock(&tm_lk);
  tm_entry *e = tm_find(t);
  if (e) {
    long left = 0;   /* Restzeit in tmCount: negativ in µs */
    if (e->armed) { uint64_t now = mach_absolute_time(); if (e->deadline > now) left = -(long)(tm_abs2ns(e->deadline - now) / 1000); }
    t->tmCount = left; t->qType &= ~kTMTaskActive;
    e->armed = 0; e->quit = 1; e->t = NULL;   /* der Faden endet; der Eintrag ist danach wieder frei */
    pthread_cond_signal(&e->c);
  }
  pthread_mutex_unlock(&tm_lk);
  return 0;
}
