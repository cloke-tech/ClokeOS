/*
 * Compositor von CloakServer: Alle Fenster zeichnen in eigene Pixmaps (Composite-Erweiterung),
 * CloakServer setzt daraus das Bild zusammen: Hintergrundbild, darüber die Fenster von unten
 * nach oben, Fensterrahmen mit runden Ecken und Schatten, Dock, Menüs und Namensschilder als
 * Glas, das die Fenster dahinter unscharf zeigt. Gezeichnet wird mit cairo auf X-Pixmaps, also
 * mit der Render-Erweiterung im X-Server, und nur dort, wo sich etwas geändert hat (Damage).
 */
#include "cloakserver.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <cairo-xlib.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool comp_on;
Visual *comp_argb;
Colormap comp_argb_cmap;

typedef struct CWin CWin;
struct CWin {
  Window id;
  int x, y, w, h, bw; /* w, h ohne Rand; die Pixmap ist w + 2 bw breit */
  bool mapped, input_only;
  Visual *vis;
  Pixmap pm;
  cairo_surface_t *surf;
  Damage dmg;
  XRectangle *shape; /* Form (XShape, Fensterkoordinaten), NULL = ganzes Rechteck */
  int nshape;
  CWin *next; /* von unten nach oben */
};

static Display *dpy;
static Window root, overlay, cm_owner;
static int SW, SH, damage_event;
static CWin *wins;
static Pixmap back_pm;
static cairo_surface_t *back, *wall_surf;
static Pixmap wall_surf_pm;
static cairo_region_t *dirty;
static int op_composite, op_damage, op_render, op_xfixes, shape_event = -1;
static XserverRegion parts; /* geänderte Teile eines Fensters, aus XDamageSubtract */

/* ---------------------------------------------------------------------------------------- */
/* Fensterliste                                                                               */

static CWin *find(Window id) {
  for (CWin *w = wins; w; w = w->next)
    if (w->id == id) return w;
  return NULL;
}

static void release(CWin *w) {
  if (w->surf) cairo_surface_destroy(w->surf);
  if (w->pm) XFreePixmap(dpy, w->pm);
  w->surf = NULL;
  w->pm = None;
}

/* Form des Fensters holen: Programme ohne Alphakanal (Darlings X11-Fenster, z. B. ein Ziehbild) schneiden sich mit
 * XShape zurecht; die Pixel außerhalb der Form sind in der Pixmap undefiniert und dürfen nicht gezeichnet werden. */
static void fetch_shape(CWin *w) {
  if (w->shape) XFree(w->shape);
  w->shape = NULL;
  w->nshape = 0;
  if (shape_event < 0) return;
  int bounding = 0, clip = 0, xb, yb, xc, yc;
  unsigned wb, hb, wc, hc;
  if (!XShapeQueryExtents(dpy, w->id, &bounding, &xb, &yb, &wb, &hb, &clip, &xc, &yc, &wc, &hc) || !bounding) return;
  int n = 0, ord;
  XRectangle *r = XShapeGetRectangles(dpy, w->id, ShapeBounding, &n, &ord);
  if (n == 1 && r[0].x <= 0 && r[0].y <= 0 && r[0].width >= w->w && r[0].height >= w->h) {
    XFree(r);
    return;
  }
  w->shape = r;
  w->nshape = n;
}

static void unlink_win(CWin *w) {
  for (CWin **p = &wins; *p; p = &(*p)->next)
    if (*p == w) {
      *p = w->next;
      return;
    }
}

/* nach above einsortieren (None = ganz unten) */
static void insert_above(CWin *w, Window above) {
  if (above == None) {
    w->next = wins;
    wins = w;
    return;
  }
  for (CWin *p = wins; p; p = p->next)
    if (p->id == above) {
      w->next = p->next;
      p->next = w;
      return;
    }
  CWin **p = &wins; /* unbekannt: ganz oben */
  while (*p) p = &(*p)->next;
  w->next = NULL;
  *p = w;
}

static void dirty_rect(int x, int y, int w, int h) {
  cairo_rectangle_int_t r = {x, y, w, h};
  cairo_region_union_rectangle(dirty, &r);
}

/* Fensterfläche samt Schatten als geändert markieren */
static void dirty_win(CWin *w) {
  int m = 56;
  dirty_rect(w->x - m, w->y - m, w->w + 2 * w->bw + 2 * m, w->h + 2 * w->bw + 2 * m + 24);
}

void comp_dirty(int x, int y, int w, int h) {
  if (comp_on) dirty_rect(x, y, w, h);
}

