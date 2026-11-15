/*
 * ClokeI2CHID: Touchpads am I²C-Bus von Notebooks (HID über I²C) für Xorg.
 *
 * Seit etwa 2015 hängen Touchpads meist nicht mehr am PS/2-Controller, sondern über HID-over-I²C an
 * einem I²C-Controller im Chipsatz (Intel LPSS, darin ein Synopsys DesignWare I²C). PureDarwin hat
 * für beides keinen Treiber und wertet ACPI nicht aus, in dem Bus, Adresse und Interrupt des
 * Touchpads stehen. Dieser Treiber:
 *
 *   - hängt sich an die I²C-Controller von Intel (IOPCIMatch in Info.plist), schaltet sie ein
 *     (PCI-Befehlsregister, D0, LPSS-Resets) und betreibt den DesignWare-Kern ohne Interrupts,
 *   - sucht an jedem Controller unter den üblichen Adressen und Deskriptor-Registern ein HID-Gerät
 *     (HID-Deskriptor mit Länge 30 und Version 1.00),
 *   - schaltet es ein, liest den Report-Deskriptor und sucht darin die Maus-Sammlung, die
 *     Precision-Touchpads bis zum Umschalten in den Touchpad-Modus melden (relative Bewegung, Tasten,
 *     Antippen als Klick macht die Firmware des Touchpads),
 *   - fragt das Eingaberegister alle 10 ms ab (statt des GPIO-Interrupts, den nur ACPI nennt) und
 *     gibt Bewegungen als /dev/clokeos_touchpad im Format von /dev/usb_hid_mouse aus (puredarwininput,
 *     overlay-desktop/etc/X11/xorg.conf).
 *
 * Was gefunden wurde und was nicht, steht im Kernel-Log (dmesg, "ClokeI2CHID:"), damit es das
 * Diagnosefenster (clokeos-diag) zeigt.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <pexpert/pexpert.h>
#include "ClokeTrace.h"

extern "C" {
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/vnode.h>
}

#define LOG(fmt, ...) IOLog("ClokeI2CHID: " fmt "\n", ##__VA_ARGS__)

/* IOPCIFamily-Header gibt es in PureDarwins Kernel.framework nicht. Gebraucht werden nur die
 * nicht-virtuellen Konfigurationszugriffe, die auch PureDarwins eigene Treiber aufrufen; ihre
 * Namen bindet kc-builder gegen den Kernel. Die Klasse wird nie angelegt, nur als Zeigertyp benutzt. */
class IOPCIDevice : public IOService {
public:
  UInt8 extendedConfigRead8(IOByteCount offset);
  UInt16 extendedConfigRead16(IOByteCount offset);
  UInt32 extendedConfigRead32(IOByteCount offset);
  void extendedConfigWrite16(IOByteCount offset, UInt16 data);
  void extendedConfigWrite32(IOByteCount offset, UInt32 data);
};

/* ---- Gerät /dev/clokeos_touchpad (wie in ClokePS2) ------------------------------------------- */

struct EventQueue {
  enum { kSize = 4092 }; /* Vielfaches von 12 */
  uint8_t buf[kSize];
  unsigned head = 0, tail = 0;
  IOLock *lock = nullptr;
  uint8_t sel[256] = {};
  bool open = false;
  void push(const uint8_t *rec) {
    IOLockLock(lock);
    if (open) {
      /* Rückstau (Xorg kommt auf dem einen Kern nicht nach): nur die letzten 8 Ereignisse behalten, sonst
       * läuft der Zeiger noch lange nach dem Loslassen weiter */
      unsigned pending = (tail + kSize - head) % kSize;
      if (pending > 24 * 12) head = (tail + kSize - 8 * 12) % kSize;
      unsigned next = (tail + 12) % kSize;
      if (next != head) {
        memcpy(buf + tail, rec, 12);
        tail = next;
      }
      IOLockWakeup(lock, this, false);
    }
    IOLockUnlock(lock);
    selwakeup((struct selinfo *)sel);
  }
};

static EventQueue gQ;
static int gMajor = -1;
static void *gNode;
static int gDevices; /* gefundene Touchpads (mehrere Controller teilen das eine Gerät) */
static IONotifier *gBsdNotifier;


