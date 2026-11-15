/*
 * igfx_regs.h: Anzeige-Register von Intel-Grafik Gen11/12 (Ice Lake, Tiger Lake, Alder Lake, Raptor Lake)
 * und ihre Auswertung. Ohne Kernel-Abhängigkeiten, damit test/igfx_test.cpp sie auf dem Mac mit einem
 * nachgebauten Registersatz prüfen kann.
 *
 * Quellen: Intel Graphics Programmer's Reference Manuals (öffentlich, 01.org / intel.com, "Volume 2c:
 * Command Reference: Registers" für Tiger Lake und Alder Lake) und die Register-Definitionen des
 * Linux-Treibers i915 (drivers/gpu/drm/i915/display, Dateien *_regs.h, MIT-Lizenz). Hier stehen nur Adressen
 * und Bitfelder, kein Code aus i915.
 */
#ifndef IGFX_REGS_H
#define IGFX_REGS_H

#include <stddef.h>
#include <stdint.h>

/* ---- Plattformen -------------------------------------------------------------------------------- */

enum igfx_platform { IGFX_UNKNOWN = 0, IGFX_ICL, IGFX_JSL, IGFX_TGL, IGFX_RKL, IGFX_ADLS, IGFX_ADLP, IGFX_ADLN, IGFX_RPLS, IGFX_RPLP };

struct igfx_info {
  enum igfx_platform platform;
  const char *name;
  unsigned display_ver; /* 11, 12, 13 */
  unsigned pipes;
};

/* PCI-Kennungen aus include/drm/intel/i915_pciids.h (Linux, MIT) */
static inline struct igfx_info igfx_identify(uint16_t did) {
  static const uint16_t icl[] = {0x8A50, 0x8A51, 0x8A52, 0x8A53, 0x8A54, 0x8A56, 0x8A57, 0x8A58,
                                 0x8A59, 0x8A5A, 0x8A5B, 0x8A5C, 0x8A5D, 0x8A71};
  static const uint16_t jsl[] = {0x4E51, 0x4E55, 0x4E57, 0x4E61, 0x4E71, 0x4541, 0x4551, 0x4555, 0x4557, 0x4570, 0x4571};
  static const uint16_t tgl[] = {0x9A40, 0x9A49, 0x9A59, 0x9A60, 0x9A68, 0x9A70, 0x9A78, 0x9AC0, 0x9AC9, 0x9AD9, 0x9AF8};
  static const uint16_t rkl[] = {0x4C80, 0x4C8A, 0x4C8B, 0x4C8C, 0x4C90, 0x4C9A};
  static const uint16_t adls[] = {0x4680, 0x4682, 0x4688, 0x468A, 0x468B, 0x4690, 0x4692, 0x4693};
  static const uint16_t adlp[] = {0x46A0, 0x46A1, 0x46A2, 0x46A3, 0x46A6, 0x46A8, 0x46AA, 0x462A, 0x4626,
                                  0x4628, 0x46B0, 0x46B1, 0x46B2, 0x46B3, 0x46C0, 0x46C1, 0x46C2, 0x46C3};
  static const uint16_t adln[] = {0x46D0, 0x46D1, 0x46D2, 0x46D3, 0x46D4};
  static const uint16_t rpls[] = {0xA780, 0xA781, 0xA782, 0xA783, 0xA788, 0xA789, 0xA78A, 0xA78B};
  static const uint16_t rplp[] = {0xA720, 0xA721, 0xA7A0, 0xA7A1, 0xA7A8, 0xA7A9, 0xA7AA, 0xA7AB, 0xA7AC, 0xA7AD};
  struct row {
    const uint16_t *ids;
    size_t n;
    struct igfx_info info;
  };
  static const struct row rows[] = {
      {icl, sizeof icl / 2, {IGFX_ICL, "Ice Lake", 11, 3}},
      {jsl, sizeof jsl / 2, {IGFX_JSL, "Jasper/Elkhart Lake", 11, 3}},
      {tgl, sizeof tgl / 2, {IGFX_TGL, "Tiger Lake", 12, 4}},
      {rkl, sizeof rkl / 2, {IGFX_RKL, "Rocket Lake", 12, 3}},
      {adls, sizeof adls / 2, {IGFX_ADLS, "Alder Lake-S", 12, 4}},
      {adlp, sizeof adlp / 2, {IGFX_ADLP, "Alder Lake-P", 13, 4}},
      {adln, sizeof adln / 2, {IGFX_ADLN, "Alder Lake-N", 13, 4}},
      {rpls, sizeof rpls / 2, {IGFX_RPLS, "Raptor Lake-S", 12, 4}},
      {rplp, sizeof rplp / 2, {IGFX_RPLP, "Raptor Lake-P", 13, 4}},
  };
  for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++)
    for (size_t i = 0; i < rows[r].n; i++)
      if (rows[r].ids[i] == did) return rows[r].info;
  struct igfx_info u = {IGFX_UNKNOWN, "unknown Intel GPU", 0, 3};
  return u;
}