void comp_dirty_all(void) {
  if (comp_on) dirty_rect(0, 0, SW, SH);
}

static CWin *add(Window id, Window above) {
  if (id == overlay || find(id)) return find(id);
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, id, &wa)) return NULL;
  CWin *w = calloc(1, sizeof *w);
  w->id = id;
  w->x = wa.x;
  w->y = wa.y;
  w->w = wa.width;
  w->h = wa.height;
  w->bw = wa.border_width;
  w->vis = wa.visual;
  w->input_only = wa.class == InputOnly;
  insert_above(w, above);
  if (shape_event >= 0 && !w->input_only) XShapeSelectInput(dpy, id, ShapeNotifyMask);
  if (wa.map_state == IsViewable && !w->input_only) {
    fetch_shape(w);
    w->mapped = true;
    w->dmg = XDamageCreate(dpy, id, XDamageReportNonEmpty);
    dirty_win(w);
  }
  return w;
}

/* destroyed: das Fenster gibt es nicht mehr, sein Damage-Objekt hat der Server schon entfernt */
static void remove_win(CWin *w, bool destroyed) {
  if (w->mapped) dirty_win(w);
  release(w);
  if (w->shape) XFree(w->shape);
  if (w->dmg && !destroyed) XDamageDestroy(dpy, w->dmg);
  w->dmg = None;
  unlink_win(w);
  free(w);
}

/* ---------------------------------------------------------------------------------------- */
/* Start                                                                                      */

bool comp_init(Display *d, int scr, Window r, int sw, int sh) {
  dpy = d;
  root = r;
  SW = sw;
  SH = sh;
  int ev, err, major = 0, minor = 2;
  if (!XCompositeQueryExtension(dpy, &ev, &err) || !XCompositeQueryVersion(dpy, &major, &minor) ||
      (major == 0 && minor < 3)) {
    fprintf(stderr, "CloakServer: kein Composite 0.3, ohne Compositing\n");
    return false;
  }
  if (!XDamageQueryExtension(dpy, &damage_event, &err) || !XFixesQueryExtension(dpy, &ev, &err)) {
    fprintf(stderr, "CloakServer: kein Damage/XFixes, ohne Compositing\n");
    return false;
  }
  {
    int sev, serr;
    if (XShapeQueryExtension(dpy, &sev, &serr)) shape_event = sev;
  }

  XVisualInfo tpl = {.screen = scr, .depth = 32, .class = TrueColor};
  int n = 0;
  XVisualInfo *vi = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask | VisualClassMask, &tpl, &n);
  for (int i = 0; i < n && !comp_argb; i++)
    if (vi[i].red_mask == 0xff0000 && vi[i].green_mask == 0xff00 && vi[i].blue_mask == 0xff) comp_argb = vi[i].visual;
  if (vi) XFree(vi);
  if (!comp_argb) {
    fprintf(stderr, "CloakServer: kein 32-Bit-Visual, ohne Compositing\n");
    return false;
  }
  comp_argb_cmap = XCreateColormap(dpy, root, comp_argb, AllocNone);
  int e1, e2;
  XQueryExtension(dpy, "Composite", &op_composite, &e1, &e2);
  XQueryExtension(dpy, "DAMAGE", &op_damage, &e1, &e2);
  XQueryExtension(dpy, "RENDER", &op_render, &e1, &e2);
  XQueryExtension(dpy, "XFIXES", &op_xfixes, &e1, &e2);

  XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);
  overlay = XCompositeGetOverlayWindow(dpy, root);
  XserverRegion none = XFixesCreateRegion(dpy, NULL, 0);
  XFixesSetWindowShapeRegion(dpy, overlay, ShapeInput, 0, 0, none); /* Klicks gehen durch */
  XFixesDestroyRegion(dpy, none);
  XSelectInput(dpy, overlay, ExposureMask);

  parts = XFixesCreateRegion(dpy, NULL, 0);
  back_pm = XCreatePixmap(dpy, root, SW, SH, DefaultDepth(dpy, scr));
  back = cairo_xlib_surface_create(dpy, back_pm, DefaultVisual(dpy, scr), SW, SH);
  dirty = cairo_region_create();

  /* Programme (GTK) sehen so, dass ein Compositor läuft */
  char name[32];
  snprintf(name, sizeof name, "_NET_WM_CM_S%d", scr);
  cm_owner = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
  XSetSelectionOwner(dpy, XInternAtom(dpy, name, False), cm_owner, CurrentTime);

  Window r0, p0, *kids = NULL;
  unsigned nk = 0;
  if (XQueryTree(dpy, root, &r0, &p0, &kids, &nk)) {
    Window below = None;
    for (unsigned i = 0; i < nk; i++) {
      add(kids[i], below);
      below = kids[i];
    }
    if (kids) XFree(kids);
  }
  comp_on = true;
  comp_dirty_all();
  return true;
}

