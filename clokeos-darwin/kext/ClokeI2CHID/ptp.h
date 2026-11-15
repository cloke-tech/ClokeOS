/*
 * ptp.h: Precision-Touchpad-Modus für ClokeI2CHID (ohne Kernel-Abhängigkeiten, Host-Test in test/ptp_test.cpp).
 *
 * Im einfachen Maus-Modus macht die Firmware des Touchpads Klick, Antippen und Bewegung selbst – am HP 15s
 * (Synaptics/ELAN) schlecht: beim Ziehen mit gedrückter Klickfläche kommt kaum Bewegung, dazwischen leere
 * Berichte ("losgelassen"). Windows und Linux (hid-multitouch) schalten solche Touchpads deshalb in den
 * Touchpad-Modus: Feature "Input Mode" = 3 (Microsoft "Windows Precision Touchpad"-Spezifikation, öffentlich;
 * HID Usage Tables, Digitizer Page 0x0D). Dann meldet das Touchpad jeden Finger mit Position, und der Treiber
 * entscheidet selbst:
 *   - Zeiger folgt dem Finger, der sich bewegt (auch während ein anderer auf der Klickfläche drückt),
 *   - Klickfläche drücken = linke Taste (auch wenn ein zweiter Finger zieht),
 *   - kurz antippen = Klick, mit zwei Fingern = Rechtsklick,
 *   - zwei Finger bewegen = Rollen (Richtung wie macOS: Inhalt folgt den Fingern).
 * Eigener Code nach der Spezifikation; Linux' hid-multitouch (GPL) nur als Beschreibung gelesen.
 */
#ifndef CLOKE_PTP_H
#define CLOKE_PTP_H

#include <stddef.h>
#include <stdint.h>

enum { PTP_MAX_FINGERS = 5 };

struct PtpField {
  int off = -1, size = 0; /* Bit-Lage hinter der Report-ID */
  bool sign = false;
  int32_t lmax = 0;
};

struct PtpLayout {
  uint8_t inId = 0; /* Report-ID der Touchpad-Berichte */
  int fingers = 0;
  PtpField tip[PTP_MAX_FINGERS], cid[PTP_MAX_FINGERS], x[PTP_MAX_FINGERS], y[PTP_MAX_FINGERS];
  PtpField count, button, scan;
  uint8_t featId = 0; /* Feature-Bericht mit Input Mode */
  unsigned featBytes = 0;
  PtpField mode, surfaceSw, buttonSw;
  uint8_t swId = 0; /* Feature-Bericht mit den Schaltern Oberfläche/Taste (oft ein eigener) */
  unsigned swBytes = 0;
  bool ok() const { return inId && fingers > 0 && x[0].off >= 0 && y[0].off >= 0 && mode.off >= 0; }
};

static inline int32_t ptp_get(const uint8_t *r, unsigned n, const PtpField &f) {
  if (f.off < 0 || f.size <= 0 || f.size > 32 || (unsigned)(f.off + f.size + 7) / 8 > n) return 0;
  uint64_t v = 0;
  for (int k = 0; k < f.size; k++) {
    unsigned bit = (unsigned)(f.off + k);
    if (r[bit >> 3] & (1 << (bit & 7))) v |= 1ull << k;
  }
  if (f.sign && (v & (1ull << (f.size - 1)))) v |= ~0ull << f.size;
  return (int32_t)(int64_t)v;
}

static inline void ptp_put(uint8_t *r, unsigned n, const PtpField &f, uint32_t v) {
  if (f.off < 0 || (unsigned)(f.off + f.size + 7) / 8 > n) return;
  for (int k = 0; k < f.size; k++) {
    unsigned bit = (unsigned)(f.off + k);
    if (v & (1u << k)) r[bit >> 3] |= (uint8_t)(1 << (bit & 7));
    else r[bit >> 3] &= (uint8_t)~(1 << (bit & 7));
  }
}

