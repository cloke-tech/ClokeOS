/*
 * ClokeIntelGfx: Anzeige-Treiber für Intel-Grafik Gen11/12/13 (Ice Lake bis Raptor Lake), Stufe 2.
 *
 * Das Bild selbst kommt weiter aus dem Framebuffer, den die Firmware (GOP) eingerichtet hat
 * (IOGOPFramebuffer, /dev/fb0, Xorg). Dieser Treiber hängt sich daneben an dieselbe PCI-Funktion
 * (eigene IOMatchCategory, IOGOPFramebuffer bleibt) und
 *
 *   - liest aus, was die Firmware programmiert hat: Pipes, Transcoder-Timings, gemessene Bildfrequenz,
 *     Ebene 1 (Größe, Schrittweite, Tiling), Skalierer, DDI-Ausgänge, Panel-Strom, Hintergrundlicht,
 *   - stellt die Helligkeit über den PWM-Regler im Chipsatz ein (oder, wenn das Panel sie über eDP-AUX
 *     regelt, über DPCD),
 *   - gibt beides über /dev/clokeos_gfx aus: lesen = Zustand als Text, schreiben = Befehl
 *       brightness 0..100 | brightness +10 | brightness -10 | dpcd | rescan | scanout-test
 *     (clokeos-brightness im Userland ist ein dünner Aufsatz darauf),
 *   - erster Schritt zum eigenen Treiber statt GOP: scanout-test legt einen eigenen Bildspeicher im
 *     Arbeitsspeicher an, trägt ihn selbst in die Seitentabelle der GPU (GGTT) ein, schaltet Ebene 1 darauf
 *     um, prüft über PLANE_SURFLIVE, dass die Anzeige wirklich daraus liest, und stellt danach zurück.
 *
 * Boot-Arg cloke_gfx=0 schaltet den Treiber ab, cloke_gfx=1 (Standard) wie oben, cloke_gfx=2 macht beim
 * Start zusätzlich den scanout-test (3 Sekunden Farbbalken, Ergebnis im Kernel-Log).
 *
 * Register und Bitfelder: igfx_regs.h (Intel PRMs, i915-Header). Die Hardware-Schnittstelle (PCI, MMIO,
 * Wartezeiten) kommt aus ../include/cloke_lkpi.h, der gemeinsamen Linux-Schicht für ClokeOS-Kexts.
 */
#include "cloke_lkpi.h"
#include "igfx_report.h"

extern "C" {
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/vnode.h>
}

#define LOG(fmt, ...) IOLog("ClokeIntelGfx: " fmt "\n", ##__VA_ARGS__)

enum { kReportSize = 8192, kMmioSize = 0x200000 };

class ClokeIntelGfx : public IOService {
  OSDeclareDefaultStructors(ClokeIntelGfx);

public:
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;

  /* von /dev/clokeos_gfx aufgerufen, unter gLock */
  void buildReport();
  void command(const char *cmd);

private:
  struct pci_dev pdev;
  struct lkpi_iomap mmio;
  struct igfx_info info;
  unsigned hz[4];
  char extra[1024]; /* Ergebnis des letzten Befehls, steht am Ende des Berichts */
  size_t extraLen;
  /* bleibt stehen: was beim Start geschah (scanout-test) und die DPCD-Werte; sonst überschreibt der nächste
   * Befehl (z. B. clokeos-brightness) das Ergebnis, bevor clokeos-diag es auf den Stick schreibt */
  char sticky[2048];
  size_t stickyLen;
  bool toSticky;