/* Fehler aus Wettläufen: Ein Fenster verschwindet zwischen Ereignis und Zeichnen, dann scheitern
 * NameWindowPixmap und die Render-Aufrufe auf der Pixmap. Das nächste Ereignis räumt auf. */
bool comp_quiet_error(XErrorEvent *e) {
  if (!comp_on) return false;
  int op = e->request_code;
  return op == op_composite || op == op_damage || op == op_render || op == op_xfixes ||
         (op == 54 /* X_FreePixmap */ && e->error_code == BadPixmap);
}

/* ---------------------------------------------------------------------------------------- */
/* Ereignisse                                                                                 */

void comp_handle(XEvent *ev) {
  if (!comp_on) return;
  CWin *w;
  switch (ev->type) {
  case CreateNotify:
    if (ev->xcreatewindow.parent == root) {
      CWin *top = wins;
      while (top && top->next) top = top->next;
      add(ev->xcreatewindow.window, top ? top->id : None);
    }
    break;
  case DestroyNotify:
    if ((w = find(ev->xdestroywindow.window))) remove_win(w, true);
    break;
  case ReparentNotify:
    if (ev->xreparent.parent == root) {
      CWin *top = wins;
      while (top && top->next) top = top->next;
      add(ev->xreparent.window, top ? top->id : None);
    } else if ((w = find(ev->xreparent.window))) {
      remove_win(w, false);
    }
    break;
  case MapNotify:
    if ((w = find(ev->xmap.window)) && !w->input_only) {
      w->mapped = true;
      fetch_shape(w);
      release(w);
      if (!w->dmg) w->dmg = XDamageCreate(dpy, w->id, XDamageReportNonEmpty);
      dirty_win(w);
    }
    break;
  case UnmapNotify:
    if ((w = find(ev->xunmap.window)) && w->mapped) {
      dirty_win(w);
      w->mapped = false;
      release(w);
      if (w->dmg) XDamageDestroy(dpy, w->dmg);
      w->dmg = None;
    }
    break;
  case ConfigureNotify:
    if (ev->xconfigure.event != root || !(w = find(ev->xconfigure.window))) break;
    if (w->mapped) dirty_win(w);
    if (w->w != ev->xconfigure.width || w->h != ev->xconfigure.height || w->bw != ev->xconfigure.border_width)
      release(w);
    w->x = ev->xconfigure.x;
    w->y = ev->xconfigure.y;
    w->w = ev->xconfigure.width;
    w->h = ev->xconfigure.height;
    w->bw = ev->xconfigure.border_width;
    unlink_win(w);
    insert_above(w, ev->xconfigure.above);
    if (w->mapped) dirty_win(w);
    break;
  case CirculateNotify:
    if ((w = find(ev->xcirculate.window))) {
      unlink_win(w);
      if (ev->xcirculate.place == PlaceOnBottom) insert_above(w, None);
      else insert_above(w, (Window)-1);
      if (w->mapped) dirty_win(w);
    }
    break;
  case Expose:
    if (ev->xexpose.window == overlay) dirty_rect(ev->xexpose.x, ev->xexpose.y, ev->xexpose.width, ev->xexpose.height);
    break;
  default:
    if (shape_event >= 0 && ev->type == shape_event + ShapeNotify) {
      XShapeEvent *se = (XShapeEvent *)ev;
      if (se->kind == ShapeBounding && (w = find(se->window))) {
        if (w->mapped) dirty_win(w);
        fetch_shape(w);
      }
    } else if (ev->type == damage_event + XDamageNotify) {
      XDamageNotifyEvent *de = (XDamageNotifyEvent *)ev;
      XDamageSubtract(dpy, de->damage, None, parts);
      if ((w = find(de->drawable)) && w->mapped) {
        int n = 0;
        XRectangle *r = XFixesFetchRegion(dpy, parts, &n);
        for (int i = 0; i < n; i++) dirty_rect(w->x + w->bw + r[i].x, w->y + w->bw + r[i].y, r[i].width, r[i].height);
        if (r) XFree(r);
      }
    }
  }
}