static int tp_open(dev_t, int, int, proc_t) {
  IOLockLock(gQ.lock);
  gQ.open = true;
  gQ.head = gQ.tail = 0;
  IOLockUnlock(gQ.lock);
  return 0;
}
static int tp_close(dev_t, int, int, proc_t) {
  IOLockLock(gQ.lock);
  gQ.open = false;
  IOLockUnlock(gQ.lock);
  return 0;
}
static int tp_read(dev_t, struct uio *uio, int ioflag) {
  uint8_t rec[12];
  int err = 0;
  bool any = false;
  IOLockLock(gQ.lock);
  while (!err && uio_resid(uio) >= 12) {
    if (gQ.head == gQ.tail) {
      if (any) break;
      if (ioflag & IO_NDELAY) {
        err = EWOULDBLOCK;
        break;
      }
      if (IOLockSleep(gQ.lock, &gQ, THREAD_INTERRUPTIBLE) != THREAD_AWAKENED) {
        err = EINTR;
        break;
      }
      continue;
    }
    memcpy(rec, gQ.buf + gQ.head, 12);
    gQ.head = (gQ.head + 12) % EventQueue::kSize;
    IOLockUnlock(gQ.lock);
    err = uiomove((char *)rec, 12, uio);
    any = true;
    IOLockLock(gQ.lock);
  }
  IOLockUnlock(gQ.lock);
  return err;
}
static int tp_select(dev_t, int which, void *wql, proc_t p) {
  if (which != FREAD) return 0;
  IOLockLock(gQ.lock);
  bool ready = gQ.head != gQ.tail;
  IOLockUnlock(gQ.lock);
  if (ready) return 1;
  selrecord(p, (struct selinfo *)gQ.sel, wql);
  return 0;
}
static int tp_write(dev_t, struct uio *, int) { return ENODEV; }
static int tp_ioctl(dev_t, u_long, caddr_t, int, proc_t) { return ENOTTY; }
static int tp_stop(struct tty *, int) { return 0; }
static int tp_reset(int) { return 0; }
static void tp_strategy(struct buf *) {}
static struct cdevsw gCdev = {tp_open,   tp_close, tp_read, tp_write,    tp_ioctl, tp_stop, tp_reset,
                              nullptr,   tp_select, nullptr, tp_strategy, nullptr,  nullptr,  0};

/* Das Gerät erst anlegen, wenn BSD (devfs) läuft; die PCI-Treiber starten früher */
static bool makeNode(void *, void *, IOService *, IONotifier *) {
  if (gMajor >= 0) return true;
  gMajor = cdevsw_add(-1, &gCdev);
  if (gMajor >= 0) gNode = devfs_make_node(makedev((uint32_t)gMajor, 0), DEVFS_CHAR, 0, 0, 0644, "clokeos_touchpad");
  IOLog("ClokeI2CHID: /dev/clokeos_touchpad %s\n", gNode ? "ready" : "failed");
  return true;
}

/* ---- DesignWare I²C ------------------------------------------------------------------------- */

enum {
  IC_CON = 0x00, IC_TAR = 0x04, IC_DATA_CMD = 0x10, IC_SS_SCL_HCNT = 0x14, IC_SS_SCL_LCNT = 0x18,
  IC_FS_SCL_HCNT = 0x1c, IC_FS_SCL_LCNT = 0x20, IC_INTR_MASK = 0x30, IC_RAW_INTR_STAT = 0x34,
  IC_RX_TL = 0x38, IC_TX_TL = 0x3c, IC_CLR_INTR = 0x40, IC_CLR_TX_ABRT = 0x54, IC_CLR_STOP_DET = 0x60,
  IC_ENABLE = 0x6c, IC_STATUS = 0x70, IC_TXFLR = 0x74, IC_RXFLR = 0x78, IC_SDA_HOLD = 0x7c,
  IC_TX_ABRT_SOURCE = 0x80, IC_ENABLE_STATUS = 0x9c, IC_COMP_PARAM_1 = 0xf4, IC_COMP_TYPE = 0xfc,
  LPSS_RESETS = 0x204,
};
enum { CMD_READ = 0x100, CMD_STOP = 0x200, CMD_RESTART = 0x400 };
enum { INTR_TX_ABRT = 0x40, INTR_STOP_DET = 0x200 };

/* ---- Adresse für einen BAR, den die Firmware nicht vergeben hat ----------------------------------
 * HPs Firmware lässt die LPSS-I²C-Controller im PCI-Modus ohne Adresse (BAR0 = 4: 64 Bit, Adresse 0);
 * Windows weist sie beim Start zu, PureDarwin tut es nicht. Der Treiber sieht sich dazu alle
 * PCI-Geräte an, merkt sich den niedrigsten belegten Speicherbereich und legt BAR0 knapp darunter,
 * wo im Fenster der Firmware sehr wahrscheinlich nichts liegt. Ob dort wirklich der Controller
 * antwortet, zeigt dessen Kennung (IC_COMP_TYPE); sonst wird BAR0 zurückgesetzt. */
