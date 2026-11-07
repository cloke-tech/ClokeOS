// AudioFile / ExtAudioFile / AudioFormat für PCM in WAV, AIFF(-C) und CAF (eigener Code, kein Apple-Code). Dazu Lesen
// komprimierter CAF (Pakettabelle 'pakt', Magic Cookie 'kuki'); dekodieren kann der Wandler nur ALAC.
// Keine Benutzerdaten-Chunks, kein Streaming-Parser (AudioFileStream).
#include "at_internal.h"
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// --- Ein-/Ausgabe: Datei oder Rückrufe -------------------------------------------------------
typedef struct {
  int fd;
  void *cd; AudioFile_ReadProc rd; AudioFile_WriteProc wr; AudioFile_GetSizeProc gs; AudioFile_SetSizeProc ss;
} IO;
static int io_read(IO *io, SInt64 off, void *b, UInt32 n, UInt32 *got) {
  if (io->rd) { UInt32 g = 0; OSStatus r = io->rd(io->cd, off, n, b, &g); *got = g; return r ? -1 : 0; }
  ssize_t r = pread(io->fd, b, n, off);
  if (r < 0) return -1;
  *got = (UInt32)r; return 0;
}
static int io_write(IO *io, SInt64 off, const void *b, UInt32 n) {
  if (io->wr) { UInt32 g = 0; OSStatus r = io->wr(io->cd, off, n, b, &g); return r || g != n ? -1 : 0; }
  return pwrite(io->fd, b, n, off) == (ssize_t)n ? 0 : -1;
}
static SInt64 io_size(IO *io) {
  if (io->gs) return io->gs(io->cd);
  off_t e = lseek(io->fd, 0, SEEK_END);
  return e < 0 ? 0 : e;
}