/* ---------------------------------------------------------------------------------------- */
/* Schatten: ein unscharfes Rechteck mit runden Ecken, in neun Teilen auf jede Größe gezogen   */

/* Maske: Rechteck mit runden Ecken, Rand blur, innen so groß, dass die Mitte jeder Seite eine
 * gerade Kante ist (volle Deckung innen). Ecken sind c = 2 blur + radius groß. */
typedef struct { int blur, radius, n, c; cairo_surface_t *mask; } ShadowMask;
static ShadowMask masks[8];

static void box_blur_a8(unsigned char *p, int n, int stride, int r) {
  int *acc = malloc(sizeof(int) * (n + 1));
  for (int pass = 0; pass < 3; pass++)
    for (int dir = 0; dir < 2; dir++)
      for (int j = 0; j < n; j++) {
        acc[0] = 0;
        for (int i = 0; i < n; i++) acc[i + 1] = acc[i] + (dir ? p[i * stride + j] : p[j * stride + i]);
        for (int i = 0; i < n; i++) {
          int a = i - r < 0 ? 0 : i - r, b = i + r + 1 > n ? n : i + r + 1;
          unsigned char v = (acc[b] - acc[a]) / (b - a);
          if (dir) p[i * stride + j] = v;
          else p[j * stride + i] = v;
        }
      }
  free(acc);
}

static ShadowMask *shadow_mask(int blur, int radius) {
  for (int i = 0; i < 8; i++)
    if (masks[i].mask && masks[i].blur == blur && masks[i].radius == radius) return &masks[i];
  ShadowMask *m = NULL;
  for (int i = 0; i < 8 && !m; i++)
    if (!masks[i].mask) m = &masks[i];
  if (!m) m = &masks[0];
  if (m->mask) cairo_surface_destroy(m->mask);
  int n = 4 * blur + 2 * radius + 1;
  cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_A8, n, n);
  cairo_t *cr = cairo_create(img);
  rrect(cr, blur, blur, n - 2 * blur, n - 2 * blur, radius);
  cairo_set_source_rgba(cr, 0, 0, 0, 1);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_surface_flush(img);
  box_blur_a8(cairo_image_surface_get_data(img), n, cairo_image_surface_get_stride(img), blur / 2 > 0 ? blur / 2 : 1);
  cairo_surface_mark_dirty(img);
  m->mask = cairo_surface_create_similar(back, CAIRO_CONTENT_ALPHA, n, n);
  cr = cairo_create(m->mask);
  cairo_set_source_surface(cr, img, 0, 0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(img);
  m->blur = blur;
  m->radius = radius;
  m->n = n;
  m->c = 2 * blur + radius;
  return m;
}

/* Teil (sx, sy, sw, sh) der Maske nach (dx, dy, dw, dh) */
static void mask_piece(cairo_t *cr, cairo_surface_t *mask, double sx, double sy, double sw, double sh, double dx,
                       double dy, double dw, double dh) {
  if (dw <= 0 || dh <= 0) return;
  cairo_save(cr);
  cairo_rectangle(cr, dx, dy, dw, dh);
  cairo_clip(cr);
  cairo_pattern_t *p = cairo_pattern_create_for_surface(mask);
  cairo_matrix_t mx;
  cairo_matrix_init_translate(&mx, sx, sy);
  cairo_matrix_scale(&mx, sw / dw, sh / dh);
  cairo_matrix_translate(&mx, -dx, -dy);
  cairo_pattern_set_matrix(p, &mx);
  cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);
  cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
  cairo_mask(cr, p);
  cairo_pattern_destroy(p);
  cairo_restore(cr);
}

