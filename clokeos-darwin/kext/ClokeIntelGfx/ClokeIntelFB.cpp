/*
 * ClokeIntelFB: eigener Framebuffer für Intel-Grafik Gen12/13 (Tiger Lake, Alder Lake, Raptor Lake),
 * Stufe 2b auf dem Weg zum echten Grafiktreiber (docs/drivers.md).
 *
 * Statt das Bild der Firmware (GOP) weiterzubenutzen, legt der Treiber seinen eigenen Bildspeicher an
 * und lässt die Anzeige daraus lesen:
 *   - 4-KB-Seiten aus dem Arbeitsspeicher (PureDarwin liefert zusammenhängend nur eine Seite), einzeln
 *     in die Seitentabelle der GPU (GGTT) eingetragen, bei 16 MB, also innerhalb der Aperture (BAR2):
 *     dort sieht die CPU die Seiten als einen zusammenhängenden Bereich, den Xorg (puredarwingop über
 *     PDGOP, Speicher-Typ kIOFBVRAMMemory) wie bisher abbilden kann,
 *   - das aktuelle Bild wird hineinkopiert, dann zeigt Ebene 1 der aktiven Pipe darauf (PLANE_SURF),
 *     geprüft über PLANE_SURFLIVE,
 *   - Kernel-Konsole (setConsoleInfo) und /dev/fb0 zeigen danach auf den neuen Speicher.
 *
 * Der Treiber ist ein IOFramebuffer und gewinnt auf Intel-Grafik gegen IOGOPFramebuffer (höherer
 * IOProbeScore, dieselbe Match-Kategorie). Ist er abgeschaltet (Boot-Arg cloke_fb=0) oder klappt die
 * Übernahme nicht, gibt er auf, bevor er IOFramebuffer startet, und IOKit nimmt wie bisher
 * IOGOPFramebuffer. Auf dem HP 15s (Alder Lake) seit 15.11.2026 getestet und Standard.
 *
 * Danach (2c/2d): Modusliste, Skalierer, Hardware-Cursor, vollständiges Modesetting.
 */
#include "cloke_lkpi.h"
#include "igfx_regs.h"

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/graphics/IOFramebuffer.h>

extern "C" {
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/uio.h>
}

#define LOG(fmt, ...) IOLog("ClokeIntelFB: " fmt "\n", ##__VA_ARGS__)

enum { kModeID = 1, kDepth = 0, kGpuAddr = 16 << 20, kMmioSize = 0x200000 };

class ClokeIntelFB : public IOFramebuffer {
  OSDeclareDefaultStructors(ClokeIntelFB);

public:
  IOService *probe(IOService *provider, SInt32 *score) override;
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;

  IOReturn enableController() override { return kIOReturnSuccess; }
  const char *getPixelFormats() override { return IO32BitDirectPixels; }
  IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override {
    *mode = kModeID;
    *depth = kDepth;
    return kIOReturnSuccess;
  }
  IOReturn setDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
  IODeviceMemory *getApertureRange(IOPixelAperture) override { return IODeviceMemory::withRange(aperture, fbSize); }
  IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
  IOReturn getInformationForDisplayMode(IODisplayModeID, IODisplayModeInformation *info) override;
  /* wie IOGOPFramebuffer */
  UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return (UInt64)(uintptr_t)getPixelFormats(); }
  IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture ap,
                               IOPixelInformation *info) override;
  IOReturn getDisplayModes(IODisplayModeID *all) override {
    *all = kModeID;
    return kIOReturnSuccess;
  }
  IOItemCount getDisplayModeCount() override { return 1; }

private:
  struct pci_dev pdev;
  struct lkpi_iomap mmio, gtt, fbmap;
  struct igfx_info info;
  IOBufferMemoryDescriptor *buf;
  u64 *savedPte;
  u64 pages, fbSize, aperture;
  u32 oldSurf, width, height, stride, refreshMilli;
  unsigned pipe;
  bool flipped;

  u32 rd(u32 reg) { return readl(mmio.base + reg); }
  void wr(u32 reg, u32 v) { writel(v, mmio.base + reg); }
  static u32 rdThunk(void *self, u32 reg) { return ((ClokeIntelFB *)self)->rd(reg); }
  bool takeOver();
  void giveBack();
  void ggttFlush() {
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);
  }
};

