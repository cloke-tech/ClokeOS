/*
 * clokeintel: Xorg-Grafiktreiber von ClokeOS für den eigenen Framebuffer-Treiber (kext/ClokeIntelGfx,
 * ClokeIntelFB). Ersetzt PureDarwins puredarwingop, wo ClokeIntelFB läuft.
 *
 * Aufbau wie xf86-video-dummy (busloser Treiber, MIT): X zeichnet in einen Schattenpuffer im
 * Arbeitsspeicher, geänderte Bereiche (Damage) kommen einmal je Durchlauf (BlockHandler) in den
 * Bildspeicher. Neu gegenüber puredarwingop:
 *   - Doppelpufferung: Kopiert wird in den gerade nicht angezeigten der beiden Puffer, dann schaltet
 *     /dev/clokeos_fb (CLOKEOS_FB_FLIP) im nächsten Vblank um. Weil jeder Puffer nur jedes zweite Bild
 *     bekommt, wird die Änderung dieses und des vorigen Durchlaufs kopiert. Kein Reißen mehr.
 *   - Vor dem Schreiben in einen Puffer wird gewartet, bis er nicht mehr angezeigt wird (CLOKEOS_FB_WAIT).
 * Ohne /dev/clokeos_fb (anderer Framebuffer, z. B. IOGOPFramebuffer in QEMU) wie bisher direkt in den
 * einen Bildspeicher.
 *
 * Den Bildspeicher bildet der Treiber über die IOKit-Schnittstelle von IOFramebuffer ab (Speicher-Typ
 * kIOFBVRAMMemory), die Geometrie kommt aus GetCurrentDisplayMode/GetPixelInformation.
 */
#include "xorg-server.h"
#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86str.h"
#include "xf86Module.h"
#include "fb.h"
#include "damage.h"
#include "micmap.h"
#include "mipointer.h"
#include "colormapst.h"

#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "clokeos_fb.h"

#define CI_NAME "clokeintel"
#define CI_VERSION 1000

/* IOFramebuffer-User-Client (IOFramebufferUserClient): Selektoren und Speicher-Typ */
#define kCIGetPixelInformation 1
#define kCIGetCurrentDisplayMode 2
#define kCIVRAMMemory 110

typedef struct {
  uint32_t bytesPerRow, bytesPerPlane, bitsPerPixel, pixelType, componentCount, bitsPerComponent;
  uint32_t componentMasks[16];
  char pixelFormat[64];
  uint32_t flags, activeWidth, activeHeight, reserved[2];
} CIPixelInfo;

typedef struct {
  io_service_t service;
  io_connect_t connect;
  mach_vm_address_t vram;
  mach_vm_size_t vramSize;
  uint32_t width, height, stride;
  int ctl; /* /dev/clokeos_fb oder -1 */
  struct clokeos_fb_info info;
  unsigned back;    /* Puffer, in den als nächstes kopiert wird */
  RegionRec prev;   /* Änderung des vorigen Durchlaufs (fehlt dem anderen Puffer noch) */
  void *shadow;
  DamagePtr damage;
  unsigned long flips, flipWaits;
  /* Messung für clokeos-diag (alle 10 s, wenn sich etwas tut) */
  double periodStart, lastFlip, copyMs, waitMs, maxGap;
  unsigned long periodFlips, periodBytes;
  CreateScreenResourcesProcPtr CreateScreenResources;
  ScreenBlockHandlerProcPtr BlockHandler;
  CloseScreenProcPtr CloseScreen;
} CIRec, *CIPtr;

static CIPtr CIGet(ScrnInfoPtr s) {
  if (!s->driverPrivate) s->driverPrivate = xnfcalloc(sizeof(CIRec), 1);
  return (CIPtr)s->driverPrivate;
}

/* ---- Bildspeicher öffnen ------------------------------------------------------------------------- */

static void CIClose(CIPtr p) {
  if (p->vram && p->connect) IOConnectUnmapMemory64(p->connect, kCIVRAMMemory, mach_task_self(), p->vram);
  if (p->connect) IOServiceClose(p->connect);
  if (p->service) IOObjectRelease(p->service);
  if (p->ctl >= 0) close(p->ctl);
  p->vram = 0, p->connect = 0, p->service = 0, p->ctl = -1;
}