static void shadow(cairo_t *cr, const WinStyle *st, double x, double y, double w, double h) {
  ShadowMask *m = shadow_mask(st->blur, (int)st->radius);
  int c = m->c; /* Ecke in der Maske */
  double sx = x - m->blur, sy = y - m->blur + st->offset, sw = w + 2 * m->blur, sh = h + 2 * m->blur;
  cairo_save(cr);
  /* wie box-shadow in CSS: nicht unter dem Fenster selbst */
  cairo_rectangle(cr, sx - 1, sy - 1, sw + 2, sh + 2);
  rrect(cr, x, y, w, h, st->radius);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_clip(cr);
  cairo_set_source_rgba(cr, 10 / 255., 5 / 255., 30 / 255., st->alpha);
  if (sw < 2 * c) c = (int)(sw / 2);
  if (sh < 2 * c) c = (int)(sh / 2);
  int n = m->n, e = n - 2 * c; /* Mitte der Maske, mindestens 1 Pixel */
  if (e < 1) e = 1;
  mask_piece(cr, m->mask, 0, 0, c, c, sx, sy, c, c);
  mask_piece(cr, m->mask, n - c, 0, c, c, sx + sw - c, sy, c, c);
  mask_piece(cr, m->mask, 0, n - c, c, c, sx, sy + sh - c, c, c);
  mask_piece(cr, m->mask, n - c, n - c, c, c, sx + sw - c, sy + sh - c, c, c);
  mask_piece(cr, m->mask, c, 0, e, c, sx + c, sy, sw - 2 * c, c);
  mask_piece(cr, m->mask, c, n - c, e, c, sx + c, sy + sh - c, sw - 2 * c, c);
  mask_piece(cr, m->mask, 0, c, c, e, sx, sy + c, c, sh - 2 * c);
  mask_piece(cr, m->mask, n - c, c, c, e, sx + sw - c, sy + c, c, sh - 2 * c);
  cairo_restore(cr);
}

/* ---------------------------------------------------------------------------------------- */
/* Glas: was hinter dem Fenster liegt, verkleinert und wieder vergrößert (wirkt wie Unschärfe) */

#define GLASS_MARGIN 24

static void backdrop(cairo_t *cr, const WinStyle *st, int x, int y, int w, int h) {
  int bx = x - GLASS_MARGIN, by = y - GLASS_MARGIN, bw = w + 2 * GLASS_MARGIN, bh = h + 2 * GLASS_MARGIN;
  cairo_surface_t *src = back;
  double ox = bx, oy = by; /* Lage des Ausschnitts in src */
  cairo_surface_t *steps[4] = {NULL};
  int sw = bw, sh = bh;
  for (int i = 0; i < 4; i++) { /* Halbierungen: 1/2, 1/4, 1/8, 1/16 */
    sw = (sw + 1) / 2;
    sh = (sh + 1) / 2;
    steps[i] = cairo_surface_create_similar(back, CAIRO_CONTENT_COLOR, sw, sh);
    cairo_t *c = cairo_create(steps[i]);
    cairo_scale(c, .5, .5);
    cairo_set_source_surface(c, src, -ox, -oy);
    cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(c), CAIRO_EXTEND_PAD);
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    cairo_paint(c);
    cairo_destroy(c);
    src = steps[i];
    ox = oy = 0;
  }
  cairo_save(cr);
  rrect(cr, x, y, w, h, st->radius);
  cairo_clip(cr);
  cairo_translate(cr, bx, by);
  cairo_scale(cr, (double)bw / sw, (double)bh / sh);
  cairo_set_source_surface(cr, steps[3], 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_paint(cr);
  cairo_restore(cr);
  for (int i = 0; i < 4; i++) cairo_surface_destroy(steps[i]);
}

/* ---------------------------------------------------------------------------------------- */
/* Zusammensetzen                                                                             */

static cairo_surface_t *win_surface(CWin *w) {
  if (w->surf) return w->surf;
  w->pm = XCompositeNameWindowPixmap(dpy, w->id);
  if (!w->pm) return NULL;
  w->surf = cairo_xlib_surface_create(dpy, w->pm, w->vis, w->w + 2 * w->bw, w->h + 2 * w->bw);
  return w->surf;
}

static bool overlaps(int x, int y, int w, int h) {
  cairo_rectangle_int_t r = {x, y, w, h};
  return cairo_region_contains_rectangle(dirty, &r) != CAIRO_REGION_OVERLAP_OUT;
}