/* Report-Deskriptor durchgehen: Touchpad-Sammlung (0x0D:0x05) und das Feature Input Mode (0x0D:0x52) */
static inline bool ptp_parse(const uint8_t *d, unsigned n, PtpLayout *L) {
  *L = PtpLayout();
  uint32_t page = 0, rsize = 0, rcount = 0, rid = 0, umin = 0, umax = 0;
  int32_t lmin = 0, lmax = 0;
  uint32_t usages[32];
  unsigned nus = 0;
  unsigned inPos[256] = {}, featPos[256] = {};
  int depth = 0, tpDepth = -1, fingerDepth = -1;
  uint32_t collUsage = 0;
  for (unsigned i = 0; i < n;) {
    uint8_t b = d[i++];
    if (b == 0xfe) {
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
      else if (tag == 1) lmin = sv;
      else if (tag == 2) lmax = sz == 4 ? (int32_t)v : (int32_t)(lmin < 0 ? sv : (int32_t)v);
      else if (tag == 7) rsize = v;
      else if (tag == 8) rid = v & 0xff;
      else if (tag == 9) rcount = v;
    } else if (type == 2) { /* local */
      uint32_t u = sz == 4 ? v : (page << 16 | v);
      if (tag == 0 && nus < 32) usages[nus++] = u;
      else if (tag == 1) umin = u;
      else if (tag == 2) umax = u;
    } else if (type == 0) { /* main */
      if (tag == 10) { /* Collection */
        collUsage = nus ? usages[0] : 0;
        if (v == 1 && collUsage == 0x000D0005 && tpDepth < 0 && !L->inId) tpDepth = depth;
        if (tpDepth >= 0 && collUsage == 0x000D0022 && fingerDepth < 0 && L->fingers < PTP_MAX_FINGERS) {
          fingerDepth = depth;
          L->fingers++;
        }
        depth++;
      } else if (tag == 12) { /* End Collection */
        depth--;
        if (depth == fingerDepth) fingerDepth = -1;
        if (depth == tpDepth) tpDepth = -1;
      } else if (tag == 8 || tag == 11) { /* Input, Feature */
        unsigned *pos = tag == 8 ? inPos : featPos;
        bool isConst = v & 1;
        for (unsigned k = 0; k < rcount && !isConst; k++) {
          uint32_t u = k < nus ? usages[k] : nus ? usages[nus - 1] : 0;
          if (!nus && umin) u = umin + k <= umax ? umin + k : 0;
          PtpField f;
          f.off = (int)(pos[rid] + k * rsize);
          f.size = (int)rsize;
          f.sign = lmin < 0;
          f.lmax = lmax;
          if (tag == 8 && tpDepth >= 0) {
            int fi = L->fingers - 1;
            if (fingerDepth >= 0 && fi >= 0) {
              if (u == 0x000D0042) L->tip[fi] = f;
              else if (u == 0x000D0051) L->cid[fi] = f;
              else if (u == 0x00010030) L->x[fi] = f;
              else if (u == 0x00010031) L->y[fi] = f;
            } else if (u == 0x000D0054) L->count = f;
            else if (u == 0x000D0056) L->scan = f;
            else if (u == 0x00090001) L->button = f;
            if (!L->inId && (u == 0x00010030 || u == 0x000D0042)) L->inId = (uint8_t)rid;
          } else if (tag == 11) {
            if (u == 0x000D0052) L->mode = f, L->featId = (uint8_t)rid;
            else if (u == 0x000D0057) L->surfaceSw = f, L->swId = (uint8_t)rid;
            else if (u == 0x000D0058) L->buttonSw = f, L->swId = (uint8_t)rid;
          }
        }
        pos[rid] += rsize * rcount;
        if (tag == 11 && rid == L->featId && L->mode.off >= 0) L->featBytes = (featPos[rid] + 7) / 8;
        if (tag == 11 && L->swId && rid == L->swId) L->swBytes = (featPos[rid] + 7) / 8;
      }
      nus = 0;
      umin = umax = 0;
    }
  }
  return L->ok();
}

/* Feature-Bericht "Touchpad-Modus an" (ohne Report-ID), Länge L->featBytes; stehen die Schalter im selben
 * Bericht, werden sie mit gesetzt */
static inline void ptp_mode_report(const PtpLayout *L, uint8_t *buf) {
  for (unsigned i = 0; i < L->featBytes; i++) buf[i] = 0;
  ptp_put(buf, L->featBytes, L->mode, 3);
  if (L->swId == L->featId) {
    ptp_put(buf, L->featBytes, L->surfaceSw, 1);
    ptp_put(buf, L->featBytes, L->buttonSw, 1);
  }
}
/* eigener Schalter-Bericht (L->swId != L->featId), Länge L->swBytes */
static inline void ptp_switch_report(const PtpLayout *L, uint8_t *buf) {
  for (unsigned i = 0; i < L->swBytes; i++) buf[i] = 0;
  ptp_put(buf, L->swBytes, L->surfaceSw, 1);
  ptp_put(buf, L->swBytes, L->buttonSw, 1);
}