static uint64_t gLowHigh = ~0ull, gLowLow = ~0ull; /* niedrigste belegte Adresse: ab 4 GB, unter 4 GB */
static uint64_t gTopPch;                            /* höchste Adresse ab 4 GB eines Geräts, das keine Grafik ist */
static unsigned gAssigned;
/* Mit mehreren Kernen starten die beiden Controller (51e8, 51e9) zugleich, wählten dieselbe Adresse und
 * antworteten beide auf dieselben Lesezugriffe: Der Rechner blieb stehen oder lieferte nichts. Suche und
 * Prüfung der Adresse laufen deshalb nacheinander. */
static IOLock *gAssignLock = IOLockAlloc();

static void scanBars() {
  gLowHigh = gLowLow = ~0ull;
  gTopPch = 0;
  OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
  OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
  OSSafeReleaseNULL(m);
  if (!it) return;
  while (OSObject *o = it->getNextObject()) {
    IOService *sv = OSDynamicCast(IOService, o);
    if (!sv) continue;
    IOPCIDevice *d = (IOPCIDevice *)sv;
    uint32_t id = d->extendedConfigRead32(0);
    if ((id & 0xffff) == 0xffff) continue;
    unsigned nbar = (d->extendedConfigRead8(0x0e) & 0x7f) == 0 ? 6 : 2;
    char line[120];
    int n = snprintf(line, sizeof line, "%04x:%04x", id & 0xffff, id >> 16);
    for (unsigned i = 0; i < nbar; i++) {
      uint32_t v = d->extendedConfigRead32(0x10 + 4 * i);
      if (!v || (v & 1)) continue;
      uint64_t base = v & ~0xfull;
      if ((v & 6) == 4 && i + 1 < nbar) base |= (uint64_t)d->extendedConfigRead32(0x14 + 4 * i) << 32, i++;
      if (!base) continue;
      n += snprintf(line + n, sizeof line - (size_t)n, " 0x%llx", (unsigned long long)base);
      if ((base >> 32) && d->extendedConfigRead8(0x0b) != 0x03 && base > gTopPch) gTopPch = base;
      if (base >> 32) { if (base < gLowHigh) gLowHigh = base; }
      else if (base < gLowLow) gLowLow = base;
    }
    LOG("PCI %s", line);
    cloke_trace("PCI %s", line);
  }
  it->release();
  LOG("lowest memory BAR: above 4 GB 0x%llx, below 4 GB 0x%llx, highest non-display above 4 GB 0x%llx",
      (unsigned long long)gLowHigh, (unsigned long long)gLowLow, (unsigned long long)gTopPch);
}

/* Felder eines Berichts im Report-Deskriptor (Bit-Lage) */
struct Field {
  int off = -1, size = 0;
  bool sign = false;
};

class ClokeI2CHID : public IOService {
  OSDeclareDefaultStructors(ClokeI2CHID)

  IOPCIDevice *pci = nullptr;
  IOMemoryMap *map = nullptr;
  volatile uint8_t *base = nullptr;
  unsigned fifo = 32;
  IOWorkLoop *wl = nullptr;
  IOTimerEventSource *timer = nullptr;

  uint8_t addr = 0;
  uint16_t inputReg = 0, cmdReg = 0, maxInput = 0;
  uint8_t mouseId = 0; /* Report-ID der Maus-Sammlung, 0 = ohne IDs */
  Field btn[3], fx, fy, fwheel;
  uint8_t *rbuf = nullptr;
  uint8_t lastButtons = 0;
  /* Entprellen: Loslassen erst nach kDebounceMs weitergeben; kommt vorher wieder "gedrückt", war es Prellen
   * (am HP wurden aus Klick + Ziehen Doppelklicks: Fenster maximiert, mehrere Fenster geöffnet) */
  enum { kDebounceMs = 35 };
  bool releasePending = false;
  uint8_t pendingButtons = 0;
  uint64_t releaseAtMs = 0;
  unsigned btnLog = 0, bounces = 0;
  uint8_t lastRaw = 0; /* zuletzt gemeldete Tasten (vor dem Entprellen), nur fürs Protokoll */
  uint8_t lastRep[64] = {};
  unsigned lastLen = 0, dupRun = 0;
  uint32_t abrtSrc = 0; /* IC_TX_ABRT_SOURCE des letzten Abbruchs; Bit 0: Adresse ohne ACK */
  unsigned polls = 0, reports = 0, mouseReports = 0, readErrors = 0;

  uint32_t rd(unsigned r) { return *(volatile uint32_t *)(base + r); }
  void wr(unsigned r, uint32_t v) { *(volatile uint32_t *)(base + r) = v; }
  bool enable(bool on);
  bool xfer(uint8_t target, const uint8_t *w, unsigned nw, uint8_t *r, unsigned nr);
  bool probe(uint8_t a, uint16_t descReg);
  bool parseReportDescriptor(const uint8_t *d, unsigned n);
  void poll(IOTimerEventSource *);
  void report(const uint8_t *r, unsigned n);
  void emit(int dx, int dy, int dz, uint8_t buttons);
  static uint64_t nowMs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
  }