void comp_repaint(void) {
  if (!comp_on || cairo_region_is_empty(dirty)) return;
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  /* Glas hängt von seiner ganzen Umgebung ab: berührt die Änderung ein Glasfenster, wird es ganz
   * neu gezeichnet (zweimal, weil sich Glasflächen gegenseitig berühren können). */
  for (int pass = 0; pass < 2; pass++)
    for (CWin *w = wins; w; w = w->next) {
      if (!w->mapped) continue;
      WinStyle st = cs_window_style(w->id);
      int m = st.kind == WK_GLASS ? GLASS_MARGIN : 0;
      int gx = w->x + st.il - m, gy = w->y + st.it - m, gw = w->w - st.il - st.ir + 2 * m, gh = w->h - st.it - st.ib + 2 * m;
      if (st.kind == WK_GLASS && overlaps(gx, gy, gw, gh)) dirty_rect(gx, gy, gw, gh);
    }
  cairo_rectangle_int_t full = {0, 0, SW, SH};
  cairo_region_intersect_rectangle(dirty, &full);

  cairo_t *cr = cairo_create(back);
  int nr = cairo_region_num_rectangles(dirty);
  for (int i = 0; i < nr; i++) {
    cairo_rectangle_int_t r;
    cairo_region_get_rectangle(dirty, i, &r);
    cairo_rectangle(cr, r.x, r.y, r.width, r.height);
  }
  cairo_clip(cr);

  Pixmap wp = cs_wallpaper();
  if (wp != wall_surf_pm) {
    if (wall_surf) cairo_surface_destroy(wall_surf);
    wall_surf = cairo_xlib_surface_create(dpy, wp, DefaultVisual(dpy, DefaultScreen(dpy)), SW, SH);
    wall_surf_pm = wp;
  }
  cairo_set_source_surface(cr, wall_surf, 0, 0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  for (CWin *w = wins; w; w = w->next) {
    if (!w->mapped || w->input_only) continue;
    int ww = w->w + 2 * w->bw, wh = w->h + 2 * w->bw;
    WinStyle st = cs_window_style(w->id);
    bool moving = st.shrink != 0 || st.fade != 0 || st.dx != 0 || st.dy != 0;
    int m = st.blur ? st.blur + st.offset + 2 : 0;
    if (!moving && !overlaps(w->x - m, w->y - m, ww + 2 * m, wh + 2 * m)) continue;
    cairo_surface_t *s = win_surface(w);
    if (!s) continue;
    double a = 1 - st.fade;
    cairo_matrix_t place; /* Fensterkoordinaten -> Bildschirm, bei Animation verkleinert und verschoben */
    if (moving) {
      double k = 1 - st.shrink;
      cairo_matrix_init_translate(&place, w->x + ww / 2.0 + st.dx, w->y + wh / 2.0 + st.dy);
      cairo_matrix_scale(&place, k, k);
      cairo_matrix_translate(&place, -ww / 2.0, -wh / 2.0);
      st.alpha *= a;
    } else {
      cairo_matrix_init_translate(&place, w->x, w->y);
    }
    if (st.blur) {
      cairo_save(cr);
      cairo_transform(cr, &place);
      shadow(cr, &st, st.il, st.it, ww - st.il - st.ir, wh - st.it - st.ib);
      cairo_restore(cr);
    }
    if (st.kind == WK_GLASS && !moving)
      backdrop(cr, &st, w->x + st.il, w->y + st.it, ww - st.il - st.ir, wh - st.it - st.ib);
    cairo_save(cr);
    cairo_transform(cr, &place);
    if (st.kind == WK_FRAME && st.radius > 0) { /* 32-Bit-Fenster haben ihre Ecken selbst */
      rrect(cr, 0, 0, ww, wh, st.radius);
      cairo_clip(cr);
    }
    if (w->shape) {
      for (int i = 0; i < w->nshape; i++)
        cairo_rectangle(cr, w->bw + w->shape[i].x, w->bw + w->shape[i].y, w->shape[i].width, w->shape[i].height);
      cairo_clip(cr);
    }
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint_with_alpha(cr, a);
    cs_window_overlay(w->id, cr, a); /* Ampelknöpfe über Programmen, die oben selbst zeichnen */
    cairo_restore(cr);
  }
  cairo_destroy(cr);

  /* fertiges Bild in das Overlay-Fenster */
  cairo_surface_t *os = cairo_xlib_surface_create(dpy, overlay, DefaultVisual(dpy, DefaultScreen(dpy)), SW, SH);
  cr = cairo_create(os);
  for (int i = 0; i < nr; i++) {
    cairo_rectangle_int_t r;
    cairo_region_get_rectangle(dirty, i, &r);
    cairo_rectangle(cr, r.x, r.y, r.width, r.height);
  }
  cairo_clip(cr);
  cairo_set_source_surface(cr, back, 0, 0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(os);
  if (getenv("CLOAKSERVER_DEBUG")) {
    XSync(dpy, False);
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    cairo_rectangle_int_t ext;
    cairo_region_get_extents(dirty, &ext);
    fprintf(stderr, "CloakServer: Bild %dx%d in %.0f ms (um %.3f s)\n", ext.width, ext.height,
            (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, t1.tv_sec % 1000 + t1.tv_nsec / 1e9);
  }
  cairo_region_destroy(dirty);
  dirty = cairo_region_create();
}
