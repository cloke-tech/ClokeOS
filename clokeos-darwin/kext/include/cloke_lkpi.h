/*
 * cloke_lkpi.h: dünne Linux-Treiber-Schnittstelle auf IOKit für ClokeOS-Kexts.
 *
 * Linux-Treiber (und die Doku dazu) sprechen Hardware über eine kleine, immer gleiche Menge an
 * Funktionen an: PCI-Konfiguration, BARs abbilden, readl/writel, Wartezeiten, DMA-Speicher, Firmware,
 * Interrupts, Sperren, Speicher. Diese Datei bildet genau diese Menge auf das ab, was PureDarwins
 * Kernel wirklich kann, damit Treiberwissen (oder Code mit verträglicher Lizenz, z. B. MIT/BSD
 * aus i915, rtw88, iwlwifi) ohne Umschreiben jeder Zeile in einen Kext wandern kann. Vorbild ist
 * FreeBSDs LinuxKPI, nur viel kleiner: nur, was ein Treiber zum Starten braucht.
 *
 * PureDarwin-Eigenheiten, die hier abgefangen werden (gemessen auf dem HP 15s und in QEMU):
 *   - IOPCIFamily legt keine IODeviceMemory für BARs an (mapDeviceMemoryWithIndex liefert 0):
 *     pci_iomap liest die Adresse aus dem Konfigurationsraum und bildet sie selbst ab.
 *   - Die IOPCIFamily-Header fehlen im Kernel.framework: IOPCIDevice wird hier mit den nicht-
 *     virtuellen Konfigurationszugriffen deklariert, die kc-builder gegen den Kernel bindet.
 *   - Physisch zusammenhängender DMA-Speicher unter 4 GB gibt es nur seitenweise (4 KB):
 *     dma_alloc_coherent lehnt Größeres ab, statt still falschen Speicher zu liefern.
 *   - Stapelrahmen ab 4 KB rufen ___chkstk_darwin, das es im Kernel nicht gibt: große Puffer
 *     immer mit kzalloc anlegen.
 *
 * Alles ist static inline: jeder Kext bindet nur, was er benutzt, und es gibt keine gemeinsame
 * Bibliothek, die zu Kollisionen zwischen Kexts führen könnte.
 */
#ifndef CLOKE_LKPI_H
#define CLOKE_LKPI_H

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSByteOrder.h>
#include <pexpert/pexpert.h>

extern "C" {
#include <kern/clock.h>
#include <sys/errno.h>
}

/* ---- Typen und Makros --------------------------------------------------------------------------- */

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t dma_addr_t;
typedef uint64_t resource_size_t;
#define __iomem

#define BIT(n) (1u << (n))
#define BIT_ULL(n) (1ull << (n))
#define GENMASK(h, l) (((~0u) << (l)) & (~0u >> (31 - (h))))
#define GENMASK_ULL(h, l) (((~0ull) << (l)) & (~0ull >> (63 - (h))))
/* REG_FIELD_GET/PREP wie in i915 und linux/bitfield.h: Maske bestimmt die Verschiebung */
#define FIELD_GET(mask, v) ((u32)(((v) & (mask)) >> __builtin_ctz(mask)))
#define FIELD_PREP(mask, v) ((u32)(((u32)(v) << __builtin_ctz(mask)) & (mask)))
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define lkpi_min(a, b) ((a) < (b) ? (a) : (b))
#define lkpi_max(a, b) ((a) > (b) ? (a) : (b))
#define DIV_ROUND_CLOSEST(x, d) (((x) + ((d) / 2)) / (d))
#define DIV_ROUND_UP(x, d) (((x) + (d) - 1) / (d))

/* ---- Ausgabe ------------------------------------------------------------------------------------ */

