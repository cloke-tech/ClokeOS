/* Host-Test für ptp.h: Deskriptor nach dem Beispiel der Windows-Precision-Touchpad-Spezifikation
 * (zwei Finger, Scan Time, Contact Count, Taste; Input Mode in Bericht 4, Schalter in Bericht 6).
 *   c++ -std=c++17 -I.. ptp_test.cpp -o /tmp/ptp_test && /tmp/ptp_test */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "ptp.h"

static const uint8_t finger[] = {
    0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,                                     // Finger
    0x15, 0x00, 0x25, 0x01, 0x09, 0x47, 0x09, 0x42, 0x95, 0x02, 0x75, 0x01, 0x81, 0x02, // Confidence, Tip
    0x95, 0x06, 0x81, 0x03,                                                 // Füllbits
    0x25, 0x0F, 0x75, 0x04, 0x95, 0x01, 0x09, 0x51, 0x81, 0x02,             // Contact ID
    0x75, 0x04, 0x81, 0x03,                                                 // Füllbits
    0x05, 0x01, 0x15, 0x00, 0x26, 0xE4, 0x04, 0x75, 0x10, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, // X 0..1252
    0x26, 0x2B, 0x03, 0x09, 0x31, 0x81, 0x02,                               // Y 0..811
    0xC0};

static std::vector<uint8_t> descriptor() {
  std::vector<uint8_t> d = {0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x03};
  d.insert(d.end(), finger, finger + sizeof finger);
  d.insert(d.end(), finger, finger + sizeof finger);
  const uint8_t tail[] = {
      0x05, 0x0D, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0x09, 0x56, 0x81, 0x02, // Scan Time
      0x09, 0x54, 0x25, 0x7F, 0x75, 0x08, 0x81, 0x02,                                           // Contact Count
      0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,                   // Button 1
      0x95, 0x07, 0x81, 0x03, 0xC0,
      // Mausanteil (Report 1), wie ihn Touchpads zusätzlich haben
      0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x02,
      0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
      0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
      // Device Configuration: Input Mode (Report 4), Schalter (Report 6)
      0x05, 0x0D, 0x09, 0x0E, 0xA1, 0x01, 0x85, 0x04, 0x09, 0x22, 0xA1, 0x02, 0x09, 0x52, 0x15, 0x00, 0x25, 0x0A,
      0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, 0xC0, 0x09, 0x22, 0xA1, 0x00, 0x85, 0x06, 0x09, 0x57, 0x09, 0x58, 0x75,
      0x01, 0x95, 0x02, 0x25, 0x01, 0xB1, 0x02, 0x95, 0x06, 0xB1, 0x03, 0xC0, 0xC0};
  d.insert(d.end(), tail, tail + sizeof tail);
  return d;
}

struct Ev { int dx, dy, w; uint8_t b; };
static std::vector<Ev> evs;
static void emit(void *, int dx, int dy, int w, uint8_t b) { evs.push_back({dx, dy, w, b}); }

/* Bericht 3 ohne ID: je Finger 1 Byte Flags, 1 Byte ID, 2+2 Byte X/Y; dann Scan, Count, Taste */
static std::vector<uint8_t> rep(int n, const int *xs, const int *ys, int scan, int count, bool btn) {
  std::vector<uint8_t> r(2 * 6 + 2 + 1 + 1, 0);
  for (int f = 0; f < n; f++) {
    uint8_t *p = &r[f * 6];
    p[0] = 0x03; /* Confidence + Tip */
    p[1] = (uint8_t)f;
    p[2] = (uint8_t)xs[f], p[3] = (uint8_t)(xs[f] >> 8), p[4] = (uint8_t)ys[f], p[5] = (uint8_t)(ys[f] >> 8);
  }
  r[12] = (uint8_t)scan, r[13] = (uint8_t)(scan >> 8), r[14] = (uint8_t)count, r[15] = btn ? 1 : 0;
  return r;
}

