/* Host-Test für igfx_regs.h/igfx_report.h: Registersatz wie ihn die Firmware eines Alder-Lake-Notebooks
 * mit 1920x1080-eDP-Panel hinterlässt (Werte nach Intel-PRM-Kodierung zusammengesetzt).
 *   c++ -std=c++17 -I.. igfx_test.cpp -o /tmp/igfx_test && /tmp/igfx_test */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include "igfx_report.h"

static std::map<uint32_t, uint32_t> R;
static uint32_t rd(void *, uint32_t reg) { auto i = R.find(reg); return i == R.end() ? 0 : i->second; }

int main() {
  struct igfx_info info = igfx_identify(0x46A8);
  assert(info.platform == IGFX_ADLP && info.display_ver == 13 && info.pipes == 4);
  assert(igfx_identify(0x1234).platform == IGFX_UNKNOWN);
  /* Pipe A: 1920x1080, total 2200x1125 (148,5 MHz bei 60 Hz), eDP auf DDI A, 8 bpc */
  R[TRANS_CONF(0)] = TRANS_CONF_ENABLE | TRANS_CONF_STATE;
  R[TRANS_HTOTAL(0)] = (2199u << 16) | 1919u;
  R[TRANS_VTOTAL(0)] = (1124u << 16) | 1079u;
  R[PIPESRC(0)] = (1919u << 16) | 1079u;
  R[TRANS_DDI_FUNC_CTL(0)] = (1u << 31) | ((0u + 1) << 27) | (2u << 24) | (0u << 20);
  R[PLANE_CTL(0)] = PLANE_CTL_ENABLE | (4u << 23);
  R[PLANE_SIZE(0)] = (1079u << 16) | 1919u;
  R[PLANE_STRIDE(0)] = 7680 / 64;
  R[PLANE_SURF(0)] = 0x00800000;
  R[DDI_BUF_CTL(0)] = DDI_BUF_CTL_ENABLE;
  R[PP_STATUS] = PP_ON;
  R[PP_CONTROL] = PP_CONTROL_BACKLIGHT | PP_CONTROL_POWER_ON;
  R[BLC_PWM_CTL(0)] = BLC_PWM_ENABLE;
  R[BLC_PWM_FREQ(0)] = 0x5dc0; /* 24000 */
  R[BLC_PWM_DUTY(0)] = 12000;

  struct igfx_mode m = igfx_read_mode(rd, nullptr, 0, info.display_ver);
  assert(m.active && m.hactive == 1920 && m.vactive == 1080 && m.htotal == 2200 && m.vtotal == 1125);
  assert(m.src_w == 1920 && m.src_h == 1080 && !m.scaled);
  assert(m.plane_on && m.plane_w == 1920 && m.plane_h == 1080 && m.stride_bytes == 7680);
  assert(m.ddi_port == 0 && m.ddi_mode == 2 && igfx_bpc_bits(m.bpc) == 8);
  /* Gen11 kodiert den Port in 30:28 */
  R[TRANS_DDI_FUNC_CTL(1)] = (1u << 31) | (2u << 28);
  assert(igfx_read_mode(rd, nullptr, 1, 11).ddi_port == 2);

  struct igfx_backlight b = igfx_read_backlight(rd, nullptr);
  assert(b.controller == 0 && b.percent == 50 && !b.inverted);
  assert(igfx_duty_for(&b, 100) == 24000 && igfx_duty_for(&b, 0) == 1200 && igfx_duty_for(&b, 25) == 6000);
  R[BLC_PWM_CTL(0)] |= BLC_PWM_POLARITY;
  b = igfx_read_backlight(rd, nullptr);
  assert(b.inverted && b.percent == 50 && igfx_duty_for(&b, 100) == 0);
  R[BLC_PWM_CTL(0)] &= ~BLC_PWM_POLARITY;

  uint8_t hdr[4] = {0x97, 0x00, 0x02, 0x00}, back[4];
  assert(igfx_aux_pack(hdr, 4) == 0x97000200u);
  assert(igfx_aux_pack(hdr, 2) == 0x97000000u);
  igfx_aux_unpack(0x00112233u, back, 4);
  assert(back[0] == 0x00 && back[3] == 0x33);
  assert((igfx_aux_ctl(4) & AUX_MSG_SIZE_MASK) >> AUX_MSG_SIZE_SHIFT == 4);

  char buf[4096];
  struct igfx_text t = {buf, sizeof buf, 0};
  unsigned hz[4] = {60001, 0, 0, 0};
  igfx_report(&t, rd, nullptr, &info, 0x46A8, 0x6000000000ull, hz);
  fputs(buf, stdout);
  assert(strstr(buf, "pipe A: 1920x1080 (total 2200x1125) 60.001 Hz, pixel clock ~148502 kHz"));
  assert(strstr(buf, "output: DDI A, DP, 8 bpc"));
  assert(strstr(buf, "backlight: PWM controller 0, 50%"));
  assert(strstr(buf, "pipe B: off"));
  /* zu kleiner Puffer: abgeschnitten, aber nullterminiert */
  char small[40];
  struct igfx_text s = {small, sizeof small, 0};
  igfx_report(&s, rd, nullptr, &info, 0x46A8, 0, hz);
  assert(s.len < sizeof small && strlen(small) == s.len);
  puts("igfx_test: ok");
}