/* ---- Register ----------------------------------------------------------------------------------- */

/* Transcoder/Pipe A..D liegen 0x1000 auseinander */
#define TRANS_HTOTAL(t) (0x60000u + 0x1000u * (t))
#define TRANS_HBLANK(t) (0x60004u + 0x1000u * (t))
#define TRANS_HSYNC(t) (0x60008u + 0x1000u * (t))
#define TRANS_VTOTAL(t) (0x6000Cu + 0x1000u * (t))
#define TRANS_VBLANK(t) (0x60010u + 0x1000u * (t))
#define TRANS_VSYNC(t) (0x60014u + 0x1000u * (t))
#define PIPESRC(p) (0x6001Cu + 0x1000u * (p))
#define TRANS_DDI_FUNC_CTL(t) (0x60400u + 0x1000u * (t))
#define TRANS_CONF(t) (0x70008u + 0x1000u * (t)) /* PIPECONF */
#define PIPE_FRMCOUNT(p) (0x70040u + 0x1000u * (p))
/* Universal Plane 1 je Pipe (das, was GOP benutzt) */
#define PLANE_CTL(p) (0x70180u + 0x1000u * (p))
#define PLANE_STRIDE(p) (0x70188u + 0x1000u * (p))
#define PLANE_POS(p) (0x7018Cu + 0x1000u * (p))
#define PLANE_SIZE(p) (0x70190u + 0x1000u * (p))
#define PLANE_SURF(p) (0x7019Cu + 0x1000u * (p))
#define PLANE_OFFSET(p) (0x701A4u + 0x1000u * (p))
#define PLANE_SURFLIVE(p) (0x701ACu + 0x1000u * (p)) /* Adresse, die gerade wirklich ausgelesen wird */
/* Pipe-Skalierer 1 (Panel-Fitter) */
#define PS_CTRL(p) (((p) < 2 ? 0x68180u : 0x69180u) + ((p) & 1) * 0x800u)
#define PS_WIN_POS(p) (((p) < 2 ? 0x68170u : 0x69170u) + ((p) & 1) * 0x800u)
#define PS_WIN_SZ(p) (((p) < 2 ? 0x68174u : 0x69174u) + ((p) & 1) * 0x800u)
/* DDI A..E und AUX-Kanäle */
#define DDI_BUF_CTL(port) (0x64000u + 0x100u * (port))
#define DP_AUX_CH_CTL(ch) (0x64010u + 0x100u * (ch))
#define DP_AUX_CH_DATA(ch, i) (0x64014u + 0x100u * (ch) + 4u * (i))
/* Stromversorgung der Anzeige-Bereiche */
#define PWR_WELL_CTL2 0x45404u
/* Panel Self Refresh (Gen12: je Transcoder); steht er an, holt die Anzeige keine neuen Bilder */
#define TRANS_PSR_CTL(t) (0x60800u + 0x1000u * (t))
#define TRANS_PSR2_CTL(t) (0x60900u + 0x1000u * (t))
/* GGTT: Seitentabelle der GPU, ab Gen8 in der oberen Hälfte von BAR0 (GTTMMADR + 8 MB), 8 Byte je 4-KB-Seite.
 * Größe aus GMCH_CTRL (PCI-Konfiguration 0x50, GGMS Bits 7:6: 1 << GGMS MB Tabelle). Nach dem Ändern
 * von Einträgen den TLB über GFX_FLSH_CNTL leeren. */
