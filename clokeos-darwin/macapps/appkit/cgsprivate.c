/* Private CoreGraphics-/SkyLight-Funktionen, die Mac-Programme aufrufen und die Darlings CoreGraphics nicht hat. Die erzeugten
 * Platzhalter (Rückgabe 0) reichen dafür nicht, wenn 0 "Erfolg" heißt und das Programm dann Ausgabewerte liest, die nie geschrieben wurden.
 *
 *   CGSGetSymbolicHotKeyRepresentationList(Art, Elementgröße, Platz, Puffer, *Anzahl)
 *       Logic Pro fragt so die systemweiten Tastenkürzel ab: erst mit Platz 0 nach der Anzahl, dann mit Puffer. Ohne geschriebene
 *       Anzahl richtete Logic eine Tabelle mit uninitialisierter Größe ein und rechnete endlos darüber (Hauptfaden belegt, keine
 *       Ereignisse). Hier: es gibt keine systemweiten Kürzel, Anzahl 0.
 *   CGSIsSymbolicHotKeyEnabled    kein Kürzel ist eingeschaltet */
#include <stdint.h>

int32_t CGSGetSymbolicHotKeyRepresentationList(int32_t kind, int32_t elemSize, uint32_t capacity, void *buffer, uint32_t *count) {
    (void)kind; (void)elemSize; (void)capacity; (void)buffer;
    if (count) *count = 0;
    return 0;
}

int CGSIsSymbolicHotKeyEnabled(int32_t hotKey) {
    (void)hotKey;
    return 0;
}
