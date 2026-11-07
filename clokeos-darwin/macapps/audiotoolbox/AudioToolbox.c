// AudioToolbox-Teilmenge für ClokeOS, Teil 1: AudioComponent-Register, AudioUnit-C-API und ein eingebauter
// Ausgabe-Unit (eigener Code, kein Apple-Code). Teil 2 (AudioConverter) steht in convert.c, Teil 3 (AudioFile,
// ExtAudioFile) in file.c. Genaue Liste: docs/audio-stack.md, Abschnitt 5.
//
// Aufbau wie bei Apple: Eine Komponente liefert über eine Fabrikfunktion ein AudioComponentPlugInInterface
// (Open/Close/Lookup); jede AudioUnit-Funktion schlägt per Lookup(selector) die Methode des Plug-ins nach und
// ruft sie mit dem Plug-in-Zeiger auf. Eingebaute Units und AUv2-Bundles (.component) gehen deshalb denselben Weg.
#include "at_internal.h"
#include <CoreAudio/CoreAudio.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static AudioComponent at_FindNext(AudioComponent prev, const AudioComponentDescription *d);
static OSStatus at_InstanceNew(AudioComponent c, AudioComponentInstance *out);
static OSStatus at_Initialize(AudioUnit u);
static OSStatus at_GetProperty(AudioUnit u, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, void *data, UInt32 *size);
static OSStatus at_SetProperty(AudioUnit u, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, const void *data, UInt32 size);
static OSStatus at_OutputStart(AudioUnit u);

#ifndef kAudioUnitSubType_DefaultOutput
#define kAudioUnitSubType_DefaultOutput 'def '
#endif

// --- Register --------------------------------------------------------------------------------
struct OpaqueAudioComponent {
  AudioComponentDescription d;
  char name[128];
  UInt32 version;
  AudioComponentFactoryFunction factory;      // eingebaut oder per AudioComponentRegister
  char exec[1024], fname[128];                // AUv2-Bundle: Mach-O und Name der Fabrikfunktion
  void *dl;
  struct OpaqueAudioComponent *next;
};
struct ComponentInstanceRecord {
  AudioComponentPlugInInterface *iface;
  AudioComponent comp;
};

static pthread_mutex_t reg_lk = PTHREAD_MUTEX_INITIALIZER;
static AudioComponent reg_head, reg_tail;
static int reg_scanned;

static AudioComponent reg_add(const AudioComponentDescription *d, const char *name, UInt32 ver, AudioComponentFactoryFunction f,
                              const char *exec, const char *fname) {
  AudioComponent c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->d = *d; c->d.componentFlagsMask = 0;
  snprintf(c->name, sizeof c->name, "%s", name ? name : "");
  c->version = ver; c->factory = f;
  if (exec) snprintf(c->exec, sizeof c->exec, "%s", exec);
  if (fname) snprintf(c->fname, sizeof c->fname, "%s", fname);
  if (reg_tail) reg_tail->next = c; else reg_head = c;
  reg_tail = c;
  return c;
}

static UInt32 fourcc(CFTypeRef v) {
  if (!v) return 0;
  if (CFGetTypeID(v) == CFNumberGetTypeID()) { SInt32 n = 0; CFNumberGetValue(v, kCFNumberSInt32Type, &n); return (UInt32)n; }
  if (CFGetTypeID(v) == CFStringGetTypeID()) {
    char b[8] = { 0 };
    if (!CFStringGetCString(v, b, sizeof b, kCFStringEncodingMacRoman) || strlen(b) != 4) return 0;
    return ((UInt32)(uint8_t)b[0] << 24) | ((UInt32)(uint8_t)b[1] << 16) | ((UInt32)(uint8_t)b[2] << 8) | (uint8_t)b[3];
  }
  return 0;
}
static int cfstr(CFTypeRef v, char *out, size_t cap) {
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return 0;
  return CFStringGetCString(v, out, cap, kCFStringEncodingUTF8);
}
static CFPropertyListRef read_plist(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > (8 << 20)) { fclose(f); return NULL; }
  UInt8 *b = malloc(n);
  if (!b || fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  fclose(f);
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, b, n); free(b);
  CFPropertyListRef pl = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL);
  CFRelease(data);
  return pl;
}