  u32 rd(u32 reg) { return readl(mmio.base + reg); }
  void wr(u32 reg, u32 v) { writel(v, mmio.base + reg); }
  static u32 rdThunk(void *self, u32 reg) { return ((ClokeIntelGfx *)self)->rd(reg); }
  void note(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  void measureHz();
  int setBrightness(int percent, bool relative);
  int aux(const u8 *send, unsigned ns, u8 *recv, unsigned nrMax);
  int dpcdRead(u32 addr, u8 *buf, unsigned len);
  int dpcdWrite(u32 addr, const u8 *buf, unsigned len);
  void dumpDpcd();
  void publish();
  int scanoutTest(unsigned seconds);
};

OSDefineMetaClassAndStructors(ClokeIntelGfx, IOService)

/* ---- /dev/clokeos_gfx ----------------------------------------------------------------------------- */

static ClokeIntelGfx *gDrv;
static IOLock *gLock;
static char *gReport;
static size_t gReportLen;
static int gMajor = -1;
static void *gNode;
static IONotifier *gBsdNotifier;

static int gfx_open(dev_t, int, int, proc_t) {
  if (!gDrv) return ENXIO;
  IOLockLock(gLock);
  gDrv->buildReport();
  IOLockUnlock(gLock);
  return 0;
}
static int gfx_close(dev_t, int, int, proc_t) { return 0; }
static int gfx_read(dev_t, struct uio *uio, int) {
  IOLockLock(gLock);
  int err = 0;
  off_t off = uio_offset(uio);
  if (off >= 0 && (size_t)off < gReportLen) {
    size_t n = gReportLen - (size_t)off;
    if (n > (size_t)uio_resid(uio)) n = (size_t)uio_resid(uio);
    err = uiomove(gReport + off, (int)n, uio);
  }
  IOLockUnlock(gLock);
  return err;
}
static int gfx_write(dev_t, struct uio *uio, int) {
  char cmd[128];
  size_t n = (size_t)uio_resid(uio);
  if (n >= sizeof cmd) n = sizeof cmd - 1;
  int err = uiomove(cmd, (int)n, uio);
  if (err) return err;
  cmd[n] = 0;
  /* Rest einer längeren Eingabe verwerfen, damit echo nicht hängt */
  uio_setresid(uio, 0);
  if (!gDrv) return ENXIO;
  IOLockLock(gLock);
  gDrv->command(cmd);
  IOLockUnlock(gLock);
  return 0;
}
static int gfx_ioctl(dev_t, u_long, caddr_t, int, proc_t) { return ENOTTY; }
static int gfx_stop(struct tty *, int) { return 0; }
static int gfx_reset(int) { return 0; }
static int gfx_select(dev_t, int, void *, proc_t) { return 1; }
static void gfx_strategy(struct buf *) {}
static struct cdevsw gCdev = {gfx_open, gfx_close, gfx_read, gfx_write, gfx_ioctl, gfx_stop, gfx_reset,
                              nullptr,  gfx_select, nullptr, gfx_strategy, nullptr, nullptr, 0};

static bool makeNode(void *, void *, IOService *, IONotifier *) {
  if (gMajor >= 0) return true;
  gMajor = cdevsw_add(-1, &gCdev);
  /* 0666: Helligkeit stellt der angemeldete Benutzer (Tasten, Einstellungen); die Befehle sind auf
   * Helligkeit und Lesen begrenzt */
  if (gMajor >= 0) gNode = devfs_make_node(makedev((uint32_t)gMajor, 0), DEVFS_CHAR, 0, 0, 0666, "clokeos_gfx");
  LOG("/dev/clokeos_gfx %s", gNode ? "ready" : "failed");
  return true;
}

/* ---- Treiber --------------------------------------------------------------------------------------- */

void ClokeIntelGfx::note(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *buf = toSticky ? sticky : extra;
  size_t cap = toSticky ? sizeof sticky : sizeof extra, &len = toSticky ? stickyLen : extraLen;
  if (len < cap) {
    int n = vsnprintf(buf + len, cap - len, fmt, ap);
    if (n > 0) len = lkpi_min(len + (size_t)n, cap - 1);
  }
  va_end(ap);
}

/* Bildfrequenz je Pipe: von einem Bildwechsel bis zum 30. danach messen (ein festes Fenster von 250 ms
 * ergab am HP 57,7 statt 60 Hz, weil nur ganze Bilder zählen) */
void ClokeIntelGfx::measureHz() {
  for (unsigned p = 0; p < info.pipes; p++) {
    hz[p] = 0;
    if (!(rd(TRANS_CONF(p)) & TRANS_CONF_STATE)) continue;
    u32 c0 = rd(PIPE_FRMCOUNT(p));
    if (wait_for(rd(PIPE_FRMCOUNT(p)) != c0, 100)) continue; /* steht still (PSR?) */
    u32 a = rd(PIPE_FRMCOUNT(p));
    u64 t0 = ktime_get_ns();
    if (wait_for(rd(PIPE_FRMCOUNT(p)) - a >= 30, 1000)) continue;
    u64 dt = ktime_get_ns() - t0;
    u32 frames = rd(PIPE_FRMCOUNT(p)) - a;
    hz[p] = dt ? (unsigned)((u64)frames * 1000000000000ull / dt) : 0; /* mHz */
  }
}

void ClokeIntelGfx::buildReport() {
  struct igfx_text t = {gReport, kReportSize, 0};
  igfx_report(&t, rdThunk, this, &info, pdev.device, mmio.phys, hz);
  u16 gmch = pdev.pci->extendedConfigRead16(PCI_GMCH_CTRL);
  igfx_printf(&t, "GGTT: GMCH_CTRL 0x%04x, %llu MB address space; aperture (BAR2) 0x%llx\n", gmch,
              (unsigned long long)(igfx_ggtt_size(gmch) >> 20), (unsigned long long)pdev.bar[2].start);
  if (stickyLen) igfx_printf(&t, "%s", sticky);
  if (extraLen) igfx_printf(&t, "%s", extra);
  gReportLen = t.len;
}

void ClokeIntelGfx::publish() {
  char s[96];
  setProperty("ClokeGfx Platform", info.name);
  for (unsigned p = 0; p < info.pipes; p++) {
    struct igfx_mode m = igfx_read_mode(rdThunk, this, p, info.display_ver);
    if (!m.active) continue;
    snprintf(s, sizeof s, "%ux%u@%u.%03u pipe %c", m.hactive, m.vactive, hz[p] / 1000, hz[p] % 1000, 'A' + p);
    setProperty("ClokeGfx Mode", s);
    break;
  }
  struct igfx_backlight b = igfx_read_backlight(rdThunk, this);
  if (b.controller >= 0) setProperty("ClokeGfx Backlight", b.percent, 32);
}

int ClokeIntelGfx::setBrightness(int percent, bool relative) {
  if (info.platform == IGFX_UNKNOWN) {
    note("brightness: unknown GPU, not touching registers\n");
    return -ENODEV;
  }
  struct igfx_backlight b = igfx_read_backlight(rdThunk, this);
  if (b.controller >= 0 && b.freq) {
    int target = relative ? (int)b.percent + percent : percent;
    if (target < 0) target = 0;
    u32 duty = igfx_duty_for(&b, (unsigned)target);
    wr(BLC_PWM_DUTY((unsigned)b.controller), duty);
    posting_read(mmio.base + BLC_PWM_DUTY((unsigned)b.controller));
    struct igfx_backlight a = igfx_read_backlight(rdThunk, this);
    LOG("brightness %d%% -> duty %u of %u, now %u%%", target, duty, b.freq, a.percent);
    note("brightness: %u%% (PWM controller %d, duty %u of %u)\n", a.percent, b.controller, a.duty, a.freq);
    setProperty("ClokeGfx Backlight", a.percent, 32);
    return 0;
  }
  /* kein PWM-Regler: Panel mit Helligkeit über eDP-AUX (DPCD 0x722/0x723)? */
  u8 cap[3] = {};
  if (dpcdRead(DPCD_EDP_REV, cap, 3) == 0 && (cap[2] & 0x02)) {
    u8 mode = 0, cur[2] = {};
    dpcdRead(DPCD_EDP_BACKLIGHT_MODE_SET, &mode, 1);
    dpcdRead(DPCD_EDP_BRIGHTNESS_MSB, cur, 2);
    bool wide = cap[2] & 0x04; /* 16 Bit statt 8 */
    unsigned maxv = wide ? 0xffff : 0xff;
    unsigned now = wide ? ((unsigned)cur[0] << 8 | cur[1]) : cur[0];
    int target = relative ? (int)(now * 100 + maxv / 2) / (int)maxv + percent : percent;
    if (target < 5) target = 5;
    if (target > 100) target = 100;
    unsigned v = (unsigned)target * maxv / 100;
    if ((mode & 3) != 2) {
      u8 m = (u8)((mode & ~3u) | 2); /* Helligkeit aus DPCD statt PWM-Pin */
      dpcdWrite(DPCD_EDP_BACKLIGHT_MODE_SET, &m, 1);
    }
    u8 out[2] = {(u8)(wide ? v >> 8 : v), (u8)v};
    int r = dpcdWrite(DPCD_EDP_BRIGHTNESS_MSB, out, wide ? 2 : 1);
    note("brightness: %d%% over eDP AUX (%s)\n", target, r ? "write failed" : "ok");
    return r;
  }
  note("brightness: no PWM controller and no eDP AUX brightness\n");
  return -ENODEV;
}

/* Eine AUX-Transaktion auf Kanal A (eDP); gibt die Länge der Antwort zurück oder einen Fehler */
int ClokeIntelGfx::aux(const u8 *send, unsigned ns, u8 *recv, unsigned nrMax) {
  const unsigned ch = 0;
  if (ns > 20) return -EINVAL;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (wait_for(!(rd(DP_AUX_CH_CTL(ch)) & AUX_SEND_BUSY), 10)) return -EBUSY;
    for (unsigned i = 0; i < ns; i += 4) wr(DP_AUX_CH_DATA(ch, i / 4), igfx_aux_pack(send + i, ns - i));
    wr(DP_AUX_CH_CTL(ch), igfx_aux_ctl(ns));
    int w = wait_for(!(rd(DP_AUX_CH_CTL(ch)) & AUX_SEND_BUSY), 10);
    u32 st = rd(DP_AUX_CH_CTL(ch));
    /* Statusbits durch Zurückschreiben löschen (SEND_BUSY ist hier aus, startet also nichts) */
    wr(DP_AUX_CH_CTL(ch), st | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_RECEIVE_ERROR);
    if (w || (st & (AUX_TIME_OUT_ERROR | AUX_RECEIVE_ERROR)) || !(st & AUX_DONE)) {
      IODelay(500);
      continue;
    }
    unsigned n = (st & AUX_MSG_SIZE_MASK) >> AUX_MSG_SIZE_SHIFT;
    if (n > 20) n = 20;
    if (n > nrMax) n = nrMax;
    for (unsigned i = 0; i < n; i += 4) igfx_aux_unpack(rd(DP_AUX_CH_DATA(ch, i / 4)), recv + i, n - i);
    return (int)n;
  }
  return -ETIMEDOUT;
}