static Bool CIOpen(ScrnInfoPtr pScrn, CIPtr p) {
  static const char *classes[] = {"ClokeIntelFB", "IOGOPFramebuffer", "IOFramebuffer"};
  mach_port_t master = MACH_PORT_NULL;
  p->ctl = -1;
  IOMasterPort(MACH_PORT_NULL, &master);
  for (unsigned i = 0; i < sizeof classes / sizeof classes[0] && !p->service; i++) {
    p->service = IOServiceGetMatchingService(master, IOServiceMatching(classes[i]));
    if (p->service) xf86DrvMsg(pScrn->scrnIndex, X_INFO, "framebuffer service %s\n", classes[i]);
  }
  if (!p->service) {
    xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "no IOFramebuffer\n");
    return FALSE;
  }
  if (IOServiceOpen(p->service, mach_task_self(), 0, &p->connect) != KERN_SUCCESS) {
    xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "IOServiceOpen failed\n");
    return FALSE;
  }
  uint64_t out[2] = {0, 0};
  uint32_t nout = 2;
  if (IOConnectCallScalarMethod(p->connect, kCIGetCurrentDisplayMode, NULL, 0, out, &nout) != KERN_SUCCESS)
    return FALSE;
  uint64_t in[3] = {out[0], out[1], 0};
  CIPixelInfo pi;
  size_t piSize = sizeof pi;
  memset(&pi, 0, sizeof pi);
  if (IOConnectCallMethod(p->connect, kCIGetPixelInformation, in, 3, NULL, 0, NULL, NULL, &pi, &piSize) != KERN_SUCCESS)
    return FALSE;
  if (pi.bitsPerPixel != 32) {
    xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "%u bpp, only 32 supported\n", pi.bitsPerPixel);
    return FALSE;
  }
  if (IOConnectMapMemory64(p->connect, kCIVRAMMemory, mach_task_self(), &p->vram, &p->vramSize, kIOMapAnywhere) !=
      KERN_SUCCESS)
    return FALSE;
  p->width = pi.activeWidth, p->height = pi.activeHeight, p->stride = pi.bytesPerRow;

  /* Doppelpufferung nur mit ClokeIntelFB und passender Geometrie */
  p->ctl = open("/dev/clokeos_fb", O_RDWR);
  if (p->ctl >= 0 && (ioctl(p->ctl, CLOKEOS_FB_GET_INFO, &p->info) != 0 || p->info.version != CLOKEOS_FB_VERSION ||
                      p->info.width != p->width || p->info.stride != p->stride || p->info.nbuf < 2 ||
                      p->info.buf_offset[1] + (uint64_t)p->stride * p->height > p->vramSize)) {
    close(p->ctl);
    p->ctl = -1;
  }
  xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%ux%u stride %u, VRAM %llu KB, %s\n", p->width, p->height, p->stride,
             (unsigned long long)(p->vramSize >> 10),
             p->ctl >= 0 ? "double buffered, flips on vblank" : "single buffer");
  return TRUE;
}

/* ---- Kopieren und Umschalten --------------------------------------------------------------------- */