// Liest <dir>/<x>.component/Contents/Info.plist, Schlüssel AudioComponents (AUv2 mit factoryFunction).
static void scan_dir(const char *dir) {
  DIR *dp = opendir(dir);
  if (!dp) return;
  struct dirent *e;
  while ((e = readdir(dp))) {
    size_t l = strlen(e->d_name);
    if (l < 11 || strcmp(e->d_name + l - 10, ".component")) continue;
    char path[1200], exec[256] = { 0 };
    snprintf(path, sizeof path, "%s/%s/Contents/Info.plist", dir, e->d_name);
    CFPropertyListRef pl = read_plist(path);
    if (!pl) continue;
    if (CFGetTypeID(pl) == CFDictionaryGetTypeID()) {
      cfstr(CFDictionaryGetValue(pl, CFSTR("CFBundleExecutable")), exec, sizeof exec);
      CFTypeRef arr = CFDictionaryGetValue(pl, CFSTR("AudioComponents"));
      if (exec[0] && arr && CFGetTypeID(arr) == CFArrayGetTypeID()) {
        for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
          CFTypeRef d = CFArrayGetValueAtIndex(arr, i);
          if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) continue;
          AudioComponentDescription cd = { fourcc(CFDictionaryGetValue(d, CFSTR("type"))), fourcc(CFDictionaryGetValue(d, CFSTR("subtype"))),
                                           fourcc(CFDictionaryGetValue(d, CFSTR("manufacturer"))), 0, 0 };
          char fn[128] = { 0 }, nm[128] = { 0 }; SInt32 ver = 0;
          if (!cfstr(CFDictionaryGetValue(d, CFSTR("factoryFunction")), fn, sizeof fn) || !cd.componentType) continue;
          cfstr(CFDictionaryGetValue(d, CFSTR("name")), nm, sizeof nm);
          CFTypeRef v = CFDictionaryGetValue(d, CFSTR("version"));
          if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberSInt32Type, &ver);
          char ex[1200]; snprintf(ex, sizeof ex, "%s/%s/Contents/MacOS/%s", dir, e->d_name, exec);
          reg_add(&cd, nm, (UInt32)ver, NULL, ex, fn);
        }
      }
    }
    CFRelease(pl);
  }
  closedir(dp);
}

static AudioComponentPlugInInterface *out_factory(const AudioComponentDescription *d);
static void reg_init(void) {
  pthread_mutex_lock(&reg_lk);
  if (!reg_scanned) {
    reg_scanned = 1;
    AudioComponentDescription o = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    reg_add(&o, "Apple: Default Output", 0x10000, out_factory, NULL, NULL);
    o.componentSubType = kAudioUnitSubType_SystemOutput; reg_add(&o, "Apple: System Output", 0x10000, out_factory, NULL, NULL);
    o.componentSubType = kAudioUnitSubType_HALOutput; reg_add(&o, "Apple: AUHAL", 0x10000, out_factory, NULL, NULL);
    const char *ev = getenv("CLOKE_AU_PATH");     // Test: weitere Verzeichnisse, durch ':' getrennt
    if (ev) { char *cp = strdup(ev), *sv = NULL; for (char *t = strtok_r(cp, ":", &sv); t; t = strtok_r(NULL, ":", &sv)) scan_dir(t); free(cp); }
    scan_dir("/Library/Audio/Plug-Ins/Components");
    scan_dir("/System/Library/Components");
    const char *home = getenv("HOME");
    if (home) { char p[1100]; snprintf(p, sizeof p, "%s/Library/Audio/Plug-Ins/Components", home); scan_dir(p); }
  }
  pthread_mutex_unlock(&reg_lk);
}

static AudioComponent at_FindNext(AudioComponent prev, const AudioComponentDescription *d) {
  reg_init();
  AudioComponent c = prev ? prev->next : reg_head;
  for (; c; c = c->next)
    if ((!d->componentType || d->componentType == c->d.componentType) && (!d->componentSubType || d->componentSubType == c->d.componentSubType) &&
        (!d->componentManufacturer || d->componentManufacturer == c->d.componentManufacturer)) return c;
  return NULL;
}
UInt32 AudioComponentCount(const AudioComponentDescription *d) {
  UInt32 n = 0; for (AudioComponent c = AudioComponentFindNext(NULL, d); c; c = AudioComponentFindNext(c, d)) n++;
  return n;
}
OSStatus AudioComponentGetDescription(AudioComponent c, AudioComponentDescription *d) {
  if (!c || !d) return kAudio_ParamError;
  *d = c->d; return 0;
}
OSStatus AudioComponentGetVersion(AudioComponent c, UInt32 *v) {
  if (!c || !v) return kAudio_ParamError;
  *v = c->version; return 0;
}
OSStatus AudioComponentCopyName(AudioComponent c, CFStringRef *name) {
  if (!c || !name) return kAudio_ParamError;
  *name = CFStringCreateWithCString(kCFAllocatorDefault, c->name, kCFStringEncodingUTF8);
  return *name ? 0 : kAudio_MemFullError;
}
OSStatus AudioComponentCopyConfigurationInfo(AudioComponent c, CFDictionaryRef *info) {
  (void)c; if (info) *info = NULL; return kAudio_UnimplementedError;
}
AudioComponent AudioComponentRegister(const AudioComponentDescription *d, CFStringRef name, UInt32 ver, AudioComponentFactoryFunction f) {
  reg_init();
  char nm[128] = { 0 }; cfstr(name, nm, sizeof nm);
  pthread_mutex_lock(&reg_lk);
  AudioComponent c = reg_add(d, nm, ver, f, NULL, NULL);
  pthread_mutex_unlock(&reg_lk);
  return c;
}