OSDefineMetaClassAndStructors(ClokeIntelFB, IOFramebuffer)

/* ---- /dev/fb0 (wie IOGOPFramebuffer: nur schreiben, ab Offset) ----------------------------------- */

static volatile u8 *gFb;
static size_t gFbSize;
static int gFbMajor = -1;
static void *gFbNode;
static IONotifier *gFbNotifier;

static int fb0_open(dev_t, int, int, proc_t) { return gFb ? 0 : ENXIO; }
static int fb0_close(dev_t, int, int, proc_t) { return 0; }
static int fb0_write(dev_t, struct uio *uio, int) {
  if (!gFb) return ENXIO;
  off_t off = uio_offset(uio);
  if (off < 0 || (u64)off >= gFbSize) return ENOSPC;
  size_t n = lkpi_min(gFbSize - (size_t)off, (size_t)uio_resid(uio));
  return uiomove((char *)(gFb + off), (int)n, uio);
}
static int fb0_rw(dev_t, struct uio *, int) { return ENODEV; }
static int fb0_ioctl(dev_t, u_long, caddr_t, int, proc_t) { return ENOTTY; }
static int fb0_stop(struct tty *, int) { return 0; }
static int fb0_reset(int) { return 0; }
static int fb0_select(dev_t, int, void *, proc_t) { return 1; }
static void fb0_strategy(struct buf *) {}
static struct cdevsw gFbCdev = {fb0_open, fb0_close, fb0_rw, fb0_write, fb0_ioctl, fb0_stop, fb0_reset,
                                nullptr,  fb0_select, nullptr, fb0_strategy, nullptr, nullptr, 0};

static bool fb0Node(void *, void *, IOService *, IONotifier *) {
  if (gFbMajor >= 0) return true;
  gFbMajor = cdevsw_add(-1, &gFbCdev);
  /* cloakserver-start wartet auf /dev/fb0, bevor es Xorg startet */
  if (gFbMajor >= 0) gFbNode = devfs_make_node(makedev((uint32_t)gFbMajor, 0), DEVFS_CHAR, 0, 0, 0666, "fb0");
  IOLog("ClokeIntelFB: /dev/fb0 %s\n", gFbNode ? "ready" : "failed");
  return true;
}

/* ---- Übernahme ------------------------------------------------------------------------------------ */

IOService *ClokeIntelFB::probe(IOService *provider, SInt32 *score) {
  if (!lkpi_bootarg("cloke_fb", 1) || !lkpi_bootarg("cloke_gfx", 1)) return nullptr;
  IOPCIDevice *p = (IOPCIDevice *)provider;
  u16 vendor = p->extendedConfigRead16(PCI_VENDOR_ID), device = p->extendedConfigRead16(PCI_DEVICE_ID);
  if (vendor != 0x8086) return nullptr;
  struct igfx_info i = igfx_identify(device);
  if (i.display_ver < 12) {
    LOG("8086:%04x (%s): not supported yet, leaving the display to IOGOPFramebuffer", device, i.name);
    return nullptr;
  }
  return IOFramebuffer::probe(provider, score);
}