int ClokeIntelGfx::dpcdRead(u32 addr, u8 *buf, unsigned len) {
  if (!len || len > 16) return -EINVAL;
  u8 hdr[4] = {(u8)(0x90 | ((addr >> 16) & 0xf)), (u8)(addr >> 8), (u8)addr, (u8)(len - 1)};
  u8 rep[20];
  for (int i = 0; i < 7; i++) {
    int n = aux(hdr, 4, rep, sizeof rep);
    if (n < 1) return n < 0 ? n : -EIO;
    u8 ack = rep[0] >> 4;
    if (ack == 0x2) { /* DEFER */
      IODelay(500);
      continue;
    }
    if (ack != 0 || (unsigned)n < len + 1) return -EIO;
    memcpy(buf, rep + 1, len);
    return 0;
  }
  return -ETIMEDOUT;
}

int ClokeIntelGfx::dpcdWrite(u32 addr, const u8 *buf, unsigned len) {
  if (!len || len > 16) return -EINVAL;
  u8 msg[20] = {(u8)(0x80 | ((addr >> 16) & 0xf)), (u8)(addr >> 8), (u8)addr, (u8)(len - 1)};
  memcpy(msg + 4, buf, len);
  u8 rep[4];
  for (int i = 0; i < 7; i++) {
    int n = aux(msg, 4 + len, rep, sizeof rep);
    if (n < 1) return n < 0 ? n : -EIO;
    u8 ack = rep[0] >> 4;
    if (ack == 0x2) {
      IODelay(500);
      continue;
    }
    return ack == 0 ? 0 : -EIO;
  }
  return -ETIMEDOUT;
}