/* ---- Gesten ---------------------------------------------------------------------------------------- */

typedef void (*ptp_emit_fn)(void *ctx, int dx, int dy, int wheel, uint8_t buttons);

struct PtpState {
  struct {
    int32_t cid, x, y;
    bool on;
  } prev[PTP_MAX_FINGERS];
  int32_t lastScan = -1;
  bool haveScan = false;
  uint8_t buttons = 0;     /* zuletzt ausgegeben */
  uint8_t clickButton = 0; /* beim Drücken der Klickfläche festgelegt (links/rechts) */
  /* laufende Berührung, für Antippen */
  bool touching = false, clicked = false;
  int maxFingers = 0;
  uint64_t touchStart = 0;
  int64_t moved = 0;
  int32_t fracX = 0, fracY = 0, scrollAcc = 0; /* Reste in 1/256 Bildpunkt bzw. Geräteeinheiten */
  /* Hybrid-Meldung (weniger Finger-Felder als Finger, z. B. HP 15s: eines): die Finger eines Bildes kommen in
   * mehreren Berichten, der erste trägt die Gesamtzahl. Hier gesammelt, bis alle da sind. */
  struct {
    int32_t cid, x, y;
  } frame[PTP_MAX_FINGERS];
  int frameHave = 0, frameWant = 0, frameSeen = 0;
  bool frameBtn = false;
};

