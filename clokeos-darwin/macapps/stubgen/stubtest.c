// Prüfprogramm für die Stub-Frameworks (macapps/stubs.sh): lädt jedes mit dlopen und ruft einige Symbole auf.
//   DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks CLOKE_STUB_VERBOSE=1 stubtest
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <objc/runtime.h>
#include <objc/message.h>

static int fails, checks;
#define CHECK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL  %s\n", what); } else printf("ok    %s\n", what); } while (0)

static const char *fw[] = {
  "Metal", "MetalKit", "MetalPerformanceShaders", "ScreenCaptureKit", "StoreKit", "Network", "CryptoKit",
  "UserNotifications", "GameController", "MultipeerConnectivity", "QuickLookUI", "PDFKit", "GameplayKit",
  "CoreMotion", "iTunesLibrary", "DiscRecording", "UniformTypeIdentifiers", "CoreML", "WebKit", "JavaScriptCore",
  "Security", "AVFoundation", "AVFAudio", "CoreMedia", "CoreVideo", "VideoToolbox", "MediaToolbox", "AVKit",
  "CoreMIDI", "Cinematic", 0 };
static const char *priv[] = { "PackageKit", "AssetCacheServices", 0 };

static void *open_fw(const char *dir, const char *n) {
  char p[512];
  snprintf(p, sizeof p, "/System/Library/%s/%s.framework/Versions/A/%s", dir, n, n);
  void *h = dlopen(p, RTLD_NOW);
  checks++;
  if (!h) { fails++; printf("FAIL  dlopen %s: %s\n", n, dlerror()); } else printf("ok    dlopen %s\n", n);
  return h;
}

typedef void *(*fn_p)(void);
typedef int (*cfgetcs_t)(const void *, char *, long, unsigned);

int main(void) {
  setvbuf(stdout, 0, _IONBF, 0);
  void *h[64] = {0}; int n = 0;
  for (int i = 0; fw[i]; i++) h[n++] = open_fw("Frameworks", fw[i]);
  for (int i = 0; priv[i]; i++) h[n++] = open_fw("PrivateFrameworks", priv[i]);
  void *mg = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
  CHECK(mg != 0, "dlopen libMobileGestalt");

  // Metal: keine GPU
  void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Versions/A/Metal", RTLD_NOW);
  fn_p dev = (fn_p)dlsym(metal, "MTLCreateSystemDefaultDevice");
  CHECK(dev && dev() == 0, "MTLCreateSystemDefaultDevice() == nil");
  fn_p all = (fn_p)dlsym(metal, "MTLCopyAllDevices");
  void *arr = all ? all() : 0;
  unsigned long n0 = arr ? ((unsigned long (*)(id, SEL))objc_msgSend)((id)arr, sel_registerName("count")) : 99;
  CHECK(arr && n0 == 0, "MTLCopyAllDevices() == leeres NSArray");
  void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation", RTLD_NOW);

  // C-Funktionen und Konstanten
  void *cm = dlopen("/System/Library/Frameworks/CoreMedia.framework/Versions/A/CoreMedia", RTLD_NOW);
  struct { long long v; int ts; unsigned fl; long long ep; } *zero = dlsym(cm, "kCMTimeZero");
  CHECK(zero && zero->ts == 1 && zero->fl == 1, "kCMTimeZero = 0/1 gültig");
  CHECK(((int (*)(void))dlsym(cm, "CMTimeCompare"))() == 0, "CMTimeCompare() == 0");
  void *gc = dlopen("/System/Library/Frameworks/GameController.framework/Versions/A/GameController", RTLD_NOW);
  const void **note = dlsym(gc, "GCControllerDidConnectNotification");
  cfgetcs_t gets = (cfgetcs_t)dlsym(cf, "CFStringGetCString");
  char buf[128] = "";
  CHECK(note && *note && gets && gets(*note, buf, sizeof buf, 0x08000100) && !strcmp(buf, "GCControllerDidConnectNotification"),
        "NSString-Konstante = ihr Name");
  CHECK(((int (*)(void))dlsym(dlopen("/System/Library/Frameworks/Security.framework/Versions/A/Security", RTLD_NOW), "AuthorizationCreate"))() == -4,
        "AuthorizationCreate() == errSecUnimplemented");
  CHECK(mg && ((int (*)(void))dlsym(mg, "MGGetBoolAnswer"))() == 0, "MGGetBoolAnswer() == 0");

  // ObjC-Klassen: Nachrichten an sie und ihre Objekte stürzen nicht ab und liefern nil
  const char *cls[] = { "MTKView", "MTLTextureDescriptor", "SCShareableContent", "WKWebView", "GCController", "UTType",
                        "PKInstallClient", "AVPlayerView", "QLPreviewPanel", "AVAudioEngine", "UNNotificationRequest", 0 };
  for (int i = 0; cls[i]; i++) {
    Class c = objc_getClass(cls[i]);
    char w[128]; snprintf(w, sizeof w, "Klasse %s: +alloc, -initWithFrame:, -fooBar: liefern nil/0", cls[i]);
    int ok = c != 0;
    if (ok) {
      id o = ((id (*)(Class, SEL))objc_msgSend)(c, sel_registerName("alloc"));
      id r1 = ((id (*)(id, SEL, int))objc_msgSend)(o, sel_registerName("initWithFrame:"), 1);
      id r2 = ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName("fooBar:"));
      id r3 = ((id (*)(Class, SEL))objc_msgSend)(c, sel_registerName("sharedThing"));
      ok = o != 0 && r2 == 0 && r3 == 0 && (r1 == 0 || r1 == o);
    }
    CHECK(ok, w);
  }
  printf("%d Prüfungen, %d fehlgeschlagen\n", checks, fails);
  return fails != 0;
}
