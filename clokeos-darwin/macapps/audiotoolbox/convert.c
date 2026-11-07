// AudioConverter für PCM <-> PCM (eigener Code): Format, Verschachtelung, Kanalzahl, Abtastrate (linear). Als einziger Codec
// ALAC -> PCM (Dekodieren mit Apples quelloffenem Decoder in alac/, über alac_glue.cpp); AAC/MP3 usw. liefern
// kAudioConverterErr_FormatNotSupported.
#include "at_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int at_pcm_ok(const AudioStreamBasicDescription *f) {
  if (f->mFormatID != kAudioFormatLinearPCM || !f->mChannelsPerFrame || f->mChannelsPerFrame > AT_MAXCH) return 0;
  UInt32 bits = f->mBitsPerChannel;
  if (f->mFormatFlags & kAudioFormatFlagIsFloat) { if (bits != 32 && bits != 64) return 0; }
  else if (bits < 8 || bits > 32) return 0;
  UInt32 bpc = at_pcm_bytes_per_frame(f) / ((f->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 1 : f->mChannelsPerFrame);
  return bpc * 8 >= bits && bpc <= 8 && bpc >= 1;
}
UInt32 at_pcm_bytes_per_frame(const AudioStreamBasicDescription *f) {
  if (f->mBytesPerFrame) return f->mBytesPerFrame;
  UInt32 b = (f->mBitsPerChannel + 7) / 8;
  return (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? b : b * f->mChannelsPerFrame;
}
UInt32 at_pcm_nbuffers(const AudioStreamBasicDescription *f) {
  return (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? f->mChannelsPerFrame : 1;
}

static inline float rd_sample(const AudioStreamBasicDescription *f, const uint8_t *p, UInt32 bpc) {
  UInt32 fl = f->mFormatFlags, bits = f->mBitsPerChannel;
  int be = (fl & kAudioFormatFlagIsBigEndian) != 0;
  if (fl & kAudioFormatFlagIsFloat) {
    if (bpc == 4) { uint32_t u = 0; for (int i = 0; i < 4; i++) u |= (uint32_t)p[be ? 3 - i : i] << (8 * i); float x; memcpy(&x, &u, 4); return x; }
    uint64_t u = 0; for (int i = 0; i < 8; i++) u |= (uint64_t)p[be ? 7 - i : i] << (8 * i); double x; memcpy(&x, &u, 8); return (float)x;
  }
  uint64_t u = 0; for (UInt32 i = 0; i < bpc; i++) u |= (uint64_t)p[be ? bpc - 1 - i : i] << (8 * i);
  int64_t v;
  if (!(fl & kAudioFormatFlagIsSignedInteger)) {           // ohne Vorzeichen (8 Bit WAV): Mitte abziehen
    if (bits < bpc * 8 && (fl & kAudioFormatFlagIsAlignedHigh)) u >>= bpc * 8 - bits;
    v = (int64_t)u - ((int64_t)1 << (bits - 1));
  } else {
    if (bits < bpc * 8 && (fl & kAudioFormatFlagIsAlignedHigh)) u >>= bpc * 8 - bits;
    if (bits < 64 && (u >> (bits - 1)) & 1) u |= ~(uint64_t)0 << bits;     // Vorzeichen erweitern
    v = (int64_t)u;
  }
  return (float)((double)v / (double)((int64_t)1 << (bits - 1)));
}
static inline void wr_sample(const AudioStreamBasicDescription *f, uint8_t *p, UInt32 bpc, float x) {
  UInt32 fl = f->mFormatFlags, bits = f->mBitsPerChannel;
  int be = (fl & kAudioFormatFlagIsBigEndian) != 0;
  if (fl & kAudioFormatFlagIsFloat) {
    if (bpc == 4) { uint32_t u; memcpy(&u, &x, 4); for (int i = 0; i < 4; i++) p[be ? 3 - i : i] = (uint8_t)(u >> (8 * i)); }
    else { double d = x; uint64_t u; memcpy(&u, &d, 8); for (int i = 0; i < 8; i++) p[be ? 7 - i : i] = (uint8_t)(u >> (8 * i)); }
    return;
  }
  double scale = (double)((int64_t)1 << (bits - 1)), v = (double)x * scale;
  double hi = scale - 1, lo = -scale;
  v = v >= 0 ? floor(v + 0.5) : -floor(-v + 0.5);
  if (v > hi) v = hi; else if (v < lo) v = lo;
  int64_t s = (int64_t)v;
  uint64_t u;
  if (!(fl & kAudioFormatFlagIsSignedInteger)) u = (uint64_t)(s + ((int64_t)1 << (bits - 1)));
  else u = (uint64_t)s & ((bits < 64) ? (((uint64_t)1 << bits) - 1) : ~(uint64_t)0);
  if (bits < bpc * 8 && (fl & kAudioFormatFlagIsAlignedHigh)) u <<= bpc * 8 - bits;
  for (UInt32 i = 0; i < bpc; i++) p[be ? bpc - 1 - i : i] = (uint8_t)(u >> (8 * i));
}

void at_decode(const AudioStreamBasicDescription *f, const AudioBufferList *abl, UInt32 n, float **pl) {
  UInt32 ch = f->mChannelsPerFrame, bpf = at_pcm_bytes_per_frame(f);
  int ni = (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
  UInt32 bpc = ni ? bpf : bpf / ch;
  for (UInt32 c = 0; c < ch; c++) {
    if (ni && c >= abl->mNumberBuffers) { memset(pl[c], 0, n * sizeof(float)); continue; }
    const uint8_t *base = ni ? abl->mBuffers[c].mData : (const uint8_t *)abl->mBuffers[0].mData + c * bpc;
    UInt32 step = ni ? bpf : bpf;
    if (!base) { memset(pl[c], 0, n * sizeof(float)); continue; }
    for (UInt32 i = 0; i < n; i++) pl[c][i] = rd_sample(f, base + (size_t)i * step, bpc);
  }
}
void at_encode(const AudioStreamBasicDescription *f, AudioBufferList *abl, UInt32 n, float **pl) {
  UInt32 ch = f->mChannelsPerFrame, bpf = at_pcm_bytes_per_frame(f);
  int ni = (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
  UInt32 bpc = ni ? bpf : bpf / ch;
  for (UInt32 c = 0; c < ch; c++) {
    if (ni && c >= abl->mNumberBuffers) continue;
    uint8_t *base = ni ? abl->mBuffers[c].mData : (uint8_t *)abl->mBuffers[0].mData + c * bpc;
    if (!base) continue;
    for (UInt32 i = 0; i < n; i++) wr_sample(f, base + (size_t)i * bpf, bpc, pl[c][i]);
  }
}
void at_chanmap(UInt32 sch, float **src, UInt32 dch, float **dst, UInt32 n) {
  if (sch == dch) { for (UInt32 c = 0; c < dch; c++) if (dst[c] != src[c]) memcpy(dst[c], src[c], n * sizeof(float)); return; }
  if (sch == 1) { for (UInt32 c = 0; c < dch; c++) memcpy(dst[c], src[0], n * sizeof(float)); return; }
  if (dch == 1) {
    for (UInt32 i = 0; i < n; i++) { float s = 0; for (UInt32 c = 0; c < sch; c++) s += src[c][i]; dst[0][i] = s / sch; }
    return;
  }
  for (UInt32 c = 0; c < dch; c++) { if (c < sch) memcpy(dst[c], src[c], n * sizeof(float)); else memset(dst[c], 0, n * sizeof(float)); }
}

// --- Wandler ---------------------------------------------------------------------------------
struct OpaqueAudioConverter {
  AudioStreamBasicDescription s, d;
  double ratio;                       // Eingangsrate / Ausgangsrate
  float *fifo[AT_MAXCH]; UInt32 fifoN, fifoCap;   // dekodierte Eingangsframes (Eingangskanalzahl)
  double pos;                         // Leseposition im FIFO (Frames)
  UInt32 quality, complexity, primeMethod;
  // ALAC-Quelle: s ist das komprimierte Format, sp das PCM, das der Decoder liefert (verschachtelte Ganzzahlen)
  int alac; void *dec; AudioStreamBasicDescription sp; uint8_t *cookie; UInt32 cookieSize; uint8_t *pcm;
  UInt32 skip;                        // nach dem Suchen zu verwerfende dekodierte Frames (at_conv_skip)
};
void *at_alac_new(const void *cookie, uint32_t size, uint32_t fpp, uint32_t bits, uint32_t ch, uint32_t rate);
void at_alac_free(void *p);
void at_alac_info(void *p, uint32_t *bits, uint32_t *ch, uint32_t *fpp);
uint32_t at_alac_decode(void *p, const uint8_t *pkt, uint32_t bytes, uint8_t *out);

// Bittiefe der Quelle aus den ALAC-Formatflags (kAppleLosslessFormatFlag_16/20/24/32BitSourceData = 1..4)
static UInt32 alac_bits(const AudioStreamBasicDescription *s) {
  switch (s->mFormatFlags) { case 2: return 20; case 3: return 24; case 4: return 32; default: return 16; }
}
// Decoder (neu) anlegen und das PCM-Zwischenformat festlegen
static int alac_setup(struct OpaqueAudioConverter *c) {
  if (c->dec) { at_alac_free(c->dec); c->dec = NULL; }
  c->dec = at_alac_new(c->cookie, c->cookieSize, c->s.mFramesPerPacket, alac_bits(&c->s), c->s.mChannelsPerFrame, (uint32_t)c->s.mSampleRate);
  if (!c->dec) return 0;
  uint32_t bits, ch, fpp; at_alac_info(c->dec, &bits, &ch, &fpp);
  if (!ch || ch > AT_MAXCH || !fpp) return 0;
  UInt32 bpc = bits == 16 ? 2 : bits == 32 ? 4 : 3;
  c->sp = (AudioStreamBasicDescription){ c->s.mSampleRate, kAudioFormatLinearPCM,
    kAudioFormatFlagIsSignedInteger | (bits == 20 ? kAudioFormatFlagIsAlignedHigh : kAudioFormatFlagIsPacked), ch * bpc, 1, ch * bpc, ch, bits, 0 };
  c->s.mFramesPerPacket = fpp; c->s.mChannelsPerFrame = ch;
  free(c->pcm); c->pcm = malloc((size_t)fpp * ch * 4 + 64);
  return c->pcm != NULL;
}
void at_conv_skip(AudioConverterRef c, UInt32 frames) { if (c) c->skip = frames; }

OSStatus AudioConverterNew(const AudioStreamBasicDescription *s, const AudioStreamBasicDescription *d, AudioConverterRef *out) {
  if (!s || !d || !out) return kAudio_ParamError;
  int alac = s->mFormatID == kAudioFormatAppleLossless && s->mChannelsPerFrame && s->mChannelsPerFrame <= AT_MAXCH;
  if ((!alac && !at_pcm_ok(s)) || !at_pcm_ok(d)) return kAudioConverterErr_FormatNotSupported;
  struct OpaqueAudioConverter *c = calloc(1, sizeof *c);
  if (!c) return kAudio_MemFullError;
  c->s = *s; c->d = *d;
  if (alac) {
    c->alac = 1;
    if (!alac_setup(c)) { AudioConverterDispose(c); return kAudioConverterErr_FormatNotSupported; }
    c->d.mBytesPerFrame = c->d.mBytesPerFrame ? c->d.mBytesPerFrame : at_pcm_bytes_per_frame(d);
    c->ratio = (s->mSampleRate > 0 && d->mSampleRate > 0) ? s->mSampleRate / d->mSampleRate : 1.0;
    c->quality = kAudioConverterQuality_Medium; c->complexity = kAudioConverterSampleRateConverterComplexity_Linear;
    *out = c;
    return 0;
  }
  if (!c->s.mBytesPerFrame) c->s.mBytesPerFrame = at_pcm_bytes_per_frame(s);
  if (!c->d.mBytesPerFrame) c->d.mBytesPerFrame = at_pcm_bytes_per_frame(d);
  c->ratio = (s->mSampleRate > 0 && d->mSampleRate > 0) ? s->mSampleRate / d->mSampleRate : 1.0;
  c->quality = kAudioConverterQuality_Medium;
  c->complexity = kAudioConverterSampleRateConverterComplexity_Linear;
  *out = c;
  return 0;
}
OSStatus AudioConverterDispose(AudioConverterRef c) {
  if (!c) return kAudio_ParamError;
  for (int i = 0; i < AT_MAXCH; i++) free(c->fifo[i]);
  if (c->dec) at_alac_free(c->dec);
  free(c->cookie); free(c->pcm);
  free(c);
  return 0;
}
OSStatus AudioConverterReset(AudioConverterRef c) {
  if (!c) return kAudio_ParamError;
  c->fifoN = 0; c->pos = 0; c->skip = 0;
  return 0;
}

static int fifo_reserve(struct OpaqueAudioConverter *c, UInt32 need) {
  if (need <= c->fifoCap) return 1;
  UInt32 cap = need + 4096;
  for (UInt32 ch = 0; ch < (c->alac ? c->sp.mChannelsPerFrame : c->s.mChannelsPerFrame); ch++) {
    float *p = realloc(c->fifo[ch], cap * sizeof(float));
    if (!p) return 0;
    c->fifo[ch] = p;
  }
  c->fifoCap = cap;
  return 1;
}

OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc proc, void *user,
                                         UInt32 *ioPackets, AudioBufferList *out, AudioStreamPacketDescription *outDesc) {
  if (!c || !proc || !ioPackets || !out) return kAudio_ParamError;
  const AudioStreamBasicDescription *D = &c->d, *S = c->alac ? &c->sp : &c->s;
  UInt32 want = *ioPackets, dbpf = D->mBytesPerFrame, dch = D->mChannelsPerFrame;
  // Platz im Ausgang begrenzt die Frames
  for (UInt32 b = 0; b < out->mNumberBuffers && b < at_pcm_nbuffers(D); b++)
    if (out->mBuffers[b].mDataByteSize / dbpf < want) want = out->mBuffers[b].mDataByteSize / dbpf;
  if (out->mNumberBuffers < at_pcm_nbuffers(D)) return kAudio_ParamError;
  float *res[AT_MAXCH]; float *mem = malloc((size_t)(want ? want : 1) * dch * sizeof(float));
  if (!mem) return kAudio_MemFullError;
  for (UInt32 k = 0; k < dch; k++) res[k] = mem + (size_t)k * want;
  OSStatus err = 0; UInt32 made = 0;
  AudioBufferList *abl = alloca(sizeof(AudioBufferList) + sizeof(AudioBuffer) * AT_MAXCH);
  int exact = c->ratio == 1.0;
  while (made < want) {
    // Wie viele Eingangsframes werden für den Rest gebraucht? (+1 für die lineare Nachbarschaft)
    double ix = c->pos;
    UInt32 i0 = (UInt32)ix;
    int needNext = !exact && (ix - i0) > 0;
    UInt32 need = i0 + 1 + (needNext ? 1 : 0);
    if (need > c->fifoN) {
      UInt32 ask = (UInt32)ceil((want - made) * c->ratio) + 2;
      if (ask < 64) ask = 64;
      if (c->alac) {                    // Pakete holen und einzeln dekodieren
        UInt32 fpp = c->s.mFramesPerPacket, pk = (ask + fpp - 1) / fpp;
        abl->mNumberBuffers = 1; abl->mBuffers[0] = (AudioBuffer){ c->s.mChannelsPerFrame, 0, NULL };
        AudioStreamPacketDescription *pd = NULL;
        OSStatus r = proc(c, &pk, abl, &pd, user);
        if (r) { err = r; break; }
        if (!pk || !abl->mBuffers[0].mData) break;
        const uint8_t *data = abl->mBuffers[0].mData; UInt32 total = abl->mBuffers[0].mDataByteSize, off = 0, decoded = 0;
        for (UInt32 i = 0; i < pk; i++) {
          UInt32 po = pd ? (UInt32)pd[i].mStartOffset : off, pb = pd ? pd[i].mDataByteSize : total / pk;
          off = po + pb;
          if (po + pb > total || !pb) break;
          UInt32 n = at_alac_decode(c->dec, data + po, pb, c->pcm);
          if (!n) continue;
          decoded = 1;
          UInt32 sk = c->skip < n ? c->skip : n; c->skip -= sk;
          if (n == sk) continue;
          if (!fifo_reserve(c, c->fifoN + n - sk)) { err = kAudio_MemFullError; break; }
          AudioBufferList one = { 1, { { S->mChannelsPerFrame, (n - sk) * S->mBytesPerFrame, c->pcm + (size_t)sk * S->mBytesPerFrame } } };
          float *pl[AT_MAXCH]; for (UInt32 k = 0; k < S->mChannelsPerFrame; k++) pl[k] = c->fifo[k] + c->fifoN;
          at_decode(S, &one, n - sk, pl);
          c->fifoN += n - sk;
        }
        if (err) break;
        if (!decoded) break;
        continue;
      }
      UInt32 nb = at_pcm_nbuffers(S);
      abl->mNumberBuffers = nb;
      for (UInt32 b = 0; b < nb; b++) abl->mBuffers[b] = (AudioBuffer){ (S->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 1 : S->mChannelsPerFrame, 0, NULL };
      UInt32 got = ask;
      AudioStreamPacketDescription *pd = NULL;
      OSStatus r = proc(c, &got, abl, &pd, user);
      if (r) { err = r; break; }
      if (!got) break;
      UInt32 have = abl->mBuffers[0].mDataByteSize / S->mBytesPerFrame;
      if (have < got) got = have;
      if (!got) break;
      if (!fifo_reserve(c, c->fifoN + got)) { err = kAudio_MemFullError; break; }
      float *pl[AT_MAXCH]; for (UInt32 k = 0; k < S->mChannelsPerFrame; k++) pl[k] = c->fifo[k] + c->fifoN;
      at_decode(S, abl, got, pl);
      c->fifoN += got;
      continue;
    }
    // Ein Ausgangsframe: linear interpolieren, dann auf die Ausgangskanäle abbilden
    float row[AT_MAXCH], orow[AT_MAXCH];
    for (UInt32 k = 0; k < S->mChannelsPerFrame; k++) {
      float a = c->fifo[k][i0];
      row[k] = needNext ? (float)(a + (c->fifo[k][i0 + 1] - a) * (ix - i0)) : a;
    }
    float *rp[AT_MAXCH], *op[AT_MAXCH];
    for (UInt32 k = 0; k < S->mChannelsPerFrame; k++) rp[k] = &row[k];
    for (UInt32 k = 0; k < dch; k++) op[k] = &orow[k];
    at_chanmap(S->mChannelsPerFrame, rp, dch, op, 1);
    for (UInt32 k = 0; k < dch; k++) res[k][made] = orow[k];
    made++;
    c->pos += c->ratio;
  }
  // Verbrauchte Eingangsframes verwerfen
  UInt32 drop = (UInt32)c->pos;
  if (drop > c->fifoN) drop = c->fifoN;
  if (drop) {
    for (UInt32 k = 0; k < S->mChannelsPerFrame; k++) memmove(c->fifo[k], c->fifo[k] + drop, (c->fifoN - drop) * sizeof(float));
    c->fifoN -= drop; c->pos -= drop;
  }
  if (made) {
    for (UInt32 b = 0; b < at_pcm_nbuffers(D); b++) out->mBuffers[b].mDataByteSize = made * dbpf;
    float *r2[AT_MAXCH]; for (UInt32 k = 0; k < dch; k++) r2[k] = res[k];
    at_encode(D, out, made, r2);
  } else for (UInt32 b = 0; b < out->mNumberBuffers; b++) out->mBuffers[b].mDataByteSize = 0;
  free(mem);
  *ioPackets = made;
  (void)outDesc;
  return made ? 0 : err;
}

OSStatus AudioConverterConvertBuffer(AudioConverterRef c, UInt32 inSize, const void *in, UInt32 *ioOutSize, void *out) {
  if (!c || !in || !ioOutSize || !out) return kAudio_ParamError;
  if (c->ratio != 1.0) return kAudioConverterErr_InvalidInputSize;
  const AudioStreamBasicDescription *S = &c->s, *D = &c->d;
  if (((S->mFormatFlags | D->mFormatFlags) & kAudioFormatFlagIsNonInterleaved) && (S->mChannelsPerFrame > 1 || D->mChannelsPerFrame > 1))
    return kAudioConverterErr_FormatNotSupported;       // ein Puffer kann nur verschachtelte Daten tragen
  UInt32 n = inSize / S->mBytesPerFrame, room = *ioOutSize / D->mBytesPerFrame;
  if (n > room) n = room;
  AudioBufferList sa = { 1, { { S->mChannelsPerFrame, inSize, (void *)in } } };
  AudioBufferList da = { 1, { { D->mChannelsPerFrame, *ioOutSize, out } } };
  float *a[AT_MAXCH], *b[AT_MAXCH]; float *mem = malloc((size_t)(n ? n : 1) * (S->mChannelsPerFrame + D->mChannelsPerFrame) * sizeof(float));
  if (!mem) return kAudio_MemFullError;
  for (UInt32 k = 0; k < S->mChannelsPerFrame; k++) a[k] = mem + (size_t)k * n;
  for (UInt32 k = 0; k < D->mChannelsPerFrame; k++) b[k] = mem + (size_t)(S->mChannelsPerFrame + k) * n;
  at_decode(S, &sa, n, a); at_chanmap(S->mChannelsPerFrame, a, D->mChannelsPerFrame, b, n); at_encode(D, &da, n, b);
  free(mem);
  *ioOutSize = n * D->mBytesPerFrame;
  return 0;
}

// --- Eigenschaften ---------------------------------------------------------------------------
static int cprop(AudioConverterRef c, AudioConverterPropertyID id, UInt32 *size, int *writable, void *data, const void *setv) {
  UInt32 u = 0; int w = 0, ok = 1; const void *p = &u; UInt32 sz = 4;
  switch (id) {
    case kAudioConverterCurrentInputStreamDescription: p = &c->s; sz = sizeof c->s; break;
    case kAudioConverterCurrentOutputStreamDescription: p = &c->d; sz = sizeof c->d; break;
    case kAudioConverterSampleRateConverterQuality: p = &c->quality; w = 1; break;
    case kAudioConverterSampleRateConverterComplexity: p = &c->complexity; w = 1; break;
    case kAudioConverterPrimeMethod: p = &c->primeMethod; w = 1; break;
    case kAudioConverterPropertyMinimumInputBufferSize: u = c->alac ? c->s.mFramesPerPacket * c->sp.mBytesPerFrame + 64 : c->s.mBytesPerFrame; break;
    case kAudioConverterPropertyMinimumOutputBufferSize: u = c->d.mBytesPerFrame; break;
    case kAudioConverterPropertyMaximumInputPacketSize: u = c->alac ? c->s.mFramesPerPacket * c->sp.mBytesPerFrame + 64 : c->s.mBytesPerFrame; break;
    case kAudioConverterDecompressionMagicCookie: if (!c->alac) { ok = 0; break; } p = c->cookie; sz = c->cookieSize; w = 1; break;
    case kAudioConverterPropertyMaximumOutputPacketSize: u = c->d.mBytesPerFrame; break;
    case kAudioConverterPropertyMaximumInputBufferSize: u = 1 << 20; break;
    case kAudioConverterCodecQuality: w = 1; break;
    case kAudioConverterEncodeBitRate: case kAudioConverterApplicableEncodeBitRates: ok = 0; break;
    default: ok = 0;
  }
  if (!ok) return 0;
  if (size) *size = sz;
  if (writable) *writable = w;
  if (data && sz) memcpy(data, p, sz);
  if (setv) { if (!w) return -1; if (id == kAudioConverterSampleRateConverterQuality) c->quality = *(const UInt32 *)setv;
    else if (id == kAudioConverterSampleRateConverterComplexity) c->complexity = *(const UInt32 *)setv;
    else if (id == kAudioConverterPrimeMethod) c->primeMethod = *(const UInt32 *)setv; }
  return 1;
}
OSStatus AudioConverterGetPropertyInfo(AudioConverterRef c, AudioConverterPropertyID id, UInt32 *size, Boolean *w) {
  int wr = 0; UInt32 sz = 0;
  if (!c) return kAudio_ParamError;
  if (!cprop(c, id, &sz, &wr, NULL, NULL)) return kAudioConverterErr_PropertyNotSupported;
  if (size) *size = sz;
  if (w) *w = wr;
  return 0;
}
OSStatus AudioConverterGetProperty(AudioConverterRef c, AudioConverterPropertyID id, UInt32 *size, void *data) {
  UInt32 sz = 0; int wr;
  if (!c || !size || !data) return kAudio_ParamError;
  if (!cprop(c, id, &sz, &wr, NULL, NULL)) return kAudioConverterErr_PropertyNotSupported;
  if (*size < sz) return kAudioConverterErr_BadPropertySizeError;
  cprop(c, id, NULL, NULL, data, NULL);
  *size = sz;
  return 0;
}
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID id, UInt32 size, const void *data) {
  UInt32 sz = 0; int wr = 0;
  if (!c || !data) return kAudio_ParamError;
  if (id == kAudioConverterChannelMap || id == kAudioConverterInputChannelLayout || id == kAudioConverterOutputChannelLayout)
    return 0;     // Layouts werden ignoriert, die Abbildung ist fest (at_chanmap)
  if (id == kAudioConverterDecompressionMagicCookie && c->alac) {      // Cookie übernehmen, Decoder neu aufsetzen
    free(c->cookie); c->cookie = malloc(size ? size : 1); if (!c->cookie) return kAudio_MemFullError;
    memcpy(c->cookie, data, size); c->cookieSize = size;
    return alac_setup(c) ? 0 : kAudioConverterErr_FormatNotSupported;
  }
  if (!cprop(c, id, &sz, &wr, NULL, NULL)) return kAudioConverterErr_PropertyNotSupported;
  if (!wr) return kAudioConverterErr_PropertyNotSupported;
  if (size != sz && id != kAudioConverterCodecQuality) return kAudioConverterErr_BadPropertySizeError;
  if (id == kAudioConverterCodecQuality) return 0;
  cprop(c, id, NULL, NULL, NULL, data);
  return 0;
}
