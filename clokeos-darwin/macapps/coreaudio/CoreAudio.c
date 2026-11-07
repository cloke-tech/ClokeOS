// Kleinste Core-Audio-HAL für ClokeOS (eigener Code, kein Apple-Code).
//
// Öffentliche Schnittstelle wie in Apples <CoreAudio/AudioHardware.h>; dahinter steht genau ein
// Ausgabegerät, das über /dev/dsp0 (RavynHDAudio, fest 48 kHz, s16le, stereo) spielt. Der IOProc
// liefert Float32 interleaved, die Umwandlung nach s16 passiert hier. Eingabe, Aggregate, Listener
// und mehrere Geräte gibt es nicht (docs/audio-stack.md, Abschnitt "User-space Core Audio shim").
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

enum { kObjDevice = 2, kObjStream = 3 };
#define RATE 48000.0
#define CHANNELS 2
#define MAXFRAMES 4096
#define MAXPROCS 8
#define MAXSTATS 16384

typedef struct { AudioDeviceIOProc proc; void *client; } Slot;

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static Slot slots[MAXPROCS];
static UInt32 frames = 512;     // kAudioDevicePropertyBufferFrameSize = Länge eines Schreibblocks
static int running, stop_req;
static pthread_t th;
static int fd = -1;
static int to_file;             // Ausgabe in eine Datei (CLOKE_CA_DEVICE): WAV-Kopf, Takt per mach_wait_until
static UInt64 file_bytes;
// CLOKE_CA_TRACE=1: Gerätestart, IOProcs und unbekannte Eigenschaften auf stderr
static int ca_trace(void) { static int t = -1; if (t < 0) t = getenv("CLOKE_CA_TRACE") ? atoi(getenv("CLOKE_CA_TRACE")) : 0; return t; }
static void fourcc(char o[5], UInt32 v) { for (int i = 0; i < 4; i++) { char c = (char)(v >> (24 - 8 * i)); o[i] = c >= 32 && c < 127 ? c : '?'; } o[4] = 0; }
static void wav_header(int f, UInt64 data) {
  unsigned char h[44] = "RIFF....WAVEfmt ";
  UInt32 v; UInt16 w;
  #define PUT32(o, x) (v = (UInt32)(x), memcpy(h + (o), &v, 4))
  #define PUT16(o, x) (w = (UInt16)(x), memcpy(h + (o), &w, 2))
  PUT32(4, 36 + data); PUT32(16, 16); PUT16(20, 1); PUT16(22, CHANNELS); PUT32(24, (UInt32)RATE);
  PUT32(28, (UInt32)RATE * CHANNELS * 2); PUT16(32, CHANNELS * 2); PUT16(34, 16);
  memcpy(h + 36, "data", 4); PUT32(40, data);
  pwrite(f, h, 44, 0);
}
static mach_timebase_info_data_t tb;
// Messung (CLOKE_CA_STATS=1): Abstand zwischen zwei IOProc-Aufrufen in ms
static double p_min = 1e18, p_max, p_sum; static unsigned long p_n;
static float p_all[MAXSTATS];

static UInt64 ns_to_host(UInt64 ns) { return ns * tb.denom / tb.numer; }
static UInt64 host_to_ns(UInt64 h) { return h * tb.numer / tb.denom; }

static AudioStreamBasicDescription fmt_f32(void) {
  AudioStreamBasicDescription d = { RATE, kAudioFormatLinearPCM,
    kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 4 * CHANNELS, 1, 4 * CHANNELS, CHANNELS, 32, 0 };
  return d;
}
static AudioStreamBasicDescription fmt_s16(void) {
  AudioStreamBasicDescription d = { RATE, kAudioFormatLinearPCM,
    kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, 2 * CHANNELS, 1, 2 * CHANNELS, CHANNELS, 16, 0 };
  return d;
}