static double CINow(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* gibt die Zahl der kopierten Bytes zurück */
static unsigned long CICopy(ScrnInfoPtr pScrn, CIPtr p, uint8_t *dst, RegionPtr region) {
  unsigned long bytes = 0;
  int n = RegionNumRects(region);
  BoxPtr b = RegionRects(region);
  for (; n--; b++) {
    int x1 = max(b->x1, 0), y1 = max(b->y1, 0);
    int x2 = min(b->x2, (int)p->width), y2 = min(b->y2, (int)p->height);
    if (x2 <= x1 || y2 <= y1) continue;
    size_t w = (size_t)(x2 - x1) * 4;
    for (int y = y1; y < y2; y++) {
      size_t off = (size_t)y * p->stride + (size_t)x1 * 4;
      memcpy(dst + off, (uint8_t *)p->shadow + off, w);
    }
    bytes += w * (size_t)(y2 - y1);
  }
  return bytes;
}

static void CIBlockHandler(ScreenPtr pScreen, void *timeout) {
  ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
  CIPtr p = CIGet(pScrn);
  pScreen->BlockHandler = p->BlockHandler;
  (*pScreen->BlockHandler)(pScreen, timeout);
  p->BlockHandler = pScreen->BlockHandler;
  pScreen->BlockHandler = CIBlockHandler;
  if (!p->damage) return;
  RegionPtr dmg = DamageRegion(p->damage);
  if (!RegionNotEmpty(dmg)) return;

  if (p->ctl < 0) {
    CICopy(pScrn, p, (uint8_t *)(uintptr_t)p->vram, dmg);
    DamageEmpty(p->damage);
    return;
  }
  /* Der Puffer, in den jetzt kopiert wird, war beim vorigen Umschalten der angezeigte: warten, bis die
   * Anzeige wirklich auf den anderen gewechselt hat (meist schon geschehen) */
  double t0 = CINow();
  uint32_t other = 1 - p->back;
  if (ioctl(p->ctl, CLOKEOS_FB_WAIT, &other) != 0) p->flipWaits++;
  double t1 = CINow();
  RegionRec both;
  RegionNull(&both);
  RegionUnion(&both, dmg, &p->prev);
  p->periodBytes += CICopy(pScrn, p, (uint8_t *)(uintptr_t)p->vram + p->info.buf_offset[p->back], &both);
  RegionUninit(&both);
  double t2 = CINow();
  p->waitMs += (t1 - t0) * 1e3, p->copyMs += (t2 - t1) * 1e3;
  if (p->lastFlip && t2 - p->lastFlip > p->maxGap) p->maxGap = t2 - p->lastFlip;
  p->lastFlip = t2;
  p->periodFlips++;
  if (!p->periodStart) p->periodStart = t2;
  if (t2 - p->periodStart >= 10) {
    double secs = t2 - p->periodStart;
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "perf: %.1f flips/s, copy %.2f ms, wait %.2f ms per flip, %.1f MB/s, longest gap %.0f ms\n",
               p->periodFlips / secs, p->copyMs / p->periodFlips, p->waitMs / p->periodFlips,
               p->periodBytes / secs / 1e6, p->maxGap * 1e3);
    p->periodStart = t2, p->periodFlips = 0, p->periodBytes = 0, p->copyMs = p->waitMs = p->maxGap = 0;
  }
  uint32_t b = p->back;
  if (ioctl(p->ctl, CLOKEOS_FB_FLIP, &b) == 0) {
    p->flips++;
    p->back = 1 - p->back;
    /* für clokeos-diag: läuft das Umschalten, und wie oft musste gewartet werden? */
    if (p->flips == 1 || p->flips == 100 || p->flips % 1000 == 0)
      xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%lu flips, %lu waits that timed out\n", p->flips, p->flipWaits);
  }
  RegionCopy(&p->prev, dmg);
  DamageEmpty(p->damage);
}

static void CIDamageReport(DamagePtr d, RegionPtr r, void *c) {
  (void)d, (void)r, (void)c;
}

static Bool CICreateScreenResources(ScreenPtr pScreen) {
  ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
  CIPtr p = CIGet(pScrn);
  pScreen->CreateScreenResources = p->CreateScreenResources;
  Bool ok = (*pScreen->CreateScreenResources)(pScreen);
  p->CreateScreenResources = pScreen->CreateScreenResources;
  pScreen->CreateScreenResources = CICreateScreenResources;
  if (!ok) return FALSE;
  p->damage = DamageCreate(CIDamageReport, NULL, DamageReportNonEmpty, TRUE, pScreen, NULL);
  if (!p->damage) return FALSE;
  DamageRegister(&(*pScreen->GetScreenPixmap)(pScreen)->drawable, p->damage);
  DamageSetReportAfterOp(p->damage, TRUE);
  return TRUE;
}

/* ---- Bildschirm ----------------------------------------------------------------------------------- */

static Bool CICloseScreen(ScreenPtr pScreen) {
  ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
  CIPtr p = CIGet(pScrn);
  xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%lu flips, %lu waits that timed out\n", p->flips, p->flipWaits);
  if (p->damage) {
    DamageUnregister(p->damage);
    DamageDestroy(p->damage);
    p->damage = NULL;
  }
  /* angezeigt bleibt Puffer 0 (dort zeichnen Kernel-Konsole und puredarwingop) */
  if (p->ctl >= 0) {
    uint32_t zero = 0;
    BoxRec all = {0, 0, (short)p->width, (short)p->height};
    RegionRec r;
    RegionInit(&r, &all, 1);
    CICopy(pScrn, p, (uint8_t *)(uintptr_t)p->vram, &r);
    RegionUninit(&r);
    ioctl(p->ctl, CLOKEOS_FB_FLIP, &zero);
  }
  RegionUninit(&p->prev);
  free(p->shadow);
  p->shadow = NULL;
  CIClose(p);
  pScreen->BlockHandler = p->BlockHandler;
  pScreen->CloseScreen = p->CloseScreen;
  return (*pScreen->CloseScreen)(pScreen);
}