int main() {
  auto d = descriptor();
  PtpLayout L;
  assert(ptp_parse(d.data(), (unsigned)d.size(), &L));
  assert(L.inId == 3 && L.fingers == 2 && L.featId == 4 && L.swId == 6);
  assert(L.tip[0].off == 1 && L.cid[0].off == 8 && L.x[0].off == 16 && L.y[0].off == 32 && L.x[0].lmax == 1252);
  assert(L.x[1].off == 64 && L.scan.off == 96 && L.count.off == 112 && L.button.off == 120);
  assert(L.featBytes == 1 && L.swBytes == 1);
  uint8_t fb[4], sb[4];
  ptp_mode_report(&L, fb);
  ptp_switch_report(&L, sb);
  assert(fb[0] == 3 && sb[0] == 3);

  PtpState S;
  int x1[] = {600, 0}, y1[] = {400, 0};
  uint64_t t = 1000;
  /* Antippen: ein Finger kurz drauf und weg */
  auto r = rep(1, x1, y1, 100, 1, false);
  ptp_input(&S, &L, r.data(), (unsigned)r.size(), t, emit, nullptr);
  r = rep(0, x1, y1, 200, 0, false);
  ptp_input(&S, &L, r.data(), (unsigned)r.size(), t + 80, emit, nullptr);
  assert(evs.size() == 2 && evs[0].b == 1 && evs[1].b == 0);
  /* leerer Lesevorgang und Wiederholung werden verworfen */
  std::vector<uint8_t> z(16, 0);
  ptp_input(&S, &L, z.data(), 16, t + 90, emit, nullptr);
  ptp_input(&S, &L, r.data(), (unsigned)r.size(), t + 95, emit, nullptr);
  assert(evs.size() == 2);
  /* Klickfläche drücken (ein Finger) und mit einem zweiten Finger ziehen: links gedrückt, Bewegung kommt an */
  evs.clear();
  int xa[] = {600, 300}, ya[] = {700, 400};
  r = rep(1, xa, ya, 300, 1, false);
  ptp_input(&S, &L, r.data(), (unsigned)r.size(), t + 1000, emit, nullptr);
  r = rep(1, xa, ya, 310, 1, true);
  ptp_input(&S, &L, r.data(), (unsigned)r.size(), t + 1010, emit, nullptr);
  assert(evs.back().b == 1);
  int moved = 0;
  for (int k = 1; k <= 10; k++) {
    int xb[] = {600, 300 + 10 * k}, yb[] = {700, 400};
    r = rep(2, xb, yb, 310 + 10 * k, 2, true);
    ptp_input(&S, &L, r.data(), (unsigned)r.size(), t + 1010 + 10 * k, emit, nullptr);
    moved += evs.back().dx;
    assert(evs.back().b == 1); /* bleibt links, auch mit zweitem Finger */
  }
  printf("drag with pressed clickpad: %d px for 100 units\n", moved);
  assert(moved > 100 && moved < 200); /* ganze Breite = eine Bildschirmbreite */
  r = rep(0, xa, ya, 500, 0, false);
  ptp_input(&S, &L, r.data(), (unsigned)r.size(), t + 1200, emit, nullptr);
  assert(evs.back().b == 0);
  /* zwei Finger nach oben bewegen: rollen, Inhalt folgt (Rad negativ) */
  evs.clear();
  int wheel = 0;
  for (int k = 0; k < 10; k++) {
    int xs[] = {500, 700}, ys[] = {600 - 20 * k, 600 - 20 * k};
    r = rep(2, xs, ys, 600 + 10 * k, 2, false);
    ptp_input(&S, &L, r.data(), (unsigned)r.size(), t + 2000 + 10 * k, emit, nullptr);
  }
  for (auto &e : evs) wheel += e.w, assert(e.dx == 0 && e.dy == 0);
  printf("two-finger scroll up 180 units: wheel %d\n", wheel);
  assert(wheel < 0);
  /* Klick mit zwei Fingern auf der Fläche = rechts */
  evs.clear();
  int xr[] = {500, 700}, yr[] = {600, 600};
  r = rep(2, xr, yr, 900, 2, true);
  ptp_input(&S, &L, r.data(), (unsigned)r.size(), t + 3000, emit, nullptr);
  assert(evs.back().b == 2);
  /* Hybrid wie am HP: ein Finger-Feld je Bericht; zwei Finger kommen in zwei Berichten (Anzahl 2, dann 0) */
  {
    std::vector<uint8_t> h = {0x05, 0x0D, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x03};
    h.insert(h.end(), finger, finger + sizeof finger);
    const uint8_t t2[] = {0x05, 0x0D, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0x09, 0x56, 0x81, 0x02,
                          0x09, 0x54, 0x25, 0x7F, 0x75, 0x08, 0x81, 0x02, 0x05, 0x09, 0x09, 0x01, 0x25, 0x01, 0x75,
                          0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03, 0xC0,
                          0x05, 0x0D, 0x09, 0x0E, 0xA1, 0x01, 0x85, 0x04, 0x09, 0x52, 0x15, 0x00, 0x25, 0x0A,
                          0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, 0xC0};
    h.insert(h.end(), t2, t2 + sizeof t2);
    PtpLayout H;
    assert(ptp_parse(h.data(), (unsigned)h.size(), &H) && H.fingers == 1 && H.scan.off == 48);
    PtpState T;
    evs.clear();
    auto one = [&](int cid, int x, int y, int scan, int cnt, bool tip) {
      std::vector<uint8_t> r(10, 0);
      r[0] = tip ? 3 : 1, r[1] = (uint8_t)cid, r[2] = (uint8_t)x, r[3] = (uint8_t)(x >> 8), r[4] = (uint8_t)y,
      r[5] = (uint8_t)(y >> 8), r[6] = (uint8_t)scan, r[7] = (uint8_t)(scan >> 8), r[8] = (uint8_t)cnt;
      ptp_input(&T, &H, r.data(), 10, 5000 + scan, emit, nullptr);
    };
    /* zwei Finger nach oben: rollen, kein Zeiger */
    for (int k = 0; k < 8; k++) {
      one(0, 500, 700 - 25 * k, 10 + 2 * k, 2, true);
      one(1, 800, 700 - 25 * k, 11 + 2 * k, 0, true);
    }
    int w = 0;
    for (auto &e : evs) w += e.w, assert(e.dx == 0 && e.dy == 0);
    printf("hybrid two-finger scroll: wheel %d\n", w);
    assert(w < 0);
    /* Finger hebt ab: Anzahl 1, Tip 0 -> Bild fertig, keine Finger */
    one(0, 0, 0, 60, 0, false); /* alle Finger weg (Anzahl 0) */
    evs.clear();
    one(0, 500, 500, 100, 1, true);
    one(0, 500, 500, 140, 1, false);
    assert(evs.size() == 2 && evs[0].b == 1 && evs[1].b == 0); /* als Antippen erkannt */
  }
  puts("ptp_test: ok");
}