static OSStatus at_InstanceNew(AudioComponent c, AudioComponentInstance *out) {
  if (!c || !out) return kAudio_ParamError;
  AudioComponentFactoryFunction f = c->factory;
  if (!f) {
    if (!c->dl) c->dl = dlopen(c->exec, RTLD_NOW | RTLD_LOCAL);
    if (c->dl) f = (AudioComponentFactoryFunction)dlsym(c->dl, c->fname);
    if (!f) { fprintf(stderr, "AudioToolbox: %s: %s\n", c->exec, dlerror()); return kAudioComponentErr_InstanceInvalidated; }
    c->factory = f;
  }
  AudioComponentInstance in = calloc(1, sizeof *in);
  if (!in) return kAudio_MemFullError;
  in->comp = c;
  in->iface = f(&c->d);
  if (!in->iface) { free(in); return kAudioComponentErr_InstanceInvalidated; }
  OSStatus r = in->iface->Open(in->iface, in);
  if (r) { free(in); return r; }
  *out = in;
  return 0;
}
void AudioComponentInstantiate(AudioComponent c, AudioComponentInstantiationOptions o, void (^done)(AudioComponentInstance, OSStatus)) {
  (void)o;
  AudioComponentInstance in = NULL; OSStatus r = AudioComponentInstanceNew(c, &in);
  if (done) done(r ? NULL : in, r);
}
OSStatus AudioComponentInstanceDispose(AudioComponentInstance in) {
  if (!in) return kAudio_ParamError;
  in->iface->Close(in->iface);
  free(in);
  return 0;
}
AudioComponent AudioComponentInstanceGetComponent(AudioComponentInstance in) { return in ? in->comp : NULL; }
Boolean AudioComponentInstanceCanDo(AudioComponentInstance in, SInt16 sel) { return in && in->iface->Lookup(sel) != NULL; }

// --- AudioUnit-C-API: Weiterleitung an die Plug-in-Methode ------------------------------------
#define METHOD(u, sel, T) ((T)((u) ? (u)->iface->Lookup(sel) : NULL))

