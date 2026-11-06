/* libclokeos_carbon: die wenigen Carbon-Funktionen aus CoreServices, die Logic Pros OMF.framework
 * bindet und PureDarwins CoreServices-Attrappe nicht hat. macapps/carbon.sh trägt die Bibliothek
 * als LC_REEXPORT_DYLIB in CoreServices und in das Carbon-Framework (eine leere Hülle) ein.
 *
 * Nur das Nötige, mit dem Verhalten der Mac-Fassung, soweit es aus der Beschreibung folgt:
 *   NewPtr / DisposePtr           malloc / free (NewPtr(0) liefert einen gültigen Zeiger)
 *   TickCount                     60-Hz-Ticks seit dem Start
 *   UCConvertCFAbsoluteTimeToSeconds
 *                                 CFAbsoluteTime (Sekunden seit 2001) -> Sekunden seit 1904
 *   PBGetVolumeInfoSync           meldet nsvErr (-35): es gibt keine Volume-Verwaltung des Classic-Mac-OS */
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