#define GGTT_OFFSET_IN_BAR0 0x800000u
#define PCI_GMCH_CTRL 0x50
#define GFX_FLSH_CNTL 0x101008u
#define GGTT_PTE_PRESENT 1ull
/* Panel-Stromversorgung und Hintergrundlicht im Chipsatz (South Display, Cannon Point und neuer) */
#define PP_STATUS 0xC7200u
#define PP_CONTROL 0xC7204u
#define BLC_PWM_CTL(c) (0xC8250u + 0x100u * (c))
#define BLC_PWM_FREQ(c) (0xC8254u + 0x100u * (c))
#define BLC_PWM_DUTY(c) (0xC8258u + 0x100u * (c))

#define TRANS_CONF_ENABLE (1u << 31)
#define TRANS_CONF_STATE (1u << 30)
#define PLANE_CTL_ENABLE (1u << 31)
#define PS_CTRL_ENABLE (1u << 31)
#define DDI_BUF_CTL_ENABLE (1u << 31)
#define BLC_PWM_ENABLE (1u << 31)
#define BLC_PWM_POLARITY (1u << 29)
#define PP_ON (1u << 31)
#define PP_CONTROL_BACKLIGHT (1u << 2)
#define PP_CONTROL_POWER_ON (1u << 0)

#define AUX_SEND_BUSY (1u << 31)
#define AUX_DONE (1u << 30)
#define AUX_TIME_OUT_ERROR (1u << 28)
#define AUX_TIME_OUT_MAX (3u << 26)
#define AUX_RECEIVE_ERROR (1u << 25)
#define AUX_MSG_SIZE_SHIFT 20
#define AUX_MSG_SIZE_MASK (0x1fu << 20)
#define AUX_FW_SYNC_PULSE(n) (((n) - 1u) << 5)
#define AUX_SYNC_PULSE(n) ((n) - 1u)

/* DPCD-Adressen (VESA eDP 1.4) */
#define DPCD_EDP_REV 0x700
#define DPCD_EDP_GENERAL_CAP_1 0x701
#define DPCD_EDP_BACKLIGHT_ADJ_CAP 0x702
#define DPCD_EDP_BACKLIGHT_MODE_SET 0x721
#define DPCD_EDP_BRIGHTNESS_MSB 0x722
#define DPCD_EDP_BRIGHTNESS_LSB 0x723

static inline uint64_t igfx_ggtt_size(uint16_t gmch_ctrl) {
  unsigned ggms = (gmch_ctrl >> 6) & 3;
  if (!ggms) return 0;
  return (uint64_t)(1u << ggms) * 1024 * 1024 / 8 * 4096; /* Einträge × 4 KB */
}
static inline uint64_t igfx_ggtt_pte(uint64_t phys) { return (phys & 0x7ffffff000ull) | GGTT_PTE_PRESENT; }

/* ---- Auswertung --------------------------------------------------------------------------------- */

typedef uint32_t (*igfx_read_fn)(void *ctx, uint32_t reg);

struct igfx_mode {
  int active;   /* Transcoder an und läuft */
  unsigned hactive, vactive, htotal, vtotal;
  unsigned src_w, src_h; /* PIPESRC: Größe, die die Ebenen liefern */
  int scaled;            /* Pipe-Skalierer an */
  int plane_on;
  unsigned plane_w, plane_h, stride_bytes;
  uint32_t surf, plane_ctl;
  unsigned tiling; /* PLANE_CTL 12:10: 0 linear, 1 X, 4 Y, 5 Yf (Gen12: 4 = Tile-4/Y) */
  int ddi_port;    /* -1: unbekannt, 0 = A ... */
  unsigned ddi_mode, bpc;
};

static inline unsigned igfx_lo(uint32_t v) { return (v & 0xffffu) + 1; }
static inline unsigned igfx_hi(uint32_t v) { return ((v >> 16) & 0xffffu) + 1; }