/* Ein Touchpad-Bericht r (hinter der Report-ID), n Bytes */
static inline void ptp_input(PtpState *S, const PtpLayout *L, const uint8_t *r, unsigned n, uint64_t nowMs,
                             ptp_emit_fn emit, void *ctx) {
  if (L->scan.off >= 0) {
    int32_t sc = ptp_get(r, n, L->scan);
    bool empty = true;
    for (unsigned i = 0; i < n; i++)
      if (r[i]) empty = false;
    /* leerer oder wiederholter Lesevorgang (ohne Interrupt-Leitung liest der Treiber auch ohne neue Daten) */
    if (empty || (S->haveScan && sc == S->lastScan)) return;
    S->lastScan = sc, S->haveScan = true;
  }
  int32_t cnt = L->count.off >= 0 ? ptp_get(r, n, L->count) : -1;
  bool btnNow = L->button.off >= 0 && ptp_get(r, n, L->button);
  /* Ein Bericht mit Anzahl > 0 (oder ohne Anzahl-Feld) beginnt ein Bild; Anzahl 0 ist bei offenem Bild die
   * Fortsetzung (Hybrid), sonst "alle Finger weg" */
  /* Manche Touchpads (HP 15s?) schreiben die Gesamtzahl in jeden Bericht statt nur in den ersten: ein Bericht mit
   * Anzahl > 0 setzt das offene Bild deshalb nur fort, wenn sein Finger darin noch fehlt */
  bool continues = false;
  if (cnt > 0 && S->frameWant && S->frameSeen < S->frameWant) {
    continues = true;
    for (int f = 0; f < L->fingers && continues; f++) {
      if (L->tip[f].off >= 0 && !ptp_get(r, n, L->tip[f])) continue;
      int32_t id = L->cid[f].off >= 0 ? ptp_get(r, n, L->cid[f]) : f;
      for (int k = 0; k < S->frameHave; k++)
        if (S->frame[k].cid == id) continues = false;
    }
  }
  if (continues)
    S->frameBtn = S->frameBtn || btnNow;
  else if (cnt != 0) {
    S->frameHave = S->frameSeen = 0;
    S->frameWant = cnt > 0 ? cnt : 0;
    S->frameBtn = btnNow;
  } else if (!S->frameWant) {
    S->frameHave = S->frameSeen = 0;
    S->frameBtn = btnNow;
  } else
    S->frameBtn = S->frameBtn || btnNow;
  /* Finger-Felder dieses Berichts: belegt sind höchstens so viele, wie im Bild noch fehlen */
  int slots = L->fingers;
  if (S->frameWant && S->frameWant - S->frameSeen < slots) slots = S->frameWant - S->frameSeen;
  for (int f = 0; f < slots; f++) {
    S->frameSeen++;
    bool tip = L->tip[f].off >= 0 ? ptp_get(r, n, L->tip[f]) != 0 : true;
    if (!tip || S->frameHave >= PTP_MAX_FINGERS) continue;
    S->frame[S->frameHave].cid = L->cid[f].off >= 0 ? ptp_get(r, n, L->cid[f]) : f;
    S->frame[S->frameHave].x = ptp_get(r, n, L->x[f]);
    S->frame[S->frameHave].y = ptp_get(r, n, L->y[f]);
    S->frameHave++;
  }
  if (S->frameWant && S->frameSeen < S->frameWant) return; /* weitere Berichte dieses Bildes folgen */
  S->frameWant = 0;
  struct {
    int32_t cid, x, y;
  } cur[PTP_MAX_FINGERS];
  int nc = S->frameHave;
  for (int i = 0; i < nc; i++) cur[i].cid = S->frame[i].cid, cur[i].x = S->frame[i].x, cur[i].y = S->frame[i].y;
  bool btn = S->frameBtn;
  int32_t lmaxX = L->x[0].lmax > 0 ? L->x[0].lmax : 4000, lmaxY = L->y[0].lmax > 0 ? L->y[0].lmax : 3000;

  /* Bewegung je Finger gegenüber dem vorigen Bericht; der Finger mit der größten Bewegung führt */
  int32_t mdx = 0, mdy = 0, best = -1, sumDy = 0, both = 0;
  for (int i = 0; i < nc; i++) {
    for (int k = 0; k < PTP_MAX_FINGERS; k++) {
      if (!S->prev[k].on || S->prev[k].cid != cur[i].cid) continue;
      int32_t dx = cur[i].x - S->prev[k].x, dy = cur[i].y - S->prev[k].y;
      int32_t m = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
      if (m > best) best = m, mdx = dx, mdy = dy;
      sumDy += dy, both++;
    }
  }
  for (int k = 0; k < PTP_MAX_FINGERS; k++) S->prev[k].on = false;
  for (int i = 0; i < nc && i < PTP_MAX_FINGERS; i++) S->prev[i] = {cur[i].cid, cur[i].x, cur[i].y, true};

  /* Berührung (für Antippen) */
  if (nc && !S->touching) {
    S->touching = true, S->clicked = false, S->maxFingers = 0, S->moved = 0, S->touchStart = nowMs;
  }
  if (nc > S->maxFingers) S->maxFingers = nc;
  if (best > 0) S->moved += best;

  /* Klickfläche: immer links (ein Finger drückt, ein anderer zieht ein Fenster); rechts mit zwei Fingern tippen */
  uint8_t buttons = S->buttons & ~3u;
  if (btn) {
    if (!S->clickButton) S->clickButton = 1;
    buttons |= S->clickButton;
    S->clicked = true;
  } else
    S->clickButton = 0;

  int outDx = 0, outDy = 0, wheel = 0;
  if (nc == 2 && !btn && both == 2) { /* zwei Finger: rollen, eine Raste je 1/30 der Höhe */
    S->scrollAcc += sumDy / 2;
    int32_t step = lmaxY / 30 > 0 ? lmaxY / 30 : 40;
    while (S->scrollAcc >= step) wheel++, S->scrollAcc -= step;
    while (S->scrollAcc <= -step) wheel--, S->scrollAcc += step;
  } else if (best > 0) {
    S->scrollAcc = 0;
    /* ganze Breite des Touchpads = eine Bildschirmbreite (1920); beschleunigt wird nur von Xorg (sonst doppelt,
     * am HP "wie auf Eis") */
    int64_t gx = (int64_t)mdx * 1920 * 256 / lmaxX, gy = (int64_t)mdy * 1920 * 256 / lmaxX;
    S->fracX += (int32_t)gx, S->fracY += (int32_t)gy;
    outDx = S->fracX / 256, outDy = S->fracY / 256;
    S->fracX -= outDx * 256, S->fracY -= outDy * 256;
  }
  if (outDx || outDy || wheel || buttons != S->buttons) {
    emit(ctx, outDx, outDy, wheel, buttons);
    S->buttons = buttons;
  }

  /* alle Finger weg: kurz und ohne Bewegung angetippt? */
  if (!nc && S->touching) {
    S->touching = false;
    if (!S->clicked && nowMs - S->touchStart < 200 && S->moved < lmaxX / 40 && S->maxFingers <= 2) {
      uint8_t b = S->maxFingers == 2 ? 2 : 1;
      emit(ctx, 0, 0, 0, (uint8_t)(S->buttons | b));
      emit(ctx, 0, 0, 0, S->buttons);
    }
    S->scrollAcc = 0;
  }
}

#endif