/* Linux-Formatstrings enden mit \n, IOLog will das auch */
#define lkpi_log(dev, lvl, fmt, ...) IOLog("%s: " lvl fmt, (dev)->name, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...) lkpi_log(dev, "", fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) lkpi_log(dev, "warning: ", fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...) lkpi_log(dev, "error: ", fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...)                                                                                   \
  do {                                                                                                           \
    if ((dev)->debug) lkpi_log(dev, "debug: ", fmt, ##__VA_ARGS__);                                              \
  } while (0)
#define WARN_ON(c)                                                                                               \
  ({                                                                                                             \
    bool _w = !!(c);                                                                                             \
    if (_w) IOLog("WARN_ON(%s) at %s:%d\n", #c, __FILE__, __LINE__);                                             \
    _w;                                                                                                          \
  })

/* ---- Zeit --------------------------------------------------------------------------------------- */

static inline void udelay(unsigned us) { IODelay(us); }
static inline void ndelay(unsigned ns) { IODelay((ns + 999) / 1000); }
static inline void mdelay(unsigned ms) { IODelay(ms * 1000); }
static inline void msleep(unsigned ms) { IOSleep(ms); }
static inline void usleep_range(unsigned lo, unsigned hi) {
  if (lo < 1000) IODelay(lo);
  else IOSleep((lo + 999) / 1000);
}
static inline u64 ktime_get_ns(void) {
  uint64_t abs = mach_absolute_time(), ns = 0;
  absolutetime_to_nanoseconds(abs, &ns);
  return ns;
}

/* i915: wait_for(COND, MS) – 0 wenn COND innerhalb von MS Millisekunden wahr wurde, sonst -ETIMEDOUT */
#define wait_for_us(COND, US)                                                                                    \
  ({                                                                                                             \
    u64 _end = ktime_get_ns() + (u64)(US) * 1000;                                                                \
    int _r = -ETIMEDOUT;                                                                                         \
    for (;;) {                                                                                                   \
      if (COND) {                                                                                                \
        _r = 0;                                                                                                  \
        break;                                                                                                   \
      }                                                                                                          \
      if (ktime_get_ns() > _end) break;                                                                          \
      IODelay(10);                                                                                               \
    }                                                                                                            \
    _r;                                                                                                          \
  })
#define wait_for(COND, MS) wait_for_us(COND, (MS) * 1000)

/* ---- MMIO --------------------------------------------------------------------------------------- */

static inline u32 readl(const volatile void __iomem *a) {
  u32 v = *(const volatile u32 *)a;
  __asm__ volatile("" ::: "memory");
  return v;
}
static inline void writel(u32 v, volatile void __iomem *a) {
  __asm__ volatile("" ::: "memory");
  *(volatile u32 *)a = v;
}
static inline u16 readw(const volatile void __iomem *a) { return *(const volatile u16 *)a; }
static inline void writew(u16 v, volatile void __iomem *a) { *(volatile u16 *)a = v; }
static inline u8 readb(const volatile void __iomem *a) { return *(const volatile u8 *)a; }
static inline void writeb(u8 v, volatile void __iomem *a) { *(volatile u8 *)a = v; }
static inline u64 readq(const volatile void __iomem *a) { return *(const volatile u64 *)a; }
static inline void writeq(u64 v, volatile void __iomem *a) { *(volatile u64 *)a = v; }
#define ioread32 readl
#define iowrite32 writel
/* Lesen nach Schreiben, damit ein gepostetes Schreiben das Gerät erreicht hat */
#define posting_read(a) ((void)readl(a))

/* ---- PCI ---------------------------------------------------------------------------------------- */

/* Siehe Kopf: nur die nicht-virtuellen Zugriffe, die auch PureDarwins eigene Treiber benutzen */
class IOPCIDevice : public IOService {
public:
  UInt8 extendedConfigRead8(IOByteCount offset);
  UInt16 extendedConfigRead16(IOByteCount offset);
  UInt32 extendedConfigRead32(IOByteCount offset);
  void extendedConfigWrite8(IOByteCount offset, UInt8 data);
  void extendedConfigWrite16(IOByteCount offset, UInt16 data);
  void extendedConfigWrite32(IOByteCount offset, UInt32 data);
};

#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_COMMAND_IO 0x1
#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_MASTER 0x4
#define PCI_COMMAND_INTX_DISABLE 0x400
#define PCI_REVISION_ID 0x08
#define PCI_CLASS_REVISION 0x08
#define PCI_BASE_ADDRESS_0 0x10
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c
#define PCI_SUBSYSTEM_ID 0x2e
#define PCI_CAPABILITY_LIST 0x34
#define PCI_CAP_ID_PM 0x01
#define PCI_CAP_ID_MSI 0x05
#define PCI_CAP_ID_EXP 0x10
#define PCI_CAP_ID_MSIX 0x11

struct pci_bar {
  u64 start; /* physische Adresse, 0 = nicht vergeben */
  bool mem64, prefetch, io;
};

struct pci_dev {
  IOPCIDevice *pci;
  IOService *driver;
  const char *name; /* Präfix für dev_info & Co. */
  u16 vendor, device, subsystem_vendor, subsystem_device;
  u8 revision;
  u32 class_code; /* 0xCCSSPP */
  struct pci_bar bar[6];
  bool debug;
};

static inline int pci_read_config_byte(struct pci_dev *d, int where, u8 *v) {
  *v = d->pci->extendedConfigRead8((IOByteCount)(where));
  return 0;
}
static inline int pci_read_config_word(struct pci_dev *d, int where, u16 *v) {
  *v = d->pci->extendedConfigRead16((IOByteCount)(where));
  return 0;
}
static inline int pci_read_config_dword(struct pci_dev *d, int where, u32 *v) {
  *v = d->pci->extendedConfigRead32((IOByteCount)(where));
  return 0;
}
static inline int pci_write_config_byte(struct pci_dev *d, int where, u8 v) {
  d->pci->extendedConfigWrite8((IOByteCount)(where), v);
  return 0;
}
static inline int pci_write_config_word(struct pci_dev *d, int where, u16 v) {
  d->pci->extendedConfigWrite16((IOByteCount)(where), v);
  return 0;
}
static inline int pci_write_config_dword(struct pci_dev *d, int where, u32 v) {
  d->pci->extendedConfigWrite32((IOByteCount)(where), v);
  return 0;
}

static inline int pci_find_capability(struct pci_dev *d, int cap_id) {
  u8 p = d->pci->extendedConfigRead8((IOByteCount)(PCI_CAPABILITY_LIST)) & ~3u;
  for (int n = 0; p && n < 48; n++) {
    if (d->pci->extendedConfigRead8((IOByteCount)(p)) == cap_id) return p;
    p = d->pci->extendedConfigRead8((IOByteCount)(p + 1u)) & ~3u;
  }
  return 0;
}

/* Kennungen und BAR-Adressen lesen, wie die Firmware sie vergeben hat. Ohne Größenbestimmung: dafür
 * müsste man ~0 in die BARs schreiben, und das stört ein Gerät, das gerade läuft (die Anzeige). */
static inline void lkpi_pci_init(struct pci_dev *d, IOService *driver, IOService *provider, const char *name) {
  d->pci = (IOPCIDevice *)provider;
  d->driver = driver;
  d->name = name;
  d->vendor = d->pci->extendedConfigRead16((IOByteCount)(PCI_VENDOR_ID));
  d->device = d->pci->extendedConfigRead16((IOByteCount)(PCI_DEVICE_ID));
  d->subsystem_vendor = d->pci->extendedConfigRead16((IOByteCount)(PCI_SUBSYSTEM_VENDOR_ID));
  d->subsystem_device = d->pci->extendedConfigRead16((IOByteCount)(PCI_SUBSYSTEM_ID));
  u32 cr = d->pci->extendedConfigRead32((IOByteCount)(PCI_CLASS_REVISION));
  d->revision = (u8)cr;
  d->class_code = cr >> 8;
  for (int i = 0; i < 6; i++) {
    u32 lo = d->pci->extendedConfigRead32((IOByteCount)(PCI_BASE_ADDRESS_0 + 4 * i));
    struct pci_bar *b = &d->bar[i];
    *b = {};
    if (lo & 1) {
      b->io = true;
      b->start = lo & ~3u;
      continue;
    }
    b->mem64 = (lo & 6) == 4;
    b->prefetch = lo & 8;
    b->start = lo & ~0xfull;
    if (b->mem64 && i < 5) {
      b->start |= (u64)d->pci->extendedConfigRead32((IOByteCount)(PCI_BASE_ADDRESS_0 + 4 * (i + 1))) << 32;
      d->bar[++i] = {};
    }
  }
}

static inline resource_size_t pci_resource_start(struct pci_dev *d, int bar) { return d->bar[bar].start; }

static inline int pci_enable_device_mem(struct pci_dev *d) {
  u16 cmd = d->pci->extendedConfigRead16((IOByteCount)(PCI_COMMAND));
  if (!(cmd & PCI_COMMAND_MEMORY)) d->pci->extendedConfigWrite16((IOByteCount)(PCI_COMMAND), cmd | PCI_COMMAND_MEMORY);
  /* D0, falls die Firmware das Gerät schlafen gelegt hat */
  int pm = pci_find_capability(d, PCI_CAP_ID_PM);
  if (pm) {
    u16 pmcsr = d->pci->extendedConfigRead16((IOByteCount)(pm + 4));
    if (pmcsr & 3) {
      d->pci->extendedConfigWrite16((IOByteCount)(pm + 4), pmcsr & ~3u);
      IOSleep(10);
    }
  }
  return 0;
}
static inline void pci_set_master(struct pci_dev *d) {
  u16 cmd = d->pci->extendedConfigRead16((IOByteCount)(PCI_COMMAND));
  if (!(cmd & PCI_COMMAND_MASTER)) d->pci->extendedConfigWrite16((IOByteCount)(PCI_COMMAND), cmd | PCI_COMMAND_MASTER);
}

/* Abbildung eines BAR-Ausschnitts; die IOMemoryMap gehört dem Aufrufer (lkpi_iounmap) */
struct lkpi_iomap {
  IOMemoryMap *map;
  volatile u8 __iomem *base;
  u64 phys, len;
};
static inline int lkpi_ioremap(struct lkpi_iomap *m, u64 phys, u64 len) {
  *m = {};
  if (!phys || phys == 0xfffffff0ull) return -ENODEV;
  IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(phys, len, kIODirectionNone);
  m->map = md ? md->map(kIOMapInhibitCache) : nullptr;
  OSSafeReleaseNULL(md);
  if (!m->map || m->map->getLength() < len) {
    OSSafeReleaseNULL(m->map);
    return -ENOMEM;
  }
  m->base = (volatile u8 *)m->map->getVirtualAddress();
  m->phys = phys;
  m->len = len;
  return 0;
}
static inline int pci_iomap_range(struct pci_dev *d, int bar, u64 off, u64 len, struct lkpi_iomap *m) {
  if (d->bar[bar].io) return -EINVAL;
  return lkpi_ioremap(m, d->bar[bar].start + off, len);
}
static inline void lkpi_iounmap(struct lkpi_iomap *m) {
  OSSafeReleaseNULL(m->map);
  m->base = nullptr;
}

/* ---- Speicher ----------------------------------------------------------------------------------- */

/* kzalloc/kfree: IOMalloc braucht die Größe beim Freigeben, sie steht in einem Kopf davor */
static inline void *kzalloc(size_t n, int = 0) {
  size_t *p = (size_t *)IOMallocZero(n + 16);
  if (!p) return nullptr;
  p[0] = n + 16;
  return (u8 *)p + 16;
}
static inline void *kmalloc(size_t n, int f = 0) { return kzalloc(n, f); }
static inline void *kcalloc(size_t n, size_t sz, int f = 0) { return kzalloc(n * sz, f); }
static inline void kfree(const void *p) {
  if (!p) return;
  size_t *h = (size_t *)((u8 *)p - 16);
  IOFree(h, h[0]);
}
#define GFP_KERNEL 0
#define GFP_ATOMIC 0

/* DMA-Speicher: physisch zusammenhängend, unter 4 GB, nicht cachebar abgebildet. PureDarwin liefert
 * das nur bis 4 KB (ClokeRtw88: inTaskWithPhysicalMask 4096 ok, 4256 nicht) – Größeres als mehrere
 * Seiten-Segmente anlegen (Ringe, Firmware-Download in Blöcken). */
struct lkpi_dma {
  IOBufferMemoryDescriptor *bmd;
};
static inline void *dma_alloc_coherent(struct pci_dev *d, size_t size, dma_addr_t *handle, struct lkpi_dma *owner) {
  owner->bmd = nullptr;
  if (size > PAGE_SIZE) {
    dev_err(d, "dma_alloc_coherent: %lu bytes, PureDarwin gives contiguous memory only per page\n", (unsigned long)size);
    return nullptr;
  }
  owner->bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous |
                                                                                   kIOMapInhibitCache,
                                                                PAGE_SIZE, 0x00000000fffff000ull);
  if (!owner->bmd) return nullptr;
  owner->bmd->prepare();
  *handle = owner->bmd->getPhysicalSegment(0, nullptr, 0);
  bzero(owner->bmd->getBytesNoCopy(), PAGE_SIZE);
  return owner->bmd->getBytesNoCopy();
}
static inline void dma_free_coherent(struct lkpi_dma *owner) {
  if (!owner->bmd) return;
  owner->bmd->complete();
  OSSafeReleaseNULL(owner->bmd);
}

/* ---- Sperren ------------------------------------------------------------------------------------ */

struct mutex {
  IOLock *l;
};
static inline void mutex_init(struct mutex *m) { m->l = IOLockAlloc(); }
static inline void mutex_destroy(struct mutex *m) {
  if (m->l) IOLockFree(m->l);
  m->l = nullptr;
}
static inline void mutex_lock(struct mutex *m) { IOLockLock(m->l); }
static inline void mutex_unlock(struct mutex *m) { IOLockUnlock(m->l); }

typedef struct {
  IOSimpleLock *l;
  IOInterruptState s;
} spinlock_t;
static inline void spin_lock_init(spinlock_t *s) { s->l = IOSimpleLockAlloc(); }
static inline void spin_lock_irqsave_(spinlock_t *s) { s->s = IOSimpleLockLockDisableInterrupt(s->l); }
static inline void spin_unlock_irqrestore_(spinlock_t *s) { IOSimpleLockUnlockEnableInterrupt(s->l, s->s); }
#define spin_lock_irqsave(s, flags) ((void)(flags), spin_lock_irqsave_(s))
#define spin_unlock_irqrestore(s, flags) ((void)(flags), spin_unlock_irqrestore_(s))

/* ---- Firmware ----------------------------------------------------------------------------------- */

/* Kein Dateisystem, wenn Treiber starten (sie laufen vor dem Root-Mount): Firmware wird beim Bauen
 * als C-Feld eingebaut (kext/build.sh) und hier unter ihrem linux-firmware-Namen angemeldet. */
struct firmware {
  size_t size;
  const u8 *data;
};
struct lkpi_fw_entry {
  const char *name;
  const u8 *data;
  size_t size;
};
static inline int request_firmware_from(const struct firmware **fw, const char *name, const struct lkpi_fw_entry *table,
                                        size_t n, struct pci_dev *d) {
  static struct firmware slots[8];
  static unsigned next;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(table[i].name, name) == 0) {
      struct firmware *f = &slots[next++ % ARRAY_SIZE(slots)];
      f->data = table[i].data;
      f->size = table[i].size;
      *fw = f;
      return 0;
    }
  }
  dev_err(d, "firmware %s is not built in\n", name);
  *fw = nullptr;
  return -ENOENT;
}
static inline void release_firmware(const struct firmware *) {}