struct OpaqueAudioFileID {
  IO io; AudioFileTypeID type; AudioStreamBasicDescription fmt;
  int writable, dirty, ownfd;
  SInt64 dataOff, dataBytes;       // Lage der PCM-Daten
  SInt64 sizePos;                  // Stelle des Größenfelds der Datenchunks (WAV/AIFF/CAF)
  SInt64 hdrEnd;
  UInt64 frames;                   // AIFF: Frames aus COMM
  // komprimierte CAF: Magic Cookie und Pakettabelle (Anfang jedes Pakets relativ zu dataOff, npk+1 Einträge)
  uint8_t *cookie; UInt32 cookieSize;
  SInt64 npk, *pkoff; UInt32 maxpk;
  SInt64 validFrames; UInt32 priming, remainder;
};
static int af_vbr(AudioFileID f) { return f->fmt.mFormatID != kAudioFormatLinearPCM; }

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd32be(const uint8_t *p) { return p[3] | p[2] << 8 | p[1] << 16 | (uint32_t)p[0] << 24; }
static uint16_t rd16le(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint16_t rd16be(const uint8_t *p) { return p[1] | p[0] << 8; }
static uint64_t rd64be(const uint8_t *p) { return (uint64_t)rd32be(p) << 32 | rd32be(p + 4); }
static void wr32le(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void wr32be(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[3 - i] = v >> (8 * i); }
static void wr16le(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void wr16be(uint8_t *p, uint32_t v) { p[1] = v; p[0] = v >> 8; }
static void wr64be(uint8_t *p, uint64_t v) { wr32be(p, v >> 32); wr32be(p + 4, (uint32_t)v); }

static double ext80(const uint8_t *p) {
  int sign = p[0] >> 7, e = ((p[0] & 0x7f) << 8) | p[1];
  uint64_t m = rd64be(p + 2);
  if (!e && !m) return 0;
  double v = ldexp((double)m, e - 16383 - 63);
  return sign ? -v : v;
}
static void put_ext80(uint8_t *p, double v) {
  memset(p, 0, 10);
  if (v <= 0) return;
  int e; double m = frexp(v, &e);          // v = m * 2^e, 0.5 <= m < 1
  uint64_t mant = (uint64_t)ldexp(m, 64);
  uint16_t ex = (uint16_t)(e - 1 + 16383);
  p[0] = ex >> 8; p[1] = ex & 0xff;
  wr64be(p + 2, mant);
}

static void set_pcm(AudioStreamBasicDescription *f, double rate, UInt32 ch, UInt32 bits, int flt, int be, int sgn) {
  memset(f, 0, sizeof *f);
  f->mSampleRate = rate; f->mFormatID = kAudioFormatLinearPCM; f->mChannelsPerFrame = ch; f->mBitsPerChannel = bits;
  f->mFormatFlags = kAudioFormatFlagIsPacked | (flt ? kAudioFormatFlagIsFloat : sgn ? kAudioFormatFlagIsSignedInteger : 0) | (be ? kAudioFormatFlagIsBigEndian : 0);
  f->mBytesPerFrame = f->mBytesPerPacket = ch * ((bits + 7) / 8); f->mFramesPerPacket = 1;
}

static OSStatus parse_wav(AudioFileID f) {
  uint8_t h[12], c[8]; UInt32 g; SInt64 pos = 12, size = io_size(&f->io); int have_fmt = 0;
  if (io_read(&f->io, 0, h, 12, &g) || g < 12) return kAudioFileInvalidFileError;
  while (pos + 8 <= size) {
    if (io_read(&f->io, pos, c, 8, &g) || g < 8) break;
    uint32_t id = rd32be(c), len = rd32le(c + 4);
    if (id == 'fmt ') {
      uint8_t b[40]; memset(b, 0, sizeof b);
      if (io_read(&f->io, pos + 8, b, len < 40 ? len : 40, &g) || len < 16) return kAudioFileInvalidFileError;
      unsigned tag = rd16le(b); UInt32 ch = rd16le(b + 2), bits = rd16le(b + 14); double rate = rd32le(b + 4);
      if (tag == 0xFFFE && len >= 40) tag = rd16le(b + 24);
      if ((tag != 1 && tag != 3) || !ch || !bits) return kAudioFileUnsupportedDataFormatError;
      set_pcm(&f->fmt, rate, ch, bits, tag == 3, 0, bits > 8);
      UInt32 ba = rd16le(b + 12); if (ba) f->fmt.mBytesPerFrame = f->fmt.mBytesPerPacket = ba;
      have_fmt = 1;
    } else if (id == 'data') {
      if (!have_fmt) return kAudioFileInvalidFileError;
      f->dataOff = pos + 8; f->sizePos = pos + 4;
      SInt64 left = size - f->dataOff;
      f->dataBytes = (len == 0xFFFFFFFFu || len > left) ? left : len;
      return 0;
    }
    pos += 8 + len + (len & 1);
  }
  return kAudioFileInvalidFileError;
}
static OSStatus parse_aiff(AudioFileID f) {
  uint8_t h[12], c[8]; UInt32 g; SInt64 pos = 12, size = io_size(&f->io); int have_fmt = 0;
  if (io_read(&f->io, 0, h, 12, &g) || g < 12) return kAudioFileInvalidFileError;
  int aifc = rd32be(h + 8) == 'AIFC';
  while (pos + 8 <= size) {
    if (io_read(&f->io, pos, c, 8, &g) || g < 8) break;
    uint32_t id = rd32be(c), len = rd32be(c + 4);
    if (id == 'COMM') {
      uint8_t b[32]; memset(b, 0, sizeof b);
      if (io_read(&f->io, pos + 8, b, len < 32 ? len : 32, &g) || len < 18) return kAudioFileInvalidFileError;
      UInt32 ch = rd16be(b), bits = rd16be(b + 6); double rate = ext80(b + 8); uint32_t comp = 'NONE';
      f->frames = rd32be(b + 2);
      if (aifc && len >= 22) comp = rd32be(b + 18);
      int flt = 0, be = 1, sgn = 1;
      switch (comp) {
        case 'NONE': case 'twos': break;
        case 'sowt': be = 0; break;
        case 'fl32': case 'FL32': flt = 1; break;
        case 'fl64': case 'FL64': flt = 1; break;
        case 'raw ': sgn = 0; break;
        case 'in24': case 'in32': break;
        default: return kAudioFileUnsupportedDataFormatError;
      }
      if (!ch || !bits) return kAudioFileInvalidFileError;
      set_pcm(&f->fmt, rate, ch, bits, flt, be, sgn);
      have_fmt = 1;
    } else if (id == 'SSND') {
      if (!have_fmt) return kAudioFileInvalidFileError;
      uint8_t b[8]; if (io_read(&f->io, pos + 8, b, 8, &g) || g < 8) return kAudioFileInvalidFileError;
      f->dataOff = pos + 16 + rd32be(b); f->sizePos = pos + 4;
      SInt64 left = size - f->dataOff, want = (SInt64)len - 8 - rd32be(b);
      f->dataBytes = want > left ? left : want;
      return 0;
    }
    pos += 8 + len + (len & 1);
  }
  return kAudioFileInvalidFileError;
}
static OSStatus parse_caf(AudioFileID f) {
  uint8_t c[12]; UInt32 g; SInt64 pos = 8, size = io_size(&f->io); int have_fmt = 0, have_data = 0;
  SInt64 paktPos = -1, paktLen = 0;
  while (pos + 12 <= size) {
    if (io_read(&f->io, pos, c, 12, &g) || g < 12) break;
    uint32_t id = rd32be(c); int64_t len = (int64_t)rd64be(c + 4);
    if (id == 'desc') {
      uint8_t b[32]; if (len < 32 || io_read(&f->io, pos + 12, b, 32, &g) || g < 32) return kAudioFileInvalidFileError;
      uint32_t fid = rd32be(b + 8), fl = rd32be(b + 12);
      uint64_t rb = rd64be(b); double rate; memcpy(&rate, &rb, 8);
      // Reihenfolge: bytesPerPacket, framesPerPacket, channels, bits
      f->fmt = (AudioStreamBasicDescription){ rate, fid, fl, rd32be(b + 16), rd32be(b + 20), 0, rd32be(b + 24), rd32be(b + 28), 0 };
      if (!f->fmt.mChannelsPerFrame) return kAudioFileInvalidFileError;
      if (fid == 'lpcm') {
        f->fmt.mFormatID = kAudioFormatLinearPCM;
        f->fmt.mBytesPerFrame = f->fmt.mFramesPerPacket ? f->fmt.mBytesPerPacket / f->fmt.mFramesPerPacket : f->fmt.mBytesPerPacket;
        f->fmt.mFormatFlags = ((fl & 1) ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) | ((fl & 2) ? 0 : kAudioFormatFlagIsBigEndian) | kAudioFormatFlagIsPacked;
        if (!(fl & 1) && f->fmt.mBitsPerChannel == 8) f->fmt.mFormatFlags &= ~kAudioFormatFlagIsBigEndian;   // egal bei einem Byte
        if (!f->fmt.mBytesPerFrame) return kAudioFileInvalidFileError;
      } else if (!f->fmt.mFramesPerPacket) return kAudioFileUnsupportedDataFormatError;   // variable Frames je Paket: nicht unterstützt
      have_fmt = 1;
    } else if (id == 'kuki' && len > 0 && len < (1 << 20)) {
      free(f->cookie); f->cookie = malloc((size_t)len);
      if (!f->cookie || io_read(&f->io, pos + 12, f->cookie, (UInt32)len, &g) || g < len) return kAudioFileInvalidFileError;
      f->cookieSize = (UInt32)len;
    } else if (id == 'pakt') {
      paktPos = pos + 12; paktLen = len;
    } else if (id == 'data') {
      if (!have_fmt) return kAudioFileInvalidFileError;
      f->dataOff = pos + 12 + 4; f->sizePos = pos + 4;
      SInt64 left = size - f->dataOff;
      f->dataBytes = (len < 0 || len - 4 > left) ? left : len - 4;
      have_data = 1;
      if (len < 0) break;           // Daten bis zum Dateiende
    }
    if (len < 0) break;
    pos += 12 + len;
  }
  if (!have_data) return kAudioFileInvalidFileError;
  if (!af_vbr(f)) return 0;
  // Komprimiert: feste Paketgröße oder Pakettabelle (Größen als BER-Zahlen: 7 Bit je Byte, oberes Bit = weiter)
  f->validFrames = 0;
  if (f->fmt.mBytesPerPacket) {
    f->npk = f->dataBytes / f->fmt.mBytesPerPacket; f->maxpk = f->fmt.mBytesPerPacket;
    f->pkoff = malloc((size_t)(f->npk + 1) * sizeof(SInt64));
    if (!f->pkoff) return kAudio_MemFullError;
    for (SInt64 i = 0; i <= f->npk; i++) f->pkoff[i] = i * f->fmt.mBytesPerPacket;
  } else {
    if (paktPos < 0 || paktLen < 24 || paktLen > (1 << 28)) return kAudioFileInvalidFileError;
    uint8_t *t = malloc((size_t)paktLen);
    if (!t || io_read(&f->io, paktPos, t, (UInt32)paktLen, &g) || g < paktLen) { free(t); return kAudioFileInvalidFileError; }
    f->npk = (SInt64)rd64be(t); f->validFrames = (SInt64)rd64be(t + 8); f->priming = rd32be(t + 16); f->remainder = rd32be(t + 20);
    f->pkoff = malloc((size_t)(f->npk + 1) * sizeof(SInt64));
    if (!f->pkoff) { free(t); return kAudio_MemFullError; }
    SInt64 q = 24, o = 0;
    for (SInt64 i = 0; i < f->npk; i++) {
      uint64_t v = 0; int more = 1;
      while (more && q < paktLen) { v = (v << 7) | (t[q] & 0x7f); more = t[q] & 0x80; q++; }
      f->pkoff[i] = o; o += (SInt64)v; if (v > f->maxpk) f->maxpk = (UInt32)v;
    }
    f->pkoff[f->npk] = o;
    free(t);
  }
  if (!f->validFrames) f->validFrames = f->npk * f->fmt.mFramesPerPacket - f->priming - f->remainder;
  return 0;
}

static AudioFileID af_new(void) { return calloc(1, sizeof(struct OpaqueAudioFileID)); }
static OSStatus af_open(AudioFileID f) {
  uint8_t h[12]; UInt32 g;
  if (io_read(&f->io, 0, h, 12, &g) || g < 12) return kAudioFileInvalidFileError;
  uint32_t a = rd32be(h), b = rd32be(h + 8);
  if (a == 'RIFF' && b == 'WAVE') { f->type = kAudioFileWAVEType; return parse_wav(f); }
  if (a == 'FORM' && (b == 'AIFF' || b == 'AIFC')) { f->type = b == 'AIFF' ? kAudioFileAIFFType : kAudioFileAIFCType; return parse_aiff(f); }
  if (a == 'caff') { f->type = kAudioFileCAFType; return parse_caf(f); }
  return kAudioFileUnsupportedFileTypeError;
}

// Kopf schreiben (und beim Schließen mit den echten Größen wiederholen).
static OSStatus af_write_header(AudioFileID f) {
  uint8_t h[128]; UInt32 n = 0; const AudioStreamBasicDescription *d = &f->fmt;
  SInt64 db = f->dataBytes; UInt32 pad = db & 1;
  UInt32 bpf = d->mBytesPerFrame; UInt64 fr = bpf ? db / bpf : 0;
  if (f->type == kAudioFileWAVEType) {
    int ext = d->mChannelsPerFrame > 2 || (d->mBitsPerChannel > 16 && !(d->mFormatFlags & kAudioFormatFlagIsFloat));
    int flt = (d->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    memcpy(h, "RIFF", 4); wr32le(h + 4, (uint32_t)(f->dataOff - 8 + db + pad)); memcpy(h + 8, "WAVEfmt ", 8);
    wr32le(h + 16, ext ? 40 : 16);
    wr16le(h + 20, ext ? 0xFFFE : flt ? 3 : 1); wr16le(h + 22, d->mChannelsPerFrame); wr32le(h + 24, (uint32_t)d->mSampleRate);
    wr32le(h + 28, (uint32_t)d->mSampleRate * bpf); wr16le(h + 32, bpf); wr16le(h + 34, d->mBitsPerChannel);
    n = 36;
    if (ext) {
      wr16le(h + n, 22); wr16le(h + n + 2, d->mBitsPerChannel); wr32le(h + n + 4, 0);
      wr16le(h + n + 8, flt ? 3 : 1);       // GUID: Format-Tag, dann fester Rest
      static const uint8_t g[14] = { 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
      memcpy(h + n + 10, g, 14);
      n += 24;
    }
    memcpy(h + n, "data", 4); wr32le(h + n + 4, (uint32_t)db); n += 8;
    f->dataOff = n; f->sizePos = n - 4;
  } else if (f->type == kAudioFileAIFFType || f->type == kAudioFileAIFCType) {
    int c = f->type == kAudioFileAIFCType; uint32_t comp = (d->mFormatFlags & kAudioFormatFlagIsFloat) ? (d->mBitsPerChannel == 64 ? 'fl64' : 'fl32')
      : (d->mFormatFlags & kAudioFormatFlagIsBigEndian) || d->mBitsPerChannel == 8 ? 'NONE' : 'sowt';
    uint32_t commLen = c ? 24 : 18;
    memcpy(h, "FORM", 4); memcpy(h + 8, c ? "AIFC" : "AIFF", 4); n = 12;
    if (c) { memcpy(h + n, "FVER", 4); wr32be(h + n + 4, 4); wr32be(h + n + 8, 0xA2805140u); n += 12; }
    memcpy(h + n, "COMM", 4); wr32be(h + n + 4, commLen); wr16be(h + n + 8, d->mChannelsPerFrame);
    wr32be(h + n + 10, (uint32_t)fr); wr16be(h + n + 14, d->mBitsPerChannel); put_ext80(h + n + 16, d->mSampleRate);
    if (c) { wr32be(h + n + 26, comp); h[n + 30] = 0; h[n + 31] = 0; }
    n += 8 + commLen;
    memcpy(h + n, "SSND", 4); wr32be(h + n + 4, (uint32_t)(db + 8)); wr32be(h + n + 8, 0); wr32be(h + n + 12, 0); n += 16;
    wr32be(h + 4, (uint32_t)(n - 8 + db + pad));
    f->dataOff = n; f->sizePos = n - 12;
  } else {
    memcpy(h, "caff", 4); wr16be(h + 4, 1); wr16be(h + 6, 0);
    memcpy(h + 8, "desc", 4); wr64be(h + 12, 32);
    uint64_t rb; memcpy(&rb, &d->mSampleRate, 8); wr64be(h + 20, rb);
    memcpy(h + 28, "lpcm", 4);
    uint32_t fl = ((d->mFormatFlags & kAudioFormatFlagIsFloat) ? 1 : 0) | ((d->mFormatFlags & kAudioFormatFlagIsBigEndian) ? 0 : 2);
    wr32be(h + 32, fl); wr32be(h + 36, bpf); wr32be(h + 40, 1); wr32be(h + 44, d->mChannelsPerFrame); wr32be(h + 48, d->mBitsPerChannel);
    memcpy(h + 52, "data", 4); wr64be(h + 56, (uint64_t)(db + 4)); wr32be(h + 64, 0);
    n = 68; f->dataOff = n; f->sizePos = 56;
  }
  if (io_write(&f->io, 0, h, n)) return kAudioFileUnspecifiedError;
  if (pad) { uint8_t z = 0; io_write(&f->io, f->dataOff + db, &z, 1); }
  return 0;
}

static OSStatus af_check_create_fmt(AudioFileTypeID t, const AudioStreamBasicDescription *d) {
  if (t != kAudioFileWAVEType && t != kAudioFileAIFFType && t != kAudioFileAIFCType && t != kAudioFileCAFType) return kAudioFileUnsupportedFileTypeError;
  if (d->mFormatID != kAudioFormatLinearPCM || !at_pcm_ok(d) || (d->mFormatFlags & kAudioFormatFlagIsNonInterleaved)) return kAudioFileUnsupportedDataFormatError;
  if (d->mBitsPerChannel * d->mChannelsPerFrame != d->mBytesPerFrame * 8 && d->mBytesPerFrame) return kAudioFileUnsupportedDataFormatError;   // nur gepackt
  int flt = (d->mFormatFlags & kAudioFormatFlagIsFloat) != 0, be = (d->mFormatFlags & kAudioFormatFlagIsBigEndian) != 0, sgn = (d->mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
  if (t == kAudioFileWAVEType && (be || (!flt && (d->mBitsPerChannel > 8) != sgn) || (!flt && d->mBitsPerChannel == 8 && sgn))) return kAudioFileUnsupportedDataFormatError;
  if (t == kAudioFileAIFFType && (flt || (!be && d->mBitsPerChannel > 8) || !sgn)) return kAudioFileUnsupportedDataFormatError;
  if (t == kAudioFileAIFCType && (!sgn && !flt)) return kAudioFileUnsupportedDataFormatError;
  if (t == kAudioFileCAFType && !flt && !sgn) return kAudioFileUnsupportedDataFormatError;
  return 0;
}

OSStatus AudioFileCreateWithURL(CFURLRef url, AudioFileTypeID type, const AudioStreamBasicDescription *fmt, AudioFileFlags flags, AudioFileID *out) {
  if (!url || !fmt || !out) return kAudio_ParamError;
  AudioStreamBasicDescription d = *fmt;
  if (!d.mBytesPerFrame) d.mBytesPerFrame = d.mChannelsPerFrame * ((d.mBitsPerChannel + 7) / 8);
  OSStatus r = af_check_create_fmt(type, &d);
  if (r) return r;
  char path[1024];
  if (!CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path)) return kAudioFileBadPropertySizeError;
  int fd = open(path, O_RDWR | O_CREAT | ((flags & kAudioFileFlags_EraseFile) ? O_TRUNC : O_EXCL), 0644);
  if (fd < 0) return kAudioFilePermissionsError;
  AudioFileID f = af_new();
  if (!f) { close(fd); return kAudio_MemFullError; }
  f->io.fd = fd; f->ownfd = 1; f->writable = 1; f->type = type; f->fmt = d; f->fmt.mBytesPerPacket = d.mBytesPerFrame; f->fmt.mFramesPerPacket = 1;
  r = af_write_header(f);
  if (r) { close(fd); free(f); return r; }
  *out = f;
  return 0;
}
OSStatus AudioFileOpenURL(CFURLRef url, AudioFilePermissions perm, AudioFileTypeID hint, AudioFileID *out) {
  (void)hint;
  if (!url || !out) return kAudio_ParamError;
  char path[1024];
  if (!CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, sizeof path)) return kAudioFileBadPropertySizeError;
  int fd = open(path, (perm & kAudioFileWritePermission) ? O_RDWR : O_RDONLY);
  if (fd < 0) return kAudioFileFileNotFoundError;
  AudioFileID f = af_new();
  if (!f) { close(fd); return kAudio_MemFullError; }
  f->io.fd = fd; f->ownfd = 1; f->writable = (perm & kAudioFileWritePermission) != 0;
  OSStatus r = af_open(f);
  if (r) { close(fd); free(f); return r; }
  *out = f;
  return 0;
}
OSStatus AudioFileOpenWithCallbacks(void *cd, AudioFile_ReadProc rd, AudioFile_WriteProc wr, AudioFile_GetSizeProc gs, AudioFile_SetSizeProc ss,
                                    AudioFileTypeID hint, AudioFileID *out) {
  (void)hint;
  if (!rd || !gs || !out) return kAudio_ParamError;
  AudioFileID f = af_new();
  if (!f) return kAudio_MemFullError;
  f->io = (IO){ -1, cd, rd, wr, gs, ss }; f->writable = wr != NULL;
  OSStatus r = af_open(f);
  if (r) { free(f); return r; }
  *out = f;
  return 0;
}
OSStatus AudioFileInitializeWithCallbacks(void *cd, AudioFile_ReadProc rd, AudioFile_WriteProc wr, AudioFile_GetSizeProc gs, AudioFile_SetSizeProc ss,
                                          AudioFileTypeID type, const AudioStreamBasicDescription *fmt, AudioFileFlags flags, AudioFileID *out) {
  (void)flags;
  if (!wr || !gs || !fmt || !out) return kAudio_ParamError;
  AudioStreamBasicDescription d = *fmt;
  if (!d.mBytesPerFrame) d.mBytesPerFrame = d.mChannelsPerFrame * ((d.mBitsPerChannel + 7) / 8);
  OSStatus r = af_check_create_fmt(type, &d);
  if (r) return r;
  AudioFileID f = af_new();
  if (!f) return kAudio_MemFullError;
  f->io = (IO){ -1, cd, rd, wr, gs, ss }; f->writable = 1; f->type = type; f->fmt = d; f->fmt.mBytesPerPacket = d.mBytesPerFrame; f->fmt.mFramesPerPacket = 1;
  r = af_write_header(f);
  if (r) { free(f); return r; }
  *out = f;
  return 0;
}
OSStatus AudioFileClose(AudioFileID f) {
  if (!f) return kAudio_ParamError;
  OSStatus r = 0;
  if (f->writable && f->dirty) r = af_write_header(f);
  if (f->ownfd) close(f->io.fd);
  free(f->cookie); free(f->pkoff);
  free(f);
  return r;
}
OSStatus AudioFileOptimize(AudioFileID f) { return f ? 0 : kAudio_ParamError; }

OSStatus AudioFileReadBytes(AudioFileID f, Boolean cache, SInt64 start, UInt32 *n, void *buf) {
  (void)cache;
  if (!f || !n || !buf) return kAudio_ParamError;
  if (start < 0 || start >= f->dataBytes) { *n = 0; return kAudioFileEndOfFileError; }
  UInt32 want = *n; if ((SInt64)want > f->dataBytes - start) want = (UInt32)(f->dataBytes - start);
  UInt32 got = 0;
  if (io_read(&f->io, f->dataOff + start, buf, want, &got)) { *n = 0; return kAudioFileUnspecifiedError; }
  *n = got;
  return got < *n ? kAudioFileEndOfFileError : 0;
}
OSStatus AudioFileWriteBytes(AudioFileID f, Boolean cache, SInt64 start, UInt32 *n, const void *buf) {
  (void)cache;
  if (!f || !n || !buf) return kAudio_ParamError;
  if (!f->writable) return kAudioFilePermissionsError;
  if (start < 0) return kAudio_ParamError;
  // Lücken mit Nullen füllen (Daten dahinter erst nach den vorhandenen)
  if (start > f->dataBytes) { static const uint8_t z[4096] = { 0 }; for (SInt64 p = f->dataBytes; p < start; ) { UInt32 c = (UInt32)((start - p) < 4096 ? start - p : 4096); if (io_write(&f->io, f->dataOff + p, z, c)) return kAudioFileUnspecifiedError; p += c; } }
  if (io_write(&f->io, f->dataOff + start, buf, *n)) return kAudioFileUnspecifiedError;
  if (start + *n > f->dataBytes) f->dataBytes = start + *n;
  f->dirty = 1;
  return 0;
}
OSStatus AudioFileReadPacketData(AudioFileID f, Boolean cache, UInt32 *nbytes, AudioStreamPacketDescription *d, SInt64 start, UInt32 *npk, void *buf) {
  (void)cache;
  if (!f || !nbytes || !npk || !buf) return kAudio_ParamError;
  if (af_vbr(f)) {                     // komprimiert: so viele ganze Pakete, wie in nbytes passen, mit Beschreibungen
    if (start < 0 || start >= f->npk) { *npk = 0; *nbytes = 0; return kAudioFileEndOfFileError; }
    UInt32 n = 0; SInt64 base = f->pkoff[start];
    while (n < *npk && start + n < f->npk && f->pkoff[start + n + 1] - base <= (SInt64)*nbytes) n++;
    if (!n) { *npk = 0; *nbytes = 0; return kAudioFileBadPropertySizeError; }
    UInt32 bytes = (UInt32)(f->pkoff[start + n] - base), got = 0;
    if (io_read(&f->io, f->dataOff + base, buf, bytes, &got) || got < bytes) return kAudioFileUnspecifiedError;
    if (d) for (UInt32 i = 0; i < n; i++)
      d[i] = (AudioStreamPacketDescription){ f->pkoff[start + i] - base, 0, (UInt32)(f->pkoff[start + i + 1] - f->pkoff[start + i]) };
    *npk = n; *nbytes = bytes;
    return 0;
  }
  UInt32 bpp = f->fmt.mBytesPerPacket;
  SInt64 total = f->dataBytes / bpp;
  if (start < 0 || start >= total) { *npk = 0; *nbytes = 0; return kAudioFileEndOfFileError; }
  UInt32 n = *npk; if (n > *nbytes / bpp) n = *nbytes / bpp; if ((SInt64)n > total - start) n = (UInt32)(total - start);
  UInt32 got = 0;
  if (io_read(&f->io, f->dataOff + start * bpp, buf, n * bpp, &got)) return kAudioFileUnspecifiedError;
  *npk = got / bpp; *nbytes = *npk * bpp;
  return 0;
}
OSStatus AudioFileReadPackets(AudioFileID f, Boolean cache, UInt32 *nbytes, AudioStreamPacketDescription *d, SInt64 start, UInt32 *npk, void *buf) {
  return AudioFileReadPacketData(f, cache, nbytes, d, start, npk, buf);
}
OSStatus AudioFileWritePackets(AudioFileID f, Boolean cache, UInt32 nbytes, const AudioStreamPacketDescription *d, SInt64 start, UInt32 *npk, const void *buf) {
  (void)d;
  if (!f || !npk || !buf) return kAudio_ParamError;
  UInt32 bpp = f->fmt.mBytesPerPacket, bytes = *npk * bpp;
  if (bytes > nbytes) { bytes = nbytes / bpp * bpp; *npk = bytes / bpp; }
  return AudioFileWriteBytes(f, cache, start * bpp, &bytes, buf);
}

// Benutzerdaten (Chunks) gibt es nicht
OSStatus AudioFileCountUserData(AudioFileID f, UInt32 id, UInt32 *n) { if (!f || !n) return kAudio_ParamError; *n = 0; return 0; }
OSStatus AudioFileGetUserDataSize(AudioFileID f, UInt32 id, UInt32 i, UInt32 *s) { (void)f; (void)id; (void)i; (void)s; return kAudioFileUnsupportedPropertyError; }
OSStatus AudioFileGetUserData(AudioFileID f, UInt32 id, UInt32 i, UInt32 *s, void *d) { (void)f; (void)id; (void)i; (void)s; (void)d; return kAudioFileUnsupportedPropertyError; }
OSStatus AudioFileSetUserData(AudioFileID f, UInt32 id, UInt32 i, UInt32 s, const void *d) { (void)f; (void)id; (void)i; (void)s; (void)d; return kAudioFileUnsupportedPropertyError; }
OSStatus AudioFileRemoveUserData(AudioFileID f, UInt32 id, UInt32 i) { (void)f; (void)id; (void)i; return kAudioFileUnsupportedPropertyError; }

static int afprop(AudioFileID f, AudioFilePropertyID id, UInt32 *size, UInt32 *w, void *out) {
  union { UInt32 u; UInt64 q; SInt64 s; Float64 d; AudioStreamBasicDescription f; AudioFilePacketTableInfo t; } v; UInt32 sz = 4; v.q = 0; *w = 0;
  if (id == kAudioFilePropertyMagicCookieData) {
    if (!f->cookie) return 0;
    *size = f->cookieSize; if (out) memcpy(out, f->cookie, f->cookieSize);
    return 1;
  }
  if (af_vbr(f)) switch (id) {         // komprimiert: Werte aus der Pakettabelle
    case kAudioFilePropertyAudioDataPacketCount: v.q = f->npk; sz = 8; goto done;
    case kAudioFilePropertyMaximumPacketSize: case kAudioFilePropertyPacketSizeUpperBound: v.u = f->maxpk; goto done;
    case kAudioFilePropertyEstimatedDuration: v.d = f->fmt.mSampleRate > 0 ? (double)f->validFrames / f->fmt.mSampleRate : 0; sz = 8; goto done;
    case kAudioFilePropertyBitRate: v.u = f->validFrames ? (UInt32)(f->dataBytes * 8 * f->fmt.mSampleRate / f->validFrames) : 0; goto done;
    case kAudioFilePropertyPacketTableInfo: v.t = (AudioFilePacketTableInfo){ f->validFrames, (SInt32)f->priming, (SInt32)f->remainder }; sz = sizeof v.t; goto done;
  }
  switch (id) {
    case kAudioFilePropertyFileFormat: v.u = f->type; break;
    case kAudioFilePropertyDataFormat: v.f = f->fmt; sz = sizeof v.f; break;
    case kAudioFilePropertyIsOptimized: v.u = 1; break;
    case kAudioFilePropertyDataOffset: v.s = f->dataOff; sz = 8; break;
    case kAudioFilePropertyAudioDataByteCount: v.q = f->dataBytes; sz = 8; break;
    case kAudioFilePropertyAudioDataPacketCount: v.q = f->dataBytes / f->fmt.mBytesPerPacket; sz = 8; break;
    case kAudioFilePropertyMaximumPacketSize: case kAudioFilePropertyPacketSizeUpperBound: v.u = f->fmt.mBytesPerPacket; break;
    case kAudioFilePropertyEstimatedDuration: v.d = f->fmt.mSampleRate > 0 ? (double)(f->dataBytes / f->fmt.mBytesPerPacket) / f->fmt.mSampleRate : 0; sz = 8; break;
    case kAudioFilePropertyBitRate: v.u = (UInt32)(f->fmt.mSampleRate * f->fmt.mBytesPerFrame * 8); break;
    case kAudioFilePropertyChannelLayout: *w = 1; return 0;
    default: return 0;
  }
done:
  *size = sz;
  if (out) memcpy(out, &v, sz);
  return 1;
}
OSStatus AudioFileGetPropertyInfo(AudioFileID f, AudioFilePropertyID id, UInt32 *size, UInt32 *w) {
  UInt32 sz = 0, wr = 0;
  if (!f) return kAudio_ParamError;
  if (!afprop(f, id, &sz, &wr, NULL)) return kAudioFileUnsupportedPropertyError;
  if (size) *size = sz;
  if (w) *w = wr;
  return 0;
}
OSStatus AudioFileGetProperty(AudioFileID f, AudioFilePropertyID id, UInt32 *size, void *out) {
  UInt32 sz = 0, wr = 0;
  if (!f || !size || !out) return kAudio_ParamError;
  if (!afprop(f, id, &sz, &wr, NULL)) return kAudioFileUnsupportedPropertyError;
  if (*size < sz) return kAudioFileBadPropertySizeError;
  afprop(f, id, &sz, &wr, out);
  *size = sz;
  return 0;
}
OSStatus AudioFileSetProperty(AudioFileID f, AudioFilePropertyID id, UInt32 size, const void *data) {
  (void)size; (void)data;
  if (!f) return kAudio_ParamError;
  if (id == kAudioFilePropertyChannelLayout) return 0;      // wird nicht gespeichert
  return kAudioFileUnsupportedPropertyError;
}

// --- AudioFormat (nur PCM) ---------------------------------------------------------------------
OSStatus AudioFormatGetPropertyInfo(AudioFormatPropertyID id, UInt32 specSize, const void *spec, UInt32 *outSize) {
  (void)specSize; (void)spec;
  switch (id) {
    case kAudioFormatProperty_FormatInfo: *outSize = sizeof(AudioStreamBasicDescription); return 0;
    case kAudioFormatProperty_FormatIsVBR: case kAudioFormatProperty_FormatIsExternallyFramed: *outSize = 4; return 0;
    case kAudioFormatProperty_FormatName: *outSize = sizeof(CFStringRef); return 0;
    default: return kAudioFormatUnsupportedPropertyError;
  }
}
OSStatus AudioFormatGetProperty(AudioFormatPropertyID id, UInt32 specSize, const void *spec, UInt32 *ioSize, void *out) {
  UInt32 need = 0;
  OSStatus r = AudioFormatGetPropertyInfo(id, specSize, spec, &need);
  if (r) return r;
  if (!ioSize || !out || *ioSize < need) return kAudioFormatBadPropertySizeError;
  if (!spec || specSize < sizeof(AudioStreamBasicDescription) * (id == kAudioFormatProperty_FormatInfo || id == kAudioFormatProperty_FormatName)) {
    if (id == kAudioFormatProperty_FormatInfo || id == kAudioFormatProperty_FormatName) return kAudioFormatBadSpecifierSizeError;
  }
  switch (id) {
    case kAudioFormatProperty_FormatInfo: {
      AudioStreamBasicDescription *d = out; *d = *(const AudioStreamBasicDescription *)spec;
      if (d->mFormatID == kAudioFormatAppleLossless) { if (!d->mFramesPerPacket) d->mFramesPerPacket = 4096; break; }
      if (d->mFormatID != kAudioFormatLinearPCM) return kAudioFormatUnknownFormatError;
      if (!d->mBytesPerFrame && d->mBitsPerChannel) d->mBytesPerFrame = d->mChannelsPerFrame * ((d->mBitsPerChannel + 7) / 8);
      if (!d->mFramesPerPacket) d->mFramesPerPacket = 1;
      if (!d->mBytesPerPacket) d->mBytesPerPacket = d->mBytesPerFrame;
      break; }
    case kAudioFormatProperty_FormatIsVBR: case kAudioFormatProperty_FormatIsExternallyFramed:
      *(UInt32 *)out = spec && specSize >= sizeof(AudioStreamBasicDescription) && ((const AudioStreamBasicDescription *)spec)->mFormatID == kAudioFormatAppleLossless; break;
    case kAudioFormatProperty_FormatName: {
      const AudioStreamBasicDescription *d = spec;
      if (d->mFormatID != kAudioFormatLinearPCM) return kAudioFormatUnknownFormatError;
      char b[96]; snprintf(b, sizeof b, "%u-bit %s Linear PCM", (unsigned)d->mBitsPerChannel, (d->mFormatFlags & kAudioFormatFlagIsFloat) ? "Floating Point" : "Integer");
      *(CFStringRef *)out = CFStringCreateWithCString(kCFAllocatorDefault, b, kCFStringEncodingUTF8); break; }
  }
  *ioSize = need;
  return 0;
}

// --- ExtAudioFile --------------------------------------------------------------------------
struct OpaqueExtAudioFile {
  AudioFileID af; int ownaf, writing;
  AudioStreamBasicDescription ffmt, cfmt; int hascfmt;
  AudioConverterRef conv;
  SInt64 fpos;          // nächstes Dateiframe, das der Wandler lesen wird
  SInt64 cpos;          // Clientframes gelesen/geschrieben
  UInt32 chunkcap; void *chunk;
  AudioStreamPacketDescription *pdesc; UInt32 pdcap;   // komprimiert: Beschreibungen der gelesenen Pakete
};
typedef struct OpaqueExtAudioFile *ExtFile;

static OSStatus ext_make(AudioFileID af, int own, int writing, ExtAudioFileRef *out) {
  ExtFile e = calloc(1, sizeof *e);
  if (!e) return kAudio_MemFullError;
  e->af = af; e->ownaf = own; e->writing = writing; e->ffmt = af->fmt; e->cfmt = af->fmt;
  e->cpos = writing ? 0 : 0;
  *out = e;
  return 0;
}
OSStatus ExtAudioFileOpenURL(CFURLRef url, ExtAudioFileRef *out) {
  AudioFileID af; OSStatus r = AudioFileOpenURL(url, kAudioFileReadPermission, 0, &af);
  if (r) return r;
  r = ext_make(af, 1, 0, out);
  if (r) AudioFileClose(af);
  return r;
}
OSStatus ExtAudioFileCreateWithURL(CFURLRef url, AudioFileTypeID type, const AudioStreamBasicDescription *fmt, const AudioChannelLayout *lay,
                                   UInt32 flags, ExtAudioFileRef *out) {
  (void)lay;
  AudioFileID af; OSStatus r = AudioFileCreateWithURL(url, type, fmt, flags, &af);
  if (r) return r;
  r = ext_make(af, 1, 1, out);
  if (r) AudioFileClose(af);
  return r;
}
OSStatus ExtAudioFileWrapAudioFileID(AudioFileID af, Boolean forWriting, ExtAudioFileRef *out) {
  if (!af || !out) return kAudio_ParamError;
  return ext_make(af, 0, forWriting, out);
}
OSStatus ExtAudioFileDispose(ExtAudioFileRef e) {
  if (!e) return kAudio_ParamError;
  OSStatus r = 0;
  if (e->conv) AudioConverterDispose(e->conv);
  if (e->af && e->ownaf) r = AudioFileClose(e->af);
  else if (e->af && e->af->writable && e->af->dirty) { r = af_write_header(e->af); }
  free(e->chunk); free(e->pdesc); free(e);
  return r;
}
// Leseposition für Clientframe cf: PCM = Dateiframe; komprimiert = Paket mit dem Frame (plus Vorlauf), Rest wird im Wandler verworfen
static void ext_locate(ExtFile e, SInt64 cf) {
  double cr = e->cfmt.mSampleRate > 0 ? e->cfmt.mSampleRate : e->ffmt.mSampleRate;
  SInt64 ff = (SInt64)floor(cf * e->ffmt.mSampleRate / cr);
  if (!af_vbr(e->af)) { e->fpos = ff; return; }
  ff += e->af->priming;
  UInt32 fpp = e->af->fmt.mFramesPerPacket;
  e->fpos = ff / fpp;
  if (e->conv) { AudioConverterReset(e->conv); at_conv_skip(e->conv, (UInt32)(ff % fpp)); }
}
static OSStatus ext_setclient(ExtFile e, const AudioStreamBasicDescription *c) {
  AudioStreamBasicDescription d = *c;
  if (!at_pcm_ok(&d)) return kExtAudioFileError_InvalidDataFormat;
  if (!d.mBytesPerFrame) d.mBytesPerFrame = at_pcm_bytes_per_frame(&d);
  if (!d.mSampleRate) d.mSampleRate = e->ffmt.mSampleRate;
  if (e->conv) { AudioConverterDispose(e->conv); e->conv = NULL; }
  OSStatus r = e->writing ? AudioConverterNew(&d, &e->ffmt, &e->conv) : AudioConverterNew(&e->ffmt, &d, &e->conv);
  if (r) return r;
  if (!e->writing && e->af->cookie) AudioConverterSetProperty(e->conv, kAudioConverterDecompressionMagicCookie, e->af->cookieSize, e->af->cookie);
  e->cfmt = d; e->hascfmt = 1;
  if (!e->writing) ext_locate(e, e->cpos);
  return 0;
}
OSStatus ExtAudioFileSetProperty(ExtAudioFileRef e, ExtAudioFilePropertyID id, UInt32 size, const void *data) {
  if (!e || !data) return kAudio_ParamError;
  switch (id) {
    case kExtAudioFileProperty_ClientDataFormat:
      if (size != sizeof(AudioStreamBasicDescription)) return kExtAudioFileError_InvalidPropertySize;
      return ext_setclient(e, data);
    case kExtAudioFileProperty_ClientChannelLayout: case kExtAudioFileProperty_FileChannelLayout: case kExtAudioFileProperty_ConverterConfig:
    case kExtAudioFileProperty_CodecManufacturer: case kExtAudioFileProperty_IOBufferSizeBytes: case kExtAudioFileProperty_IOBuffer:
      return 0;
    default: return kExtAudioFileError_InvalidProperty;
  }
}
OSStatus ExtAudioFileGetPropertyInfo(ExtAudioFileRef e, ExtAudioFilePropertyID id, UInt32 *size, Boolean *w) {
  if (!e) return kAudio_ParamError;
  UInt32 s; Boolean wr = 0;
  switch (id) {
    case kExtAudioFileProperty_FileDataFormat: s = sizeof(AudioStreamBasicDescription); break;
    case kExtAudioFileProperty_ClientDataFormat: s = sizeof(AudioStreamBasicDescription); wr = 1; break;
    case kExtAudioFileProperty_FileLengthFrames: s = 8; break;
    case kExtAudioFileProperty_AudioFile: s = sizeof(AudioFileID); break;
    case kExtAudioFileProperty_AudioConverter: s = sizeof(AudioConverterRef); break;
    default: return kExtAudioFileError_InvalidProperty;
  }
  if (size) *size = s;
  if (w) *w = wr;
  return 0;
}
OSStatus ExtAudioFileGetProperty(ExtAudioFileRef e, ExtAudioFilePropertyID id, UInt32 *size, void *out) {
  if (!e || !size || !out) return kAudio_ParamError;
  UInt32 need; OSStatus r = ExtAudioFileGetPropertyInfo(e, id, &need, NULL);
  if (r) return r;
  if (*size < need) return kExtAudioFileError_InvalidPropertySize;
  switch (id) {
    case kExtAudioFileProperty_FileDataFormat: *(AudioStreamBasicDescription *)out = e->ffmt; break;
    case kExtAudioFileProperty_ClientDataFormat: *(AudioStreamBasicDescription *)out = e->cfmt; break;
    case kExtAudioFileProperty_FileLengthFrames: *(SInt64 *)out = af_vbr(e->af) ? e->af->validFrames : e->af->dataBytes / e->af->fmt.mBytesPerPacket; break;
    case kExtAudioFileProperty_AudioFile: *(AudioFileID *)out = e->af; break;
    case kExtAudioFileProperty_AudioConverter: if (!e->conv) return kExtAudioFileError_InvalidProperty; *(AudioConverterRef *)out = e->conv; break;
  }
  *size = need;
  return 0;
}

typedef struct { ExtFile e; const AudioBufferList *src; UInt32 srcFrames; int given; } WrCtx;
static OSStatus rd_input(AudioConverterRef c, UInt32 *np, AudioBufferList *io, AudioStreamPacketDescription **pd, void *ud) {
  ExtFile e = ud; (void)c;
  if (af_vbr(e->af)) {                 // komprimiert: *np Pakete mit Beschreibungen
    UInt32 n = *np > 64 ? 64 : (*np ? *np : 1), cap = n * e->af->maxpk;
    if (e->chunkcap < cap) { free(e->chunk); e->chunk = malloc(e->chunkcap = cap); if (!e->chunk) { e->chunkcap = 0; return kAudio_MemFullError; } }
    if (e->pdcap < n) { free(e->pdesc); e->pdesc = malloc(sizeof *e->pdesc * (e->pdcap = n)); if (!e->pdesc) { e->pdcap = 0; return kAudio_MemFullError; } }
    UInt32 bytes = cap, pk = n;
    OSStatus r = AudioFileReadPacketData(e->af, 0, &bytes, e->pdesc, e->fpos, &pk, e->chunk);
    if (r == kAudioFileEndOfFileError) { *np = 0; io->mBuffers[0].mDataByteSize = 0; return 0; }
    if (r) return r;
    e->fpos += pk;
    io->mBuffers[0].mData = e->chunk; io->mBuffers[0].mDataByteSize = bytes; io->mBuffers[0].mNumberChannels = e->ffmt.mChannelsPerFrame;
    if (pd) *pd = e->pdesc;
    *np = pk;
    return 0;
  }
  UInt32 bpf = e->ffmt.mBytesPerFrame, n = *np > 4096 ? 4096 : *np;
  if (e->chunkcap < n * bpf) { free(e->chunk); e->chunk = malloc(e->chunkcap = n * bpf); if (!e->chunk) { e->chunkcap = 0; return kAudio_MemFullError; } }
  UInt32 bytes = n * bpf, pk = n;
  OSStatus r = AudioFileReadPacketData(e->af, 0, &bytes, NULL, e->fpos, &pk, e->chunk);
  if (r == kAudioFileEndOfFileError) { *np = 0; io->mBuffers[0].mDataByteSize = 0; return 0; }
  if (r) return r;
  e->fpos += pk;
  io->mBuffers[0].mData = e->chunk; io->mBuffers[0].mDataByteSize = pk * bpf; io->mBuffers[0].mNumberChannels = e->ffmt.mChannelsPerFrame;
  *np = pk;
  return 0;
}
OSStatus ExtAudioFileRead(ExtAudioFileRef e, UInt32 *nframes, AudioBufferList *io) {
  if (!e || !nframes || !io) return kAudio_ParamError;
  if (e->writing) return kExtAudioFileError_InvalidOperationOrder;
  if (!e->conv) { OSStatus r = ext_setclient(e, &e->cfmt); if (r) return r; }
  UInt32 n = *nframes;
  if (af_vbr(e->af)) {                 // nicht über die gültigen Frames hinaus (Rest des letzten Pakets)
    double cr = e->cfmt.mSampleRate > 0 ? e->cfmt.mSampleRate : e->ffmt.mSampleRate;
    SInt64 total = (SInt64)floor(e->af->validFrames * cr / e->ffmt.mSampleRate);
    if (e->cpos >= total) { *nframes = 0; for (UInt32 b = 0; b < io->mNumberBuffers; b++) io->mBuffers[b].mDataByteSize = 0; return 0; }
    if ((SInt64)n > total - e->cpos) n = (UInt32)(total - e->cpos);
  }
  OSStatus r = AudioConverterFillComplexBuffer(e->conv, rd_input, e, &n, io, NULL);
  if (r && !n) { *nframes = 0; return r; }
  e->cpos += n; *nframes = n;
  return 0;
}
static OSStatus wr_input(AudioConverterRef c, UInt32 *np, AudioBufferList *io, AudioStreamPacketDescription **pd, void *ud) {
  WrCtx *w = ud; (void)c; (void)pd;
  if (w->given) { *np = 0; io->mBuffers[0].mDataByteSize = 0; return 0; }
  w->given = 1;
  for (UInt32 b = 0; b < io->mNumberBuffers; b++) io->mBuffers[b] = w->src->mBuffers[b];
  *np = w->srcFrames;
  return 0;
}
OSStatus ExtAudioFileWrite(ExtAudioFileRef e, UInt32 nframes, const AudioBufferList *data) {
  if (!e || !data) return kAudio_ParamError;
  if (!e->writing) return kExtAudioFileError_InvalidOperationOrder;
  if (!nframes) return 0;
  if (!e->conv) { OSStatus r = ext_setclient(e, &e->cfmt); if (r) return r; }
  WrCtx w = { e, data, nframes, 0 };
  UInt32 bpf = e->ffmt.mBytesPerFrame, cap = 4096;
  uint8_t *buf = malloc((size_t)cap * bpf);
  if (!buf) return kAudio_MemFullError;
  OSStatus r = 0;
  for (;;) {
    AudioBufferList o = { 1, { { e->ffmt.mChannelsPerFrame, cap * bpf, buf } } };
    UInt32 n = cap;
    OSStatus fr = AudioConverterFillComplexBuffer(e->conv, wr_input, &w, &n, &o, NULL);
    if (n) { r = AudioFileWritePackets(e->af, 0, n * bpf, NULL, e->fpos, &n, buf); if (r) break; e->fpos += n; }
    if (fr && !n) { r = fr; break; }
    if (n < cap) break;
  }
  free(buf);
  if (!r) e->cpos += nframes;
  return r;
}
OSStatus ExtAudioFileWriteAsync(ExtAudioFileRef e, UInt32 nframes, const AudioBufferList *data) {
  if (!nframes && !data) return 0;      // Anmeldung des Puffers
  return ExtAudioFileWrite(e, nframes, data);
}
OSStatus ExtAudioFileSeek(ExtAudioFileRef e, SInt64 frame) {
  if (!e) return kAudio_ParamError;
  if (e->writing) return kExtAudioFileError_InvalidOperationOrder;
  if (frame < 0) return kAudio_ParamError;
  e->cpos = frame;
  if (e->conv && !af_vbr(e->af)) AudioConverterReset(e->conv);
  ext_locate(e, frame);
  return 0;
}
OSStatus ExtAudioFileTell(ExtAudioFileRef e, SInt64 *frame) {
  if (!e || !frame) return kAudio_ParamError;
  *frame = e->cpos;
  return 0;
}