static Bool CIScreenInit(ScreenPtr pScreen, int argc, char **argv) {
  ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
  CIPtr p = CIGet(pScrn);
  (void)argc, (void)argv;
  p->shadow = calloc(1, (size_t)p->stride * p->height);
  if (!p->shadow) return FALSE;
  RegionNull(&p->prev);
  /* schwarz in alle Puffer, X zeichnet gleich darüber */
  memset((void *)(uintptr_t)p->vram, 0, (size_t)p->vramSize);
  p->back = p->ctl >= 0 ? 1 - p->info.shown : 0;

  miClearVisualTypes();
  if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth), pScrn->rgbBits, pScrn->defaultVisual))
    return FALSE;
  if (!miSetPixmapDepths()) return FALSE;
  if (!fbScreenInit(pScreen, p->shadow, pScrn->virtualX, pScrn->virtualY, pScrn->xDpi, pScrn->yDpi,
                    pScrn->displayWidth, pScrn->bitsPerPixel))
    return FALSE;
  for (VisualPtr v = pScreen->visuals + pScreen->numVisuals; --v >= pScreen->visuals;) {
    if ((v->class | DynamicClass) == DirectColor) {
      v->offsetRed = pScrn->offset.red, v->offsetGreen = pScrn->offset.green, v->offsetBlue = pScrn->offset.blue;
      v->redMask = pScrn->mask.red, v->greenMask = pScrn->mask.green, v->blueMask = pScrn->mask.blue;
    }
  }
  if (!fbPictureInit(pScreen, NULL, 0)) return FALSE;
  xf86SetBlackWhitePixels(pScreen);
  miDCInitialize(pScreen, xf86GetPointerScreenFuncs());
  if (!miCreateDefColormap(pScreen)) return FALSE;
  xf86SetBackingStore(pScreen);
  if (!DamageSetup(pScreen)) return FALSE;
  p->CreateScreenResources = pScreen->CreateScreenResources;
  pScreen->CreateScreenResources = CICreateScreenResources;
  p->BlockHandler = pScreen->BlockHandler;
  pScreen->BlockHandler = CIBlockHandler;
  p->CloseScreen = pScreen->CloseScreen;
  pScreen->CloseScreen = CICloseScreen;
  return TRUE;
}

static Bool CIPreInit(ScrnInfoPtr pScrn, int flags) {
  if (flags & PROBE_DETECT || pScrn->numEntities != 1) return FALSE;
  CIPtr p = CIGet(pScrn);
  if (!CIOpen(pScrn, p)) {
    CIClose(p);
    return FALSE;
  }
  rgb weight = {0, 0, 0};
  Gamma gamma = {0.0, 0.0, 0.0};
  pScrn->monitor = pScrn->confScreen->monitor;
  if (!xf86SetDepthBpp(pScrn, 24, 0, 32, Support32bppFb)) return FALSE;
  xf86PrintDepthBpp(pScrn);
  if (!xf86SetWeight(pScrn, weight, weight) || !xf86SetDefaultVisual(pScrn, -1)) return FALSE;
  pScrn->progClock = TRUE;
  pScrn->rgbBits = 8;
  pScrn->chipset = (char *)CI_NAME;
  pScrn->videoRam = (int)(p->vramSize >> 10);
  xf86CollectOptions(pScrn, NULL);

  unsigned hz = p->ctl >= 0 && p->info.refresh_mhz ? p->info.refresh_mhz / 1000 : 60;
  DisplayModePtr m = xnfcalloc(sizeof(DisplayModeRec), 1);
  m->name = (char *)"native";
  m->type = M_T_DRIVER | M_T_PREFERRED;
  m->HDisplay = m->HSyncStart = m->HSyncEnd = m->HTotal = (int)p->width;
  m->VDisplay = m->VSyncStart = m->VSyncEnd = m->VTotal = (int)p->height;
  m->Clock = (int)(p->width * p->height / 1000 * hz);
  m->CrtcHDisplay = m->HDisplay;
  m->CrtcVDisplay = m->VDisplay;
  m->next = m->prev = m;
  pScrn->modes = pScrn->currentMode = m;
  pScrn->virtualX = (int)p->width;
  pScrn->virtualY = (int)p->height;
  pScrn->displayWidth = (int)(p->stride / 4);
  pScrn->xDpi = pScrn->yDpi = 96;
  xf86SetGamma(pScrn, gamma);
  return xf86LoadSubModule(pScrn, "fb") != NULL;
}