public:
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(ClokeI2CHID, IOService)

bool ClokeI2CHID::enable(bool on) {
  wr(IC_ENABLE, on ? 1 : 0);
  for (int i = 0; i < 500; i++) {
    if ((rd(IC_ENABLE_STATUS) & 1) == (on ? 1u : 0u)) return true;
    IODelay(25);
  }
  return false;
}

/* Schreiben, dann (mit Neustart) lesen; ohne Interrupts, die FIFOs werden abgefragt */
bool ClokeI2CHID::xfer(uint8_t target, const uint8_t *w, unsigned nw, uint8_t *r, unsigned nr) {
  abrtSrc = 0;
  enable(false);
  wr(IC_TAR, target);
  (void)rd(IC_CLR_INTR);
  if (!enable(true)) return false;
  unsigned total = nw + nr, sent = 0, got = 0;
  for (int spins = 0; spins < 200000 && (sent < total || got < nr); spins++) {
    uint32_t raw = rd(IC_RAW_INTR_STAT);
    if (raw & INTR_TX_ABRT) {
      abrtSrc = rd(IC_TX_ABRT_SOURCE);
      (void)rd(IC_CLR_TX_ABRT);
      return false;
    }
    /* Befehle nachschieben, solange im Sende-FIFO Platz ist und die offenen Lesebefehle ins
     * Empfangs-FIFO passen */
    for (unsigned rq = sent > nw ? sent - nw : 0; sent < total && rd(IC_TXFLR) < fifo && rq - got < fifo;
         rq = sent > nw ? sent - nw : 0) {
      uint32_t c;
      if (sent < nw) c = w[sent];
      else c = CMD_READ | (sent == nw && nw ? CMD_RESTART : 0);
      if (sent == total - 1) c |= CMD_STOP;
      wr(IC_DATA_CMD, c);
      sent++;
    }
    while (got < nr && rd(IC_RXFLR)) r[got++] = (uint8_t)rd(IC_DATA_CMD);
    if (sent == total && got == nr) break;
    IODelay(2);
  }
  /* auf STOP warten, damit der nächste Zugriff sauber beginnt */
  for (int i = 0; i < 2000 && !(rd(IC_RAW_INTR_STAT) & INTR_STOP_DET); i++) IODelay(5);
  (void)rd(IC_CLR_STOP_DET);
  return got == nr;
}

/* HID-Deskriptor an Adresse a, Register descReg (HID over I²C 1.0, 30 Byte) */
bool ClokeI2CHID::probe(uint8_t a, uint16_t descReg) {
  uint8_t reg[2] = {(uint8_t)descReg, (uint8_t)(descReg >> 8)}, d[30] = {};
  if (!xfer(a, reg, 2, d, 30)) return false;
  uint16_t len = (uint16_t)(d[0] | d[1] << 8), ver = (uint16_t)(d[2] | d[3] << 8);
  if (len != 30 || ver != 0x0100) {
    LOG("  0x%02x register 0x%04x: no HID descriptor (length %u, version %04x)", a, descReg, len, ver);
    return false;
  }
  uint16_t repLen = (uint16_t)(d[4] | d[5] << 8), repReg = (uint16_t)(d[6] | d[7] << 8);
  inputReg = (uint16_t)(d[8] | d[9] << 8);
  maxInput = (uint16_t)(d[10] | d[11] << 8);
  cmdReg = (uint16_t)(d[16] | d[17] << 8);
  uint16_t vid = (uint16_t)(d[20] | d[21] << 8), pid = (uint16_t)(d[22] | d[23] << 8);
  LOG("HID device at 0x%02x (descriptor register 0x%04x): vendor %04x product %04x, report descriptor %u bytes",
      a, descReg, vid, pid, repLen);
  if (!repLen || repLen > 4096 || maxInput < 3 || maxInput > 512) return false;
  addr = a;

  uint8_t on[4] = {(uint8_t)cmdReg, (uint8_t)(cmdReg >> 8), 0x00, 0x08}; /* SET_POWER ON */
  xfer(a, on, 4, nullptr, 0);
  IOSleep(5);
  uint8_t rst[4] = {(uint8_t)cmdReg, (uint8_t)(cmdReg >> 8), 0x00, 0x01}; /* RESET */
  xfer(a, rst, 4, nullptr, 0);
  IOSleep(100);

  uint8_t *rep = (uint8_t *)IOMalloc(repLen);
  uint8_t rr[2] = {(uint8_t)repReg, (uint8_t)(repReg >> 8)};
  bool ok = rep && xfer(a, rr, 2, rep, repLen) && parseReportDescriptor(rep, repLen);
  if (rep) IOFree(rep, repLen);
  if (!ok) {
    LOG("no mouse collection in the report descriptor");
    return false;
  }
  rbuf = (uint8_t *)IOMalloc(maxInput);
  /* die Antwort auf RESET (Länge 0) abholen */
  if (rbuf) xfer(a, nullptr, 0, rbuf, maxInput);
  return rbuf != nullptr;
}