/* ---- Interrupts --------------------------------------------------------------------------------- */

/* request_irq: Handler läuft im Workloop des Treibers (nicht im Interrupt), wie ein threaded IRQ
 * unter Linux. Index 0 = was IOPCIFamily als erstes anbietet (bei PureDarwin MSI, falls das Gerät es
 * kann, sonst INTx). Ungetestet auf echter Hardware: bisherige ClokeOS-Treiber pollen. Wer pollen will,
 * nimmt lkpi_poll_start. */
typedef void (*lkpi_irq_handler_t)(void *data);
struct lkpi_irq {
  IOWorkLoop *wl;
  IOInterruptEventSource *src;
  IOTimerEventSource *timer;
  lkpi_irq_handler_t fn;
  void *data;
  unsigned period_ms;
};
/* PureDarwins Event-Sources haben kein RefCon: Zuordnung Quelle -> lkpi_irq über eine kleine Tabelle */
static inline struct lkpi_irq **lkpi_irq_table(void) {
  static struct lkpi_irq *t[16];
  return t;
}
static inline struct lkpi_irq *lkpi_irq_find(const void *src) {
  struct lkpi_irq **t = lkpi_irq_table();
  for (int i = 0; i < 16; i++)
    if (t[i] && ((const void *)t[i]->src == src || (const void *)t[i]->timer == src)) return t[i];
  return nullptr;
}
static inline bool lkpi_irq_add(struct lkpi_irq *irq) {
  struct lkpi_irq **t = lkpi_irq_table();
  for (int i = 0; i < 16; i++)
    if (!t[i]) {
      t[i] = irq;
      return true;
    }
  return false;
}
static inline void lkpi_irq_del(struct lkpi_irq *irq) {
  struct lkpi_irq **t = lkpi_irq_table();
  for (int i = 0; i < 16; i++)
    if (t[i] == irq) t[i] = nullptr;
}
static inline int request_irq(struct pci_dev *d, struct lkpi_irq *irq, lkpi_irq_handler_t fn, void *data, int index = 0) {
  irq->fn = fn;
  irq->data = data;
  irq->wl = IOWorkLoop::workLoop();
  if (!irq->wl) return -ENOMEM;
  irq->src = IOInterruptEventSource::interruptEventSource(
      (OSObject *)d->driver,
      [](OSObject *, IOInterruptEventSource *s, int) {
        struct lkpi_irq *self = lkpi_irq_find(s);
        if (self && self->fn) self->fn(self->data);
      },
      d->pci, index);
  if (!irq->src || !lkpi_irq_add(irq)) return -ENODEV;
  if (irq->wl->addEventSource(irq->src) != kIOReturnSuccess) return -EIO;
  irq->src->enable();
  return 0;
}
static inline int lkpi_poll_start(struct pci_dev *d, struct lkpi_irq *irq, lkpi_irq_handler_t fn, void *data,
                                  unsigned period_ms) {
  irq->fn = fn;
  irq->data = data;
  irq->period_ms = period_ms;
  irq->wl = IOWorkLoop::workLoop();
  if (!irq->wl) return -ENOMEM;
  irq->timer = IOTimerEventSource::timerEventSource((OSObject *)d->driver, [](OSObject *, IOTimerEventSource *t) {
    struct lkpi_irq *self = lkpi_irq_find(t);
    if (!self) return;
    if (self->fn) self->fn(self->data);
    t->setTimeoutMS(self->period_ms);
  });
  if (!irq->timer || !lkpi_irq_add(irq)) return -ENOMEM;
  if (irq->wl->addEventSource(irq->timer) != kIOReturnSuccess) return -EIO;
  irq->timer->setTimeoutMS(period_ms);
  return 0;
}
static inline void free_irq(struct lkpi_irq *irq) {
  lkpi_irq_del(irq);
  if (irq->src) {
    irq->src->disable();
    if (irq->wl) irq->wl->removeEventSource(irq->src);
  }
  if (irq->timer) {
    irq->timer->cancelTimeout();
    if (irq->wl) irq->wl->removeEventSource(irq->timer);
  }
  OSSafeReleaseNULL(irq->src);
  OSSafeReleaseNULL(irq->timer);
  OSSafeReleaseNULL(irq->wl);
}

/* ---- Boot-Args ---------------------------------------------------------------------------------- */

/* Jeder Treiber hat einen Schalter cloke_<name>=N (0 = aus), damit ein Hänger beim Start auf dem
 * echten Gerät ohne neues Image eingegrenzt werden kann */
static inline u32 lkpi_bootarg(const char *name, u32 def) {
  u32 v = def;
  PE_parse_boot_argn(name, &v, sizeof v);
  return v;
}

#endif