static OSStatus at_Initialize(AudioUnit u) {
  OSStatus (*m)(void *) = METHOD(u, kAudioUnitInitializeSelect, OSStatus (*)(void *));
  return m ? m(u->iface) : kAudio_UnimplementedError;
}
OSStatus AudioUnitUninitialize(AudioUnit u) {
  OSStatus (*m)(void *) = METHOD(u, kAudioUnitUninitializeSelect, OSStatus (*)(void *));
  return m ? m(u->iface) : kAudio_UnimplementedError;
}
OSStatus AudioUnitGetPropertyInfo(AudioUnit u, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, UInt32 *size, Boolean *w) {
  OSStatus (*m)(void *, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, UInt32 *, Boolean *) =
    METHOD(u, kAudioUnitGetPropertyInfoSelect, OSStatus (*)(void *, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, UInt32 *, Boolean *));
  return m ? m(u->iface, p, s, e, size, w) : kAudio_UnimplementedError;
}
static OSStatus at_GetProperty(AudioUnit u, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, void *data, UInt32 *size) {
  OSStatus (*m)(void *, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, void *, UInt32 *) =
    METHOD(u, kAudioUnitGetPropertySelect, OSStatus (*)(void *, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, void *, UInt32 *));
  return m ? m(u->iface, p, s, e, data, size) : kAudio_UnimplementedError;
}
static OSStatus at_SetProperty(AudioUnit u, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, const void *data, UInt32 size) {
  OSStatus (*m)(void *, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *, UInt32) =
    METHOD(u, kAudioUnitSetPropertySelect, OSStatus (*)(void *, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *, UInt32));
  return m ? m(u->iface, p, s, e, data, size) : kAudio_UnimplementedError;
}
OSStatus AudioUnitAddPropertyListener(AudioUnit u, AudioUnitPropertyID p, AudioUnitPropertyListenerProc proc, void *ud) {
  OSStatus (*m)(void *, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void *) =
    METHOD(u, kAudioUnitAddPropertyListenerSelect, OSStatus (*)(void *, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void *));
  return m ? m(u->iface, p, proc, ud) : kAudio_UnimplementedError;
}
OSStatus AudioUnitRemovePropertyListenerWithUserData(AudioUnit u, AudioUnitPropertyID p, AudioUnitPropertyListenerProc proc, void *ud) {
  OSStatus (*m)(void *, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void *) =
    METHOD(u, kAudioUnitRemovePropertyListenerWithUserDataSelect, OSStatus (*)(void *, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void *));
  return m ? m(u->iface, p, proc, ud) : kAudio_UnimplementedError;
}
OSStatus AudioUnitAddRenderNotify(AudioUnit u, AURenderCallback proc, void *ud) {
  OSStatus (*m)(void *, AURenderCallback, void *) = METHOD(u, kAudioUnitAddRenderNotifySelect, OSStatus (*)(void *, AURenderCallback, void *));
  return m ? m(u->iface, proc, ud) : kAudio_UnimplementedError;
}
OSStatus AudioUnitRemoveRenderNotify(AudioUnit u, AURenderCallback proc, void *ud) {
  OSStatus (*m)(void *, AURenderCallback, void *) = METHOD(u, kAudioUnitRemoveRenderNotifySelect, OSStatus (*)(void *, AURenderCallback, void *));
  return m ? m(u->iface, proc, ud) : kAudio_UnimplementedError;
}
OSStatus AudioUnitGetParameter(AudioUnit u, AudioUnitParameterID p, AudioUnitScope s, AudioUnitElement e, AudioUnitParameterValue *v) {
  OSStatus (*m)(void *, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue *) =
    METHOD(u, kAudioUnitGetParameterSelect, OSStatus (*)(void *, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue *));
  return m ? m(u->iface, p, s, e, v) : kAudio_UnimplementedError;
}
OSStatus AudioUnitSetParameter(AudioUnit u, AudioUnitParameterID p, AudioUnitScope s, AudioUnitElement e, AudioUnitParameterValue v, UInt32 off) {
  OSStatus (*m)(void *, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue, UInt32) =
    METHOD(u, kAudioUnitSetParameterSelect, OSStatus (*)(void *, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, AudioUnitParameterValue, UInt32));
  return m ? m(u->iface, p, s, e, v, off) : kAudio_UnimplementedError;
}
OSStatus AudioUnitScheduleParameters(AudioUnit u, const AudioUnitParameterEvent *ev, UInt32 n) {
  OSStatus (*m)(void *, const AudioUnitParameterEvent *, UInt32) =
    METHOD(u, kAudioUnitScheduleParametersSelect, OSStatus (*)(void *, const AudioUnitParameterEvent *, UInt32));
  return m ? m(u->iface, ev, n) : kAudio_UnimplementedError;
}
OSStatus AudioUnitRender(AudioUnit u, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
  OSStatus (*m)(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *) =
    METHOD(u, kAudioUnitRenderSelect, OSStatus (*)(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *));
  return m ? m(u->iface, fl, ts, bus, n, io) : kAudio_UnimplementedError;
}
OSStatus AudioUnitReset(AudioUnit u, AudioUnitScope s, AudioUnitElement e) {
  OSStatus (*m)(void *, AudioUnitScope, AudioUnitElement) = METHOD(u, kAudioUnitResetSelect, OSStatus (*)(void *, AudioUnitScope, AudioUnitElement));
  return m ? m(u->iface, s, e) : kAudio_UnimplementedError;
}
static OSStatus at_OutputStart(AudioUnit u) {
  OSStatus (*m)(void *) = METHOD(u, kAudioOutputUnitStartSelect, OSStatus (*)(void *));
  return m ? m(u->iface) : kAudio_UnimplementedError;
}
OSStatus AudioOutputUnitStop(AudioUnit u) {
  OSStatus (*m)(void *) = METHOD(u, kAudioOutputUnitStopSelect, OSStatus (*)(void *));
  return m ? m(u->iface) : kAudio_UnimplementedError;
}
OSStatus MusicDeviceMIDIEvent(MusicDeviceComponent u, UInt32 st, UInt32 d1, UInt32 d2, UInt32 off) {
  OSStatus (*m)(void *, UInt32, UInt32, UInt32, UInt32) = METHOD(u, kMusicDeviceMIDIEventSelect, OSStatus (*)(void *, UInt32, UInt32, UInt32, UInt32));
  return m ? m(u->iface, st, d1, d2, off) : kAudio_UnimplementedError;
}
OSStatus MusicDeviceSysEx(MusicDeviceComponent u, const UInt8 *data, UInt32 n) {
  OSStatus (*m)(void *, const UInt8 *, UInt32) = METHOD(u, kMusicDeviceSysExSelect, OSStatus (*)(void *, const UInt8 *, UInt32));
  return m ? m(u->iface, data, n) : kAudio_UnimplementedError;
}

