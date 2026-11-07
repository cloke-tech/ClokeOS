// C-Schnittstelle zu Apples quelloffenem ALAC-Decoder (alac/, Apache 2.0) für convert.c. Ohne libc++: das Objekt liegt in
// calloc-Speicher (placement new), Ausnahmen und RTTI sind aus.
#include <new>
#include <stdlib.h>
#include <string.h>
#include "alac/ALACDecoder.h"
#include "alac/ALACBitUtilities.h"
#include "alac/EndianPortable.h"

extern "C" {

// Neuer Decoder aus dem Magic Cookie (mit oder ohne 'frma'/'alac'-Hülle). Ohne Cookie: Standardwerte aus Frames je Paket,
// Bittiefe, Kanälen und Rate (so schreibt Apples Encoder sie).
void *at_alac_new(const void *cookie, uint32_t size, uint32_t fpp, uint32_t bits, uint32_t ch, uint32_t rate) {
  void *mem = calloc(1, sizeof(ALACDecoder));
  if (!mem) return NULL;
  ALACDecoder *d = new (mem) ALACDecoder();
  ALACSpecificConfig cfg;
  if (!cookie || size < sizeof cfg) {
    memset(&cfg, 0, sizeof cfg);
    cfg.frameLength = Swap32NtoB(fpp ? fpp : 4096); cfg.compatibleVersion = 0; cfg.bitDepth = (uint8_t)bits;
    cfg.pb = 40; cfg.mb = 10; cfg.kb = 14; cfg.numChannels = (uint8_t)ch; cfg.maxRun = Swap16NtoB(255);
    cfg.maxFrameBytes = 0; cfg.avgBitRate = 0; cfg.sampleRate = Swap32NtoB(rate);
    cookie = &cfg; size = sizeof cfg;
  }
  if (d->Init((void *)cookie, size) != 0) { d->~ALACDecoder(); free(mem); return NULL; }
  return d;
}
void at_alac_free(void *p) {
  if (!p) return;
  ALACDecoder *d = (ALACDecoder *)p;
  d->~ALACDecoder();
  free(p);
}
// Kenndaten nach Init: Bittiefe (16/20/24/32), Kanäle, Frames je Paket
void at_alac_info(void *p, uint32_t *bits, uint32_t *ch, uint32_t *fpp) {
  ALACDecoder *d = (ALACDecoder *)p;
  *bits = d->mConfig.bitDepth; *ch = d->mConfig.numChannels; *fpp = d->mConfig.frameLength;
}
// Ein Paket dekodieren. out: verschachtelte Ganzzahlen in Bittiefe (16 Bit: int16, 20/24 Bit: 3 Byte gepackt, 32 Bit: int32),
// Byteordnung der Maschine. Gibt die Zahl der Frames zurück (0 bei Fehler).
uint32_t at_alac_decode(void *p, const uint8_t *pkt, uint32_t bytes, uint8_t *out) {
  ALACDecoder *d = (ALACDecoder *)p;
  BitBuffer b; BitBufferInit(&b, (uint8_t *)pkt, bytes);
  uint32_t n = 0;
  if (d->Decode(&b, out, d->mConfig.frameLength, d->mConfig.numChannels, &n) != 0) return 0;
  return n;
}

}