static void *io_thread(void *arg) {
  (void)arg;
  mach_timebase_info(&tb);
  UInt32 f = frames;
  // Echtzeitklasse (Periode = Blocklänge); schlägt es fehl, läuft der Thread normal weiter
  struct thread_time_constraint_policy pol = {
    (uint32_t)ns_to_host((UInt64)(f * 1e9 / RATE)), (uint32_t)ns_to_host(2000000),
    (uint32_t)ns_to_host((UInt64)(f * 1e9 / RATE) / 2), 1 };
  thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);

  float *buf = malloc(MAXFRAMES * CHANNELS * sizeof(float));
  float *mix = malloc(MAXFRAMES * CHANNELS * sizeof(float));
  short *pcm = malloc(MAXFRAMES * CHANNELS * sizeof(short));
  Float64 sample = 0; UInt64 last = 0, t0 = mach_absolute_time();
  while (!stop_req) {
    pthread_mutex_lock(&lk);
    f = frames;
    Slot local[MAXPROCS]; memcpy(local, slots, sizeof slots);
    pthread_mutex_unlock(&lk);
    memset(buf, 0, f * CHANNELS * sizeof(float));

    UInt64 now = mach_absolute_time();
    if (last) {
      double p = (double)host_to_ns(now - last) / 1e6;
      if (p < p_min) p_min = p;
      if (p > p_max) p_max = p;
      p_sum += p; if (p_n < MAXSTATS) p_all[p_n] = (float)p; p_n++;
    }
    last = now;
    AudioTimeStamp ts = { .mSampleTime = sample, .mHostTime = now, .mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid };
    // Der Block wird erst nach dem Schreiben hörbar: Ausgabezeit einen Block später
    AudioTimeStamp out = ts; out.mSampleTime = sample + f; out.mHostTime = now + ns_to_host((UInt64)(f * 1e9 / RATE));
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    for (int i = 0; i < MAXPROCS; i++) {
      if (!local[i].proc) continue;
      // Jeder IOProc bekommt einen genullten Puffer, die Ergebnisse werden addiert
      memset(mix, 0, f * CHANNELS * sizeof(float));
      abl.mBuffers[0] = (AudioBuffer){ CHANNELS, f * CHANNELS * sizeof(float), mix };
      local[i].proc(kObjDevice, &ts, NULL, &ts, &abl, &out, local[i].client);
      for (UInt32 k = 0; k < f * CHANNELS; k++) buf[k] += mix[k];
    }
    for (UInt32 k = 0; k < f * CHANNELS; k++) {
      float v = buf[k]; v = v > 1 ? 1 : v < -1 ? -1 : v;
      pcm[k] = (short)(v * 32767.0f + (v < 0 ? -0.5f : 0.5f));
    }
    // write() blockiert, wenn der Ringpuffer des Treibers voll ist: das ist der Takt des Geräts
    size_t left = f * CHANNELS * sizeof(short); const char *p = (const char *)pcm;
    while (left && !stop_req) { ssize_t n = write(fd, p, left); if (n <= 0) break; p += n; left -= n; file_bytes += n; }
    sample += f;
    // Eine Datei blockiert nie: dann gibt die Uhr den Gerätetakt vor (Echtzeit, damit Logic wie mit Hardware spielt)
    if (to_file) mach_wait_until(t0 + ns_to_host((UInt64)(sample * 1e9 / RATE)));
  }
  free(buf); free(mix); free(pcm);
  return NULL;
}

// --- Eigenschaften --------------------------------------------------------------------------
typedef struct { UInt32 size; int ok; } Q;