bool ClokeIntelFB::takeOver() {
  if (pci_iomap_range(&pdev, 0, 0, kMmioSize, &mmio)) return false;
  /* aktive Pipe mit linearer Ebene 1, wie GOP sie hinterlässt */
  struct igfx_mode m = {};
  bool found = false;
  for (unsigned p = 0; p < info.pipes && !found; p++) {
    m = igfx_read_mode(rdThunk, this, p, info.display_ver);
    if (m.active && m.plane_on && m.tiling == 0 && m.stride_bytes) pipe = p, found = true;
  }
  if (!found || !pdev.bar[2].start) {
    LOG("no active pipe with a linear plane (or no aperture), giving up");
    return false;
  }
  width = m.plane_w, height = m.plane_h, stride = m.stride_bytes;
  fbSize = ((u64)stride * height + 0xfff) & ~0xfffull;
  pages = fbSize >> 12;
  oldSurf = rd(PLANE_SURF(pipe));
  u64 gopStart = oldSurf & ~0xfffull;
  if ((u64)kGpuAddr < gopStart + fbSize && (u64)kGpuAddr + fbSize > gopStart) {
    LOG("firmware framebuffer at GGTT 0x%llx overlaps ours, giving up", (unsigned long long)gopStart);
    return false;
  }
  u64 ggtt = igfx_ggtt_size(pdev.pci->extendedConfigRead16(PCI_GMCH_CTRL));
  if (ggtt < (u64)kGpuAddr + fbSize) return false;
  aperture = pdev.bar[2].start + kGpuAddr;

  /* GGTT-Einträge für unseren Bereich abbilden und sichern */
  u64 pteOff = GGTT_OFFSET_IN_BAR0 + ((u64)kGpuAddr >> 12) * 8;
  if (lkpi_ioremap(&gtt, pdev.bar[0].start + pteOff, (pages * 8 + 0xfff) & ~0xfffull)) return false;
  volatile u64 *pte = (volatile u64 *)gtt.base;
  savedPte = (u64 *)kzalloc(pages * 8);
  buf = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, fbSize, PAGE_SIZE);
  if (!savedPte || !buf || buf->prepare() != kIOReturnSuccess) {
    LOG("no memory for %llu bytes", (unsigned long long)fbSize);
    return false;
  }
  for (u64 i = 0; i < pages; i++) savedPte[i] = pte[i];

  /* Das aktuelle Bild (Logo) übernehmen: aus der Aperture der Firmware lesen, über die gecachte
   * Abbildung schreiben, dann aus dem Cache in den Speicher schieben (die Anzeige liest am Cache vorbei) */
  struct lkpi_iomap gop;
  u8 *dst = (u8 *)buf->getBytesNoCopy();
  if (!lkpi_ioremap(&gop, pdev.bar[2].start + gopStart, fbSize)) {
    for (u64 o = 0; o < fbSize; o += 8) *(u64 *)(dst + o) = *(volatile u64 *)(gop.base + o);
    lkpi_iounmap(&gop);
  } else {
    bzero(dst, fbSize);
  }
  for (u64 o = 0; o < fbSize; o += 64) __builtin_ia32_clflush(dst + o);
  __builtin_ia32_mfence();

  for (u64 i = 0; i < pages; i++) {
    IOByteCount len = 0;
    addr64_t phys = buf->getPhysicalSegment(i << 12, &len, kIOMemoryMapperNone);
    if (!phys) {
      LOG("no physical address for page %llu", (unsigned long long)i);
      for (u64 k = 0; k < i; k++) pte[k] = savedPte[k];
      ggttFlush();
      return false;
    }
    pte[i] = igfx_ggtt_pte(phys);
  }
  (void)pte[pages - 1];
  ggttFlush();

  /* Umschalten mit dem nächsten Bildwechsel, prüfen, ob die Anzeige wirklich von uns liest */
  wr(PLANE_SURF(pipe), (u32)kGpuAddr | (oldSurf & 0xfff));
  u32 live = 0;
  if (wait_for(((live = rd(PLANE_SURFLIVE(pipe))) & ~0xfffu) == (u32)kGpuAddr, 200)) {
    LOG("PLANE_SURFLIVE stayed 0x%08x, back to the firmware framebuffer", live);
    wr(PLANE_SURF(pipe), oldSurf);
    for (u64 i = 0; i < pages; i++) pte[i] = savedPte[i];
    ggttFlush();
    return false;
  }
  flipped = true;

  /* CPU-Sicht für /dev/fb0: die Aperture an unserer GGTT-Adresse, Write-Combining */
  IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(aperture, fbSize, kIODirectionInOut);
  fbmap.map = md ? md->map(kIOMapWriteCombineCache) : nullptr;
  OSSafeReleaseNULL(md);
  if (fbmap.map) {
    gFb = (volatile u8 *)fbmap.map->getVirtualAddress();
    gFbSize = (size_t)stride * height;
  }
  /* Bildfrequenz für getInformationForDisplayMode */
  u32 a = rd(PIPE_FRMCOUNT(pipe));
  u64 t0 = ktime_get_ns();
  IOSleep(500);
  u64 dt = ktime_get_ns() - t0;
  refreshMilli = dt ? (u32)((u64)(rd(PIPE_FRMCOUNT(pipe)) - a) * 1000000000000ull / dt) : 60000;
  LOG("pipe %c scans out own framebuffer: %ux%u stride %u, GGTT 0x%x, aperture 0x%llx, %llu pages", 'A' + pipe,
      width, height, stride, (unsigned)kGpuAddr, (unsigned long long)aperture, (unsigned long long)pages);
  return true;
}