// --- Eingebauter Ausgabe-Unit (DefaultOutput / SystemOutput / HALOutput) --------------------------------
// Spielt über die CoreAudio-HAL (unser Shim: ein Gerät, 48 kHz Float32 stereo am IOProc). Der Eingang des
// Units (Scope Input, Element 0) hat ein frei einstellbares PCM-Format; was nicht 48 kHz/Float32/2 Kanäle ist,
// wandert durch einen AudioConverter. Quelle: Render-Callback oder Verbindung zu einem anderen Unit.
#define HW_RATE 48000.0
#define HW_CH 2
#define DEVICE_ID 2     // = Gerät des Shims; die Nummer ist dort fest
typedef struct {
  AudioComponentPlugInInterface iface;
  AudioComponentInstance inst;
  AudioStreamBasicDescription fmt;       // Eingangsformat
  AURenderCallbackStruct cb; AudioUnitConnection conn;
  UInt32 maxframes, initialized, running; AudioDeviceID dev;
  AudioDeviceIOProcID ioproc; AudioConverterRef conv;
  float vol;
  void *scratch; UInt32 scratchcap; Float64 sample;
  AudioUnit self_unit;
} Out;

static AudioStreamBasicDescription hw_fmt(void) {
  AudioStreamBasicDescription d = { HW_RATE, kAudioFormatLinearPCM, kAudioFormatFlagsNativeFloatPacked, 4 * HW_CH, 1, 4 * HW_CH, HW_CH, 32, 0 };
  return d;
}
static int is_hw(const AudioStreamBasicDescription *f) {
  return f->mFormatID == kAudioFormatLinearPCM && f->mSampleRate == HW_RATE && f->mChannelsPerFrame == HW_CH &&
         f->mBitsPerChannel == 32 && (f->mFormatFlags & (kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagIsBigEndian)) == kAudioFormatFlagIsFloat;
}

// Holt n Frames im Eingangsformat von der Quelle in buf (Größe passend zu fmt).
static OSStatus out_pull(Out *o, const AudioTimeStamp *ts, UInt32 n, AudioBufferList *abl) {
  AudioUnitRenderActionFlags fl = 0;
  if (o->cb.inputProc) return o->cb.inputProc(o->cb.inputProcRefCon, &fl, ts, 0, n, abl);
  if (o->conn.sourceAudioUnit) return AudioUnitRender(o->conn.sourceAudioUnit, &fl, ts, o->conn.sourceOutputNumber, n, abl);
  return kAudioUnitErr_NoConnection;
}
static OSStatus conv_input(AudioConverterRef c, UInt32 *np, AudioBufferList *io, AudioStreamPacketDescription **pd, void *ud) {
  Out *o = ud; (void)c; (void)pd;
  UInt32 n = *np < o->maxframes ? *np : o->maxframes, bpf = o->fmt.mBytesPerFrame;
  UInt32 nb = at_pcm_nbuffers(&o->fmt), ch = nb == 1 ? o->fmt.mChannelsPerFrame : 1;
  if ((size_t)n * bpf * nb > o->scratchcap) n = o->scratchcap / (bpf * nb);
  for (UInt32 b = 0; b < nb; b++) io->mBuffers[b] = (AudioBuffer){ ch, n * bpf, (char *)o->scratch + (size_t)b * n * bpf };
  AudioTimeStamp ts = { .mSampleTime = o->sample, .mFlags = kAudioTimeStampSampleTimeValid };
  OSStatus r = out_pull(o, &ts, n, io);
  if (r) { memset(o->scratch, 0, (size_t)n * bpf * nb); r = 0; }
  o->sample += n;
  *np = n;
  return r;
}
static OSStatus out_ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *it,
                           AudioBufferList *out, const AudioTimeStamp *ot, void *client) {
  Out *o = client; (void)dev; (void)in; (void)it; (void)ot;
  UInt32 n = out->mBuffers[0].mDataByteSize / (HW_CH * sizeof(float));
  float *dst = out->mBuffers[0].mData;
  if (!n) return 0;
  if (!o->conv) {
    AudioBufferList abl = { 1, { { HW_CH, n * HW_CH * (UInt32)sizeof(float), dst } } };
    AudioTimeStamp ts = *now; ts.mSampleTime = o->sample; ts.mFlags |= kAudioTimeStampSampleTimeValid;
    if (out_pull(o, &ts, n, &abl)) memset(dst, 0, n * HW_CH * sizeof(float));
    else if (abl.mBuffers[0].mData != dst) memcpy(dst, abl.mBuffers[0].mData, n * HW_CH * sizeof(float));
    o->sample += n;
  } else {
    AudioBufferList abl = { 1, { { HW_CH, n * HW_CH * (UInt32)sizeof(float), dst } } };
    UInt32 got = n;
    AudioConverterFillComplexBuffer(o->conv, conv_input, o, &got, &abl, NULL);
    if (got < n) memset(dst + got * HW_CH, 0, (n - got) * HW_CH * sizeof(float));
  }
  if (o->vol != 1.0f) for (UInt32 i = 0; i < n * HW_CH; i++) dst[i] *= o->vol;
  return 0;
}