/* Kurzer HID-Parser: Maus-Anwendungssammlung (Generic Desktop 0x02) mit Tasten 1-3, X, Y, Rad */
bool ClokeI2CHID::parseReportDescriptor(const uint8_t *d, unsigned n) {
  unsigned page = 0, rsize = 0, rcount = 0, rid = 0, usageMin = 0, usageMax = 0;
  unsigned usages[16], nus = 0;
  int depth = 0, mouseDepth = -1;
  bool logicalNeg = false;
  unsigned bitpos[256] = {};
  bool found = false;
  for (unsigned i = 0; i < n;) {
    uint8_t b = d[i++];
    if (b == 0xfe) { /* langes Element */
      if (i + 1 >= n) break;
      i += 2 + d[i];
      continue;
    }
    unsigned sz = b & 3, type = (b >> 2) & 3, tag = b >> 4;
    if (sz == 3) sz = 4;
    if (i + sz > n) break;
    uint32_t v = 0;
    for (unsigned k = 0; k < sz; k++) v |= (uint32_t)d[i + k] << (8 * k);
    int32_t sv = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
    i += sz;
    if (type == 1) { /* global */
      if (tag == 0) page = v;
      else if (tag == 1) logicalNeg = sv < 0;
      else if (tag == 7) rsize = v;
      else if (tag == 8) rid = v;
      else if (tag == 9) rcount = v;
    } else if (type == 2) { /* lokal */
      unsigned u = sz == 4 ? v : (page << 16) | v;
      if (tag == 0 && nus < 16) usages[nus++] = u;
      else if (tag == 1) usageMin = u;
      else if (tag == 2) usageMax = u;
    } else if (type == 0) { /* main */
      if (tag == 10) { /* Collection */
        if (v == 1 && nus && usages[0] == 0x10002 && mouseDepth < 0 && !found) mouseDepth = depth;
        depth++;
      } else if (tag == 12) { /* End Collection */
        depth--;
        if (depth == mouseDepth) {
          mouseDepth = -1;
          if (fx.off >= 0 && fy.off >= 0) found = true;
        }
      } else if (tag == 8 && rid < 256) { /* Input */
        unsigned pos = bitpos[rid];
        bool isConst = v & 1, relative = v & 4;
        if (mouseDepth >= 0 && !found && !isConst) {
          for (unsigned k = 0; k < rcount; k++) {
            unsigned u = k < nus ? usages[k] : nus ? usages[nus - 1] : 0;
            if (!nus && usageMin) u = usageMin + k <= usageMax ? usageMin + k : 0;
            Field f;
            f.off = (int)(pos + k * rsize);
            f.size = (int)rsize;
            f.sign = logicalNeg;
            if (u >= 0x90001 && u <= 0x90003) btn[u - 0x90001] = f;
            else if (u == 0x10030 && relative) fx = f;
            else if (u == 0x10031 && relative) fy = f;
            else if (u == 0x10038 && relative) fwheel = f;
          }
          mouseId = (uint8_t)rid;
        }
        bitpos[rid] = pos + rsize * rcount;
      } else if ((tag == 9 || tag == 11) && rid < 256) { /* Output, Feature: eigene Zählung */
      }
      nus = 0;
      usageMin = usageMax = 0;
    }
  }
  if (found)
    LOG("mouse report id %u: x %d/%d y %d/%d buttons %d,%d,%d wheel %d", mouseId, fx.off, fx.size, fy.off, fy.size,
        btn[0].off, btn[1].off, btn[2].off, fwheel.off);
  return found;
}

static int32_t field(const uint8_t *r, unsigned n, const Field &f) {
  if (f.off < 0 || f.size <= 0 || f.size > 32 || (unsigned)(f.off + f.size + 7) / 8 > n) return 0;
  uint64_t v = 0;
  for (int k = 0; k < f.size; k++) {
    unsigned bit = (unsigned)(f.off + k);
    if (r[bit >> 3] & (1 << (bit & 7))) v |= 1ull << k;
  }
  if (f.sign && f.size < 64 && (v & (1ull << (f.size - 1)))) v |= ~0ull << f.size;
  return (int32_t)(int64_t)v;
}