// Schreibt den Wert nach out (falls nicht NULL und groß genug) und liefert Größe/Vorhandensein.
static Q prop1(AudioObjectID obj, const AudioObjectPropertyAddress *a, void *out, UInt32 cap);
static void fourcc(char o[5], UInt32 v);
// CLOKE_CA_TRACE=2: jede Abfrage (Objekt, Eigenschaft, Bereich, Ergebnis)
static Q prop(AudioObjectID obj, const AudioObjectPropertyAddress *a, void *out, UInt32 cap) {
  Q q = prop1(obj, a, out, cap);
  if (ca_trace() >= 2 && out) { char c[5], sc[5]; fourcc(c, a->mSelector); fourcc(sc, a->mScope);
    fprintf(stderr, "[CoreAudio] %u '%s' '%s' -> %s\n", (unsigned)obj, c, sc, q.ok ? "ja" : "nein"); }
  return q;
}
static Q prop1(AudioObjectID obj, const AudioObjectPropertyAddress *a, void *out, UInt32 cap) {
  Q q = { 0, 0 };
  #define PUT(T, ...) do { T v_ = __VA_ARGS__; q.size = sizeof v_; q.ok = 1; if (out && cap >= sizeof v_) memcpy(out, &v_, sizeof v_); } while (0)
  UInt32 sel = a->mSelector;
  if (obj == kAudioObjectSystemObject) {
    switch (sel) {
      case kAudioObjectPropertyClass: PUT(UInt32, kAudioSystemObjectClassID); break;
      case kAudioHardwarePropertyDevices: PUT(AudioObjectID, kObjDevice); break;
      case kAudioHardwarePropertyDefaultOutputDevice:
      case kAudioHardwarePropertyDefaultSystemOutputDevice: PUT(AudioObjectID, kObjDevice); break;
      case kAudioHardwarePropertyDefaultInputDevice: PUT(AudioObjectID, kAudioObjectUnknown); break;   // es gibt keinen Eingang
      default: break;
    }
  } else if (obj == kObjDevice) {
    int outscope = a->mScope == kAudioObjectPropertyScopeOutput || a->mScope == kAudioObjectPropertyScopeGlobal;
    switch (sel) {
      case kAudioObjectPropertyClass: PUT(UInt32, kAudioDeviceClassID); break;
      case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectSystemObject); break;
      case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: {
        const char *s = sel == kAudioObjectPropertyName ? "ClokeOS HDA Output" : sel == kAudioObjectPropertyManufacturer ? "ClokeOS"
                      : sel == kAudioDevicePropertyModelUID ? "ClokeOS:HDA" : "ClokeOS:dsp0";
        q.size = sizeof(CFStringRef); q.ok = 1;
        if (out && cap >= q.size) { CFStringRef str = CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); memcpy(out, &str, sizeof str); }
        break; }
      case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeBuiltIn); break;
      case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, 1); break;
      case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, running); break;
      case kAudioDevicePropertyNominalSampleRate: PUT(Float64, RATE); break;
      case kAudioDevicePropertyAvailableNominalSampleRates: PUT(AudioValueRange, ((AudioValueRange){ RATE, RATE })); break;
      case kAudioDevicePropertyBufferFrameSize: PUT(UInt32, frames); break;
      case kAudioDevicePropertyBufferFrameSizeRange: PUT(AudioValueRange, ((AudioValueRange){ 32, MAXFRAMES })); break;
      case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset: PUT(UInt32, 0); break;
      case kAudioDevicePropertyStreams:
        if (outscope) PUT(AudioObjectID, kObjStream); else q.ok = 1;   // Eingang: leere Liste
        break;
      case kAudioDevicePropertyStreamConfiguration:
        if (outscope) {
          struct { UInt32 n; AudioBuffer b; } c = { 1, { CHANNELS, 0, NULL } };
          q.size = sizeof c; q.ok = 1; if (out && cap >= sizeof c) memcpy(out, &c, sizeof c);
        } else {
          UInt32 n = 0; q.size = sizeof n; q.ok = 1; if (out && cap >= sizeof n) memcpy(out, &n, sizeof n);
        }
        break;
      default: break;
    }
  } else if (obj == kObjStream) {
    switch (sel) {
      case kAudioObjectPropertyClass: PUT(UInt32, kAudioStreamClassID); break;
      case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjDevice); break;
      case kAudioStreamPropertyDirection: PUT(UInt32, 0); break;
      case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1); break;
      case kAudioStreamPropertyLatency: PUT(UInt32, 0); break;
      case kAudioStreamPropertyVirtualFormat: PUT(AudioStreamBasicDescription, fmt_f32()); break;   // was der IOProc liefert
      case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, fmt_s16()); break;  // was /dev/dsp0 nimmt
      default: break;
    }
  }
  return q;
}