static OSStatus out_init(void *self) {
  Out *o = self;
  if (o->initialized) return 0;
  if (!o->cb.inputProc && !o->conn.sourceAudioUnit) return kAudioUnitErr_NoConnection;
  if (!at_pcm_ok(&o->fmt)) return kAudioUnitErr_FormatNotSupported;
  if (!is_hw(&o->fmt)) {
    AudioStreamBasicDescription h = hw_fmt();
    if (AudioConverterNew(&o->fmt, &h, &o->conv)) return kAudioUnitErr_FormatNotSupported;
    o->scratchcap = o->maxframes * 8 * AT_MAXCH * 4;       // reicht für Abtastratenverhältnis bis 4
    o->scratch = malloc(o->scratchcap);
    if (!o->scratch) { AudioConverterDispose(o->conv); o->conv = NULL; return kAudio_MemFullError; }
  }
  o->sample = 0;
  o->initialized = 1;
  return 0;
}
static OSStatus out_start(void *self);
static OSStatus out_stop(void *self) {
  Out *o = self;
  if (!o->running) return 0;
  AudioDeviceStop(o->dev, o->ioproc);
  AudioDeviceDestroyIOProcID(o->dev, o->ioproc);
  o->ioproc = NULL; o->running = 0;
  return 0;
}
static OSStatus out_uninit(void *self) {
  Out *o = self;
  out_stop(o);
  if (o->conv) { AudioConverterDispose(o->conv); o->conv = NULL; }
  free(o->scratch); o->scratch = NULL;
  o->initialized = 0;
  return 0;
}
static OSStatus out_start(void *self) {
  Out *o = self;
  if (!o->initialized) return kAudioUnitErr_Uninitialized;
  if (o->running) return 0;
  OSStatus r = AudioDeviceCreateIOProcID(o->dev, out_ioproc, o, &o->ioproc);
  if (r) return r;
  r = AudioDeviceStart(o->dev, o->ioproc);
  if (r) { AudioDeviceDestroyIOProcID(o->dev, o->ioproc); o->ioproc = NULL; return r; }
  o->running = 1;
  return 0;
}