void ClokeI2CHID::report(const uint8_t *r, unsigned n) {
  if (mouseId) {
    if (!n || r[0] != mouseId) return;
    r++, n--;
  }
  int dx = field(r, n, fx), dy = field(r, n, fy), dz = field(r, n, fwheel);
  uint8_t buttons = 0;
  for (int k = 0; k < 3; k++)
    if (field(r, n, btn[k])) buttons |= (uint8_t)(1 << k);
  uint64_t now = nowMs();
  if (buttons != lastRaw && btnLog < 120) {
    btnLog++;
    /* Rohbytes dazu: sind die kurzen "losgelassen"-Meldungen echte Berichte oder leere Lesevorgänge? */
    char hex[3 * 16 + 1];
    unsigned m = n < 16 ? n : 16;
    for (unsigned i = 0; i < m; i++) snprintf(hex + 3 * i, 4, "%02x ", r[i]);
    hex[m ? 3 * m - 1 : 0] = 0;
    LOG("btn %u->%u at %llu ms%s [%s] dup %u", lastRaw, buttons, (unsigned long long)now,
        releasePending ? " (release pending)" : "", hex, dupRun);
  }
  lastRaw = buttons;
  if (releasePending) {
    if (buttons == lastButtons) { /* nach dem Loslassen gleich wieder gedrückt: Prellen, beides verwerfen */
      releasePending = false;
      bounces++;
      LOG("bounce suppressed (%u so far)", bounces);
    } else if (buttons != pendingButtons) {
      releasePending = false; /* andere Taste: das Loslassen sofort ausgeben, dann weiter */
      lastButtons = pendingButtons;
      emit(0, 0, 0, pendingButtons);
    }
  }
  if (!releasePending && (lastButtons & ~buttons)) { /* Taste losgelassen: erst später weitergeben */
    releasePending = true;
    pendingButtons = buttons;
    releaseAtMs = now + kDebounceMs;
    if (dx || dy || dz) emit(dx, dy, dz, lastButtons);
    return;
  }
  if (releasePending) { /* wartet noch: Bewegung mit den alten Tasten */
    if (dx || dy || dz) emit(dx, dy, dz, lastButtons);
    return;
  }
  if (!dx && !dy && !dz && buttons == lastButtons) return;
  lastButtons = buttons;
  emit(dx, dy, dz, buttons);
}

void ClokeI2CHID::emit(int dx, int dy, int dz, uint8_t buttons) {
  do {
    int sx = dx > 127 ? 127 : dx < -127 ? -127 : dx, sy = dy > 127 ? 127 : dy < -127 ? -127 : dy;
    int sz = dz > 127 ? 127 : dz < -127 ? -127 : dz;
    uint8_t rec[12] = {0, 0, 0, 0, 0, buttons, (uint8_t)(int8_t)sx, (uint8_t)(int8_t)sy, (uint8_t)(int8_t)sz, 0, 0, 0};
    gQ.push(rec);
    dx -= sx, dy -= sy, dz -= sz;
  } while (dx || dy || dz);
}

void ClokeI2CHID::poll(IOTimerEventSource *) {
  if (xfer(addr, nullptr, 0, rbuf, maxInput)) {
    unsigned len = (unsigned)(rbuf[0] | rbuf[1] << 8);
    if (len > 2 && len <= maxInput) {
      if (!reports++)
        LOG("first input report: length %u, id %u (mouse id %u)", len, len > 2 ? rbuf[2] : 0, mouseId);
      if (!mouseId || rbuf[2] == mouseId) mouseReports++;
      /* Ohne den Interrupt-Pin sieht der Treiber nicht, ob ein Bericht neu ist; manche Touchpads liefern
       * den letzten erneut. Gleiche Berichte hintereinander werden nach dem zweiten verworfen. */
      if (len == lastLen && len <= sizeof lastRep && !memcmp(rbuf, lastRep, len)) dupRun++;
      else {
        dupRun = 0;
        lastLen = len;
        if (len <= sizeof lastRep) memcpy(lastRep, rbuf, len);
      }
      if (dupRun < 2) report(rbuf + 2, len - 2);
    }
  } else
    readErrors++;
  /* aufgeschobenes Loslassen ausgeben */
  if (releasePending && nowMs() >= releaseAtMs) {
    releasePending = false;
    lastButtons = pendingButtons;
    emit(0, 0, 0, pendingButtons);
  }
  if (++polls == 3000)
    LOG("after 30 s: %u reports (%u mouse), %u read errors", reports, mouseReports, readErrors);
  timer->setTimeoutMS(10);
}