static Bool CISwitchMode(ScrnInfoPtr s, DisplayModePtr m) { (void)s, (void)m; return TRUE; }
static void CIAdjustFrame(ScrnInfoPtr s, int x, int y) { (void)s, (void)x, (void)y; }
static Bool CIEnterVT(ScrnInfoPtr s) { (void)s; return TRUE; }
static void CILeaveVT(ScrnInfoPtr s) { (void)s; }
static ModeStatus CIValidMode(ScrnInfoPtr s, DisplayModePtr m, Bool v, int f) {
  (void)s, (void)m, (void)v, (void)f;
  return MODE_OK;
}

/* ---- Modul ---------------------------------------------------------------------------------------- */

static const OptionInfoRec CIOptions[] = {{-1, NULL, OPTV_NONE, {0}, FALSE}};
static SymTabRec CIChipsets[] = {{0, CI_NAME}, {-1, NULL}};

static const OptionInfoRec *CIAvailableOptions(int chip, int bus) {
  (void)chip, (void)bus;
  return CIOptions;
}
static void CIIdentify(int flags) {
  (void)flags;
  xf86PrintChipsets(CI_NAME, "ClokeOS driver for ClokeIntelFB (Intel graphics)", CIChipsets);
}
static Bool CIProbe(DriverPtr drv, int flags);

_X_EXPORT DriverRec CLOKEINTEL = {CI_VERSION, CI_NAME, CIIdentify, CIProbe, CIAvailableOptions, NULL, 0, NULL, NULL, NULL, NULL};

static Bool CIProbe(DriverPtr drv, int flags) {
  GDevPtr *dev;
  Bool found = FALSE;
  if (flags & PROBE_DETECT) return FALSE;
  int n = xf86MatchDevice(CI_NAME, &dev);
  for (int i = 0; i < n; i++) {
    int entity = xf86ClaimNoSlot(drv, 0, dev[i], TRUE);
    ScrnInfoPtr s = xf86AllocateScreen(drv, 0);
    if (!s) continue;
    xf86AddEntityToScreen(s, entity);
    s->driverVersion = CI_VERSION;
    s->driverName = (char *)CI_NAME;
    s->name = (char *)CI_NAME;
    s->Probe = CIProbe;
    s->PreInit = CIPreInit;
    s->ScreenInit = CIScreenInit;
    s->SwitchMode = CISwitchMode;
    s->AdjustFrame = CIAdjustFrame;
    s->EnterVT = CIEnterVT;
    s->LeaveVT = CILeaveVT;
    s->ValidMode = CIValidMode;
    found = TRUE;
  }
  free(dev);
  return found;
}

static void *CISetup(void *module, void *opts, int *errmaj, int *errmin) {
  static Bool done = FALSE;
  (void)opts, (void)errmin;
  if (!done) {
    done = TRUE;
    xf86AddDriver(&CLOKEINTEL, module, 0);
    return (void *)1;
  }
  if (errmaj) *errmaj = LDR_ONCEONLY;
  return NULL;
}

static XF86ModuleVersionInfo CIVers = {CI_NAME, "ClokeOS", MODINFOSTRING1, MODINFOSTRING2, XORG_VERSION_CURRENT, 1, 0, 0,
                                       ABI_CLASS_VIDEODRV, ABI_VIDEODRV_VERSION, MOD_CLASS_VIDEODRV, {0, 0, 0, 0}};

_X_EXPORT XF86ModuleData clokeintelModuleData = {&CIVers, CISetup, NULL};