// Eigenschaften: liefert Größe/Schreibbarkeit, kopiert bei data != NULL; set != NULL schreibt.
static OSStatus out_prop(Out *o, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, UInt32 *size, Boolean *wr, void *data, UInt32 cap, const void *set, UInt32 setsz) {
  UInt32 sz = 0; Boolean w = 0; UInt32 u = 0; Float64 d = 0; AudioStreamBasicDescription hf = hw_fmt(); const void *val = &u;
  switch (p) {
    case kAudioUnitProperty_StreamFormat:
      if (s == kAudioUnitScope_Input && e == 0) { val = &o->fmt; sz = sizeof o->fmt; w = !o->initialized; }
      else if (s == kAudioUnitScope_Output && e == 0) { val = &hf; sz = sizeof hf; }
      else if (s == kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
      else return kAudioUnitErr_InvalidElement;
      break;
    case kAudioUnitProperty_SampleRate:
      if (s == kAudioUnitScope_Global || e != 0) return kAudioUnitErr_InvalidElement;
      d = s == kAudioUnitScope_Input ? o->fmt.mSampleRate : HW_RATE; val = &d; sz = sizeof d; w = s == kAudioUnitScope_Input && !o->initialized; break;
    case kAudioUnitProperty_SetRenderCallback:
      if (s != kAudioUnitScope_Input || e != 0) return kAudioUnitErr_InvalidElement;
      val = &o->cb; sz = sizeof o->cb; w = 1; break;
    case kAudioUnitProperty_MakeConnection:
      if (s != kAudioUnitScope_Input || e != 0) return kAudioUnitErr_InvalidElement;
      val = &o->conn; sz = sizeof o->conn; w = 1; break;
    case kAudioUnitProperty_ElementCount:
      u = s == kAudioUnitScope_Global ? 1 : 1; sz = 4; break;
    case kAudioUnitProperty_MaximumFramesPerSlice:
      if (s != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
      val = &o->maxframes; sz = 4; w = 1; break;
    case kAudioUnitProperty_Latency: d = 0; val = &d; sz = sizeof d; break;
    case kAudioUnitProperty_ParameterList: sz = 0; break;
    case kAudioOutputUnitProperty_CurrentDevice: val = &o->dev; sz = 4; w = 1; break;
    case kAudioOutputUnitProperty_IsRunning: u = o->running; sz = 4; break;
    case kAudioOutputUnitProperty_HasIO: u = (s == kAudioUnitScope_Output && e == 0); sz = 4; break;
    case kAudioOutputUnitProperty_EnableIO:
      if (s == kAudioUnitScope_Output && e == 0) u = 1;
      else if (s == kAudioUnitScope_Input && e == 1) u = 0;
      else return kAudioUnitErr_InvalidElement;
      sz = 4; w = 1; break;
    default: return kAudioUnitErr_InvalidProperty;
  }
  if (set) {
    if (!w) return kAudioUnitErr_PropertyNotWritable;
    if (setsz != sz) return kAudioUnitErr_InvalidPropertyValue;
    switch (p) {
      case kAudioUnitProperty_StreamFormat: {
        AudioStreamBasicDescription f = *(const AudioStreamBasicDescription *)set;
        if (!at_pcm_ok(&f)) return kAudioUnitErr_FormatNotSupported;
        if (!f.mBytesPerFrame) f.mBytesPerFrame = at_pcm_bytes_per_frame(&f);
        o->fmt = f; break; }
      case kAudioUnitProperty_SampleRate: o->fmt.mSampleRate = *(const Float64 *)set; break;
      case kAudioUnitProperty_SetRenderCallback: o->cb = *(const AURenderCallbackStruct *)set; memset(&o->conn, 0, sizeof o->conn); break;
      case kAudioUnitProperty_MakeConnection: o->conn = *(const AudioUnitConnection *)set; memset(&o->cb, 0, sizeof o->cb); break;
      case kAudioUnitProperty_MaximumFramesPerSlice: if (!*(const UInt32 *)set) return kAudioUnitErr_InvalidPropertyValue; o->maxframes = *(const UInt32 *)set; break;
      case kAudioOutputUnitProperty_CurrentDevice:
        if (*(const UInt32 *)set != DEVICE_ID) return kAudioUnitErr_InvalidPropertyValue;     // nur das eine Gerät
        break;
      case kAudioOutputUnitProperty_EnableIO:
        if ((s == kAudioUnitScope_Output) != (*(const UInt32 *)set != 0)) return kAudioUnitErr_InvalidPropertyValue;   // Ausgang an, Eingang aus
        break;
    }
    return 0;
  }
  if (size) *size = sz;
  if (wr) *wr = w;
  if (data) { if (cap < sz) return kAudioUnitErr_InvalidPropertyValue; if (sz) memcpy(data, val, sz); }
  return 0;
}
static OSStatus out_getinfo(void *self, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, UInt32 *size, Boolean *w) {
  UInt32 sz; Boolean wr;
  OSStatus r = out_prop(self, p, s, e, &sz, &wr, NULL, 0, NULL, 0);
  if (r) return r;
  if (size) *size = sz;
  if (w) *w = wr;
  return 0;
}
static OSStatus out_get(void *self, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, void *data, UInt32 *size) {
  if (!size || !data) return kAudio_ParamError;
  UInt32 sz;
  OSStatus r = out_prop(self, p, s, e, &sz, NULL, data, *size, NULL, 0);
  if (!r) *size = sz;
  return r;
}
static OSStatus out_set(void *self, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, const void *data, UInt32 size) {
  if (!data) return kAudio_ParamError;
  return out_prop(self, p, s, e, NULL, NULL, NULL, 0, data, size);
}
static OSStatus out_getparam(void *self, AudioUnitParameterID p, AudioUnitScope s, AudioUnitElement e, AudioUnitParameterValue *v) {
  Out *o = self; (void)e;
  if (p != kHALOutputParam_Volume || s != kAudioUnitScope_Global) return kAudioUnitErr_InvalidParameter;
  *v = o->vol; return 0;
}
static OSStatus out_setparam(void *self, AudioUnitParameterID p, AudioUnitScope s, AudioUnitElement e, AudioUnitParameterValue v, UInt32 off) {
  Out *o = self; (void)e; (void)off;
  if (p != kHALOutputParam_Volume || s != kAudioUnitScope_Global) return kAudioUnitErr_InvalidParameter;
  o->vol = v < 0 ? 0 : v > 1 ? 1 : v; return 0;
}
static OSStatus out_render(void *self, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
  (void)self; (void)fl; (void)ts; (void)bus; (void)n; (void)io;
  return kAudioUnitErr_InvalidElement;           // Eingang vom Gerät (EnableIO Eingang) gibt es nicht
}
static OSStatus out_reset(void *self, AudioUnitScope s, AudioUnitElement e) {
  Out *o = self; (void)s; (void)e;
  if (o->conv) AudioConverterReset(o->conv);
  return 0;
}
static OSStatus out_open(void *self, AudioComponentInstance inst) {
  Out *o = self; o->inst = inst; return 0;
}
static OSStatus out_close(void *self) {
  Out *o = self;
  out_uninit(o);
  free(o);
  return 0;
}
static AudioComponentMethod out_lookup(SInt16 sel) {
  switch (sel) {
    case kAudioUnitInitializeSelect: return (AudioComponentMethod)out_init;
    case kAudioUnitUninitializeSelect: return (AudioComponentMethod)out_uninit;
    case kAudioUnitGetPropertyInfoSelect: return (AudioComponentMethod)out_getinfo;
    case kAudioUnitGetPropertySelect: return (AudioComponentMethod)out_get;
    case kAudioUnitSetPropertySelect: return (AudioComponentMethod)out_set;
    case kAudioUnitGetParameterSelect: return (AudioComponentMethod)out_getparam;
    case kAudioUnitSetParameterSelect: return (AudioComponentMethod)out_setparam;
    case kAudioUnitRenderSelect: return (AudioComponentMethod)out_render;
    case kAudioUnitResetSelect: return (AudioComponentMethod)out_reset;
    case kAudioOutputUnitStartSelect: return (AudioComponentMethod)out_start;
    case kAudioOutputUnitStopSelect: return (AudioComponentMethod)out_stop;
    default: return NULL;
  }
}
static AudioComponentPlugInInterface *out_factory(const AudioComponentDescription *d) {
  (void)d;
  Out *o = calloc(1, sizeof *o);
  if (!o) return NULL;
  o->iface.Open = out_open; o->iface.Close = out_close; o->iface.Lookup = out_lookup;
  o->fmt = hw_fmt();
  o->maxframes = 4096; o->vol = 1.0f; o->dev = DEVICE_ID;
  return &o->iface;
}

// Alte Komfortfunktion aus DefaultAudioOutput.h
OSStatus OpenDefaultAudioOutput(AudioUnit *out) {
  AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
  AudioComponent c = AudioComponentFindNext(NULL, &d);
  if (!c) return kAudioUnitErr_InvalidProperty;
  return AudioComponentInstanceNew(c, out);
}

// --- CLOKE_AT_TRACE=1: Einstiege, die Logics Audio-Engine benutzt (Suche, Instanz, Initialize, Start, fehlgeschlagene Eigenschaften) ---
static int at_trace(void) { static int t = -1; if (t < 0) t = getenv("CLOKE_AT_TRACE") ? atoi(getenv("CLOKE_AT_TRACE")) : 0; return t; }
static const char *cc4(UInt32 v, char o[5]) { for (int i = 0; i < 4; i++) { char c = (char)(v >> (24 - 8 * i)); o[i] = c >= 32 && c < 127 ? c : '?'; } o[4] = 0; return o; }
AudioComponent AudioComponentFindNext(AudioComponent prev, const AudioComponentDescription *d) {
  AudioComponent c = at_FindNext(prev, d);
  if (at_trace() && !prev) { char a[5], b[5], m[5];
    fprintf(stderr, "[AudioToolbox] FindNext '%s' '%s' '%s' -> %p\n", cc4(d->componentType, a), cc4(d->componentSubType, b), cc4(d->componentManufacturer, m), (void *)c); }
  return c;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
  OSStatus r = at_InstanceNew(c, out);
  if (at_trace() && c) { char a[5], b[5]; fprintf(stderr, "[AudioToolbox] InstanceNew '%s' '%s' -> %d %p\n", cc4(c->d.componentType, a), cc4(c->d.componentSubType, b), (int)r, out ? (void *)*out : NULL); }
  return r;
}
OSStatus AudioUnitInitialize(AudioUnit u) {
  OSStatus r = at_Initialize(u);
  if (at_trace()) fprintf(stderr, "[AudioToolbox] Initialize %p -> %d\n", (void *)u, (int)r);
  return r;
}
OSStatus AudioUnitGetProperty(AudioUnit u, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, void *data, UInt32 *size) {
  OSStatus r = at_GetProperty(u, p, s, e, data, size);
  if (at_trace() >= (r ? 1 : 2)) fprintf(stderr, "[AudioToolbox] GetProperty %p %u scope %u el %u -> %d\n", (void *)u, (unsigned)p, (unsigned)s, (unsigned)e, (int)r);
  return r;
}
OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, const void *data, UInt32 size) {
  OSStatus r = at_SetProperty(u, p, s, e, data, size);
  if (at_trace() >= (r ? 1 : 2)) fprintf(stderr, "[AudioToolbox] SetProperty %p %u scope %u el %u size %u -> %d\n", (void *)u, (unsigned)p, (unsigned)s, (unsigned)e, (unsigned)size, (int)r);
  return r;
}
OSStatus AudioOutputUnitStart(AudioUnit u) {
  OSStatus r = at_OutputStart(u);
  if (at_trace()) fprintf(stderr, "[AudioToolbox] OutputUnitStart %p -> %d\n", (void *)u, (int)r);
  return r;
}