void ClokeIntelGfx::dumpDpcd() {
  static const struct {
    u32 addr;
    unsigned len;
  } ranges[] = {{0x000, 16}, {0x700, 4}, {0x720, 6}};
  for (auto &r : ranges) {
    u8 b[16] = {};
    int e = dpcdRead(r.addr, b, r.len);
    if (e) {
      note("dpcd 0x%03x: error %d\n", r.addr, e);
      LOG("dpcd 0x%03x: error %d", r.addr, e);
      continue;
    }
    note("dpcd 0x%03x:", r.addr);
    for (unsigned i = 0; i < r.len; i++) note(" %02x", b[i]);
    note("\n");
  }
}

/* Eigener Bildspeicher für Ebene 1 (siehe Kopf). Gleiches Format und gleiche Schrittweite wie das Bild der
 * Firmware, nur PLANE_SURF zeigt woanders hin. Der Speicher muss nicht zusammenhängend sein: die GGTT bildet
 * jede 4-KB-Seite einzeln ab (PureDarwin liefert zusammenhängend nur eine Seite). Die Anzeige liest am
 * CPU-Cache vorbei, deshalb nach dem Malen clflush. */
int ClokeIntelGfx::scanoutTest(unsigned seconds) {
  if (info.platform == IGFX_UNKNOWN || info.display_ver < 12) {
    note("scanout-test: only for Gen12/13 (Tiger Lake and newer)\n");
    return -ENODEV;
  }
  int pipe = -1;
  struct igfx_mode m = {};
  for (unsigned p = 0; p < info.pipes && pipe < 0; p++) {
    m = igfx_read_mode(rdThunk, this, p, info.display_ver);
    if (m.active && m.plane_on && m.tiling == 0 && m.stride_bytes) pipe = (int)p;
  }
  if (pipe < 0) {
    note("scanout-test: no active pipe with a linear plane\n");
    return -ENODEV;
  }
  unsigned P = (unsigned)pipe;
  u64 size = ((u64)m.stride_bytes * m.plane_h + 0xfff) & ~0xfffull;
  u64 pages = size >> 12;
  u64 ggtt = igfx_ggtt_size(pdev.pci->extendedConfigRead16(PCI_GMCH_CTRL));
  if (ggtt < 256ull << 20) {
    note("scanout-test: GGTT size unknown (GMCH_CTRL 0x%04x)\n", pdev.pci->extendedConfigRead16(PCI_GMCH_CTRL));
    return -ENODEV;
  }
  /* freier Bereich am Ende der GGTT (die Firmware belegt den Anfang: gestohlener Speicher, GOP-Bild) */
  u64 gpuAddr = (ggtt - (64ull << 20)) & ~0xffffull;
  u64 pteOff = GGTT_OFFSET_IN_BAR0 + (gpuAddr >> 12) * 8;
  struct lkpi_iomap gtt;
  if (lkpi_ioremap(&gtt, pdev.bar[0].start + (pteOff & ~0xfffull), ((pteOff & 0xfff) + pages * 8 + 0xfff) & ~0xfffull)) {
    note("scanout-test: cannot map GGTT\n");
    return -ENOMEM;
  }
  volatile u64 *pte = (volatile u64 *)(gtt.base + (pteOff & 0xfff));
  u64 *saved = (u64 *)kzalloc(pages * 8);
  IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, size, PAGE_SIZE);
  int result = -EIO;
  u32 oldSurf = rd(PLANE_SURF(P)), live = 0;
  if (!saved || !buf || buf->prepare() != kIOReturnSuccess) {
    note("scanout-test: no memory for %llu bytes\n", (unsigned long long)size);
    goto out;
  }
  {
    /* Unter ClokeOS benutzt außer dem Bild der Firmware niemand die GGTT; ihre übrigen Einträge sind oft
     * ungelöschte Reste im gestohlenen Speicher. Nur eine Überschneidung mit dem GOP-Bild wäre schlecht. */
    u64 gopStart = oldSurf & ~0xfffull, gopEnd = gopStart + size;
    note("scanout-test: GGTT %llu MB, using 0x%llx, PTE before 0x%llx, plane SURF 0x%08x SURFLIVE 0x%08x\n",
         (unsigned long long)(ggtt >> 20), (unsigned long long)gpuAddr, (unsigned long long)pte[0], oldSurf,
         rd(PLANE_SURFLIVE(P)));
    if (gpuAddr < gopEnd && gpuAddr + size > gopStart) {
      note("scanout-test: range overlaps the firmware's framebuffer, not touching it\n");
      goto out;
    }
    for (u64 i = 0; i < pages; i++) saved[i] = pte[i];
    /* Farbbalken (ClokeOS-Palette) mit weißem Rand, XRGB8888 */
    static const u32 bars[] = {0xf2c4d6, 0xe86a92, 0x8e3b62, 0x4a1f3d, 0x2f6fde, 0x34c759, 0xffcc00, 0xff3b30};
    u8 *px = (u8 *)buf->getBytesNoCopy();
    for (unsigned y = 0; y < m.plane_h; y++) {
      u32 *row = (u32 *)(px + (u64)y * m.stride_bytes);
      for (unsigned x = 0; x < m.plane_w; x++) {
        bool edge = x < 16 || y < 16 || x >= m.plane_w - 16 || y >= m.plane_h - 16;
        row[x] = edge ? 0xffffff : bars[(u64)x * 8 / m.plane_w];
      }
    }
    for (u64 o = 0; o < size; o += 64) __builtin_ia32_clflush(px + o);
    __builtin_ia32_mfence();
    for (u64 i = 0; i < pages; i++) {
      IOByteCount len = 0;
      addr64_t phys = buf->getPhysicalSegment(i << 12, &len, kIOMemoryMapperNone);
      if (!phys) goto restore;
      pte[i] = igfx_ggtt_pte(phys);
      if (!i) note("scanout-test: first page phys 0x%llx -> PTE 0x%llx, read back 0x%llx\n", (unsigned long long)phys,
                   (unsigned long long)igfx_ggtt_pte(phys), (unsigned long long)pte[0]);
    }
    (void)pte[pages - 1];
    wr(GFX_FLSH_CNTL, 1);
    (void)rd(GFX_FLSH_CNTL);
    /* Umschalten: wirkt mit dem nächsten Bildwechsel; untere Bits (Steuerung) bleiben */
    wr(PLANE_SURF(P), (u32)gpuAddr | (oldSurf & 0xfff));
    if (wait_for(((live = rd(PLANE_SURFLIVE(P))) & ~0xfffu) == (u32)gpuAddr, 200) == 0) {
      result = 0;
      LOG("scanout-test: pipe %c now scans out own memory at GGTT 0x%llx (%llu pages), PLANE_SURFLIVE 0x%08x",
          'A' + P, (unsigned long long)gpuAddr, (unsigned long long)pages, live);
      IOSleep(seconds * 1000);
    } else {
      LOG("scanout-test: PLANE_SURFLIVE stayed 0x%08x (wanted 0x%08llx)", live, (unsigned long long)gpuAddr);
    }
    wr(PLANE_SURF(P), oldSurf);
    wait_for((rd(PLANE_SURFLIVE(P)) & ~0xfffu) == (oldSurf & ~0xfffu), 200);
  }