bool ClokeI2CHID::start(IOService *provider) {
  /* cloke_i2c=0 schaltet den Treiber ab (zum Eingrenzen von Hängern beim Start) */
  uint32_t on = 1;
  PE_parse_boot_argn("cloke_i2c", &on, sizeof on);
  if (!on) return false;
  LOG("start on %s", provider->getName());
  cloke_trace("I2C: START %s", provider->getName());
  if (!IOService::start(provider)) return false;
  pci = (IOPCIDevice *)provider;
  uint16_t vendor = pci->extendedConfigRead16(0), device = pci->extendedConfigRead16(2);
  /* Speicher und Busmaster an, D0 */
  pci->extendedConfigWrite16(4, (uint16_t)(pci->extendedConfigRead16(4) | 0x0006));
  for (uint8_t cap = pci->extendedConfigRead8(0x34), n = 0; cap && n < 32; cap = pci->extendedConfigRead8(cap + 1u), n++) {
    if (pci->extendedConfigRead8(cap) == 0x01) { /* Power Management */
      uint16_t pmcsr = pci->extendedConfigRead16(cap + 4u);
      if (pmcsr & 3) {
        pci->extendedConfigWrite16(cap + 4u, (uint16_t)(pmcsr & ~3u));
        IOSleep(10);
      }
      break;
    }
  }
  uint32_t bar0 = pci->extendedConfigRead32(0x10);
  if (bar0 & 1) { /* Ein-/Ausgabe-Ports statt Speicher: kein LPSS-I²C */
    LOG("%04x:%04x: BAR0 0x%08x is an I/O BAR, not an I2C controller", vendor, device, bar0);
    return false;
  }
  /* PureDarwins IOPCIFamily legt hier keine IODeviceMemory an (mapDeviceMemoryWithIndex liefert 0);
   * wie RavynXHCIPort die Adresse aus dem Konfigurationsbereich lesen, die die Firmware vergeben hat,
   * und den physischen Speicher selbst abbilden. BAR0 eines LPSS-Controllers sind 4 KB. */
  uint64_t phys = bar0 & ~0xfull;
  bool is64 = (bar0 & 6) == 4;
  if (is64) phys |= (uint64_t)pci->extendedConfigRead32(0x14) << 32;
  LOG("%04x:%04x: BAR0 config 0x%08x -> physical 0x%llx", vendor, device, bar0, (unsigned long long)phys);
  bool assigned = false;
  IOLockLock(gAssignLock);
  for (int attempt = 0; attempt < 5; attempt++) {
    if (phys && phys != 0xfffffff0ull) {
      IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(phys, 0x1000, kIODirectionNone);
      map = md ? md->map(kIOMapInhibitCache) : nullptr;
      OSSafeReleaseNULL(md);
      if (map && map->getLength() >= 0x1000) {
        base = (volatile uint8_t *)map->getVirtualAddress();
        if (!assigned) break;
        /* vergebene Adresse: nur gelten lassen, wenn dort der Controller antwortet. Im Reset liefert
         * IC_COMP_TYPE 0; das LPSS-Reset-Register (0x204) ist immer da, leere Adressen lesen als 0xffffffff */
        uint32_t t = rd(IC_COMP_TYPE), rr = rd(LPSS_RESETS);
        cloke_trace("I2C %04x: GELESEN TYPE 0x%08x RESETS 0x%08x", device, t, rr);
        LOG("%04x:%04x: probe at 0x%llx: COMP_TYPE 0x%08x, LPSS resets 0x%08x", vendor, device, (unsigned long long)phys, t, rr);
        if (t == 0x44570140 || (t == 0 && rr != 0xffffffffu)) break;
      }
      OSSafeReleaseNULL(map);
      base = nullptr;
    }
    /* keine (brauchbare) Adresse: selbst eine suchen und BAR0 dorthin legen */
    if (attempt == 3) break;
    if (!attempt) scanBars();
    /* Die Firmware hat die Geräte des Chipsatzes (HDA, SMBus, SPI, ...) dicht an dicht ab 0x6001000000 gelegt
     * (HP: bis 0x600113d000), die Grafik weit davor. Die Controller bekommen Platz knapp hinter dem
     * höchsten dieser Bereiche, im selben Fenster. Unter 4 GB (0x50300000 unter dem WLAN) bleibt der HP beim
     * ersten Lesen stehen, oberhalb der Grafik (0x3fffe00000) antwortet nichts. */
    if (!gTopPch) break;
    uint64_t cand = (gTopPch + 0x10000 + (uint64_t)(gAssigned + (unsigned)attempt) * 0x4000) & ~0xfffull;
    if (!is64 && (cand >> 32)) break;
    cloke_trace("I2C %04x: VERSUCH %d BAR0 = 0x%llx, SCHREIBE ...", device, attempt + 1, (unsigned long long)cand);
    pci->extendedConfigWrite16(4, (uint16_t)(pci->extendedConfigRead16(4) & ~0x0002u)); /* Speicher aus */
    pci->extendedConfigWrite32(0x10, (uint32_t)(cand & 0xfffffff0u) | (bar0 & 0xf));
    if (is64) pci->extendedConfigWrite32(0x14, (uint32_t)(cand >> 32));
    pci->extendedConfigWrite16(4, (uint16_t)(pci->extendedConfigRead16(4) | 0x0006));
    phys = cand;
    assigned = true;
    LOG("%04x:%04x: assigning BAR0 = 0x%llx (attempt %d)", vendor, device, (unsigned long long)cand, attempt + 1);
    cloke_trace("I2C %04x: BAR0 GESCHRIEBEN, LESE ...", device);
  }
  if (assigned && map) gAssigned++;
  IOLockUnlock(gAssignLock);
  if (!map) {
    if (assigned) { /* zurücksetzen: Speicher aus, BAR wieder leer */
      pci->extendedConfigWrite16(4, (uint16_t)(pci->extendedConfigRead16(4) & ~0x0002u));
      pci->extendedConfigWrite32(0x10, bar0);
      if (is64) pci->extendedConfigWrite32(0x14, 0);
    }
    LOG("%04x:%04x: no usable address for BAR0", vendor, device);
    return false;
  }
  wr(LPSS_RESETS, 0x7); /* LPSS: Funktion und DMA aus dem Reset */
  IODelay(100);
  uint32_t type = rd(IC_COMP_TYPE);
  if (type != 0x44570140) {
    LOG("%04x:%04x: no DesignWare I2C (type 0x%08x)", vendor, device, type);
    return false;
  }
  uint32_t param = rd(IC_COMP_PARAM_1);
  fifo = ((param >> 8) & 0xff) + 1;
  if (fifo < 8 || fifo > 256) fifo = 32;
  enable(false);
  /* Master, 400 kHz, Neustart erlaubt, kein Slave. Zeiten für einen Takt bis 216 MHz (Cannon Lake
   * und später); bei kleinerem Takt wird der Bus nur langsamer. */
  wr(IC_CON, 0x01 | 0x04 | 0x20 | 0x40);
  wr(IC_SS_SCL_HCNT, 1000);
  wr(IC_SS_SCL_LCNT, 1100);
  wr(IC_FS_SCL_HCNT, 200);
  wr(IC_FS_SCL_LCNT, 340);
  wr(IC_SDA_HOLD, 60);
  wr(IC_INTR_MASK, 0);
  wr(IC_RX_TL, 0);
  wr(IC_TX_TL, 0);
  LOG("%04x:%04x: DesignWare I2C, FIFO %u", vendor, device, fifo);

  /* übliche Adressen von Touchpads (ELAN, Synaptics, Goodix, FocalTech ...) und Deskriptor-Register */
  static const uint8_t addrs[] = {0x15, 0x2c, 0x10, 0x20, 0x2a, 0x24, 0x38, 0x5d, 0x14, 0x2d, 0x4c};
  static const uint16_t regs[] = {0x0001, 0x0020, 0x0000, 0x0030};
  bool found = false;
  for (uint8_t a : addrs) {
    for (uint16_t r : regs) {
      if ((found = probe(a, r))) break;
      if (abrtSrc & 1) break; /* Adresse antwortet nicht: die übrigen Register gar nicht erst */
      if (r == regs[0]) LOG("  0x%02x answers (abort source 0x%x)", a, abrtSrc);
    }
    if (found) break;
  }
  if (!found) {
    LOG("%04x:%04x: no HID touchpad found", vendor, device);
    return false;
  }

  if (!gQ.lock) gQ.lock = IOLockAlloc();
  if (!gBsdNotifier)
    gBsdNotifier = IOService::addMatchingNotification(gIOPublishNotification, IOService::resourceMatching("IOBSD"),
                                                      makeNode, nullptr, nullptr, 0);
  gDevices++;
  wl = IOWorkLoop::workLoop();
  timer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                                                          &ClokeI2CHID::poll));
  if (!wl || !timer || wl->addEventSource(timer) != kIOReturnSuccess) return false;
  timer->setTimeoutMS(10);
  LOG("touchpad ready at 0x%02x on %04x:%04x", addr, vendor, device);
  registerService();
  return true;
}

void ClokeI2CHID::stop(IOService *provider) {
  if (timer) {
    timer->cancelTimeout();
    if (wl) wl->removeEventSource(timer);
  }
  OSSafeReleaseNULL(timer);
  OSSafeReleaseNULL(wl);
  if (rbuf) IOFree(rbuf, maxInput);
  OSSafeReleaseNULL(map);
  IOService::stop(provider);
}