void ClokeIntelFB::giveBack() {
  if (flipped) {
    wr(PLANE_SURF(pipe), oldSurf);
    wait_for((rd(PLANE_SURFLIVE(pipe)) & ~0xfffu) == (oldSurf & ~0xfffu), 200);
    volatile u64 *pte = (volatile u64 *)gtt.base;
    for (u64 i = 0; i < pages; i++) pte[i] = savedPte[i];
    ggttFlush();
    flipped = false;
  }
  gFb = nullptr;
  OSSafeReleaseNULL(fbmap.map);
  if (buf) {
    buf->complete();
    OSSafeReleaseNULL(buf);
  }
  if (savedPte) kfree(savedPte), savedPte = nullptr;
  lkpi_iounmap(&gtt);
  lkpi_iounmap(&mmio);
}

bool ClokeIntelFB::start(IOService *provider) {
  lkpi_pci_init(&pdev, this, provider, "ClokeIntelFB");
  info = igfx_identify(pdev.device);
  /* Hardware zuerst: scheitert die Übernahme, ist IOFramebuffer noch nicht gestartet, und IOKit versucht
   * den nächsten Kandidaten (IOGOPFramebuffer) */
  if (!takeOver()) {
    giveBack();
    return false;
  }
  if (!IOFramebuffer::start(provider)) {
    giveBack();
    return false;
  }
  /* Kernel-Konsole auf den neuen Speicher (Bit 0 = physische Adresse, wie IOGOPFramebuffer) */
  PE_Video boot = {};
  IOPlatformExpert *pe = getPlatform();
  if (pe && pe->getConsoleInfo(&boot) == kIOReturnSuccess) {
    PE_Video c = {};
    c.v_baseAddr = (unsigned long)(aperture | 1);
    c.v_width = width;
    c.v_height = height;
    c.v_depth = 32;
    c.v_rowBytes = stride;
    c.v_display = boot.v_display ? kPEGraphicsMode : kPETextMode;
    c.v_scale = kPEScaleFactor1x;
    pe->setConsoleInfo(&c, (unsigned int)c.v_display);
  }
  setProperty("ClokeIntelFB", "own framebuffer in GGTT");
  if (!gFbNotifier)
    gFbNotifier = IOService::addMatchingNotification(gIOPublishNotification, IOService::resourceMatching("IOBSD"), fb0Node,
                                                     nullptr, nullptr, 0);
  registerService();
  return true;
}

void ClokeIntelFB::stop(IOService *provider) {
  giveBack();
  IOFramebuffer::stop(provider);
}

IOReturn ClokeIntelFB::getInformationForDisplayMode(IODisplayModeID, IODisplayModeInformation *mi) {
  bzero(mi, sizeof(*mi));
  mi->nominalWidth = width;
  mi->nominalHeight = height;
  mi->refreshRate = (IOFixed1616)(((u64)refreshMilli << 16) / 1000);
  mi->maxDepthIndex = kDepth;
  return kIOReturnSuccess;
}

IOReturn ClokeIntelFB::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture ap,
                                           IOPixelInformation *pi) {
  if (ap || depth || mode != kModeID) return kIOReturnUnsupportedMode;
  bzero(pi, sizeof(*pi));
  pi->activeWidth = width;
  pi->activeHeight = height;
  pi->bytesPerRow = stride;
  strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
  pi->pixelType = kIORGBDirectPixels;
  pi->componentMasks[0] = 0x00ff0000;
  pi->componentMasks[1] = 0x0000ff00;
  pi->componentMasks[2] = 0x000000ff;
  pi->bitsPerPixel = 32;
  pi->componentCount = 3;
  pi->bitsPerComponent = 8;
  return kIOReturnSuccess;
}