restore:
  for (u64 i = 0; i < pages; i++) pte[i] = saved[i];
  wr(GFX_FLSH_CNTL, 1);
  (void)rd(GFX_FLSH_CNTL);
  note("scanout-test: %s (pipe %c, %ux%u, GGTT 0x%llx, SURFLIVE 0x%08x, back to 0x%08x)\n",
       result ? "FAILED" : "OK, own framebuffer was on screen", 'A' + P, m.plane_w, m.plane_h,
       (unsigned long long)gpuAddr, live, rd(PLANE_SURFLIVE(P)));
out:
  if (buf) {
    buf->complete();
    buf->release();
  }
  if (saved) kfree(saved);
  lkpi_iounmap(&gtt);
  return result;
}

void ClokeIntelGfx::command(const char *cmd) {
  while (*cmd == ' ') cmd++;
  extraLen = 0;
  extra[0] = 0;
  if (!strncmp(cmd, "brightness", 10)) {
    const char *v = cmd + 10;
    while (*v == ' ') v++;
    bool rel = *v == '+' || *v == '-';
    int sign = *v == '-' ? -1 : 1;
    if (rel) v++;
    int n = 0;
    bool any = false;
    while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0'), any = true;
    if (!any) {
      note("usage: brightness 0..100 | +N | -N\n");
      return;
    }
    setBrightness(sign * n, rel);
  } else if (!strncmp(cmd, "dpcd", 4)) {
    toSticky = true;
    dumpDpcd();
    toSticky = false;
  } else if (!strncmp(cmd, "scanout-test", 12)) {
    scanoutTest(3);
  } else if (!strncmp(cmd, "rescan", 6)) {
    measureHz();
    publish();
    note("rescanned\n");
  } else {
    note("commands: brightness 0..100 | brightness +N | brightness -N | dpcd | rescan | scanout-test\n");
  }
}

