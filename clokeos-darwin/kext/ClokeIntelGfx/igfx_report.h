/*
 * igfx_report.h: Zustand der Anzeige als Text (für /dev/clokeos_gfx, das Kernel-Log und den Host-Test).
 * Braucht snprintf aus dem Kernel bzw. aus stdio.
 */
#ifndef IGFX_REPORT_H
#define IGFX_REPORT_H

#include "igfx_regs.h"

struct igfx_text {
  char *buf;
  size_t cap, len;
};

#define igfx_printf(t, ...)                                                                                      \
  do {                                                                                                           \
    if ((t)->len < (t)->cap) {                                                                                   \
      int _n = snprintf((t)->buf + (t)->len, (t)->cap - (t)->len, __VA_ARGS__);                                  \
      if (_n > 0) (t)->len += (size_t)_n < (t)->cap - (t)->len ? (size_t)_n : (t)->cap - (t)->len - 1;           \
    }                                                                                                            \
  } while (0)

static inline const char *igfx_ddi_mode_name(unsigned m) {
  static const char *n[] = {"HDMI", "DVI", "DP", "DP-MST", "FDI", "?", "DSI", "?"};
  return n[m & 7];
}
static inline unsigned igfx_bpc_bits(unsigned b) {
  static const unsigned n[] = {8, 10, 6, 12, 0, 0, 0, 0};
  return n[b & 7];
}

/* hz_milli[pipe]: gemessene Bildfrequenz in mHz (0 = nicht gemessen) */
static inline void igfx_report(struct igfx_text *t, igfx_read_fn rd, void *c, const struct igfx_info *info,
                               uint16_t did, uint64_t mmio_phys, const unsigned *hz_milli) {
  igfx_printf(t, "gpu: %s (8086:%04x), display version %u, MMIO 0x%llx\n", info->name, did, info->display_ver,
              (unsigned long long)mmio_phys);
  igfx_printf(t, "power wells: PWR_WELL_CTL2 0x%08x\n", rd(c, PWR_WELL_CTL2));
  for (unsigned p = 0; p < info->pipes; p++) {
    struct igfx_mode m = igfx_read_mode(rd, c, p, info->display_ver);
    if (!m.active) {
      igfx_printf(t, "pipe %c: off\n", 'A' + p);
      continue;
    }
    igfx_printf(t, "pipe %c: %ux%u (total %ux%u)", 'A' + p, m.hactive, m.vactive, m.htotal, m.vtotal);
    if (hz_milli && hz_milli[p]) {
      unsigned long long clk = (unsigned long long)m.htotal * m.vtotal * hz_milli[p] / 1000000ull; /* kHz */
      igfx_printf(t, " %u.%03u Hz, pixel clock ~%llu kHz", hz_milli[p] / 1000, hz_milli[p] % 1000, clk);
    }
    igfx_printf(t, "\n");
    if (m.ddi_port >= 0)
      igfx_printf(t, "  output: DDI %c, %s, %u bpc\n", 'A' + m.ddi_port, igfx_ddi_mode_name(m.ddi_mode),
                  igfx_bpc_bits(m.bpc));
    igfx_printf(t, "  source %ux%u%s\n", m.src_w, m.src_h, m.scaled ? ", scaled by pipe scaler" : "");
    if (m.plane_on)
      igfx_printf(t, "  plane 1: %ux%u, stride %u bytes, tiling %u, surface 0x%08x, PLANE_CTL 0x%08x\n", m.plane_w,
                  m.plane_h, m.stride_bytes, m.tiling, m.surf, m.plane_ctl);
    else
      igfx_printf(t, "  plane 1: off\n");
  }
  for (unsigned port = 0; port < 5; port++) {
    uint32_t v = rd(c, DDI_BUF_CTL(port));
    if (v & DDI_BUF_CTL_ENABLE) igfx_printf(t, "DDI %c: on (DDI_BUF_CTL 0x%08x)\n", 'A' + port, v);
  }
  uint32_t pps = rd(c, PP_STATUS), ppc = rd(c, PP_CONTROL);
  igfx_printf(t, "panel power: %s, backlight enable %s (PP_STATUS 0x%08x PP_CONTROL 0x%08x)\n",
              (pps & PP_ON) ? "on" : "off", (ppc & PP_CONTROL_BACKLIGHT) ? "on" : "off", pps, ppc);
  struct igfx_backlight b = igfx_read_backlight(rd, c);
  if (b.controller >= 0)
    igfx_printf(t, "backlight: PWM controller %d, %u%% (duty %u of %u%s)\n", b.controller, b.percent, b.duty, b.freq,
                b.inverted ? ", inverted" : "");
  else
    igfx_printf(t, "backlight: no PWM controller enabled (CTL1 0x%08x CTL2 0x%08x); panel may use eDP AUX "
                   "brightness, try: echo dpcd > /dev/clokeos_gfx\n",
                rd(c, BLC_PWM_CTL(0)), rd(c, BLC_PWM_CTL(1)));
}

#endif