static inline struct igfx_mode igfx_read_mode(igfx_read_fn rd, void *c, unsigned pipe, unsigned display_ver) {
  struct igfx_mode m;
  uint32_t conf = rd(c, TRANS_CONF(pipe));
  uint32_t ht = rd(c, TRANS_HTOTAL(pipe)), vt = rd(c, TRANS_VTOTAL(pipe)), src = rd(c, PIPESRC(pipe));
  uint32_t pc = rd(c, PLANE_CTL(pipe)), ps = rd(c, PLANE_SIZE(pipe)), st = rd(c, PLANE_STRIDE(pipe));
  uint32_t ddi = rd(c, TRANS_DDI_FUNC_CTL(pipe));
  m.active = (conf & TRANS_CONF_ENABLE) && (conf & TRANS_CONF_STATE);
  m.hactive = igfx_lo(ht), m.htotal = igfx_hi(ht);
  m.vactive = igfx_lo(vt), m.vtotal = igfx_hi(vt);
  /* PIPESRC: Breite oben, Höhe unten */
  m.src_w = igfx_hi(src), m.src_h = igfx_lo(src);
  m.scaled = (rd(c, PS_CTRL(pipe)) & PS_CTRL_ENABLE) != 0;
  m.plane_on = (pc & PLANE_CTL_ENABLE) != 0;
  m.plane_ctl = pc;
  m.plane_w = igfx_lo(ps), m.plane_h = igfx_hi(ps);
  m.tiling = (pc >> 10) & 7;
  /* Linear: Schrittweite in 64-Byte-Einheiten (Bits 11:0) */
  m.stride_bytes = (st & 0xfffu) * 64;
  m.surf = rd(c, PLANE_SURF(pipe)) & ~0xfffu;
  m.ddi_port = -1;
  m.ddi_mode = (ddi >> 24) & 7;
  m.bpc = (ddi >> 20) & 7;
  if (ddi & (1u << 31)) {
    /* Gen12: Port-Auswahl Bits 30:27 = Port + 1; Gen11: Bits 30:28 = Port */
    m.ddi_port = display_ver >= 12 ? (int)((ddi >> 27) & 0xf) - 1 : (int)((ddi >> 28) & 7);
  }
  return m;
}

struct igfx_backlight {
  int controller; /* -1: kein PWM-Regler an */
  uint32_t ctl, freq, duty;
  unsigned percent;
  int inverted;
};

static inline struct igfx_backlight igfx_read_backlight(igfx_read_fn rd, void *c) {
  struct igfx_backlight b = {-1, 0, 0, 0, 0, 0};
  for (unsigned ctl = 0; ctl < 2; ctl++) {
    uint32_t v = rd(c, BLC_PWM_CTL(ctl));
    if (!(v & BLC_PWM_ENABLE)) continue;
    b.controller = (int)ctl;
    b.ctl = v;
    b.freq = rd(c, BLC_PWM_FREQ(ctl));
    b.duty = rd(c, BLC_PWM_DUTY(ctl));
    b.inverted = (v & BLC_PWM_POLARITY) != 0;
    if (b.freq) {
      uint32_t d = b.inverted ? b.freq - (b.duty > b.freq ? b.freq : b.duty) : b.duty;
      b.percent = (unsigned)(((uint64_t)d * 100 + b.freq / 2) / b.freq);
    }
    break;
  }
  return b;
}

/* Tastverhältnis für percent (5..100; darunter ist das Panel meist schwarz und ohne Bild nicht mehr
 * zurückzustellen) */
static inline uint32_t igfx_duty_for(const struct igfx_backlight *b, unsigned percent) {
  if (percent < 5) percent = 5;
  if (percent > 100) percent = 100;
  uint32_t d = (uint32_t)(((uint64_t)b->freq * percent + 50) / 100);
  return b->inverted ? b->freq - d : d;
}

/* AUX-Nachrichten: Bytes in Datenregister, das erste Byte im höchsten Byte (wie intel_dp_aux_pack) */
static inline uint32_t igfx_aux_pack(const uint8_t *src, unsigned n) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4 && i < n; i++) v |= (uint32_t)src[i] << ((3 - i) * 8);
  return v;
}
static inline void igfx_aux_unpack(uint32_t v, uint8_t *dst, unsigned n) {
  for (unsigned i = 0; i < 4 && i < n; i++) dst[i] = (uint8_t)(v >> ((3 - i) * 8));
}
static inline uint32_t igfx_aux_ctl(unsigned send_bytes) {
  return AUX_SEND_BUSY | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_TIME_OUT_MAX | AUX_RECEIVE_ERROR |
         (send_bytes << AUX_MSG_SIZE_SHIFT) | AUX_FW_SYNC_PULSE(32) | AUX_SYNC_PULSE(32);
}

#endif