bool ClokeIntelGfx::start(IOService *provider) {
  if (!lkpi_bootarg("cloke_gfx", 1)) return false;
  lkpi_pci_init(&pdev, this, provider, "ClokeIntelGfx");
  if (pdev.vendor != 0x8086 || (pdev.class_code >> 16) != 0x03) {
    LOG("%04x:%04x class 0x%06x: not an Intel display controller", pdev.vendor, pdev.device, pdev.class_code);
    return false;
  }
  if (!IOService::start(provider)) return false;
  info = igfx_identify(pdev.device);
  /* BAR0 = GTTMMADR (16 MB: Register in den unteren 2 MB, dahinter die GTT); BAR2 = Aperture (GMADR) */
  LOG("8086:%04x rev %u: %s, display version %u, BAR0 0x%llx BAR2 0x%llx", pdev.device, pdev.revision, info.name,
      info.display_ver, (unsigned long long)pdev.bar[0].start, (unsigned long long)pdev.bar[2].start);
  if (info.platform == IGFX_UNKNOWN)
    LOG("unknown device id: only reading registers, no brightness control");
  if (pci_iomap_range(&pdev, 0, 0, kMmioSize, &mmio)) {
    LOG("cannot map BAR0");
    return false;
  }
  /* Antwortet das Gerät? Ein abgeschalteter Bereich liest als 0xffffffff */
  if (rd(TRANS_CONF(0)) == 0xffffffffu && rd(PP_STATUS) == 0xffffffffu) {
    LOG("MMIO reads 0xffffffff, giving up");
    lkpi_iounmap(&mmio);
    return false;
  }
  gReport = (char *)kzalloc(kReportSize);
  gLock = IOLockAlloc();
  if (!gReport || !gLock) return false;
  measureHz();
  buildReport();
  /* Bericht zeilenweise ins Kernel-Log (dmesg, clokeos-diag) */
  for (char *line = gReport; line && *line;) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    LOG("%s", line);
    if (!nl) break;
    *nl = '\n';
    line = nl + 1;
  }
  publish();
  if (lkpi_bootarg("cloke_gfx", 1) >= 2) {
    toSticky = true;
    note("boot: ");
    scanoutTest(3);
    toSticky = false;
    LOG("%s", sticky);
  }
  gDrv = this;
  if (!gBsdNotifier)
    gBsdNotifier = IOService::addMatchingNotification(gIOPublishNotification, IOService::resourceMatching("IOBSD"),
                                                      makeNode, nullptr, nullptr, 0);
  registerService();
  return true;
}

void ClokeIntelGfx::stop(IOService *provider) {
  if (gDrv == this) gDrv = nullptr;
  lkpi_iounmap(&mmio);
  IOService::stop(provider);
}