Boolean AudioObjectHasProperty(AudioObjectID o, const AudioObjectPropertyAddress *a) {
  return a && prop(o, a, NULL, 0).ok;
}
OSStatus AudioObjectIsPropertySettable(AudioObjectID o, const AudioObjectPropertyAddress *a, Boolean *s) {
  if (!a || !prop(o, a, NULL, 0).ok) return kAudioHardwareUnknownPropertyError;
  *s = o == kObjDevice && a->mSelector == kAudioDevicePropertyBufferFrameSize;
  return 0;
}
OSStatus AudioObjectGetPropertyDataSize(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *qd, UInt32 *size) {
  (void)qs; (void)qd;
  Q q = a ? prop(o, a, NULL, 0) : (Q){ 0, 0 };
  if (!q.ok) return kAudioHardwareUnknownPropertyError;
  *size = q.size;
  return 0;
}
OSStatus AudioObjectGetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *qd, UInt32 *size, void *data) {
  (void)qs; (void)qd;
  Q q = a ? prop(o, a, NULL, 0) : (Q){ 0, 0 };
  if (!q.ok) {
    if (ca_trace() && a) { char c[5]; fourcc(c, a->mSelector); fprintf(stderr, "[CoreAudio] unbekannt: Objekt %u Eigenschaft '%s'\n", (unsigned)o, c); }
    return kAudioHardwareUnknownPropertyError;
  }
  if (*size < q.size) return kAudioHardwareBadPropertySizeError;
  prop(o, a, data, *size);
  *size = q.size;
  return 0;
}
OSStatus AudioObjectSetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *qd, UInt32 size, const void *data) {
  (void)qs; (void)qd;
  if (o != kObjDevice || !a || a->mSelector != kAudioDevicePropertyBufferFrameSize) return kAudioHardwareUnknownPropertyError;
  if (size != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
  UInt32 f = *(const UInt32 *)data;
  if (f < 32 || f > MAXFRAMES) return kAudioHardwareIllegalOperationError;
  pthread_mutex_lock(&lk); frames = f; pthread_mutex_unlock(&lk);
  return 0;
}

// --- IO ---------------------------------------------------------------------------------------
OSStatus AudioDeviceCreateIOProcID(AudioObjectID dev, AudioDeviceIOProc proc, void *client, AudioDeviceIOProcID *id) {
  if (dev != kObjDevice || !proc || !id) return kAudioHardwareBadDeviceError;
  OSStatus r = kAudioHardwareIllegalOperationError;
  pthread_mutex_lock(&lk);
  for (int i = 0; i < MAXPROCS; i++) if (!slots[i].proc) { slots[i] = (Slot){ proc, client }; *id = proc; r = 0; break; }
  pthread_mutex_unlock(&lk);
  if (ca_trace()) fprintf(stderr, "[CoreAudio] CreateIOProcID %p -> %d\n", (void *)proc, (int)r);
  return r;
}
OSStatus AudioDeviceDestroyIOProcID(AudioObjectID dev, AudioDeviceIOProcID id) {
  if (dev != kObjDevice) return kAudioHardwareBadDeviceError;
  OSStatus r = kAudioHardwareBadObjectError;
  pthread_mutex_lock(&lk);
  for (int i = 0; i < MAXPROCS; i++) if (slots[i].proc == id) { slots[i].proc = NULL; r = 0; break; }
  pthread_mutex_unlock(&lk);
  return r;
}
OSStatus AudioDeviceStart(AudioObjectID dev, AudioDeviceIOProcID id) {
  (void)id;
  if (dev != kObjDevice) return kAudioHardwareBadDeviceError;
  if (running) return 0;
  const char *path = getenv("CLOKE_CA_DEVICE");   // Test: Datei statt /dev/dsp0
  fd = open(path ? path : "/dev/dsp0", O_WRONLY | (path ? O_CREAT | O_TRUNC : 0), 0644);
  if (ca_trace()) fprintf(stderr, "[CoreAudio] AudioDeviceStart %s -> fd %d\n", path ? path : "/dev/dsp0", fd);
  if (fd < 0) return kAudioHardwareNotRunningError;
  struct stat st; to_file = fstat(fd, &st) == 0 && S_ISREG(st.st_mode); file_bytes = 0;
  if (to_file) { wav_header(fd, 0); lseek(fd, 44, SEEK_SET); }
  stop_req = 0; p_min = 1e18; p_max = p_sum = 0; p_n = 0;
  if (pthread_create(&th, NULL, io_thread, NULL)) { close(fd); return kAudioHardwareIllegalOperationError; }
  running = 1;
  return 0;
}
OSStatus AudioDeviceStop(AudioObjectID dev, AudioDeviceIOProcID id) {
  (void)id;
  if (dev != kObjDevice) return kAudioHardwareBadDeviceError;
  if (!running) return 0;
  stop_req = 1; pthread_join(th, NULL); running = 0;
  if (to_file) wav_header(fd, file_bytes);
  if (ca_trace()) fprintf(stderr, "[CoreAudio] AudioDeviceStop, %llu Bytes\n", (unsigned long long)file_bytes);
  close(fd); fd = -1;
  if (getenv("CLOKE_CA_STATS") && p_n) {
    fprintf(stderr, "CoreAudio: %lu IOProc-Aufrufe, Block %u Frames (%.3f ms), Abstand min/mittel/max %.3f/%.3f/%.3f ms\n",
            p_n + 1, (unsigned)frames, frames * 1e3 / RATE, p_min, p_sum / p_n, p_max);
    // Zweite Hälfte: der Treiber nimmt anfangs seinen Ringpuffer ohne Wartezeit voll, danach gilt der Gerätetakt
    unsigned long n = p_n < MAXSTATS ? p_n : MAXSTATS, h = n / 2; double s = 0;
    for (unsigned long i = h; i < n; i++) s += p_all[i];
    fprintf(stderr, "CoreAudio: zweite Hälfte (%lu Aufrufe): mittlerer Abstand %.3f ms\n", n - h, s / (n - h));
  }
  return 0;
}

// --- Was Logic sonst noch importiert: Hostzeit, Listener (feuern nie: das Gerät ändert sich nicht), alte Eigenschafts-API ---
UInt64 AudioGetCurrentHostTime(void) { return mach_absolute_time(); }
UInt64 AudioConvertHostTimeToNanos(UInt64 h) { if (!tb.denom) mach_timebase_info(&tb); return host_to_ns(h); }
UInt64 AudioConvertNanosToHostTime(UInt64 ns) { if (!tb.denom) mach_timebase_info(&tb); return ns_to_host(ns); }
OSStatus AudioObjectAddPropertyListener(AudioObjectID o, const AudioObjectPropertyAddress *a, AudioObjectPropertyListenerProc p, void *c) {
  (void)o; (void)a; (void)p; (void)c; return 0; }
OSStatus AudioObjectRemovePropertyListener(AudioObjectID o, const AudioObjectPropertyAddress *a, AudioObjectPropertyListenerProc p, void *c) {
  (void)o; (void)a; (void)p; (void)c; return 0; }
OSStatus AudioDeviceAddPropertyListener(AudioDeviceID d, UInt32 ch, Boolean in, AudioDevicePropertyID id, AudioDevicePropertyListenerProc p, void *c) {
  (void)d; (void)ch; (void)in; (void)id; (void)p; (void)c; return 0; }
OSStatus AudioDeviceRemovePropertyListener(AudioDeviceID d, UInt32 ch, Boolean in, AudioDevicePropertyID id, AudioDevicePropertyListenerProc p) {
  (void)d; (void)ch; (void)in; (void)id; (void)p; return 0; }
static AudioObjectPropertyAddress old_addr(UInt32 id, UInt32 ch, int in) {
  AudioObjectPropertyAddress a = { id, in ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, ch };
  return a;
}
OSStatus AudioDeviceGetPropertyInfo(AudioDeviceID d, UInt32 ch, Boolean in, AudioDevicePropertyID id, UInt32 *size, Boolean *writable) {
  AudioObjectPropertyAddress a = old_addr(id, ch, in);
  if (writable) *writable = id == kAudioDevicePropertyBufferFrameSize;
  return size ? AudioObjectGetPropertyDataSize(d, &a, 0, NULL, size) : (AudioObjectHasProperty(d, &a) ? 0 : kAudioHardwareUnknownPropertyError);
}
OSStatus AudioDeviceGetProperty(AudioDeviceID d, UInt32 ch, Boolean in, AudioDevicePropertyID id, UInt32 *size, void *data) {
  AudioObjectPropertyAddress a = old_addr(id, ch, in);
  return AudioObjectGetPropertyData(d, &a, 0, NULL, size, data);
}
OSStatus AudioDeviceSetProperty(AudioDeviceID d, const AudioTimeStamp *when, UInt32 ch, Boolean in, AudioDevicePropertyID id, UInt32 size, const void *data) {
  (void)when; AudioObjectPropertyAddress a = old_addr(id, ch, in);
  return AudioObjectSetPropertyData(d, &a, 0, NULL, size, data);
}
OSStatus AudioHardwareGetPropertyInfo(AudioHardwarePropertyID id, UInt32 *size, Boolean *writable) {
  AudioObjectPropertyAddress a = { id, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  if (writable) *writable = 0;
  return size ? AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, size) : 0;
}
OSStatus AudioHardwareGetProperty(AudioHardwarePropertyID id, UInt32 *size, void *data) {
  AudioObjectPropertyAddress a = { id, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
  return AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, size, data);
}
