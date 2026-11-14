/*
 * CloakServer: der Fensterserver von ClokeOS.
 *
 * Ein X11-Fenstermanager mit der Oberfläche aus clokeos.html: Hintergrundbild "Goldenes Tor",
 * Menüleiste mit ClokeOS-Menü, App-Menüs, Netzwerk, Erscheinungsbild und Uhr, Dock mit den
 * Icons aus dem Prototyp und Laufanzeigen, Fenster mit Titelleiste und Ampelknöpfen (schließen,
 * ins Dock, zoomen). Er läuft auf Xorg mit PureDarwins GOP-Treiber; die Programme hinter den
 * Dock-Symbolen sind die des PureDarwin-Desktop-Images (Thunar, NetSurf, xfce4-terminal …).
 *
 *   CloakServer [--theme auto|hell|dunkel] [--compositor an|aus]
 *
 * Mit Compositor (comp.c, Standard) bekommen Fenster Schatten und echte runde Ecken, und Dock,
 * Menüs und Namensschilder zeigen als Glas unscharf, was dahinter liegt.
 *
 * Ressourcen (Icons, Logo) liegen in Resources/ neben dem Programm, erzeugt von resources.py
 * aus clokeos.html.
 */
#include "cloakserver.h"

#include <X11/Xatom.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#include <cairo-xlib.h>
#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <execinfo.h>
extern const void *_dyld_get_image_header(unsigned int); /* mach-o/dyld.h fehlt im Sysroot */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

#if __has_include(<ifaddrs.h>)
#include <ifaddrs.h>
#else
/* PureDarwins Header haben kein ifaddrs.h; libSystem hat getifaddrs (Layout wie bei Darwin). */
struct ifaddrs {
  struct ifaddrs *ifa_next;
  char *ifa_name;
  unsigned int ifa_flags;
  struct sockaddr *ifa_addr, *ifa_netmask, *ifa_dstaddr;
  void *ifa_data;
};
int getifaddrs(struct ifaddrs **);
void freeifaddrs(struct ifaddrs *);
#endif

/* ---------------------------------------------------------------------------------------- */
/* Maße wie in clokeos.html                                                                   */

#define MENUBAR_H 26
/* holt die Menüs von GTK-Programmen für die Menüleiste (menubar/cloakmenu.c) */
#define CLOAKMENU "/System/Library/CoreServices/CloakServer/cloakmenu"
/* Schrift der Oberfläche: Cloke Sans (Inter-Umrisse, SIL OFL), fehlt sie, nimmt fontconfig Ersatz */
#define UI_FONT "Cloke Sans"
#define UI_FONT_FILE "/Library/Fonts/ClokeSans/ClokeSans-Regular.otf"
#define GM_TOP_MAX 16 /* höchstens so viele Programm-Menüs in der Leiste */
static bool recovery; /* läuft als Wiederherstellung (recovery.c, /etc/clokeos-recovery) */
/* Oberfläche: ClokeOS (wie macOS) oder NT (Windows-Stil, ab der nächsten Anmeldung); nt_sel ist die Einstellung */
static bool nt, nt_sel;
#define TITLE_H   (nt ? 30 : 32) /* wie macOS 27 (auf dem Mac nachgemessen, docs/titlebar.md) */
#define UNI_TH    38           /* Fundus, Search …: Werkzeugleiste in der Titelleiste (Prototyp: .titlebar) */
/* _CLOKE_TITLEBAR (docs/titlebar.md): wer zeichnet die Fläche oben am Fenster */
enum { TB_BAR, TB_OVERLAY, TB_OWN };
#define LIGHT_X0  16 /* Ampelknöpfe wie bei macOS 27: Mitte des ersten 16 pt vom Rand, Abstand 23, Durchmesser 14 */
#define LIGHT_GAP 23
#define LIGHT_R   7
#define NT_START_W 48 /* Breite der Start-Schaltfläche in der NT-Taskleiste */
#define UNI_CUT   146  /* Breite des Ausschnitts links oben bei Fundus und Search: Ampelknöpfe und Zurück/Vor */   /* wie die Titelleiste im Prototyp (clokeos.html: .titlebar) */
#define BORDER    1
#define RADIUS    (nt ? 0 : 12) /* Prototyp: .win border-radius */
#define DOCK_PAD_X 8   /* entlang des Docks */
#define DOCK_PAD_Y 6   /* quer dazu */
#define DOCK_GAP  5
#define DOCK_H    (ds + 2 * DOCK_PAD_Y) /* Dicke des Docks */
#define DOCK_MARGIN 6  /* Abstand zum Bildschirmrand */
static int ds = 50;    /* Dock-Symbolgröße; ds_pref aus den Systemeinstellungen (40, 50, 64), ds ist */
static int ds_pref = 50; /* kleiner, wenn das Dock sonst nicht auf den Bildschirm passt */
#define GLASS_O   .45  /* --glass-o bei "Liquid Glass" 50 */

typedef struct { double r, g, b, a; } RGBA;

typedef struct {
  RGBA fg, fg2, fg3, menubar_fg, tint, dock, win, line, line_strong, hover, rim, rim_low, edge, pill, light_off;
} Theme;

static const Theme LIGHT = {
  .fg = {0x1e / 255., 0x1a / 255., 0x27 / 255., 1}, .fg2 = {0x6d / 255., 0x67 / 255., 0x79 / 255., 1},
  .fg3 = {0x9a / 255., 0x94 / 255., 0xa6 / 255., 1}, .menubar_fg = {0x16 / 255., 0x13 / 255., 0x1e / 255., 1},
  .tint = {248 / 255., 247 / 255., 252 / 255., GLASS_O}, .dock = {248 / 255., 247 / 255., 252 / 255., GLASS_O}, .win = {251 / 255., 250 / 255., 253 / 255., 1},
  .line = {40 / 255., 25 / 255., 70 / 255., .12}, .line_strong = {40 / 255., 25 / 255., 70 / 255., .22},
  .hover = {40 / 255., 25 / 255., 70 / 255., .06}, .rim = {1, 1, 1, .9}, .rim_low = {1, 1, 1, .35},
  .edge = {30 / 255., 20 / 255., 60 / 255., .14}, .pill = {1, 1, 1, GLASS_O + .18},
  .light_off = {0xd6 / 255., 0xd2 / 255., 0xdc / 255., 1},
};
static const Theme DARK = {
  .fg = {0xf1 / 255., 0xee / 255., 0xf6 / 255., 1}, .fg2 = {0xaa / 255., 0xa4 / 255., 0xb6 / 255., 1},
  .fg3 = {0x77 / 255., 0x71 / 255., 0x84 / 255., 1}, .menubar_fg = {0xf6 / 255., 0xf3 / 255., 0xfa / 255., 1},
  .tint = {30 / 255., 28 / 255., 40 / 255., GLASS_O},
  /* Mit der dunklen Tönung verschwand das Dock vor dem Abendhimmel; heller wie ein dunkles Glas */
  .dock = {78 / 255., 74 / 255., 96 / 255., .46}, .win = {30 / 255., 28 / 255., 37 / 255., 1},
  .line = {1, 1, 1, .1}, .line_strong = {1, 1, 1, .2}, .hover = {1, 1, 1, .12}, .rim = {1, 1, 1, .34},
  .rim_low = {1, 1, 1, .1}, .edge = {0, 0, 0, .5}, .pill = {1, 1, 1, GLASS_O * .2 + .07},
  .light_off = {0x4a / 255., 0x46 / 255., 0x53 / 255., 1},
};
static const RGBA ACCENT = {0x7c / 255., 0x5c / 255., 0xff / 255., 1};
static const RGBA L_CLOSE = {0xff / 255., 0x5f / 255., 0x57 / 255., 1};
static const RGBA L_MIN = {0xfe / 255., 0xbc / 255., 0x2e / 255., 1};
static const RGBA L_ZOOM = {0x28 / 255., 0xc8 / 255., 0x40 / 255., 1};

/* ---------------------------------------------------------------------------------------- */
/* Programme sind .app-Bundles wie bei macOS (apps.c). Icon und Name der Systemprogramme kommen */
/* aus clokeos.html, dahinter stehen echte Programme des Images (cloakserver/bundles.py).      */

typedef struct {
  Bundle b;
  cairo_surface_t *icon, *icon_lp; /* im Dock (ds) und im Launchpad (LP_ICON) */
  pid_t pid;                       /* zuletzt gestartet: Fenster ohne bekannte Klasse finden */
  double bounce_t0;                /* Hüpfen im Dock beim Start */
  bool gone, seen;                 /* Bundle gelöscht; beim Durchsuchen gefunden */
} App;
static App *apps;
static int napps, app_fundus = -1;

/* Dock: Fundus, die behaltenen Programme, laufende andere, Trennstrich, Papierkorb */
enum { DOCK_SEP = -1, DOCK_TRASH = -2 };
typedef struct { int app, pos; } DockItem;
static DockItem ditems[64];
static int nditems;
static char dock_ids[48][128]; /* „Im Dock behalten“: Bundle-Kennungen */
static int ndock_ids;
enum { POS_BOTTOM, POS_LEFT, POS_RIGHT };
static int dock_pos; /* Systemeinstellungen > Schreibtisch & Dock */

/* ---------------------------------------------------------------------------------------- */

typedef struct Client Client;
struct Client {
  Window win, frame, grip;
  int x, y, w, h;      /* Inhalt in Bildschirmkoordinaten */
  int th, bw;          /* Titelleiste und Rand; 0, wenn das Programm selbst zeichnet */
  int minw, minh;
  char title[256];
  const char *title_en; /* eigene Fenster: englischer Titel, L() übersetzt ihn (auch nach Sprachwechsel) */
  char cls[128];
  int app;             /* Index in apps oder -1 */
  bool minimized, zoomed, can_delete, can_focus, take_focus, argb;
  bool uni;            /* Fundus, Search: das Programm liegt auch hinter der Titelleiste (Werkzeugleiste = Titelleiste) */
  int tstyle;          /* TB_*: aus _CLOKE_TITLEBAR; TB_OVERLAY: Programm zeichnet oben selbst, Ampelknöpfe liegen darüber */
  int tbh, tlx, tflags; /* Höhe der Fläche des Programms, Mitte des ersten Ampelknopfs, Merkmale (1 FullSize … 4 Titel aus) */
  int thide, toff, tly; /* vom Programm ausgeblendete / abgeschaltete Ampelknöpfe (Bit 0 schließen, 1 Dock, 2 zoomen); Mitte von oben */
  Window lights;       /* TB_OVERLAY: Fenster über den Ampelknöpfen (Eingabe; ohne Compositor auch das Bild) */
  int zx, zy, zw, zh;  /* Lage vor dem Zoomen */
  int hover, pressed;  /* Ampelknopf: 0 keiner, 1 schließen, 2 ins Dock, 3 zoomen */
  int ignore_unmaps;
  int anim;            /* ANIM_*: läuft gerade eine Animation (nur mit Compositor) */
  double anim_t0, anim_p; /* Start und Fortschritt 0..1 */
  void (*draw)(Client *, cairo_t *); /* eigene Fenster (Über ClokeOS, Rückfragen) */
  void (*click)(Client *, int x, int y);
  long data;
  Client *next;        /* zuletzt nach vorn geholt zuerst */
};

/* Animationen wie in clokeos.html: .opening (winIn .2s), .mini (.34s) und zurück */
enum { ANIM_NONE, ANIM_OPEN, ANIM_MIN, ANIM_RESTORE };
static const double ANIM_S[] = {0, .22, .34, .3};

typedef struct {
  char label[160];
  char key[48];
  int kind;            /* 0 Eintrag, 1 Trennlinie, 2 Überschrift */
  bool disabled, checked;
  void (*fn)(long);
  long arg;
  int sub;             /* Programm-Menüs: 1 + Index in gm des Untermenüs, 0 = keins */
} MenuItem;
#define MENU_MAX 64

static Display *dpy;
static int scr, SW, SH;
static Window root, menubar, dock, tipwin, checkwin, menuwin;
static Visual *vis;
static Colormap cmap;
static Client *clients, *focused;
static bool dark;
static int theme_mode; /* 0 automatisch, 1 hell, 2 dunkel */
static const Theme *T = &LIGHT;
static char resdir[PATH_MAX];
static cairo_surface_t *wall_img, *wall_blur; /* ganzer Bildschirm, Unschärfe in 1/4 Größe */
static Pixmap wall_pm;
static cairo_surface_t *icon_trash, *icon_trash_full;
static int cal_day = -1;
static Cursor cur_arrow, cur_resize, cur_move;
static int cascade;
static bool debug;
static Window loginwin;      /* Anmeldebildschirm */
static Window setupwin;      /* Einrichtungsassistent (setup.c) */
static bool setup_active;
static bool locked, sleeping; /* gesperrt; Ruhezustand (Bildschirm schwarz) */

static Atom WM_PROTOCOLS, WM_WINDOW_ROLE, WM_DELETE_WINDOW, WM_TAKE_FOCUS, WM_STATE, WM_CHANGE_STATE, UTF8_STRING,
  NET_SUPPORTED, NET_WM_NAME, NET_ACTIVE_WINDOW, NET_CLIENT_LIST, NET_WORKAREA, NET_SUPPORTING_WM_CHECK,
  NET_WM_WINDOW_TYPE, NET_WM_WINDOW_TYPE_DOCK, NET_WM_WINDOW_TYPE_DESKTOP, NET_WM_WINDOW_TYPE_SPLASH,
  NET_WM_WINDOW_TYPE_NOTIFICATION, NET_WM_WINDOW_TYPE_TOOLTIP, NET_WM_WINDOW_TYPE_MENU,
  NET_WM_WINDOW_TYPE_POPUP_MENU, NET_WM_WINDOW_TYPE_DROPDOWN_MENU, NET_WM_WINDOW_TYPE_COMBO,
  NET_WM_WINDOW_TYPE_DND, NET_CLOSE_WINDOW, NET_FRAME_EXTENTS, NET_NUMBER_OF_DESKTOPS, NET_WM_PID,
  NET_CURRENT_DESKTOP, MOTIF_WM_HINTS, XROOTPMAP_ID, ESETROOT_PMAP_ID, GTK_UNIQUE_BUS_NAME,
  GTK_MENUBAR_OBJECT_PATH, UNITY_OBJECT_PATH, GTK_APPLICATION_OBJECT_PATH, GTK_WINDOW_OBJECT_PATH, CLOKE_MENU,
  CLOKE_MENU_ACTIVATE, CLOKE_TITLEBAR, NET_WM_MOVERESIZE;

/* Menü, das gerade offen ist */
static MenuItem menu[MENU_MAX];
static int nmenu, menu_hover = -1, menu_x, menu_y, menu_w, menu_h, menu_owner = -1; /* Menüleisten-Eintrag oder -1 */
/* Untermenü eines Eintrags (Programm-Menüs in der Menüleiste), neben dem Menü */
static Window subwin;
static MenuItem submenu[MENU_MAX];
static int nsub, sub_hover = -1, sub_w, sub_h, sub_of = -1;
/* add_item & Co. füllen menu, beim Aufbau eines Untermenüs submenu */
static MenuItem *mbuild = menu;
static int *mbuild_n = &nmenu;

/* Menüleiste: Einträge mit ihrer Lage */
enum { MB_LOGO, MB_APP, MB_FILE, MB_WINDOW, MB_WIFI, MB_SPOT, MB_CC, MB_CLOCK, MB_N };
#define MB_DYN MB_N /* MB_DYN + k: das k-te Menü des aktiven Programms (globale Menüleiste) */
static int mb_x[MB_N + GM_TOP_MAX], mb_w[MB_N + GM_TOP_MAX];

/* Dock: dock_x, dock_y, dock_w, dock_h ist das Glas. Mit Compositor ist das Fenster dock_lift
 * Pixel zur Bildschirmmitte hin größer (durchsichtig), damit ein Symbol beim Starten hüpfen kann. */
static int dock_x, dock_y, dock_w, dock_h, dock_lift, dock_hover = -1;

/* Bewegen und Größe ändern */
static Client *drag;
static int drag_mode; /* 1 bewegen, 2 Größe */
static int drag_px, drag_py, drag_x, drag_y, drag_w, drag_h;
static Time last_title_click;
static Client *last_title_client;

static void redraw_all(void);
static void draw_menubar(void);
static void draw_dock(void);
static void draw_frame(Client *c);
static bool has_nav(Client *c);
static bool has_uni(Client *c);
static void focus_client(Client *c);
static void raise_client(Client *c);
static void close_menu(void);
static int app_by_ident(const char *id);
static void open_path(const char *with, const char *path, bool reveal);
static const char *const *cal_subst(void);
static cairo_surface_t *bundle_icon(const Bundle *b, int size, const char *const *subst);
static void gm_focus(Client *c);
static void gm_start(void);
static void automount_start(void);
static void gm_stop(void);
static void gm_forget(Window w);
static void gm_property(Client *c, Atom a);
static Window ui_window(int x, int y, int w, int h, long mask, bool argb, Atom type);
static void show_tip(int i);
static void dock_target(Client *c, double *x, double *y);
static bool app_running(int i);
static void show_launchpad(void);
static void show_spot(void);
static void hide_spot(void);
static void sp_click(int x, int y);
static void sp_key(XKeyEvent *e);
static void draw_spot(void);
static bool sp_open;
static Window spotwin;
static void show_settings(long section);
static void show_disk_utility(long unused);
static void show_app_problem(int i);
static void update_workarea(void);
static void save_prefs(void);
static void load_prefs(void);

static void warn(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("CloakServer: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

/* Absturz: Signal und Aufrufkette nach /var/log/cloakserver-crash.log (sonst endet die Sitzung spurlos, stderr geht ins Leere),
 * dann wie vorher mit demselben Signal beenden. Mit Symbolen der eigenen Funktionen, sofern nicht weggeworfen. */
static void crash_log(int sig) {
  int fd = open("/var/log/cloakserver-crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) fd = open("/tmp/cloakserver-crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    char head[160];
    /* Ladeadresse: mit atos -o CloakServer -l <Adresse> lassen sich auch statische Funktionen benennen */
    int n = snprintf(head, sizeof head, "CloakServer: Signal %d (pid %d, Zeit %ld, geladen bei %p)\n", sig, (int)getpid(),
                     (long)time(NULL), (void *)_dyld_get_image_header(0));
    if (n > 0) write(fd, head, (size_t)n);
    void *bt[64];
    int k = backtrace(bt, 64);
    backtrace_symbols_fd(bt, k, fd);
    close(fd);
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static int xerror(Display *d, XErrorEvent *e) {
  (void)d;
  /* Fenster verschwinden oft zwischen zwei Anfragen; das ist kein Fehler des Fenstermanagers. */
  if ((e->error_code == BadWindow || e->error_code == BadMatch || e->error_code == BadDrawable ||
       comp_quiet_error(e)) &&
      !getenv("CLOAKSERVER_DEBUG"))
    return 0;
  char msg[128];
  XGetErrorText(dpy, e->error_code, msg, sizeof msg);
  warn("X-Fehler %s (Anfrage %d.%d, Ressource 0x%lx)", msg, e->request_code, e->minor_code, e->resourceid);
  return 0;
}

static bool other_wm;
static int xerror_start(Display *d, XErrorEvent *e) {
  (void)d; (void)e;
  other_wm = true;
  return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Zeichenhilfen                                                                              */

static void set(cairo_t *cr, RGBA c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }
static void seta(cairo_t *cr, RGBA c, double a) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * a); }

void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
  r = fmax(0, fmin(r, fmin(w, h) / 2));
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

/* Text mit Pango; px = Schriftgröße in Pixeln. Gibt die Breite zurück. */
static int text(cairo_t *cr, double x, double y, const char *s, double px, bool bold, RGBA col, int maxw, int align) {
  PangoLayout *l = pango_cairo_create_layout(cr);
  PangoFontDescription *f = pango_font_description_from_string(bold ? UI_FONT " SemiBold" : UI_FONT);
  pango_font_description_set_absolute_size(f, px * PANGO_SCALE);
  pango_layout_set_font_description(l, f);
  pango_layout_set_text(l, s, -1);
  if (maxw > 0) {
    pango_layout_set_width(l, maxw * PANGO_SCALE);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
  }
  int w, h;
  pango_layout_get_pixel_size(l, &w, &h);
  if (cr && col.a > 0) {
    double tx = x;
    if (align == 1) tx = x - w / 2.0;
    else if (align == 2) tx = x - w;
    cairo_move_to(cr, tx, y - h / 2.0);
    set(cr, col);
    pango_cairo_show_layout(cr, l);
  }
  pango_font_description_free(f);
  g_object_unref(l);
  return w;
}

static int text_width(const char *s, double px, bool bold) {
  cairo_surface_t *s0 = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cairo_t *cr = cairo_create(s0);
  RGBA none = {0, 0, 0, 0};
  int w = text(cr, 0, 0, s, px, bold, none, 0, 0);
  cairo_destroy(cr);
  cairo_surface_destroy(s0);
  return w;
}

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc(n + 1);
  if (fread(b, 1, n, f) != (size_t)n) n = 0;
  b[n] = 0;
  fclose(f);
  return b;
}

/* SVG aus Resources/ laden, Platzhalter @KEY@ ersetzen und in size x size Pixel zeichnen. */
/* Lage eines Symbols in seinem Feld: zoomen um k, dann verschieben */
typedef struct { double k, tx, ty; } Fit;

static cairo_surface_t *svg_render_fit(const char *name, int w, int h, const char *const *subst, const Fit *fit) {
  char path[PATH_MAX];
  if (name[0] == '/') snprintf(path, sizeof path, "%s", name);
  else snprintf(path, sizeof path, "%s/%s", resdir, name);
  char *src = read_file(path);
  if (!src) {
    warn("fehlt: %s", path);
    return NULL;
  }
  for (int i = 0; subst && subst[i]; i += 2) {
    const char *k = subst[i], *v = subst[i + 1];
    char *p;
    while ((p = strstr(src, k))) {
      size_t kl = strlen(k), vl = strlen(v), n = strlen(src);
      char *t = malloc(n - kl + vl + 1);
      memcpy(t, src, p - src);
      memcpy(t + (p - src), v, vl);
      strcpy(t + (p - src) + vl, p + kl);
      free(src);
      src = t;
    }
  }
  GError *err = NULL;
  RsvgHandle *hd = rsvg_handle_new_from_data((const guint8 *)src, strlen(src), &err);
  free(src);
  if (!hd) {
    warn("SVG %s: %s", name, err ? err->message : "?");
    return NULL;
  }
  RsvgDimensionData dim;
  rsvg_handle_get_dimensions(hd, &dim);
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(s);
  if (fit) {
    cairo_translate(cr, fit->tx, fit->ty);
    cairo_scale(cr, fit->k, fit->k);
  }
  cairo_scale(cr, (double)w / dim.width, (double)h / dim.height);
  rsvg_handle_render_cairo(hd, cr);
  cairo_destroy(cr);
  g_object_unref(hd);
  return s;
}

static cairo_surface_t *svg_render(const char *name, int w, int h, const char *const *subst) {
  return svg_render_fit(name, w, h, subst, NULL);
}

/* Symbole ohne eigene Fläche (der Papierkorb ist nur ein Eimer mit viel Luft darüber) wirken
 * neben den Quadraten der anderen klein und sitzen zu tief. Auf den Inhalt zuschneiden und so
 * hoch wie die Quadrate (58 von 64) mittig ins Feld setzen. */
static Fit icon_fit(cairo_surface_t *s) {
  Fit f = {1, 0, 0};
  int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
  int stride = cairo_image_surface_get_stride(s), x0 = w, y0 = h, x1 = -1, y1 = -1;
  cairo_surface_flush(s);
  unsigned char *p = cairo_image_surface_get_data(s);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (p[y * stride + x * 4 + 3] > 24) { /* Alpha in ARGB32 (little endian) */
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
  if (x1 < 0) return f;
  double bw = x1 - x0 + 1, bh = y1 - y0 + 1, target = w * 58.0 / 64;
  if (bw >= .85 * w || bh >= .85 * h) return f;
  f.k = target / fmax(bw, bh);
  f.tx = w / 2.0 - f.k * (x0 + bw / 2);
  f.ty = h / 2.0 - f.k * (y0 + bh / 2);
  return f;
}

static void rgba_hex(char *buf, RGBA c) {
  snprintf(buf, 16, "#%02x%02x%02x", (int)(c.r * 255 + .5), (int)(c.g * 255 + .5), (int)(c.b * 255 + .5));
}

/* Doppelt gepuffert in ein Fenster zeichnen. */
typedef void (*painter)(cairo_t *, int w, int h, void *);
static void paint_window(Window w, int width, int height, painter fn, void *arg) {
  /* Dock, Menüs und Namensschild sind mit Compositor 32-Bit-Fenster mit Durchsicht */
  Visual *v = comp_on && w && (w == dock || w == menuwin || w == subwin || w == tipwin || w == spotwin) ? comp_argb : vis;
  for (Client *c = clients; c; c = c->next)
    if (c->frame == w && c->argb) v = comp_argb;
  cairo_surface_t *s = cairo_xlib_surface_create(dpy, w, v, width, height);
  cairo_t *cr = cairo_create(s);
  cairo_push_group(cr);
  fn(cr, width, height, arg);
  cairo_pop_group_to_source(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(s);
}

/* Fensterform mit runden Ecken (XShape), damit darunter Hintergrund oder Fenster sichtbar sind. */
static void shape_round(Window w, int width, int height, double r, bool top_only) {
  Pixmap pm = XCreatePixmap(dpy, w, width, height, 1);
  cairo_surface_t *s = cairo_xlib_surface_create_for_bitmap(dpy, pm, ScreenOfDisplay(dpy, scr), width, height);
  cairo_t *cr = cairo_create(s);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  cairo_set_source_rgba(cr, 0, 0, 0, 1);
  rrect(cr, 0, 0, width, top_only ? height + r : height, r);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(s);
  XShapeCombineMask(dpy, w, ShapeBounding, 0, 0, pm, ShapeSet);
  XFreePixmap(dpy, pm);
}

/* Hintergrund als Glas: Tönung über dem, was dahinter liegt. Mit Compositor zeichnet der die
 * unscharfe Umgebung darunter, und das Fenster färbt nur die Fläche mit den runden Ecken ein;
 * ohne Compositor ist es ein unscharfer Ausschnitt des Hintergrundbilds, die Ecken schneidet
 * XShape ab. */
static void glass(cairo_t *cr, int sx, int sy, RGBA t, int w, int h, double r) {
  double alpha = t.a;
  if (comp_on) {
    rrect(cr, 0, 0, w, h, r);
    cairo_set_source_rgba(cr, t.r, t.g, t.b, alpha);
    cairo_fill(cr);
    return;
  }
  cairo_save(cr);
  cairo_translate(cr, -sx, -sy);
  cairo_scale(cr, 4, 4);
  cairo_set_source_surface(cr, wall_blur, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD); /* sonst dunkle Kante am Rand */
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_set_source_rgba(cr, t.r, t.g, t.b, alpha);
  cairo_paint(cr);
}

/* Glanzkante: Verlauf von hell oben links über durchsichtig zu schwächer unten rechts. */
static void rim(cairo_t *cr, double x, double y, double w, double h, double r) {
  cairo_pattern_t *g = cairo_pattern_create_linear(x, y, x + w * .64, y + h * 1.2);
  RGBA a = T->rim, b = T->rim_low;
  cairo_pattern_add_color_stop_rgba(g, 0, a.r, a.g, a.b, a.a);
  cairo_pattern_add_color_stop_rgba(g, .32, 1, 1, 1, 0);
  cairo_pattern_add_color_stop_rgba(g, .68, 1, 1, 1, 0);
  cairo_pattern_add_color_stop_rgba(g, 1, b.r, b.g, b.b, b.a);
  rrect(cr, x + .5, y + .5, w - 1, h - 1, r - .5);
  cairo_set_source(cr, g);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  cairo_pattern_destroy(g);
}

/* ---------------------------------------------------------------------------------------- */
/* Hintergrundbild, Thema                                                                     */

static void blur_box(unsigned char *p, int w, int h, int stride, int r, bool horiz) {
  int n = horiz ? w : h, m = horiz ? h : w;
  int *acc = malloc(sizeof(int) * (n + 1) * 4);
  for (int j = 0; j < m; j++) {
    unsigned char *base = horiz ? p + j * stride : p + j * 4;
    int step = horiz ? 4 : stride;
    for (int c = 0; c < 4; c++) acc[c] = 0;
    for (int i = 0; i < n; i++)
      for (int c = 0; c < 4; c++) acc[(i + 1) * 4 + c] = acc[i * 4 + c] + base[i * step + c];
    for (int i = 0; i < n; i++) {
      int a = i - r < 0 ? 0 : i - r, b = i + r + 1 > n ? n : i + r + 1;
      for (int c = 0; c < 4; c++) base[i * step + c] = (acc[b * 4 + c] - acc[a * 4 + c]) / (b - a);
    }
  }
  free(acc);
}

static bool want_dark(void) {
  if (recovery) return true; /* die Wiederherstellung ist dunkel, wie bei macOS */
  if (theme_mode == 1) return false;
  if (theme_mode == 2) return true;
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  return tm.tm_hour >= 19 || tm.tm_hour < 7; /* hell am Tag, dunkel am Abend */
}

static void render_icons(void);

/* XSETTINGS (freedesktop.org): CloakServer meldet GTK-Programmen das Thema über die Auswahl
 * _XSETTINGS_S0. Anders als die Umgebungsvariable GTK_THEME wechseln so auch Programme, die
 * schon laufen, zwischen Hell und Dunkel, wie bei macOS. "Adwaita-dark" liegt als kleines Thema
 * in /share/themes und lädt GTKs eingebaute dunkle Variante. */
static Window xsettings_win;
static Atom XSETTINGS_SETTINGS;
static uint32_t xsettings_serial;

static void xsettings_update(void) {
  if (!xsettings_win) return;
  static const char *name = "Net/ThemeName";
  /* Eigenes Thema ClokeOS (overlay-desktop/share/themes): Adwaita mit Farben und Formen wie
   * CloakServer; fehlt es, das eingebaute Adwaita */
  bool own = access("/share/themes/ClokeOS/gtk-3.0/gtk.css", R_OK) == 0;
  const char *val = own ? (dark ? "ClokeOS-dark" : "ClokeOS") : dark ? "Adwaita-dark" : "Adwaita";
  /* Gtk/ShellShowsMenubar: GTK-Programme zeigen ihre Menüleiste nicht im Fenster, die Menüs
   * stehen oben in der Leiste (globale Menüleiste, siehe dort). Nur wenn cloakmenu da ist. */
  bool menus = access(CLOAKMENU, X_OK) == 0;
  /* Gtk/FontName: Cloke Sans (overlay-desktop/Library/Fonts/ClokeSans) auch in den GTK-Programmen */
  bool font = access(UI_FONT_FILE, R_OK) == 0;
  /* Net/IconThemeName: ClokeOS-Symbole (Ordner wie in clokeos.html), erbt von hicolor */
  bool icons = access("/share/icons/ClokeOS/index.theme", R_OK) == 0;
  uint32_t nl = (uint32_t)strlen(name), vl = (uint32_t)strlen(val), count = 2 + menus + font + icons;
  uint16_t nl16 = (uint16_t)nl;
  unsigned char buf[512] = {0}, *p = buf;
  p += 4;                                    /* Byte-Reihenfolge 0 = LSBFirst (x86), 3 frei */
  memcpy(p, &xsettings_serial, 4), p += 4;
  memcpy(p, &count, 4), p += 4;              /* Anzahl der Einstellungen */
  *p = 1, p += 2;                            /* Typ String, 1 frei */
  memcpy(p, &nl16, 2), p += 2;
  memcpy(p, name, nl), p += (nl + 3) & ~3u;
  memcpy(p, &xsettings_serial, 4), p += 4;   /* zuletzt geändert */
  memcpy(p, &vl, 4), p += 4;
  memcpy(p, val, vl), p += (vl + 3) & ~3u;
  if (menus) {
    static const char *mname = "Gtk/ShellShowsMenubar";
    uint16_t ml = (uint16_t)strlen(mname);
    int32_t on = 1;
    *p = 0, p += 2;                          /* Typ Ganzzahl, 1 frei */
    memcpy(p, &ml, 2), p += 2;
    memcpy(p, mname, ml), p += (ml + 3) & ~3u;
    memcpy(p, &xsettings_serial, 4), p += 4;
    memcpy(p, &on, 4), p += 4;
  }
  if (font) {
    static const char *fname = "Gtk/FontName", *fval = UI_FONT " 10";
    uint16_t fl = (uint16_t)strlen(fname);
    uint32_t fvl = (uint32_t)strlen(fval);
    *p = 1, p += 2;                          /* Typ String */
    memcpy(p, &fl, 2), p += 2;
    memcpy(p, fname, fl), p += (fl + 3) & ~3u;
    memcpy(p, &xsettings_serial, 4), p += 4;
    memcpy(p, &fvl, 4), p += 4;
    memcpy(p, fval, fvl), p += (fvl + 3) & ~3u;
  }
  if (icons) {
    static const char *iname = "Net/IconThemeName", *ival = "ClokeOS";
    uint16_t il = (uint16_t)strlen(iname);
    uint32_t ivl = (uint32_t)strlen(ival);
    *p = 1, p += 2;
    memcpy(p, &il, 2), p += 2;
    memcpy(p, iname, il), p += (il + 3) & ~3u;
    memcpy(p, &xsettings_serial, 4), p += 4;
    memcpy(p, &ivl, 4), p += 4;
    memcpy(p, ival, ivl), p += (ivl + 3) & ~3u;
  }
  { /* Gtk/DecorationLayout: Fensterknöpfe in GTK-Kopfleisten links wie bei macOS (Thema: Ampelknöpfe, docs/titlebar.md);
     * NT-Oberfläche: rechts wie bei Windows */
    static const char *dname = "Gtk/DecorationLayout";
    const char *dval = nt ? "menu:minimize,maximize,close" : "close,minimize,maximize:";
    uint16_t dl = (uint16_t)strlen(dname);
    uint32_t dvl = (uint32_t)strlen(dval);
    *p = 1, p += 2;
    memcpy(p, &dl, 2), p += 2;
    memcpy(p, dname, dl), p += (dl + 3) & ~3u;
    memcpy(p, &xsettings_serial, 4), p += 4;
    memcpy(p, &dvl, 4), p += 4;
    memcpy(p, dval, dvl), p += (dvl + 3) & ~3u;
  }
  XChangeProperty(dpy, xsettings_win, XSETTINGS_SETTINGS, XSETTINGS_SETTINGS, 8, PropModeReplace, buf, (int)(p - buf));
  xsettings_serial++;
}

static void xsettings_init(void) {
  char sel_name[32];
  snprintf(sel_name, sizeof sel_name, "_XSETTINGS_S%d", scr);
  Atom sel = XInternAtom(dpy, sel_name, False);
  XSETTINGS_SETTINGS = XInternAtom(dpy, "_XSETTINGS_SETTINGS", False);
  if (XGetSelectionOwner(dpy, sel) != None) {
    warn("XSETTINGS: %s gehört schon einem anderen Programm", sel_name);
    return;
  }
  xsettings_win = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
  xsettings_update();
  XSetSelectionOwner(dpy, sel, xsettings_win, CurrentTime);
  if (XGetSelectionOwner(dpy, sel) != xsettings_win) {
    XDestroyWindow(dpy, xsettings_win);
    xsettings_win = None;
    return;
  }
  XClientMessageEvent ev = {.type = ClientMessage, .window = root, .format = 32};
  ev.message_type = XInternAtom(dpy, "MANAGER", False);
  ev.data.l[0] = CurrentTime;
  ev.data.l[1] = (long)sel;
  ev.data.l[2] = (long)xsettings_win;
  XSendEvent(dpy, root, False, StructureNotifyMask, (XEvent *)&ev);
}

/* NT: Symbole links auf dem Desktop; Doppelklick startet sie. Gemalt wird in das Hintergrundbild der
 * Wurzel (nicht in das Bild, aus dem das Glas entsteht). */
#define NT_DESK_N 4
#define NT_DESK_X 16
#define NT_DESK_CELL 96
static int nt_desk_app(int k) {
  switch (k) {
  case 0: return app_fundus;
  case 1: return app_by_ident("com.clokeos.systemeinstellungen");
  case 2: return app_by_ident("com.clokeos.search");
  default: return -1; /* Papierkorb */
  }
}
static int nt_desk_at(int x, int y) {
  int k = (y - (MENUBAR_H + 12)) / NT_DESK_CELL;
  if (x < NT_DESK_X || x >= NT_DESK_X + NT_DESK_CELL || y < MENUBAR_H + 12 || k < 0 || k >= NT_DESK_N) return -1;
  return k;
}
static void paint_desktop_icons(cairo_t *cr) {
  for (int k = 0; k < NT_DESK_N; k++) {
    int i = nt_desk_app(k);
    static cairo_surface_t *nt_trash;
    cairo_surface_t *ic = NULL;
    if (k == NT_DESK_N - 1) {
      if (!nt_trash) nt_trash = svg_render("icons/papierkorb.svg", 48, 48, NULL);
      ic = nt_trash;
    } else if (i >= 0) {
      if (!apps[i].icon_lp) apps[i].icon_lp = bundle_icon(&apps[i].b, 76, cal_subst());
      ic = apps[i].icon_lp;
    }
    const char *name = k == NT_DESK_N - 1 ? L("Trash") : (i >= 0 ? apps[i].b.name : "");
    double cx = NT_DESK_X + NT_DESK_CELL / 2.0, y = MENUBAR_H + 12 + k * NT_DESK_CELL;
    if (ic) {
      double iw = cairo_image_surface_get_width(ic), s = 48 / iw;
      cairo_save(cr);
      cairo_translate(cr, cx - 24, y + 4);
      cairo_scale(cr, s, s);
      cairo_set_source_surface(cr, ic, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    }
    cairo_set_source_rgba(cr, 0, 0, 0, .55); /* Schatten für die Beschriftung */
    text(cr, cx + 1, y + 70 + 1, name, 12, false, (RGBA){0, 0, 0, .55}, NT_DESK_CELL - 8, 1);
    text(cr, cx, y + 70, name, 12, false, (RGBA){1, 1, 1, 1}, NT_DESK_CELL - 8, 1);
  }
}

static void apply_theme(void) {
  dark = want_dark();
  T = dark ? &DARK : &LIGHT;
  unsetenv("GTK_THEME"); /* sonst hätte die Variable Vorrang vor XSETTINGS */
  setenv("CLOKEOS_DARK", dark ? "1" : "0", 1); /* Programme, die beim Start eine Seite wählen (Search) */
  xsettings_update();

  if (wall_img) cairo_surface_destroy(wall_img);
  wall_img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, SW, SH);
  cairo_t *cr = cairo_create(wall_img);
  wall_draw(cr, SW, SH, dark);
  cairo_destroy(cr);

  if (wall_blur) cairo_surface_destroy(wall_blur);
  int bw = (SW + 3) / 4, bh = (SH + 3) / 4;
  wall_blur = cairo_image_surface_create(CAIRO_FORMAT_RGB24, bw, bh);
  cr = cairo_create(wall_blur);
  cairo_scale(cr, .25, .25);
  cairo_set_source_surface(cr, wall_img, 0, 0);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(wall_blur);
  unsigned char *p = cairo_image_surface_get_data(wall_blur);
  int stride = cairo_image_surface_get_stride(wall_blur);
  for (int k = 0; k < 3; k++) { /* drei Kastenfilter ~ Gauß, Radius ~ 24 px am Bildschirm */
    blur_box(p, bw, bh, stride, 2, true);
    blur_box(p, bw, bh, stride, 2, false);
  }
  cairo_surface_mark_dirty(wall_blur);

  Pixmap old = wall_pm;
  wall_pm = XCreatePixmap(dpy, root, SW, SH, DefaultDepth(dpy, scr));
  cairo_surface_t *ps = cairo_xlib_surface_create(dpy, wall_pm, vis, SW, SH);
  cr = cairo_create(ps);
  cairo_set_source_surface(cr, wall_img, 0, 0);
  cairo_paint(cr);
  if (nt && !recovery) paint_desktop_icons(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(ps);
  XSetWindowBackgroundPixmap(dpy, root, wall_pm);
  XClearWindow(dpy, root);
  XChangeProperty(dpy, root, XROOTPMAP_ID, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&wall_pm, 1);
  XChangeProperty(dpy, root, ESETROOT_PMAP_ID, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&wall_pm, 1);
  if (old) XFreePixmap(dpy, old);
  render_icons();
  comp_dirty_all();
}

static void save_prefs(void);
static void redraw_all(void);
static void set_wallpaper(long idx) {
  wall_sel = (int)idx;
  apply_theme();
  save_prefs();
  redraw_all();
}

static void set_theme_mode(long mode) {
  theme_mode = (int)mode;
  bool was = dark;
  if (want_dark() != was || !wall_img) apply_theme();
  save_prefs();
  redraw_all();
}

/* ---------------------------------------------------------------------------------------- */
/* Icons, Programme starten                                                                   */


static const char *const *cal_subst(void) {
  static char day[8];
  static const char *cal[5];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  snprintf(day, sizeof day, "%d", tm.tm_mday);
  static char mon[24];
  char *up = g_utf8_strup(l10n_month_short(tm.tm_mon), -1);
  snprintf(mon, sizeof mon, "%s", up);
  g_free(up);
  cal[0] = "@MON@", cal[1] = mon, cal[2] = "@DAY@", cal[3] = day, cal[4] = NULL;
  cal_day = tm.tm_mday;
  return cal;
}

/* Bild mittig in size x size einpassen */
static cairo_surface_t *scaled(cairo_surface_t *src, int size) {
  int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  if (w <= 0 || h <= 0) return s;
  double k = (double)size / (w > h ? w : h);
  cairo_t *cr = cairo_create(s);
  cairo_translate(cr, (size - w * k) / 2, (size - h * k) / 2);
  cairo_scale(cr, k, k);
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  return s;
}

typedef struct { const unsigned char *d; size_t n, pos; } MemReader;
static cairo_status_t mem_read(void *closure, unsigned char *out, unsigned int len) {
  MemReader *m = closure;
  if (m->pos + len > m->n) return CAIRO_STATUS_READ_ERROR;
  memcpy(out, m->d + m->pos, len);
  m->pos += len;
  return CAIRO_STATUS_SUCCESS;
}

/* Programme ohne eigenes Icon: abgerundetes Quadrat mit Anfangsbuchstaben */
static cairo_surface_t *generic_icon(const char *name, int size) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(s);
  double k = size / 64.0, hue = 0;
  for (const char *p = name; *p; p++) hue += (unsigned char)*p;
  RGBA a = {.45 + .3 * sin(hue), .45 + .3 * sin(hue + 2.1), .45 + .3 * sin(hue + 4.2), 1};
  cairo_pattern_t *g = cairo_pattern_create_linear(0, 4 * k, 0, 60 * k);
  cairo_pattern_add_color_stop_rgb(g, 0, a.r + .15, a.g + .15, a.b + .15);
  cairo_pattern_add_color_stop_rgb(g, 1, a.r * .7, a.g * .7, a.b * .7);
  rrect(cr, 4 * k, 4 * k, 56 * k, 56 * k, 13 * k);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
  char initial[8] = {0};
  int il = 1;
  while (name[0] && name[il] && (name[il] & 0xc0) == 0x80) il++;
  snprintf(initial, sizeof initial, "%.*s", il, name[0] ? name : "?");
  for (char *p = initial; *p; p++) *p = (char)toupper((unsigned char)*p);
  text(cr, size / 2.0, size / 2.0, initial, 30 * k, true, (RGBA){1, 1, 1, 1}, 0, 1);
  cairo_destroy(cr);
  return s;
}

/* Icon eines Bundles in size x size: SVG (mit den Platzhaltern des Kalenders), PNG oder .icns */
static cairo_surface_t *bundle_icon(const Bundle *b, int size, const char *const *subst) {
  const char *ext = strrchr(b->icon, '.');
  cairo_surface_t *s = NULL;
  if (b->icon[0] && ext && !strcasecmp(ext, ".svg")) {
    s = svg_render(b->icon, size, size, subst);
    Fit f = s ? icon_fit(s) : (Fit){1, 0, 0};
    if (s && f.k != 1) {
      cairo_surface_destroy(s);
      s = svg_render_fit(b->icon, size, size, subst, &f);
    }
  } else if (b->icon[0] && ext && !strcasecmp(ext, ".icns")) {
    size_t n = 0;
    unsigned char *png = icns_png(b->icon, size * 2, &n);
    if (png) {
      MemReader m = {png, n, 0};
      cairo_surface_t *p = cairo_image_surface_create_from_png_stream(mem_read, &m);
      if (cairo_surface_status(p) == CAIRO_STATUS_SUCCESS) s = scaled(p, size);
      cairo_surface_destroy(p);
      free(png);
    }
  } else if (b->icon[0]) {
    cairo_surface_t *p = cairo_image_surface_create_from_png(b->icon);
    if (cairo_surface_status(p) == CAIRO_STATUS_SUCCESS) s = scaled(p, size);
    cairo_surface_destroy(p);
  }
  return s ? s : generic_icon(b->name, size);
}

static void render_app_icon(App *a) {
  if (a->icon) cairo_surface_destroy(a->icon);
  if (a->icon_lp) cairo_surface_destroy(a->icon_lp); /* das Launchpad zeichnet es bei Bedarf neu */
  a->icon_lp = NULL;
  a->icon = bundle_icon(&a->b, ds, cal_subst());
}

static void render_icons(void) {
  for (int i = 0; i < napps; i++)
    if (!apps[i].gone) render_app_icon(&apps[i]);
  /* der volle Papierkorb an derselben Stelle wie der leere, sonst springt der Eimer */
  if (icon_trash) cairo_surface_destroy(icon_trash);
  if (icon_trash_full) cairo_surface_destroy(icon_trash_full);
  icon_trash = svg_render("icons/papierkorb.svg", ds, ds, NULL);
  Fit f = icon_trash ? icon_fit(icon_trash) : (Fit){1, 0, 0};
  if (icon_trash && f.k != 1) {
    cairo_surface_destroy(icon_trash);
    icon_trash = svg_render_fit("icons/papierkorb.svg", ds, ds, NULL, &f);
  }
  icon_trash_full = svg_render_fit("icons/papierkorb-voll.svg", ds, ds, NULL, &f);
}

/* Konto des angemeldeten Benutzers, NULL = niemand angemeldet (siehe "Konten und Sitzung") */
typedef struct {
  uid_t uid;
  gid_t gid;
  char name[32], full[64], home[256], shell[64], passwd[128];
} Account;
static Account *session;

/* Rechte eines Kontos annehmen (im Kindprozess); root bleibt root */
static bool become(const Account *a) {
  if (!a || a->uid == 0) return true;
  return initgroups(a->name, (int)a->gid) == 0 && setgid(a->gid) == 0 && setuid(a->uid) == 0;
}

/* Programme laufen als der angemeldete Benutzer, nur Ausschalten und Neustart als root.
 * macfw: ein Mac-Programm mit Darlings Frameworks, dyld sucht sie über DYLD_FRAMEWORK_PATH. */
static pid_t spawn_env(const char *cmd, bool system, bool macfw) {
  char buf[1024];
  snprintf(buf, sizeof buf, "cd \"$HOME\" && exec %s", cmd);
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    signal(SIGCHLD, SIG_DFL);
    if (!system && !become(session)) _exit(126);
    if (macfw) setenv("DYLD_FRAMEWORK_PATH", MAC_FRAMEWORKS, 1);
    execl("/bin/sh", "sh", "-c", buf, (char *)NULL);
    _exit(127);
  }
  if (pid < 0) warn("kann %s nicht starten: %s", cmd, strerror(errno));
  return pid;
}

static pid_t spawn_as(const char *cmd, bool system) { return spawn_env(cmd, system, false); }
static pid_t spawn(const char *cmd) { return spawn_as(cmd, false); }

/* Auf ein Kind warten (system, pclose): CloakServer ignoriert SIGCHLD, damit gestartete Programme
 * keine Zombies werden. Darwin behandelt das wie SA_NOCLDWAIT, und waitpid wacht dann nicht auf,
 * wenn das Kind endet, solange noch andere Kinder laufen (cloakmenu läuft immer): CloakServer
 * stünde still. Deshalb SIGCHLD solange auf Standard und danach liegengebliebene Zombies abholen. */
static void child_wait_begin(void) { signal(SIGCHLD, SIG_DFL); }
static void child_wait_end(void) {
  signal(SIGCHLD, SIG_IGN);
  while (waitpid(-1, NULL, WNOHANG) > 0) {
  }
}
static int run_wait(const char *cmd) {
  child_wait_begin();
  int st = system(cmd);
  child_wait_end();
  return st;
}

/* 'Pfad' für sh -c */
static void sh_quote(char *out, size_t n, const char *s) {
  size_t k = 0;
  if (n < 3) return;
  out[k++] = '\'';
  for (; *s && k + 6 < n; s++) {
    if (*s == '\'') {
      memcpy(out + k, "'\\''", 4);
      k += 4;
    } else {
      out[k++] = *s;
    }
  }
  out[k++] = '\'';
  out[k] = 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Programme finden: /System/Applications, /Applications, ~/Applications (Unterordner wie      */
/* Utilities eine Ebene tief), Fundus wie der Finder in /System/Library/CoreServices.          */

static int app_by_ident(const char *id) {
  for (int i = 0; i < napps; i++)
    if (!apps[i].gone && !strcmp(apps[i].b.ident, id)) return i;
  return -1;
}

static void add_bundle(const char *path) {
  for (int i = 0; i < napps; i++)
    if (!strcmp(apps[i].b.path, path)) {
      apps[i].seen = true;
      if (apps[i].gone || bundle_mtime(path) != apps[i].b.mtime) {
        bundle_read(path, &apps[i].b);
        render_app_icon(&apps[i]);
      }
      apps[i].gone = false;
      return;
    }
  App *na = realloc(apps, (napps + 1) * sizeof *apps);
  if (!na) return;
  apps = na;
  App *a = &apps[napps];
  memset(a, 0, sizeof *a);
  if (!bundle_read(path, &a->b)) return;
  a->seen = true;
  napps++;
  render_app_icon(a);
  if (debug) warn("Programm %s (%s), Start %s", a->b.name, a->b.ident, a->b.kind == APP_OK ? "möglich" : a->b.missing);
}

static void scan_dir(const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (!d) return;
  for (struct dirent *e; (e = readdir(d));) {
    if (e->d_name[0] == '.') continue;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    size_t n = strlen(e->d_name);
    struct stat st;
    if (n > 4 && !strcmp(e->d_name + n - 4, ".app")) add_bundle(p);
    else if (depth > 0 && !stat(p, &st) && S_ISDIR(st.st_mode)) scan_dir(p, depth - 1);
  }
  closedir(d);
}

static void app_dir(char *out, size_t n, int k) { /* k = 0..4: die Ordner, deren Änderung zählt */
  const char *home = getenv("HOME") ? getenv("HOME") : "/var/root";
  static const char *fixed[] = {"/System/Applications", "/System/Applications/Utilities", "/Applications",
                                "/Applications/Utilities"};
  if (k < 4) snprintf(out, n, "%s", fixed[k]);
  else snprintf(out, n, "%s/Applications", home);
}

static long apps_signature(void) {
  long sig = 0;
  for (int k = 0; k < 5; k++) {
    char p[PATH_MAX];
    struct stat st;
    app_dir(p, sizeof p, k);
    if (!stat(p, &st)) sig = sig * 31 + (long)st.st_mtime + (long)st.st_ino;
  }
  return sig;
}

static long apps_sig;

static void scan_apps(void) {
  for (int i = 0; i < napps; i++) apps[i].seen = false;
  add_bundle("/System/Library/CoreServices/Fundus.app");
  char p[PATH_MAX];
  app_dir(p, sizeof p, 0);
  scan_dir(p, 1);
  app_dir(p, sizeof p, 2);
  scan_dir(p, 1);
  app_dir(p, sizeof p, 4);
  scan_dir(p, 1);
  for (int i = 0; i < napps; i++) apps[i].gone = !apps[i].seen;
  app_fundus = app_by_ident("com.clokeos.fundus");
  apps_sig = apps_signature();
}

/* Programm starten wie ein Doppelklick im Finder: Contents/MacOS/<CFBundleExecutable> */
static void launch(int i) {
  if (i < 0 || i >= napps || apps[i].gone) return;
  App *a = &apps[i];
  if (!strcmp(a->b.internal, "launchpad")) {
    show_launchpad();
    return;
  }
  if (!strcmp(a->b.internal, "settings")) {
    show_settings(-1);
    return;
  }
  if (!strcmp(a->b.internal, "diskutil")) {
    show_disk_utility(0);
    return;
  }
  if (a->b.kind != APP_OK) {
    show_app_problem(i);
    return;
  }
  char q[2600];
  sh_quote(q, sizeof q, a->b.exec);
  if (!app_running(i)) a->bounce_t0 = now_s();
  a->pid = spawn_env(q, false, a->b.macfw);
  draw_dock();
}

static void launch_app(long i) { launch((int)i); }
static void launch_ident(const char *id) { launch(app_by_ident(id)); }

/* Klassenliste aus CLWindowClasses: durch Leerzeichen getrennt, * = beliebiger Teil. exact: nur Einträge ohne *
 * (ein Bundle mit dem genauen Namen geht vor: "notepad*" von Wine.app darf "notepad++" aus Raven nicht wegnehmen) */
static bool class_matches(const char *classes, const char *cls, bool exact) {
  char list[256];
  snprintf(list, sizeof list, "%s", classes);
  char *save = NULL;
  for (char *tok = strtok_r(list, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
    size_t n = strlen(tok);
    char *star = strchr(tok, '*');
    if (exact && star) continue;
    if (star && star == tok + n - 1) {
      if (!strncasecmp(cls, tok, n - 1)) return true;
    } else if (star) { /* Präfix*Suffix */
      size_t pre = star - tok, suf = n - pre - 1, cl = strlen(cls);
      if (cl >= pre + suf && !strncasecmp(cls, tok, pre) && !strcasecmp(cls + cl - suf, star + 1)) return true;
    } else if (!strcasecmp(cls, tok)) {
      return true;
    }
  }
  return false;
}

/* Zu welchem Programm gehört ein Fenster? Fensterrolle = Bundle-Kennung (Programme im
 * Terminal), WM_CLASS, die PID des gestarteten Programms oder Name der Startdatei. */
static int app_for_window(Window w, const char *cls) {
  XTextProperty tp;
  int app = -1;
  if (XGetTextProperty(dpy, w, &tp, WM_WINDOW_ROLE) && tp.value) {
    app = app_by_ident((const char *)tp.value);
    XFree(tp.value);
  }
  for (int pass = 0; pass < 2 && app < 0; pass++)
    for (int i = 0; app < 0 && i < napps; i++)
      if (!apps[i].gone && apps[i].b.classes[0] && class_matches(apps[i].b.classes, cls, pass == 0)) app = i;
  Atom type;
  int fmt;
  unsigned long n, after;
  unsigned char *data = NULL;
  if (app < 0 && XGetWindowProperty(dpy, w, NET_WM_PID, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) == Success &&
      data && n == 1) {
    pid_t pid = (pid_t) * (long *)data;
    for (int i = 0; app < 0 && i < napps; i++)
      if (!apps[i].gone && apps[i].pid == pid) app = i;
  }
  if (data) XFree(data);
  for (int i = 0; app < 0 && cls[0] && i < napps; i++)
    if (!apps[i].gone && (!strcasecmp(cls, apps[i].b.exec_name) || !strcasecmp(cls, apps[i].b.name))) app = i;
  return app;
}

static bool trash_full(void) {
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/.Trash", getenv("HOME") ? getenv("HOME") : "/var/root");
  DIR *d = opendir(path);
  if (!d) return false;
  bool full = false;
  for (struct dirent *e; !full && (e = readdir(d));) full = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
  closedir(d);
  return full;
}

/* ---------------------------------------------------------------------------------------- */
/* Clients                                                                                    */

static Client *find_client(Window w) {
  for (Client *c = clients; c; c = c->next)
    if (c->win == w || c->frame == w || c->grip == w || (c->lights && c->lights == w)) return c;
  return NULL;
}

static bool get_atom_prop(Window w, Atom prop, Atom *out, int max, int *n) {
  Atom type;
  int fmt;
  unsigned long cnt, after;
  unsigned char *data = NULL;
  *n = 0;
  if (XGetWindowProperty(dpy, w, prop, 0, max, False, XA_ATOM, &type, &fmt, &cnt, &after, &data) != Success || !data)
    return false;
  for (unsigned long i = 0; i < cnt && (int)i < max; i++) out[(*n)++] = ((Atom *)data)[i];
  XFree(data);
  return *n > 0;
}

static void update_title(Client *c) {
  c->title[0] = 0;
  Atom type;
  int fmt;
  unsigned long cnt, after;
  unsigned char *data = NULL;
  if (XGetWindowProperty(dpy, c->win, NET_WM_NAME, 0, 256, False, UTF8_STRING, &type, &fmt, &cnt, &after, &data) == Success &&
      data && cnt) {
    snprintf(c->title, sizeof c->title, "%s", (char *)data);
  } else {
    char *name = NULL;
    if (XFetchName(dpy, c->win, &name) && name) {
      snprintf(c->title, sizeof c->title, "%s", name);
      XFree(name);
    }
  }
  if (data) XFree(data);
  /* xfce4-terminal und andere hängen " - " an, wenn kein Titel gesetzt ist ("Terminal - "): abschneiden */
  size_t n = strlen(c->title);
  while (n > 1 && (c->title[n - 1] == ' ' || c->title[n - 1] == '-')) c->title[--n] = 0;
}

static void set_wm_state(Client *c, long state) {
  long d[2] = {state, None};
  XChangeProperty(dpy, c->win, WM_STATE, WM_STATE, 32, PropModeReplace, (unsigned char *)d, 2);
}

static void update_client_list(void) {
  Window list[256];
  int n = 0;
  for (Client *c = clients; c && n < 256; c = c->next)
    if (!c->draw) list[n++] = c->win;
  XChangeProperty(dpy, root, NET_CLIENT_LIST, XA_WINDOW, 32, PropModeReplace, (unsigned char *)list, n);
}

static void send_configure(Client *c) {
  XConfigureEvent ce = {0};
  ce.type = ConfigureNotify;
  ce.display = dpy;
  ce.event = c->win;
  ce.window = c->win;
  ce.x = c->x;
  ce.y = c->uni ? c->y - c->th : c->y;
  ce.width = c->w;
  ce.height = c->uni ? c->h + c->th : c->h;
  ce.above = None;
  XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

/* Platz für Fenster: ohne Menüleiste und Dock (unten, links oder rechts) */
static void work_area(int *x0, int *y0, int *x1, int *y1) {
  int r = nt ? DOCK_H : DOCK_H + DOCK_MARGIN + 6;
  *x0 = 0, *y0 = MENUBAR_H, *x1 = SW, *y1 = SH;
  if (dock_pos == POS_LEFT) *x0 = r;
  else if (dock_pos == POS_RIGHT) *x1 = SW - r;
  else *y1 = SH - r;
}

/* _CLOKE_TITLEBAR (docs/titlebar.md): CARDINAL[6] Stil (TB_*), Höhe der Fläche des Programms, Merkmale, Mitte des ersten
 * Ampelknopfs von links, ausgeblendete Ampelknöpfe, Mitte der Ampelknöpfe von oben. Gesetzt vom AppKit-X11-Backend (macapps/appkit/titlebar.m), nutzbar für jedes Programm. NT-Oberfläche: immer
 * eine Titelleiste von CloakServer. */
/* Ein Fenster, das per _MOTIF_WM_HINTS keinen Rahmen will, zeichnet seine Titelleiste selbst (GTK-Kopfleisten, Raven). Wie bei
 * macOS bekommt es trotzdem Rand, runde Ecken und Schatten von CloakServer (TB_OWN). Ausnahmen: Vollbild, und GTK mit eigenem
 * Schattenrand (_GTK_FRAME_EXTENTS ungleich 0, fremdes Thema; das ClokeOS-Thema nimmt ihn weg). */
static bool own_titlebar_gets_frame(Window w) {
  Atom type, st[8];
  int fmt, n;
  unsigned long cnt, after;
  unsigned char *data = NULL;
  bool ok = true;
  if (XGetWindowProperty(dpy, w, XInternAtom(dpy, "_GTK_FRAME_EXTENTS", False), 0, 4, False, XA_CARDINAL, &type, &fmt, &cnt,
                         &after, &data) == Success && data && cnt == 4) {
    long *v = (long *)data;
    ok = v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0;
  }
  if (data) XFree(data);
  if (ok && get_atom_prop(w, XInternAtom(dpy, "_NET_WM_STATE", False), st, 8, &n))
    for (int i = 0; i < n; i++)
      if (st[i] == XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False)) ok = false;
  return ok;
}

static void read_titlebar(Client *c) {
  c->tstyle = TB_BAR, c->tbh = c->tlx = c->tflags = c->thide = c->toff = c->tly = 0;
  Atom type;
  int fmt;
  unsigned long cnt, after;
  unsigned char *data = NULL;
  if (XGetWindowProperty(dpy, c->win, CLOKE_TITLEBAR, 0, 6, False, XA_CARDINAL, &type, &fmt, &cnt, &after, &data) == Success &&
      data && fmt == 32 && cnt >= 1) {
    long *v = (long *)data;
    if (v[0] >= TB_BAR && v[0] <= TB_OWN) c->tstyle = (int)v[0];
    if (cnt >= 2 && v[1] > 0 && v[1] < 400) c->tbh = (int)v[1];
    if (cnt >= 3) c->tflags = (int)v[2];
    if (cnt >= 4 && v[3] > 0 && v[3] < 400) c->tlx = (int)v[3];
    if (cnt >= 5) c->thide = (int)(v[4] & 7), c->toff = (int)(v[4] >> 3 & 7);
    if (cnt >= 6 && v[5] > 0 && v[5] < 400) c->tly = (int)v[5];
  }
  if (data) XFree(data);
  if (nt || c->draw) c->tstyle = TB_BAR;
  if (!c->tbh) c->tbh = 32;
  if (c->tstyle == TB_OVERLAY && c->thide == 7) c->tstyle = TB_OWN; /* alle Ampelknöpfe ausgeblendet */
  if (c->tstyle != TB_OVERLAY) c->thide = 0;
  if (c->tstyle == TB_OWN) c->toff = 0;
}

/* Ampelknöpfe in Rahmenkoordinaten; bei TB_OVERLAY senkrecht mittig in der Fläche, die das Programm oben zeichnet */
static void light_centers(Client *c, double *cx, double *cy) {
  int x0 = c->tstyle == TB_OVERLAY && c->tlx ? c->tlx : LIGHT_X0;
  for (int i = 0; i < 3; i++) cx[i] = x0 + i * LIGHT_GAP;
  if (c->tstyle == TB_OVERLAY) *cy = c->tly ? c->tly : c->tbh / 2.0;
  else *cy = c->bw + c->th / 2.0;
}

/* Fläche des Fensters über den Ampelknöpfen (TB_OVERLAY), in Rahmenkoordinaten */
static void lights_rect(Client *c, int *x, int *y, int *w, int *h) {
  double cx[3], cy;
  light_centers(c, cx, &cy);
  *x = (int)cx[0] - 10, *y = (int)cy - 10, *w = (int)(cx[2] - cx[0]) + 20, *h = 20;
}

static void paint_lights(cairo_t *cr, Client *c);

static void paint_lights_win(cairo_t *cr, int w, int h, void *arg) {
  (void)w; (void)h;
  Client *c = arg;
  int x, y, lw, lh;
  lights_rect(c, &x, &y, &lw, &lh);
  cairo_translate(cr, -x, -y);
  paint_lights(cr, c);
}

/* TB_OVERLAY: ein Kindfenster des Rahmens über dem Programm. Mit Compositor nur für die Eingabe (das Bild malt
 * cs_window_overlay geglättet über das Fenster), sonst ein auf die drei Kreise zugeschnittenes Fenster. */
static void lights_update(Client *c) {
  if (c->tstyle != TB_OVERLAY) {
    if (c->lights) XDestroyWindow(dpy, c->lights);
    c->lights = None;
    return;
  }
  int x, y, w, h;
  lights_rect(c, &x, &y, &w, &h);
  if (!c->lights) {
    XSetWindowAttributes a = {0};
    a.cursor = cur_arrow;
    a.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | ExposureMask;
    if (comp_on) {
      c->lights = XCreateWindow(dpy, c->frame, x, y, w, h, 0, 0, InputOnly, CopyFromParent, CWCursor | CWEventMask, &a);
    } else {
      c->lights = XCreateWindow(dpy, c->frame, x, y, w, h, 0, CopyFromParent, InputOutput, CopyFromParent,
                                CWCursor | CWEventMask, &a);
      XRectangle r[64];
      int n = 0;
      double cx[3], cy;
      light_centers(c, cx, &cy);
      for (int i = 0; i < 3; i++)
        for (int dy = -LIGHT_R + 1; dy < LIGHT_R - 1 && n < 64; dy++) {
          double yy = dy + .5, hw = sqrt((LIGHT_R - .5) * (LIGHT_R - .5) - yy * yy);
          r[n++] = (XRectangle){(short)lround(cx[i] - x - hw), (short)(cy - y + dy), (unsigned short)lround(2 * hw), 1};
        }
      XShapeCombineRectangles(dpy, c->lights, ShapeBounding, 0, 0, r, n, ShapeSet, Unsorted);
    }
    XMapRaised(dpy, c->lights);
  } else {
    XMoveResizeWindow(dpy, c->lights, x, y, w, h);
    XRaiseWindow(dpy, c->lights);
  }
}

static void apply_geometry(Client *c) {
  int fw = c->w + 2 * c->bw, fh = c->h + c->th + 2 * c->bw;
  XMoveResizeWindow(dpy, c->frame, c->x - c->bw, c->y - c->th - c->bw, fw, fh);
  if (c->uni) {
    /* Programm von der Oberkante an; links oben bleibt ein Ausschnitt für Ampelknöpfe und Zurück/Vor */
    int cut = UNI_CUT, ch = c->h + c->th;
    XMoveResizeWindow(dpy, c->win, c->bw, c->bw, c->w, ch);
    XRectangle r[2] = {{(short)cut, 0, (unsigned short)(c->w > cut ? c->w - cut : 1), (unsigned short)c->th},
                       {0, (short)c->th, (unsigned short)c->w, (unsigned short)c->h}};
    XShapeCombineRectangles(dpy, c->win, ShapeBounding, 0, 0, r, 2, ShapeSet, YXBanded);
    XShapeCombineRectangles(dpy, c->win, ShapeInput, 0, 0, r, 2, ShapeSet, YXBanded);
  } else {
    XMoveResizeWindow(dpy, c->win, c->bw, c->th + c->bw, c->w, c->h);
  }
  if (c->grip) XMoveWindow(dpy, c->grip, fw - 16, fh - 16);
  if (c->bw) shape_round(c->frame, fw, fh, RADIUS, false);
  lights_update(c);
  send_configure(c);
  long ext[4] = {c->bw, c->bw, c->uni ? c->bw : c->th + c->bw, c->bw};
  if (!c->draw) XChangeProperty(dpy, c->win, NET_FRAME_EXTENTS, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)ext, 4);
}

static void place(Client *c, Window transient_for, bool has_pos) {
  int x0, y0, x1, bottom;
  work_area(&x0, &y0, &x1, &bottom);
  int top = y0 + c->th + c->bw + 4;
  if (c->w > x1 - x0 - 2 * c->bw) c->w = x1 - x0 - 2 * c->bw;
  if (c->h > bottom - top) c->h = bottom - top;
  Client *p = transient_for ? find_client(transient_for) : NULL;
  if (p) {
    c->x = p->x + (p->w - c->w) / 2;
    c->y = p->y + (p->h - c->h) / 2;
  } else if (!has_pos || (c->x == 0 && c->y == 0)) {
    /* mittig, jedes neue Fenster etwas weiter rechts unten */
    c->x = (x0 + x1 - c->w) / 2 + (cascade - 2) * 48;
    c->y = top + 16 + cascade * 32;
    cascade = (cascade + 1) % 5;
  }
  if (c->x + c->w > x1) c->x = x1 - c->w - c->bw;
  if (c->x < x0 + c->bw) c->x = x0 + c->bw;
  if (c->y + c->h > bottom) c->y = bottom - c->h;
  if (c->y < top) c->y = top;
}

static void grab_buttons(Client *c, bool focused_now) {
  XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
  if (!focused_now) XGrabButton(dpy, AnyButton, AnyModifier, c->win, False, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
}

static bool window_type_unmanaged(Window w, bool *splash) {
  Atom types[8];
  int n;
  *splash = false;
  if (!get_atom_prop(w, NET_WM_WINDOW_TYPE, types, 8, &n)) return false;
  for (int i = 0; i < n; i++) {
    Atom t = types[i];
    if (t == NET_WM_WINDOW_TYPE_SPLASH) *splash = true;
    if (t == NET_WM_WINDOW_TYPE_DOCK || t == NET_WM_WINDOW_TYPE_DESKTOP || t == NET_WM_WINDOW_TYPE_NOTIFICATION ||
        t == NET_WM_WINDOW_TYPE_TOOLTIP || t == NET_WM_WINDOW_TYPE_MENU || t == NET_WM_WINDOW_TYPE_POPUP_MENU ||
        t == NET_WM_WINDOW_TYPE_DROPDOWN_MENU || t == NET_WM_WINDOW_TYPE_COMBO || t == NET_WM_WINDOW_TYPE_DND ||
        t == NET_WM_WINDOW_TYPE_SPLASH)
      return true;
  }
  return false;
}

static bool wants_no_decoration(Window w) {
  Atom type;
  int fmt;
  unsigned long cnt, after;
  unsigned char *data = NULL;
  bool none = false;
  if (XGetWindowProperty(dpy, w, MOTIF_WM_HINTS, 0, 5, False, AnyPropertyType, &type, &fmt, &cnt, &after, &data) == Success &&
      data && cnt >= 3) {
    long *h = (long *)data;
    if ((h[0] & 2) && h[2] == 0) none = true;
  }
  if (data) XFree(data);
  return none;
}

/* own: ein Fenster von CloakServer selbst (Über ClokeOS, Rückfrage). Das darf nicht in die
 * Save-Set, X antwortet darauf mit BadMatch. */
static Client *manage(Window w, bool already_mapped, bool own) {
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, w, &wa) || wa.override_redirect) return NULL;
  bool splash;
  if (window_type_unmanaged(w, &splash)) {
    if (splash) XMoveWindow(dpy, w, (SW - wa.width) / 2, (SH - wa.height) / 2);
    XMapWindow(dpy, w);
    return NULL;
  }

  Client *c = calloc(1, sizeof *c);
  c->win = w;
  c->app = -1;
  c->x = wa.x;
  c->y = wa.y;
  c->w = wa.width;
  c->h = wa.height;
  /* Titelleiste von CloakServer nur ohne eigene Fläche oben (_CLOKE_TITLEBAR) und ohne _MOTIF_WM_HINTS "keine Rahmen";
   * einen Rand mit runden Ecken und Schatten bekommt auch ein Fenster, das seine Titelleiste selbst zeichnet und es sagt */
  read_titlebar(c);
  if (c->tstyle == TB_BAR && wants_no_decoration(w) && own_titlebar_gets_frame(w)) c->tstyle = TB_OWN;
  c->th = wants_no_decoration(w) || c->tstyle != TB_BAR ? 0 : TITLE_H;
  c->bw = c->th || c->tstyle != TB_BAR ? BORDER : 0;
  c->can_focus = true;
  c->minw = 120;
  c->minh = 40;

  XClassHint ch;
  if (XGetClassHint(dpy, w, &ch)) {
    snprintf(c->cls, sizeof c->cls, "%s", ch.res_class ? ch.res_class : "");
    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
  }
  c->app = app_for_window(w, c->cls);
  c->uni = has_uni(c);
  if (c->uni) c->th = UNI_TH;
  if (c->uni && c->h > c->th + 60) c->h -= c->th; /* c->h zählt ab Unterkante der Titelleiste */
  update_title(c);

  Atom *protos;
  int np;
  if (XGetWMProtocols(dpy, w, &protos, &np)) {
    for (int i = 0; i < np; i++) {
      if (protos[i] == WM_DELETE_WINDOW) c->can_delete = true;
      if (protos[i] == WM_TAKE_FOCUS) c->take_focus = true;
    }
    XFree(protos);
  }
  XWMHints *hints = XGetWMHints(dpy, w);
  if (hints) {
    if ((hints->flags & InputHint) && !hints->input) c->can_focus = false;
    XFree(hints);
  }
  XSizeHints sh;
  long supplied;
  bool has_pos = false;
  if (XGetWMNormalHints(dpy, w, &sh, &supplied)) {
    has_pos = sh.flags & (USPosition | PPosition);
    if (sh.flags & PMinSize) {
      c->minw = sh.min_width > 40 ? sh.min_width : 40;
      c->minh = sh.min_height > 20 ? sh.min_height : 20;
    }
  }
  Window tf = None;
  XGetTransientForHint(dpy, w, &tf);
  place(c, tf, has_pos);

  XSetWindowAttributes sa = {0};
  sa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask |
                  LeaveWindowMask | SubstructureRedirectMask | SubstructureNotifyMask;
  sa.background_pixel = 0;
  sa.cursor = cur_arrow;
  int fw = c->w + 2 * c->bw, fh = c->h + c->th + 2 * c->bw;
  unsigned long vm = CWEventMask | CWBackPixel | CWCursor;
  int depth = CopyFromParent;
  Visual *v = CopyFromParent;
  if (comp_on && wa.depth == 32) { /* Programme mit Durchsicht (GTK zeichnet dann selbst Schatten) */
    sa.colormap = comp_argb_cmap;
    sa.border_pixel = 0;
    vm |= CWColormap | CWBorderPixel;
    depth = 32;
    v = comp_argb;
    c->argb = true;
  }
  c->frame = XCreateWindow(dpy, root, c->x - c->bw, c->y - c->th - c->bw, fw, fh, 0, depth, InputOutput, v, vm, &sa);
  XSetWindowBorderWidth(dpy, w, 0);
  if (!own) XAddToSaveSet(dpy, w);
  if (already_mapped) c->ignore_unmaps++;
  XReparentWindow(dpy, w, c->frame, c->bw, c->uni ? c->bw : c->th + c->bw);
  XSelectInput(dpy, w, PropertyChangeMask);
  if (c->bw && (sh.flags & PMinSize ? !(sh.flags & PMaxSize) || sh.max_width != sh.min_width : true)) {
    XSetWindowAttributes ga = {.cursor = cur_resize};
    c->grip = XCreateWindow(dpy, c->frame, fw - 16, fh - 16, 16, 16, 0, 0, InputOnly, CopyFromParent, CWCursor, &ga);
    XSelectInput(dpy, c->grip, ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    XMapRaised(dpy, c->grip);
  }
  c->next = clients;
  clients = c;
  apply_geometry(c);
  XMapWindow(dpy, w);
  XMapWindow(dpy, c->frame);
  set_wm_state(c, NormalState);
  update_client_list();
  if (comp_on) {
    c->anim = ANIM_OPEN;
    c->anim_t0 = now_s();
    c->anim_p = 0;
  }
  raise_client(c);
  focus_client(c);
  return c;
}

static void unlink_client(Client *c) {
  for (Client **p = &clients; *p; p = &(*p)->next)
    if (*p == c) {
      *p = c->next;
      break;
    }
}

static void unmanage(Client *c, bool destroyed) {
  if (drag == c) drag = NULL;
  if (last_title_client == c) last_title_client = NULL;
  if (!destroyed && !c->draw) {
    XGrabServer(dpy);
    XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
    XReparentWindow(dpy, c->win, root, c->x, c->y);
    XRemoveFromSaveSet(dpy, c->win);
    set_wm_state(c, WithdrawnState);
    XUngrabServer(dpy);
  }
  XDestroyWindow(dpy, c->frame);
  gm_forget(c->win);
  unlink_client(c);
  if (focused == c) {
    focused = NULL;
    Client *n = clients;
    while (n && n->minimized) n = n->next;
    focus_client(n);
  }
  free(c);
  update_client_list();
  draw_dock();
  draw_menubar();
}

static Window lpwin;
static bool lp_open;
static void lp_click(int x, int y);
static void lp_key(XKeyEvent *e);
static void hide_launchpad(void);
static void draw_launchpad(void);

static void keep_ui_on_top(void) {
  if (lp_open) XRaiseWindow(dpy, lpwin);
  XRaiseWindow(dpy, dock);
  if (!lp_open) XRaiseWindow(dpy, menubar);
  if (menuwin) XRaiseWindow(dpy, menuwin);
  if (locked && loginwin) XRaiseWindow(dpy, loginwin);
  if (setup_active && setupwin) XRaiseWindow(dpy, setupwin);
}

static void raise_client(Client *c) {
  if (!c) return;
  unlink_client(c);
  c->next = clients;
  clients = c;
  XRaiseWindow(dpy, c->frame);
  keep_ui_on_top();
}

static void focus_client(Client *c) {
  Client *old = focused;
  if (c && c->minimized) c = NULL;
  focused = c;
  if (c) {
    if (c->can_focus) XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    else XSetInputFocus(dpy, c->frame, RevertToPointerRoot, CurrentTime);
    if (c->take_focus) {
      XEvent ev = {0};
      ev.xclient.type = ClientMessage;
      ev.xclient.window = c->win;
      ev.xclient.message_type = WM_PROTOCOLS;
      ev.xclient.format = 32;
      ev.xclient.data.l[0] = WM_TAKE_FOCUS;
      ev.xclient.data.l[1] = CurrentTime;
      XSendEvent(dpy, c->win, False, NoEventMask, &ev);
    }
    XChangeProperty(dpy, root, NET_ACTIVE_WINDOW, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&c->win, 1);
  } else {
    XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    Window none = None;
    XChangeProperty(dpy, root, NET_ACTIVE_WINDOW, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&none, 1);
  }
  for (Client *x = clients; x; x = x->next) grab_buttons(x, x == c);
  if (old && old != c) draw_frame(old);
  if (c) draw_frame(c);
  gm_focus(c);
  draw_menubar();
}

static void close_client(Client *c) {
  if (!c) return;
  if (c->draw) {
    XUnmapWindow(dpy, c->win);
    XDestroyWindow(dpy, c->win);
    unmanage(c, true);
    return;
  }
  if (c->can_delete) {
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = c->win;
    ev.xclient.message_type = WM_PROTOCOLS;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = WM_DELETE_WINDOW;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, c->win, False, NoEventMask, &ev);
  } else {
    XKillClient(dpy, c->win);
  }
}

static void minimize_client(Client *c) {
  if (!c || c->minimized) return;
  c->minimized = true;
  if (comp_on) { /* erst ins Dock fliegen lassen, ausgeblendet wird am Ende von animate() */
    c->anim = ANIM_MIN;
    c->anim_t0 = now_s();
    c->anim_p = 0;
  } else {
    XUnmapWindow(dpy, c->frame);
  }
  set_wm_state(c, IconicState);
  if (focused == c) {
    Client *n = clients;
    while (n && (n->minimized || n == c)) n = n->next;
    focus_client(n);
  }
  draw_dock();
}

static void restore_client(Client *c) {
  if (!c) return;
  if (c->minimized) {
    c->minimized = false;
    XMapWindow(dpy, c->frame);
    set_wm_state(c, NormalState);
    if (comp_on) {
      c->anim = ANIM_RESTORE;
      c->anim_t0 = now_s();
    c->anim_p = 0;
    }
  }
  raise_client(c);
  focus_client(c);
}

static void zoom_client(Client *c) {
  if (!c || !c->bw) return;
  if (c->zoomed) {
    c->x = c->zx; c->y = c->zy; c->w = c->zw; c->h = c->zh;
    c->zoomed = false;
  } else {
    int x0, y0, x1, y1;
    work_area(&x0, &y0, &x1, &y1);
    c->zx = c->x; c->zy = c->y; c->zw = c->w; c->zh = c->h;
    c->x = x0 + c->bw + 4;
    c->y = y0 + 4 + c->th + c->bw;
    c->w = x1 - x0 - 2 * c->bw - 8;
    c->h = y1 - c->y - c->bw - 2;
    c->zoomed = true;
  }
  apply_geometry(c);
  draw_frame(c);
}

/* ---------------------------------------------------------------------------------------- */
/* Rahmen                                                                                     */

/* NT: Minimieren, Maximieren, Schließen rechts, je 46 Pixel breit; i = 1 schließen, 2 ins Dock, 3 zoomen */
static int nt_btn_x(Client *c, int i, int w) { return w - c->bw - 46 * (i == 1 ? 1 : i == 3 ? 2 : 3); }

static int light_at(Client *c, int x, int y) {
  if (nt) {
    if (y < c->bw || y >= c->bw + c->th) return 0;
    int w = c->w + 2 * c->bw;
    for (int i = 1; i <= 3; i++)
      if (x >= nt_btn_x(c, i, w) && x < nt_btn_x(c, i, w) + 46) return i;
    return 0;
  }
  double cx[3], cy;
  light_centers(c, cx, &cy);
  for (int i = 0; i < 3; i++)
    if (!((c->thide | c->toff) & (1 << i)) && fabs(x - cx[i]) <= LIGHT_R + 2 && fabs(y - cy) <= LIGHT_R + 2) return i + 1;
  return 0;
}

/* Zurück und Vor neben den Ampelknöpfen (wie in clokeos.html: .tb-nav). Fundus und Search tragen sie;
 * ein Klick schickt dem Programm Alt+Links beziehungsweise Alt+Rechts, das Thunar und NetSurf als
 * Zurück und Vor kennen. */
static bool app_is(Client *c, const char *id) { return c && c->th && c->app >= 0 && !strcmp(apps[c->app].b.ident, id); }

/* Zurück und Vor: Fundus, Search, Kalender (Monat) und Notizen (vorige/nächste Notiz) */
static bool has_nav(Client *c) {
  return app_is(c, "com.clokeos.fundus") || app_is(c, "com.clokeos.search") || app_is(c, "com.clokeos.kalender") ||
         app_is(c, "com.clokeos.notizen");
}

/* Titelleiste und Werkzeugleiste des Programms bilden eine Fläche (macOS): auch Aktivitätsanzeige */
static bool has_uni(Client *c) { return !nt && (has_nav(c) || app_is(c, "com.clokeos.aktivitaet")); }

static void nav_rect(Client *c, double *x, double *y, double *w, double *h) {
  double cx[3], cy;
  light_centers(c, cx, &cy);
  if (nt) cx[2] = c->bw - 14; /* NT: Zurück/Vor ganz links */
  *x = cx[2] + 22;
  *y = cy - 12;
  *w = 62;
  *h = 24;
}

static int nav_at(Client *c, int x, int y) {
  if (!has_nav(c)) return 0;
  double nx, ny, nw, nh;
  nav_rect(c, &nx, &ny, &nw, &nh);
  if (x < nx || x > nx + nw || y < ny || y > ny + nh) return 0;
  return x < nx + nw / 2 ? 1 : 2;
}

static void send_nav_key(Client *c, int which) {
  if (app_is(c, "com.clokeos.kalender") || app_is(c, "com.clokeos.notizen")) { /* eigene Programme: Eigenschaft statt Taste */
    long v = which;
    XChangeProperty(dpy, c->win, XInternAtom(dpy, "_CLOKEOS_NAV", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
    XFlush(dpy);
    return;
  }
  KeySym ks = which == 1 ? XK_Left : XK_Right;
  XKeyEvent ev = {.type = KeyPress, .display = dpy, .window = c->win, .root = root, .subwindow = None,
                  .time = CurrentTime, .same_screen = True, .keycode = XKeysymToKeycode(dpy, ks), .state = Mod1Mask};
  if (!ev.keycode) return;
  XSendEvent(dpy, c->win, True, KeyPressMask, (XEvent *)&ev);
  ev.type = KeyRelease;
  XSendEvent(dpy, c->win, True, KeyReleaseMask, (XEvent *)&ev);
  XFlush(dpy);
}

/* Fensterrahmen der NT-Oberfläche: eckig, Titel links, rechts Minimieren, Maximieren, Schließen */
static void paint_frame_nt(cairo_t *cr, int w, int h, Client *c, bool active) {
  if (active) cairo_set_source_rgb(cr, ACCENT.r, ACCENT.g, ACCENT.b);
  else cairo_set_source_rgb(cr, dark ? .24 : .72, dark ? .24 : .72, dark ? .27 : .75);
  cairo_paint(cr);
  set(cr, T->win);
  cairo_rectangle(cr, c->bw, c->bw, w - 2 * c->bw, h - 2 * c->bw);
  cairo_fill(cr);
  set(cr, T->line);
  cairo_rectangle(cr, c->bw, c->bw + c->th - 1, w - 2 * c->bw, 1);
  cairo_fill(cr);
  double cy = c->bw + c->th / 2.0;
  for (int i = 1; i <= 3; i++) { /* Schaltflächen */
    double bx = nt_btn_x(c, i, w), s = 5;
    bool hov = c->hover == i;
    if (hov) {
      if (i == 1) cairo_set_source_rgb(cr, .77, .17, .11);
      else set(cr, T->hover);
      cairo_rectangle(cr, bx, c->bw, 46, c->th - 1);
      cairo_fill(cr);
    }
    if (i == 1 && hov) cairo_set_source_rgb(cr, 1, 1, 1);
    else set(cr, active ? T->fg : T->fg2);
    cairo_set_line_width(cr, 1);
    double mx = bx + 23;
    if (i == 1) {
      cairo_move_to(cr, mx - s, cy - s); cairo_line_to(cr, mx + s, cy + s);
      cairo_move_to(cr, mx + s, cy - s); cairo_line_to(cr, mx - s, cy + s);
    } else if (i == 2) {
      cairo_move_to(cr, mx - s, cy + .5); cairo_line_to(cr, mx + s, cy + .5);
    } else {
      cairo_rectangle(cr, mx - s + .5, cy - s + .5, 2 * s - 1, 2 * s - 1);
    }
    cairo_stroke(cr);
  }
  double tx = c->bw + 12; /* NT: Zurück/Vor liegen in der Werkzeugleiste des Programms selbst */
  const char *title = c->title[0] ? c->title : (c->app >= 0 ? apps[c->app].b.name : c->cls);
  int room = nt_btn_x(c, 2, w) - tx - 8;
  if (!c->uni && room > 24) text(cr, tx, cy, title, 12, false, active ? T->fg : T->fg2, room, 0);
}

/* Farbe der obersten Pixelzeile des Programmfensters (Mitte links), für transparente Titelleisten */
static bool app_top_color(Client *c, RGBA *out) {
  if (c->w < 40 || c->h < 4 || c->minimized) return false;
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, c->win, &wa) || wa.map_state != IsViewable) return false;
  XImage *im = XGetImage(dpy, c->win, 24, 1, 1, 1, AllPlanes, ZPixmap);
  if (!im) return false;
  unsigned long p = XGetPixel(im, 0, 0);
  XDestroyImage(im);
  *out = (RGBA){(p >> 16 & 255) / 255., (p >> 8 & 255) / 255., (p & 255) / 255., 1};
  return (p & 0xffffff) != 0; /* noch nichts gezeichnet (schwarz): normaler Grund */
}

/* Ampelknöpfe (Schließen, Ins Dock, Zoomen) in Rahmenkoordinaten; mit Symbolen, solange der Zeiger darüber ist */
static void paint_lights(cairo_t *cr, Client *c) {
  bool active = c == focused;
  double cx[3], cy;
  light_centers(c, cx, &cy);
  const RGBA lc[3] = {L_CLOSE, L_MIN, L_ZOOM};
  bool show = active || c->hover;
  for (int i = 0; i < 3; i++) {
    if (c->thide & (1 << i)) continue; /* vom Programm ausgeblendet (standardWindowButton: … setHidden:) */
    cairo_new_path(cr);
    cairo_arc(cr, cx[i], cy, LIGHT_R - .25, 0, 2 * M_PI);
    bool off = c->toff & (1 << i); /* abgeschaltet (z. B. Fenster nicht zoombar): grau wie bei macOS */
    set(cr, show && !off ? lc[i] : T->light_off);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, .12);
    cairo_set_line_width(cr, .5);
    cairo_stroke(cr);
    if (c->hover && !off) { /* Symbole: × − + */
      cairo_set_source_rgba(cr, 0, 0, 0, .55);
      cairo_set_line_width(cr, 1.2);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      double s = 3;
      if (i == 0) {
        cairo_move_to(cr, cx[i] - s, cy - s); cairo_line_to(cr, cx[i] + s, cy + s);
        cairo_move_to(cr, cx[i] + s, cy - s); cairo_line_to(cr, cx[i] - s, cy + s);
      } else {
        cairo_move_to(cr, cx[i] - s - .4, cy); cairo_line_to(cr, cx[i] + s + .4, cy);
        if (i == 2) { cairo_move_to(cr, cx[i], cy - s - .4); cairo_line_to(cr, cx[i], cy + s + .4); }
      }
      cairo_stroke(cr);
    }
  }
}

static void paint_frame(cairo_t *cr, int w, int h, void *arg) {
  Client *c = arg;
  bool active = c == focused;
  if (nt) {
    paint_frame_nt(cr, w, h, c, active);
    return;
  }
  /* Kante (--edge) als 1-Pixel-Rand, darin der Fenstergrund */
  cairo_set_source_rgb(cr, dark ? .06 : .78, dark ? .055 : .76, dark ? .075 : .83);
  cairo_paint(cr);
  set(cr, T->win);
  rrect(cr, c->bw, c->bw, w - 2 * c->bw, h - 2 * c->bw, RADIUS - 1);
  cairo_fill(cr);
  /* Transparente oder "unified" Titelleiste (_CLOKE_TITLEBAR Merkmale 2/16, z. B. Logic Pro): Titelzeile in der Farbe der
   * Oberkante des Programms und ohne Linie, Titelzeile und Werkzeugleiste sind eine Fläche wie bei macOS */
  bool blend = c->th && (c->tflags & (2 | 16));
  if (blend) {
    RGBA top;
    if (app_top_color(c, &top)) {
      set(cr, top);
      cairo_save(cr);
      rrect(cr, c->bw, c->bw, w - 2 * c->bw, h - 2 * c->bw, RADIUS - 1);
      cairo_clip(cr);
      cairo_rectangle(cr, c->bw, c->bw, w - 2 * c->bw, c->th);
      cairo_fill(cr);
      cairo_restore(cr);
    }
  }
  /* Linie unter der Titelleiste; bei Fundus und Search fehlt sie: Titelleiste und Werkzeugleiste des
   * Programms bilden eine Fläche wie beim Finder und bei Safari */
  if (c->th && !has_uni(c) && !blend) {
    set(cr, T->line);
    cairo_rectangle(cr, c->bw, c->bw + c->th - 1, w - 2 * c->bw, 1);
    cairo_fill(cr);
  }
  /* Glanzkante oben */
  cairo_save(cr);
  rrect(cr, 0, 0, w, h, RADIUS);
  cairo_clip(cr);
  RGBA rimc = T->rim;
  cairo_set_source_rgba(cr, rimc.r, rimc.g, rimc.b, dark ? rimc.a * .6 : rimc.a * .8);
  cairo_rectangle(cr, RADIUS, c->bw, w - 2 * RADIUS, 1);
  cairo_fill(cr);
  cairo_restore(cr);

  if (c->th) paint_lights(cr, c);
  int side = 84;
  if (has_nav(c)) {
    double nx, ny, nw, nh;
    nav_rect(c, &nx, &ny, &nw, &nh);
    rrect(cr, nx, ny, nw, nh, nh / 2);
    set(cr, T->pill.a > .3 ? (RGBA){1, 1, 1, .7} : (RGBA){1, 1, 1, .1});
    cairo_fill_preserve(cr);
    set(cr, T->line);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    set(cr, active ? T->fg2 : T->fg3);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    double mx = nx + nw / 4, my = ny + nh / 2;
    cairo_move_to(cr, mx + 2.5, my - 5); cairo_line_to(cr, mx - 2.5, my); cairo_line_to(cr, mx + 2.5, my + 5);
    cairo_stroke(cr);
    mx = nx + nw * 3 / 4;
    cairo_move_to(cr, mx - 2.5, my - 5); cairo_line_to(cr, mx + 2.5, my); cairo_line_to(cr, mx - 2.5, my + 5);
    cairo_stroke(cr);
    side = 150;
  }
  const char *title = c->title[0] ? c->title : (c->app >= 0 ? apps[c->app].b.name : c->cls);
  char short_title[256];
  if (has_nav(c)) { /* Thunar und NetSurf hängen ihren Namen an: "gast - Thunar" wird "gast" */
    snprintf(short_title, sizeof short_title, "%s", title);
    char *dash = strstr(short_title, " - Thunar");
    if (!dash) dash = strstr(short_title, " - NetSurf");
    if (dash && dash != short_title) *dash = 0;
    title = short_title;
  }
  /* Titel fett, 13 pt; wie bei macOS 27 links neben den Ampelknöpfen (Fenster mit Zurück/Vor: mittig) */
  double cy = c->bw + c->th / 2.0, tx = LIGHT_X0 + 2 * LIGHT_GAP + LIGHT_R + 16;
  if (c->th && !c->uni && !(c->tflags & 4)) {
    if (has_nav(c) || blend || (c->tflags & 8)) { /* mit Werkzeugleiste darunter mittig, wie bei macOS */
      if (w - 2 * side > 24) text(cr, w / 2.0, cy, title, 13, true, active ? T->fg : T->fg2, w - 2 * side, 1);
    } else if (w - tx - 16 > 24) {
      text(cr, tx, cy, title, 13, true, active ? T->fg : T->fg2, w - tx - 16, 0);
    }
  }
}

static void draw_frame(Client *c) {
  if (!c || !c->bw) return;
  paint_window(c->frame, c->w + 2 * c->bw, c->h + c->th + 2 * c->bw, paint_frame, c);
  if (c->lights) { /* Ampelknöpfe über dem Programm neu */
    int x, y, w, h;
    lights_rect(c, &x, &y, &w, &h);
    if (comp_on) comp_dirty(c->x - c->bw + x - 2, c->y - c->th - c->bw + y - 2, w + 4, h + 4);
    else paint_window(c->lights, w, h, paint_lights_win, c);
  }
}

/* Compositor (comp.c): nach dem Bild eines Rahmens; malt bei TB_OVERLAY die Ampelknöpfe geglättet über das Programm */
void cs_window_overlay(Window frame, cairo_t *cr, double alpha) {
  for (Client *c = clients; c; c = c->next)
    if (c->frame == frame) {
      if (!c->lights) return;
      cairo_push_group(cr);
      paint_lights(cr, c);
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, alpha);
      return;
    }
}

/* Das Programm hat _CLOKE_TITLEBAR geändert: Titelleiste neu einteilen, die Oberkante des Rahmens bleibt stehen */
static void titlebar_changed(Client *c) {
  int ts = c->tstyle, tbh = c->tbh, tlx = c->tlx, tf = c->tflags, hide = c->thide, off = c->toff, tly = c->tly;
  read_titlebar(c);
  if (c->uni) return;
  if (ts == c->tstyle && tbh == c->tbh && tlx == c->tlx && tly == c->tly) {
    if (tf != c->tflags || hide != c->thide || off != c->toff) draw_frame(c);
    return;
  }
  int top = c->y - c->th - c->bw;
  c->th = wants_no_decoration(c->win) || c->tstyle != TB_BAR ? 0 : TITLE_H;
  c->bw = c->th || c->tstyle != TB_BAR ? BORDER : 0;
  c->y = top + c->th + c->bw;
  apply_geometry(c);
  draw_frame(c);
}

/* ---------------------------------------------------------------------------------------- */
/* Eigene Fenster: Über ClokeOS, Rückfragen                                                   */

static char sysinfo[10][2][96];
static int nsysinfo;
static char os_version[32] = "2.0", os_codename[32] = "Presidio", os_build[16] = "2A17";
static char os_login[8] = "ja"; /* LOGIN in /etc/clokeos-release: "nein" startet ohne Anmeldung */

static void read_release(void) {
  char *s = read_file("/etc/clokeos-release");
  if (!s) return;
  struct { const char *k; char *v; size_t n; } keys[] = {
    {"VERSION=\"", os_version, sizeof os_version}, {"CODENAME=\"", os_codename, sizeof os_codename},
    {"BUILD=\"", os_build, sizeof os_build}, {"LOGIN=\"", os_login, sizeof os_login}};
  for (int i = 0; i < 4; i++) {
    char *p = strstr(s, keys[i].k);
    if (!p) continue;
    p += strlen(keys[i].k);
    char *e = strchr(p, '"');
    if (e) snprintf(keys[i].v, keys[i].n, "%.*s", (int)(e - p), p);
  }
  free(s);
}

static void add_info(const char *k, const char *v) {
  if (nsysinfo >= 10) return;
  snprintf(sysinfo[nsysinfo][0], 96, "%s", k);
  snprintf(sysinfo[nsysinfo][1], 96, "%s", v);
  nsysinfo++;
}

static void collect_sysinfo(void) {
  nsysinfo = 0;
  struct utsname u;
  char buf[128];
  if (!uname(&u)) {
    snprintf(buf, sizeof buf, "Darwin %s (XNU)", u.release);
    add_info(L("Kernel"), buf);
    add_info(L("Architecture"), u.machine);
  }
  char cpu[128] = "";
  size_t n = sizeof cpu;
  int ncpu = 0;
  size_t ni = sizeof ncpu;
  sysctlbyname("machdep.cpu.brand_string", cpu, &n, NULL, 0);
  sysctlbyname("hw.ncpu", &ncpu, &ni, NULL, 0);
  char *p = cpu;
  while (*p == ' ') p++;
  if (*p) add_info(L("Processor"), p);
  if (ncpu) {
    snprintf(buf, sizeof buf, "%d", ncpu);
    add_info(L("Cores"), buf);
  }
  uint64_t mem = 0;
  size_t nm = sizeof mem;
  if (!sysctlbyname("hw.memsize", &mem, &nm, NULL, 0) && mem) {
    snprintf(buf, sizeof buf, "%.0f GB", mem / 1073741824.0);
    add_info(L("Memory"), buf);
  }
  snprintf(buf, sizeof buf, "%d × %d", SW, SH);
  add_info(L("Display"), buf);
  char host[128] = "";
  gethostname(host, sizeof host);
  add_info(L("Computer Name"), host);
  add_info("Build", os_build);
}

static cairo_surface_t *logo_big;

static void paint_about(Client *c, cairo_t *cr) {
  int w = c->w;
  if (!logo_big) logo_big = svg_render("logo-gradient.svg", 96, 96, NULL);
  if (logo_big) {
    cairo_set_source_surface(cr, logo_big, (w - 96) / 2.0, 20);
    cairo_paint(cr);
  }
  text(cr, w / 2.0, 140, "ClokeOS", 22, true, T->fg, 0, 1);
  char ver[96];
  snprintf(ver, sizeof ver, L("“%s” · Version %s"), os_codename, os_version);
  text(cr, w / 2.0, 166, ver, 12.5, false, T->fg2, 0, 1);
  int y = 196;
  for (int i = 0; i < nsysinfo; i++, y += 19) {
    text(cr, w * .36, y, sysinfo[i][0], 11.5, true, T->fg, 0, 2);
    text(cr, w * .36 + 12, y, sysinfo[i][1], 11.5, false, T->fg2, w * .64 - 30, 0);
  }
  /* Knopf "Weitere Informationen …" */
  const char *b = L("More Info…");
  int bwid = text_width(b, 12, false) + 28;
  double bx = (w - bwid) / 2.0, by = c->h - 46;
  rrect(cr, bx, by, bwid, 26, 13);
  set(cr, T->pill.a > .3 ? (RGBA){1, 1, 1, .9} : (RGBA){1, 1, 1, .12});
  cairo_fill_preserve(cr);
  set(cr, T->line_strong);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  text(cr, w / 2.0, by + 13, b, 12, false, T->fg, 0, 1);
}

static void click_about(Client *c, int x, int y) {
  if (y >= c->h - 48 && y <= c->h - 18 && fabs(x - c->w / 2.0) < 110) launch_ident("com.clokeos.systemeinstellungen");
}

static void draw_own(Client *c);

/* Rückfragen vor Neustart (data 1), Ausschalten (0) und Neustart in die Wiederherstellung (2);
 * 3: die Startplatte hat keine Wiederherstellung (nur eine Meldung) */
static const char *confirm_ok(long data) {
  return data == 1 || data == 2 ? L("Restart") : data == 3 ? "OK" : L("Shut Down");
}

static void paint_confirm(Client *c, cairo_t *cr) {
  cairo_surface_t *lg = svg_render("logo-gradient.svg", 56, 56, NULL);
  if (lg) {
    cairo_set_source_surface(cr, lg, 22, 22);
    cairo_paint(cr);
    cairo_surface_destroy(lg);
  }
  static const char *const head[] = {"Are you sure you want to shut down your computer now?",
                                     "Are you sure you want to restart your computer now?", "Restart in Recovery now?",
                                     "No Recovery Found"};
  static const char *const sub[] = {"Unsaved changes in apps will be lost.", "Unsaved changes in apps will be lost.",
                                    "Only this once, then ClokeOS starts again.",
                                    "The startup disk has no recovery partition."};
  int k = c->data >= 0 && c->data <= 3 ? (int)c->data : 0;
  text(cr, 94, 40, L(head[k]), 13, true, T->fg, c->w - 110, 0);
  text(cr, 94, 64, L(sub[k]), 11.5, false, T->fg2, c->w - 110, 0);
  if (c->data == 3) { /* nur OK */
    const char *ok = "OK";
    int w2 = text_width(ok, 12.5, false) + 40;
    double y = c->h - 44, x2 = c->w - 20 - w2;
    rrect(cr, x2, y, w2, 28, 14);
    set(cr, ACCENT);
    cairo_fill(cr);
    text(cr, x2 + w2 / 2.0, y + 14, ok, 12.5, false, (RGBA){1, 1, 1, 1}, 0, 1);
    return;
  }
  const char *ok = confirm_ok(c->data);
  int w2 = text_width(ok, 12.5, false) + 32, w1 = text_width(L("Cancel"), 12.5, false) + 32;
  double y = c->h - 44, x2 = c->w - 20 - w2, x1 = x2 - 10 - w1;
  rrect(cr, x1, y, w1, 28, 14);
  set(cr, dark ? (RGBA){1, 1, 1, .12} : (RGBA){1, 1, 1, .95});
  cairo_fill_preserve(cr);
  set(cr, T->line_strong);
  cairo_stroke(cr);
  text(cr, x1 + w1 / 2.0, y + 14, L("Cancel"), 12.5, false, T->fg, 0, 1);
  rrect(cr, x2, y, w2, 28, 14);
  set(cr, ACCENT);
  cairo_fill(cr);
  text(cr, x2 + w2 / 2.0, y + 14, ok, 12.5, false, (RGBA){1, 1, 1, 1}, 0, 1);
}

static void click_confirm(Client *c, int x, int y) {
  const char *ok = confirm_ok(c->data);
  int w2 = text_width(ok, 12.5, false) + (c->data == 3 ? 40 : 32), w1 = text_width(L("Cancel"), 12.5, false) + 32;
  double by = c->h - 44, x2 = c->w - 20 - w2, x1 = x2 - 10 - w1;
  if (y < by || y > by + 28) return;
  if (x >= x2 && x <= x2 + w2) {
    if (c->data == 3) {
      close_client(c);
      return;
    }
    if (c->data == 2) {
      /* Boot-Argumente der EFI-Partition einmal auf die Wiederherstellung stellen; die stellt sie
       * beim Start zurück (recovery/overlay/usr/libexec/clokeos-recovery-boot) */
      int st = run_wait("/usr/libexec/clokeos-installer bootnext recovery >/dev/null 2>&1");
      if (st != 0) {
        c->data = 3;
        draw_own(c);
        return;
      }
    }
    XSync(dpy, False);
    spawn_as(c->data == 0 ? "/bin/poweroff" : "/bin/reboot", true);
  } else if (c->data != 3 && x >= x1 && x <= x1 + w1) {
    close_client(c);
  }
}

static void paint_own(cairo_t *cr, int w, int h, void *arg) {
  (void)w; (void)h;
  Client *c = arg;
  set(cr, T->win);
  cairo_paint(cr);
  c->draw(c, cr);
}

static void draw_own(Client *c) { paint_window(c->win, c->w, c->h, paint_own, c); }

static Client *own_window(int w, int h, const char *title, void (*draw)(Client *, cairo_t *),
                          void (*click)(Client *, int, int), long data) {
  for (Client *c = clients; c; c = c->next)
    if (c->draw == draw) {
      c->data = data;
      restore_client(c);
      draw_own(c);
      return c;
    }
  XSetWindowAttributes sa = {0};
  sa.event_mask = ExposureMask | ButtonPressMask;
  Window win = XCreateWindow(dpy, root, (SW - w) / 2, (SH - h) / 2 - 30, w, h, 0, CopyFromParent, InputOutput,
                             CopyFromParent, CWEventMask, &sa);
  const char *title_l = L(title);
  XStoreName(dpy, win, title_l);
  XChangeProperty(dpy, win, NET_WM_NAME, UTF8_STRING, 8, PropModeReplace, (unsigned char *)title_l, strlen(title_l));
  XClassHint ch = {.res_name = "cloakserver", .res_class = "CloakServer"};
  XSetClassHint(dpy, win, &ch);
  XSizeHints sh = {.flags = PPosition | PMinSize | PMaxSize, .min_width = w, .max_width = w, .min_height = h, .max_height = h};
  XSetWMNormalHints(dpy, win, &sh);
  Client *c = manage(win, false, true);
  if (c) {
    c->title_en = title;
    c->draw = draw;
    c->click = click;
    c->data = data;
    c->can_delete = false;
    /* manage() hat die Ereignismaske auf PropertyChange gesetzt; eigene Fenster zeichnen selbst. */
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask | PropertyChangeMask);
    update_client_list();
    draw_frame(c);
    draw_own(c);
    draw_menubar(); /* jetzt mit c->draw: Name "ClokeOS" statt der Fensterklasse */
  }
  return c;
}

static void show_about(long unused) {
  (void)unused;
  collect_sysinfo();
  own_window(420, 420, "About ClokeOS", paint_about, click_about, 0);
}

static void confirm_power(long reboot) {
  own_window(500, 140, reboot == 2 ? "Recovery" : reboot ? "Restart" : "Shut Down", paint_confirm,
             click_confirm, reboot);
}

#include "recovery.c"
#include "diskutil.c"

/* ---------------------------------------------------------------------------------------- */
/* Konten und Sitzung. Wie im Prototyp gibt es den lokalen Benutzer gast (/Users/gast), dazu   */
/* root („System Administrator“) für Verwaltungsaufgaben, denn PureDarwin hat kein su und kein */
/* sudo. CloakServer läuft als root wie loginwindow bei macOS und startet die Programme als    */
/* der angemeldete Benutzer, mit eigenem D-Bus für die Sitzung. Beim Abmelden beendet er alle  */
/* Prozesse des Benutzers.                                                                     */

static Account accounts[8], session_acc;
static int naccounts, sel_account; /* Konten auf der Anmeldung, das ausgewählte */

static void add_account(const struct passwd *p) {
  if (naccounts >= 8) return;
  Account *a = &accounts[naccounts++];
  a->uid = p->pw_uid;
  a->gid = p->pw_gid;
  snprintf(a->name, sizeof a->name, "%s", p->pw_name);
  snprintf(a->full, sizeof a->full, "%s", p->pw_gecos && p->pw_gecos[0] ? p->pw_gecos : p->pw_name);
  snprintf(a->home, sizeof a->home, "%s", p->pw_dir ? p->pw_dir : "/");
  snprintf(a->shell, sizeof a->shell, "%s", p->pw_shell ? p->pw_shell : "/bin/sh");
  snprintf(a->passwd, sizeof a->passwd, "%s", p->pw_passwd ? p->pw_passwd : "*");
}

/* Benutzer ab UID 500 wie bei macOS, mit einer Shell zum Anmelden; root zuletzt */
static void load_accounts(void) {
  naccounts = 0;
  setpwent();
  for (struct passwd *p; (p = getpwent());)
    if (p->pw_uid >= 500 && p->pw_uid < 60000 && p->pw_shell && !strstr(p->pw_shell, "false") &&
        !strstr(p->pw_shell, "nologin"))
      add_account(p);
  endpwent();
  struct passwd *r = getpwuid(0);
  if (r) {
    add_account(r);
  } else {
    struct passwd fb = {.pw_name = "root", .pw_passwd = "*", .pw_gecos = "System Administrator",
                        .pw_dir = "/var/root", .pw_shell = "/bin/zsh"};
    add_account(&fb);
  }
  if (sel_account >= naccounts) sel_account = 0;
}

/* PureDarwin setzt root kein Passwort ("*"), gast hat ein leeres Feld: Dann genügt die
 * Eingabetaste. Ist mit passwd eines gesetzt, prüft CloakServer es mit crypt(). */
static bool account_has_password(const Account *a) {
  return a->passwd[0] && strcmp(a->passwd, "*") && a->passwd[0] != '!';
}

/* Anzeigename: Gast und root heißen wie bei macOS in jeder Sprache passend */
static const char *display_name(const Account *a) {
  if (!strcmp(a->full, "Guest") || !strcmp(a->full, "System Administrator")) return L(a->full);
  return a->full;
}

static const char *session_name(void) { return session ? display_name(session) : L("System Administrator"); }

/* Umgebung der Sitzung; die von cloakserver-session (root) kommt beim Abmelden zurück */
static const char *const SESSION_ENV[] = {"HOME", "USER", "LOGNAME", "SHELL", "XDG_RUNTIME_DIR",
                                          "DBUS_SESSION_BUS_ADDRESS", "XUSERFILESEARCHPATH"};
#define NSESSION_ENV ((int)(sizeof SESSION_ENV / sizeof SESSION_ENV[0]))
static char *root_env[NSESSION_ENV];

static void save_root_env(void) {
  for (int i = 0; i < NSESSION_ENV; i++) {
    const char *v = getenv(SESSION_ENV[i]);
    root_env[i] = v ? strdup(v) : NULL;
  }
}

static void restore_root_env(void) {
  for (int i = 0; i < NSESSION_ENV; i++) {
    if (root_env[i]) setenv(SESSION_ENV[i], root_env[i], 1);
    else unsetenv(SESSION_ENV[i]);
  }
}

/* D-Bus für die Sitzung, gestartet als der Benutzer: dbus-daemon geht in den Hintergrund und gibt
 * vorher die Adresse aus (wie dbus-launch, das dasselbe tut; PureDarwins dbus-launch startet auf
 * arm64 nicht, es sucht xcb_parse_display im flachen Namensraum). */
static void start_session_bus(const Account *a) {
  int fd[2];
  if (access("/bin/dbus-daemon", X_OK) || pipe(fd)) return;
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    signal(SIGCHLD, SIG_DFL);
    /* Adresse auf fd 3: als Dienst lenkt dbus-daemon 0 bis 2 auf /dev/null um */
    close(fd[0]);
    if (fd[1] != 3) {
      dup2(fd[1], 3);
      close(fd[1]);
    }
    if (!become(a)) _exit(126);
    if (chdir(a->home)) { /* dann eben im aktuellen Ordner */ }
    execl("/bin/dbus-daemon", "dbus-daemon", "--session", "--fork", "--nopidfile", "--print-address=3", (char *)NULL);
    _exit(127);
  }
  close(fd[1]);
  char out[1024];
  size_t n = 0;
  double t0 = now_s();
  while (pid > 0 && n < sizeof out - 1 && now_s() - t0 < 20) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd[0], &r);
    struct timeval tv = {1, 0};
    if (select(fd[0] + 1, &r, NULL, NULL, &tv) <= 0) continue;
    ssize_t k = read(fd[0], out + n, sizeof out - 1 - n);
    if (k <= 0) break;
    n += k;
    if (memchr(out, '\n', n)) break; /* der Dienst hält die Leitung womöglich offen */
  }
  close(fd[0]);
  out[n] = 0;
  char *v = strstr(out, "unix:"); /* eine Zeile wie unix:path=/tmp/dbus-…,guid=… */
  if (v) {
    v[strcspn(v, "\n")] = 0;
    setenv("DBUS_SESSION_BUS_ADDRESS", v, 1);
  } else {
    warn("kein D-Bus für %s", a->name);
  }
}

static void user_env(const Account *a);

static void start_session(const Account *a) {
  session_acc = *a;
  session = &session_acc;
  warn("angemeldet: %s (UID %u)", a->name, (unsigned)a->uid);
  if (a->uid != 0) user_env(a); /* root behält die Umgebung von cloakserver-session */
  /* ~/Applications und die Einstellungen des Benutzers (Dock, Erscheinungsbild) */
  scan_apps();
  load_prefs();
  render_icons();
  if (want_dark() != dark || wall_sel || nt) apply_theme();
  update_workarea();
  if (nt) draw_dock(); /* Taskleiste statt Dock */
  gm_start(); /* als der Benutzer, auf dessen Sitzungsbus */
  automount_start();
}

static void user_env(const Account *a) {
  char buf[PATH_MAX];
  setenv("HOME", a->home, 1);
  setenv("USER", a->name, 1);
  setenv("LOGNAME", a->name, 1);
  setenv("SHELL", a->shell, 1);
  snprintf(buf, sizeof buf, "%s/%%N%%C:%s/%%N", a->home, a->home);
  setenv("XUSERFILESEARCHPATH", buf, 1);
  mkdir("/var/run/user", 0755);
  snprintf(buf, sizeof buf, "/var/run/user/%u", (unsigned)a->uid);
  mkdir(buf, 0700);
  if (chown(buf, a->uid, a->gid)) warn("chown %s: %s", buf, strerror(errno));
  setenv("XDG_RUNTIME_DIR", buf, 1);
  unsetenv("DBUS_SESSION_BUS_ADDRESS");
  start_session_bus(a);
}

static void end_session(void) {
  if (!session) return;
  gm_stop();
  if (session->uid != 0) {
    if (fork() == 0) { /* als der Benutzer alle seine Prozesse beenden, auch D-Bus und xfconfd */
      if (!become(session)) _exit(1);
      signal(SIGTERM, SIG_IGN);
      kill(-1, SIGTERM);
      sleep(3);
      kill(-1, SIGKILL);
      _exit(0);
    }
    restore_root_env();
  }
  warn("abgemeldet: %s", session->name);
  session = NULL;
}

/* ---------------------------------------------------------------------------------------- */
/* Anmeldung, wie #login in clokeos.html: Uhr, Konten, Passwortfeld, Ruhezustand, Neustart,    */
/* Ausschalten über dem unscharfen Hintergrund. Mehrere Konten stehen nebeneinander, ein Klick */
/* oder die Pfeiltasten wählen eines. Gesperrt (Ruhezustand) zeigt sie nur das angemeldete.    */

static char pw[128];
static int pwlen;
static double shake_t0 = -1; /* Wackeln nach falschem Passwort */
static int lg_arrow_x, lg_arrow_y, lg_btn_x[3], lg_btn_y;
static const char *const POWER_KIND[3] = {"sleep", "restart", "off"};
static const char *const POWER_LABEL[3] = {"Sleep", "Restart", "Shut Down"};
static cairo_surface_t *power_icon[3];
static int lg_av_x[8], lg_av_y;

static int login_count(void) { return session ? 1 : naccounts; }
static const Account *login_account(int i) { return session ? session : &accounts[i]; }
static int login_sel(void) { return session ? 0 : sel_account; }

/* Versatz beim Wackeln, wie @keyframes shake: 25 % -8, 50 % +7, 75 % -4 Pixel in 0,4 s */
static double shake_offset(void) {
  if (shake_t0 < 0) return 0;
  double t = (now_s() - shake_t0) / .4;
  if (t >= 1) return 0;
  static const double k[5] = {0, -8, 7, -4, 0};
  int i = (int)(t * 4);
  double f = t * 4 - i;
  return k[i] + (k[i + 1] - k[i]) * f;
}

static void shadow_text(cairo_t *cr, double x, double y, const char *s, double px, bool bold, double a) {
  text(cr, x, y + 2, s, px, bold, (RGBA){0, 0, 0, .22 * a}, 0, 1);
  text(cr, x, y, s, px, bold, (RGBA){1, 1, 1, a}, 0, 1);
}

static void paint_login(cairo_t *cr, int w, int h, void *arg) {
  (void)arg;
  if (sleeping) { /* Ruhezustand: nur der Bildschirm ist aus */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    return;
  }
  cairo_save(cr);
  cairo_scale(cr, 4, 4);
  cairo_set_source_surface(cr, wall_blur, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD); /* sonst dunkle Kante am Rand */
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_set_source_rgba(cr, 10 / 255., 6 / 255., 20 / 255., .18);
  cairo_paint(cr);

  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[96];
  double cx = w / 2.0;
  int top = h * 12 / 100;
  l10n_date_long(buf, sizeof buf, &tm);
  shadow_text(cr, cx, top + 12, buf, 20, true, .9);
  l10n_big_clock(buf, sizeof buf, &tm);
  shadow_text(cr, cx, top + 28 + 58, buf, 116, true, 1);

  /* Knöpfe unten */
  int py = h - 32 - 62;
  lg_btn_y = py + 19;
  for (int i = 0; i < 3; i++) {
    double bx = cx + (i - 1) * 108;
    lg_btn_x[i] = (int)bx;
    cairo_new_path(cr);
    cairo_arc(cr, bx, py + 19, 19, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 1, 1, 1, .2);
    cairo_fill(cr);
    if (!power_icon[i]) {
      char name[32];
      snprintf(name, sizeof name, "power-%s.svg", POWER_KIND[i]);
      power_icon[i] = svg_render(name, 16, 16, NULL);
    }
    if (power_icon[i]) {
      cairo_set_source_surface(cr, power_icon[i], bx - 8, py + 11);
      cairo_paint(cr);
    }
    text(cr, bx, py + 38 + 6 + 8, L(POWER_LABEL[i]), 12, false, (RGBA){1, 1, 1, .85}, 0, 1);
  }

  /* Konten in der Mitte zwischen Uhr und Knöpfen, das ausgewählte groß, die anderen kleiner
   * und blasser; root grau wie das Administratorkonto bei macOS */
  int tb = top + 28 + 116, block = 78 + 10 + 22 + 10 + 30 + 10 + 16;
  double uy = tb + ((py - tb) - block) / 2.0, sx = shake_offset();
  int n = login_count();
  lg_av_y = (int)(uy + 39);
  for (int i = 0; i < n; i++) {
    const Account *a = login_account(i);
    bool on = i == login_sel();
    double ax = cx + (i - (n - 1) / 2.0) * 170 + (on ? sx : 0), r = on ? 39 : 30, al = on ? 1 : .6;
    RGBA c = a->uid == 0 ? (RGBA){.56, .56, .6, 1} : ACCENT;
    lg_av_x[i] = (int)ax;
    cairo_pattern_t *g = cairo_pattern_create_radial(ax, lg_av_y + 6, r * .77, ax, lg_av_y + 6, r * 1.44);
    cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, .25 * al);
    cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);
    g = cairo_pattern_create_linear(ax - r, lg_av_y - r, ax + r, lg_av_y + r);
    cairo_pattern_add_color_stop_rgba(g, 0, c.r, c.g, c.b, al);
    cairo_pattern_add_color_stop_rgba(g, 1, c.r * .55, c.g * .55, c.b * .55, al);
    cairo_new_path(cr);
    cairo_arc(cr, ax, lg_av_y, r, 0, 2 * M_PI);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    char initial[8] = {0};
    int il = 1;
    const char *dn = display_name(a);
    while (dn[il] && (dn[il] & 0xc0) == 0x80) il++; /* erstes Zeichen in UTF-8 */
    snprintf(initial, sizeof initial, "%.*s", il, dn);
    for (char *p = initial; *p; p++) *p = (char)toupper((unsigned char)*p);
    text(cr, ax, lg_av_y, initial, on ? 32 : 25, true, (RGBA){1, 1, 1, al}, 0, 1);
    shadow_text(cr, ax, uy + 78 + 10 + 11, dn, on ? 17 : 14, on, on ? 1 : .8);
  }
  const Account *cur = login_account(login_sel());
  double lx = lg_av_x[login_sel()]; /* Feld und Hinweis unter dem ausgewählten Konto */

  double fx = lx - 110, fy = uy + 78 + 10 + 22 + 10;
  rrect(cr, fx, fy, 220, 30, 15);
  cairo_set_source_rgba(cr, 1, 1, 1, .2);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, .35);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  if (pwlen) {
    char dots[3 * 24 + 1] = "";
    int n = pwlen > 24 ? 24 : pwlen;
    for (int i = 0; i < n; i++) strcat(dots, "•");
    text(cr, fx + 14, fy + 15, dots, 13, false, (RGBA){1, 1, 1, 1}, 0, 0);
  } else {
    text(cr, fx + 14, fy + 15, L("Enter Password"), 13, false, (RGBA){1, 1, 1, .7}, 0, 0);
  }
  lg_arrow_x = (int)(fx + 220 - 15);
  lg_arrow_y = (int)(fy + 15);
  cairo_new_path(cr);
  cairo_arc(cr, lg_arrow_x, lg_arrow_y, 12, 0, 2 * M_PI);
  cairo_set_source_rgba(cr, 1, 1, 1, .3);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_set_line_width(cr, 2);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  double ax = lg_arrow_x - 7, ay = lg_arrow_y - 7, k = 14 / 16.0; /* Pfeil M3 8h10M9 4l4 4-4 4 */
  cairo_move_to(cr, ax + 3 * k, ay + 8 * k);
  cairo_line_to(cr, ax + 13 * k, ay + 8 * k);
  cairo_move_to(cr, ax + 9 * k, ay + 4 * k);
  cairo_line_to(cr, ax + 13 * k, ay + 8 * k);
  cairo_line_to(cr, ax + 9 * k, ay + 12 * k);
  cairo_stroke(cr);
  if (!account_has_password(cur))
    text(cr, lx, fy + 30 + 10 + 8, L("No password: press Return to log in."), 11.5, false, (RGBA){1, 1, 1, .72}, 0, 1);
}

static void draw_login(void) {
  if (locked && loginwin) paint_window(loginwin, SW, SH, paint_login, NULL);
}

static void show_login(bool sleep) {
  if (!session) load_accounts();
  close_menu();
  show_tip(-1);
  locked = true;
  sleeping = sleep;
  pwlen = 0;
  pw[0] = 0;
  if (!loginwin) loginwin = ui_window(0, 0, SW, SH, ExposureMask | ButtonPressMask | KeyPressMask, false, NET_WM_WINDOW_TYPE_SPLASH);
  XUnmapWindow(dpy, dock);
  XUnmapWindow(dpy, menubar);
  XMapRaised(dpy, loginwin);
  XSync(dpy, False);
  XSetInputFocus(dpy, loginwin, RevertToPointerRoot, CurrentTime);
  XGrabKeyboard(dpy, loginwin, True, GrabModeAsync, GrabModeAsync, CurrentTime);
  draw_login();
}

static void unlock(void) {
  locked = sleeping = false;
  memset(pw, 0, sizeof pw);
  pwlen = 0;
  XUngrabKeyboard(dpy, CurrentTime);
  XUnmapWindow(dpy, loginwin);
  XMapRaised(dpy, menubar);
  if (!recovery) XMapRaised(dpy, dock);
  Client *c = clients;
  while (c && c->minimized) c = c->next;
  focus_client(c);
  redraw_all();
}

static void try_login(void) {
  const Account *a = login_account(login_sel());
  bool ok = !account_has_password(a);
  if (!ok) {
    const char *h = crypt(pw, a->passwd);
    ok = h && !strcmp(h, a->passwd);
  }
  if (ok) {
    if (!session) start_session(a);
    unlock();
  } else {
    memset(pw, 0, sizeof pw);
    pwlen = 0;
    shake_t0 = now_s();
    draw_login();
  }
}

static void login_power(int i) {
  if (i == 0) {
    sleeping = true;
    draw_login();
  } else {
    XSync(dpy, False);
    spawn_as(i == 1 ? "/bin/reboot" : "/bin/poweroff", true);
  }
}

static void select_account(int i) {
  sel_account = i;
  memset(pw, 0, sizeof pw);
  pwlen = 0;
  draw_login();
}

static void login_click(int x, int y) {
  if (sleeping) {
    sleeping = false;
    draw_login();
    return;
  }
  for (int i = 0; !session && i < naccounts; i++) /* Avatar oder Name */
    if (i != sel_account && abs(x - lg_av_x[i]) < 80 && y > lg_av_y - 40 && y < lg_av_y + 70) {
      select_account(i);
      return;
    }
  if (hypot(x - lg_arrow_x, y - lg_arrow_y) <= 13) {
    try_login();
    return;
  }
  for (int i = 0; i < 3; i++)
    if (hypot(x - lg_btn_x[i], y - lg_btn_y) <= 21) login_power(i);
}

static void login_key(XKeyEvent *e) {
  if (sleeping) {
    sleeping = false;
    draw_login();
    return;
  }
  char buf[16];
  KeySym ks = NoSymbol;
  int n = XLookupString(e, buf, sizeof buf, &ks, NULL);
  if (ks == XK_Return || ks == XK_KP_Enter) {
    try_login();
    return;
  }
  if (!session && naccounts > 1 && (ks == XK_Left || ks == XK_Right || ks == XK_Tab)) {
    select_account((sel_account + (ks == XK_Left ? naccounts - 1 : 1)) % naccounts);
    return;
  }
  if (ks == XK_BackSpace) {
    if (pwlen) pw[--pwlen] = 0;
  } else if (ks == XK_Escape) {
    memset(pw, 0, sizeof pw);
    pwlen = 0;
  } else if (n > 0 && (unsigned char)buf[0] >= 32 && pwlen + n < (int)sizeof pw - 1) {
    memcpy(pw + pwlen, buf, n);
    pwlen += n;
    pw[pwlen] = 0;
  }
  draw_login();
}

static void cmd_sleep(long unused) {
  (void)unused;
  show_login(true);
}

static void cmd_logout(long unused) {
  (void)unused;
  Window w[256];
  int n = 0;
  for (Client *c = clients; c && n < 256; c = c->next) w[n++] = c->win;
  for (int i = 0; i < n; i++) {
    Client *c = find_client(w[i]);
    if (c) close_client(c);
  }
  end_session();
  show_login(false);
}

/* ---------------------------------------------------------------------------------------- */
/* Menüs                                                                                      */

static void add_item(const char *label, void (*fn)(long), long arg, bool disabled) {
  if (*mbuild_n >= MENU_MAX) return;
  MenuItem *m = &mbuild[(*mbuild_n)++];
  memset(m, 0, sizeof *m);
  snprintf(m->label, sizeof m->label, "%s", label);
  m->fn = fn;
  m->arg = arg;
  m->disabled = disabled || !fn;
}
static void add_sep(void) {
  /* keine Trennlinie am Anfang und keine zwei hintereinander */
  if (*mbuild_n == 0 || *mbuild_n >= MENU_MAX || mbuild[*mbuild_n - 1].kind == 1) return;
  memset(&mbuild[*mbuild_n], 0, sizeof mbuild[0]);
  mbuild[(*mbuild_n)++].kind = 1;
}
static void add_head(const char *label) {
  if (*mbuild_n >= MENU_MAX) return;
  memset(&mbuild[*mbuild_n], 0, sizeof mbuild[0]);
  snprintf(mbuild[*mbuild_n].label, sizeof mbuild[0].label, "%s", label);
  mbuild[(*mbuild_n)++].kind = 2;
}
static void end_items(void) { /* Trennlinie am Ende weglassen */
  if (*mbuild_n > 0 && mbuild[*mbuild_n - 1].kind == 1) (*mbuild_n)--;
}

#define ITEM_H 24
#define SEP_H 11
#define HEAD_H 22

static int items_y(const MenuItem *it, int i) {
  int y = 6;
  for (int k = 0; k < i; k++) y += it[k].kind == 1 ? SEP_H : it[k].kind == 2 ? HEAD_H : ITEM_H;
  return y;
}
static int item_y(int i) { return items_y(menu, i); }

static void paint_items(cairo_t *cr, int w, int h, Window win, const MenuItem *items, int n, int hover) {
  int sx, sy;
  Window child;
  XTranslateCoordinates(dpy, win, root, 0, 0, &sx, &sy, &child);
  glass(cr, sx, sy, (RGBA){T->tint.r, T->tint.g, T->tint.b, (dark ? .86 : .8) - (comp_on ? .08 : 0)}, w, h, 14);
  rim(cr, 0, 0, w, h, 14);
  for (int i = 0; i < n; i++) {
    const MenuItem *m = &items[i];
    int y = items_y(items, i);
    if (m->kind == 1) {
      set(cr, T->line);
      cairo_rectangle(cr, 15, y + 5, w - 30, 1);
      cairo_fill(cr);
      continue;
    }
    if (m->kind == 2) {
      text(cr, 18, y + HEAD_H / 2.0, m->label, 11, true, T->fg3, w - 30, 0);
      continue;
    }
    bool hl = i == hover && !m->disabled;
    if (hl) {
      rrect(cr, 6, y, w - 12, ITEM_H, 8);
      set(cr, ACCENT);
      cairo_fill(cr);
    }
    RGBA col = m->disabled ? T->fg3 : hl ? (RGBA){1, 1, 1, 1} : T->fg;
    if (m->checked) text(cr, 15, y + ITEM_H / 2.0, "✓", 12, false, col, 0, 0);
    int kw = 0;
    if (m->sub) kw = text(cr, w - 16, y + ITEM_H / 2.0, "›", 15, false, col, 0, 2) + 10;
    else if (m->key[0]) kw = text(cr, w - 16, y + ITEM_H / 2.0, m->key, 12.5, false,
                                  hl ? (RGBA){1, 1, 1, .85} : T->fg3, 0, 2) + 14;
    text(cr, 32, y + ITEM_H / 2.0, m->label, 12.5, false, col, w - 50 - kw, 0);
  }
}

static void paint_menu(cairo_t *cr, int w, int h, void *arg) {
  (void)arg;
  paint_items(cr, w, h, menuwin, menu, nmenu, menu_hover);
}
static void paint_sub(cairo_t *cr, int w, int h, void *arg) {
  (void)arg;
  paint_items(cr, w, h, subwin, submenu, nsub, sub_hover);
}

static void draw_menu(void) {
  if (menuwin) paint_window(menuwin, menu_w, menu_h, paint_menu, NULL);
}
static void draw_sub(void) {
  if (subwin) paint_window(subwin, sub_w, sub_h, paint_sub, NULL);
}

static int items_width(const MenuItem *it, int n) {
  int w = 200;
  for (int i = 0; i < n; i++) {
    int tw = text_width(it[i].label, it[i].kind == 2 ? 11 : 12.5, it[i].kind == 2) + 64;
    if (it[i].key[0]) tw += text_width(it[i].key, 12.5, false) + 24;
    if (it[i].sub) tw += 24;
    if (tw > w) w = tw;
  }
  return w > 420 ? 420 : w;
}
static int menu_width(void) { return items_width(menu, nmenu); }

static void open_menu(int x, int y, int owner, bool above) {
  /* Die Einträge sind schon aufgebaut; close_menu() (ein offenes Menü wird ersetzt) setzt nmenu auf 0.
   * Ohne das blieb das neue Menü beim Wechsel von einem offenen Menü leer, bis man erneut klickte. */
  int keep = nmenu;
  close_menu();
  nmenu = keep;
  int w = menu_width();
  int h = item_y(nmenu) + 6;
  if (above) y -= h;
  if (x + w > SW - 4) x = SW - 4 - w;
  if (x < 4) x = 4;
  if (y + h > SH - 4) y = SH - 4 - h; /* Dock links oder rechts: nicht unten aus dem Bild */
  if (y < MENUBAR_H) y = MENUBAR_H;
  menu_x = x;
  menu_y = y;
  menu_w = w;
  menu_h = h;
  menu_owner = owner;
  menu_hover = -1;
  menuwin = ui_window(x, y, w, h,
                      ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | KeyPressMask,
                      true, NET_WM_WINDOW_TYPE_POPUP_MENU);
  if (!comp_on) shape_round(menuwin, w, h, 14, false);
  XMapRaised(dpy, menuwin);
  XGrabPointer(dpy, menuwin, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync,
               None, cur_arrow, CurrentTime);
  XGrabKeyboard(dpy, menuwin, True, GrabModeAsync, GrabModeAsync, CurrentTime);
  draw_menu();
  if (owner >= 0) draw_menubar();
}

static void close_sub(void) {
  if (!subwin) return;
  XDestroyWindow(dpy, subwin);
  subwin = None;
  nsub = 0;
  sub_hover = -1;
  sub_of = -1;
}

static void build_gm_sub(int parent); /* Inhalt eines Untermenüs aus der globalen Menüleiste */

/* Untermenü des Eintrags i rechts neben dem Menü, ohne Platz dort links davon */
static void open_sub(int i) {
  if (i == sub_of && subwin) return;
  close_sub();
  if (i < 0 || i >= nmenu || !menu[i].sub) return;
  mbuild = submenu;
  mbuild_n = &nsub;
  nsub = 0;
  build_gm_sub(menu[i].sub - 1);
  end_items();
  mbuild = menu;
  mbuild_n = &nmenu;
  if (!nsub) return;
  int w = items_width(submenu, nsub), h = items_y(submenu, nsub) + 6;
  int x = menu_x + menu_w - 6, y = menu_y + item_y(i) - 6;
  if (x + w > SW - 4) x = menu_x - w + 6;
  if (x < 4) x = 4;
  if (y + h > SH - 4) y = SH - 4 - h;
  if (y < MENUBAR_H) y = MENUBAR_H;
  sub_w = w;
  sub_h = h;
  sub_of = i;
  sub_hover = -1;
  subwin = ui_window(x, y, w, h, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask,
                     true, NET_WM_WINDOW_TYPE_POPUP_MENU);
  if (!comp_on) shape_round(subwin, w, h, 14, false);
  XMapRaised(dpy, subwin);
  draw_sub();
}

static int sub_item_at(int x, int y) {
  if (x < 0 || x >= sub_w) return -1;
  for (int i = 0; i < nsub; i++)
    if (y >= items_y(submenu, i) && y < items_y(submenu, i + 1))
      return submenu[i].kind == 0 && !submenu[i].disabled ? i : -1;
  return -1;
}

static void close_menu(void) {
  if (!menuwin) return;
  close_sub();
  XUngrabPointer(dpy, CurrentTime);
  XUngrabKeyboard(dpy, CurrentTime);
  XDestroyWindow(dpy, menuwin);
  menuwin = None;
  int owner = menu_owner;
  menu_owner = -1;
  nmenu = 0;
  if (owner >= 0) draw_menubar();
  if (focused) focus_client(focused);
}

static int menu_item_at(int x, int y) {
  if (x < 0 || x >= menu_w) return -1;
  for (int i = 0; i < nmenu; i++) {
    int y0 = item_y(i), y1 = item_y(i + 1);
    if (y >= y0 && y < y1) return menu[i].kind == 0 && (!menu[i].disabled || menu[i].sub) ? i : -1;
  }
  return -1;
}

static void run_item(const MenuItem *m) {
  if (m->disabled || !m->fn) {
    close_menu();
    return;
  }
  void (*fn)(long) = m->fn;
  long arg = m->arg;
  close_menu();
  fn(arg);
}
static void activate_item(int i) {
  if (i >= 0 && i < nmenu && menu[i].sub) { /* Untermenü: nur öffnen */
    open_sub(i);
    return;
  }
  if (i < 0 || i >= nmenu) close_menu();
  else run_item(&menu[i]);
}
static void activate_sub(int i) {
  if (i < 0 || i >= nsub) close_menu();
  else run_item(&submenu[i]);
}

static void cmd_minimize(long unused) { (void)unused; minimize_client(focused); }
static void cmd_zoom(long unused) { (void)unused; zoom_client(focused); }
static void cmd_close(long unused) { (void)unused; close_client(focused); }
static void cmd_activate(long p) {
  for (Client *c = clients; c; c = c->next)
    if ((long)c == p) restore_client(c);
}
static void cmd_hide_app(long app) {
  for (Client *c = clients; c; c = c->next)
    if (c->app == app) minimize_client(c);
}
static void cmd_quit_app(long app) {
  for (Client *c = clients; c; c = c->next)
    if (c->app == app) close_client(c);
}
static void show_spot(void);
static void cmd_spot(long unused) { (void)unused; show_spot(); }

static int current_app(void) {
  if (!focused) return app_fundus;
  return focused->app;
}

static const char *current_app_name(void) {
  if (recovery && (!focused || focused->draw)) return L("Recovery");
  if (!focused) return app_fundus >= 0 ? apps[app_fundus].b.name : "Fundus";
  if (focused->app >= 0) return apps[focused->app].b.name;
  if (focused->draw) return "ClokeOS";
  return focused->cls[0] ? focused->cls : L("App");
}

/* ---------------------------------------------------------------------------------------- */
/* Globale Menüleiste: die Menüs des aktiven Programms stehen wie bei macOS oben in der Leiste,  */
/* nicht im Fenster. GTK-Programme: appmenu-gtk-module blendet ihre Menüleiste aus und gibt sie */
/* über D-Bus heraus (_GTK_UNIQUE_BUS_NAME und die Pfade am Fenster); cloakmenu holt sie als der */
/* angemeldete Benutzer von dessen Sitzungsbus und schickt sie über einen Socket hierher.       */
/* Mac-Programme (Darlings AppKit) legen dasselbe Zeilenformat als _CLOKE_MENU ans Fenster und   */
/* bekommen die Wahl als ClientMessage _CLOKE_MENU_ACTIVATE (data.l[0] = Nummer) zurück.        */
/* Eine Zeile: "<tiefe>\t<art>\t<nummer>\t<merkmale>\t<text>\t<kürzel>", art m Untermenü,       */
/* i Eintrag, s Trennlinie, merkmale aus d (gesperrt), c (Haken), r (Auswahl).                  */

#define GM_MAX 4096
typedef struct {
  int depth;
  char kind;
  unsigned id;
  bool disabled, checked;
  char label[128], key[40];
} GmItem;
static GmItem gm[GM_MAX], gm_next[GM_MAX];
static int ngm, ngm_next, gm_top[GM_TOP_MAX], ngm_top;
static Window gm_win;            /* Fenster, dessen Menüs in der Leiste stehen; None = keine */
static int gm_src;               /* 1 cloakmenu (D-Bus), 2 _CLOKE_MENU */
static bool gm_reading;          /* zwischen MENU und END */
static unsigned long gm_reading_xid;
static int gm_fd = -1;           /* Socket zu cloakmenu */
static pid_t gm_pid;
static char gm_buf[1 << 16];
static size_t gm_len;
static char gm_watch[640];       /* zuletzt an cloakmenu geschickter Auftrag */

static bool gm_parse(char *line, GmItem *it) {
  char *f[6];
  int n = 0;
  char *p = line;
  f[n++] = p;
  while (n < 6 && (p = strchr(p, '\t'))) {
    *p++ = 0;
    f[n++] = p;
  }
  if (n < 5) return false;
  memset(it, 0, sizeof *it);
  it->depth = atoi(f[0]);
  it->kind = f[1][0];
  it->id = (unsigned)strtoul(f[2], NULL, 10);
  it->disabled = strchr(f[3], 'd') != NULL;
  it->checked = strchr(f[3], 'c') != NULL;
  snprintf(it->label, sizeof it->label, "%s", f[4]);
  if (n > 5) snprintf(it->key, sizeof it->key, "%s", f[5]);
  return it->depth >= 0 && (it->kind == 'm' || it->kind == 'i' || it->kind == 's');
}

static void gm_set(const GmItem *items, int n, Window w, int src) {
  bool other = w != gm_win;
  memcpy(gm, items, (size_t)n * sizeof *items);
  ngm = n;
  gm_win = w;
  gm_src = src;
  ngm_top = 0;
  for (int i = 0; i < ngm && ngm_top < GM_TOP_MAX; i++)
    if (gm[i].depth == 0 && gm[i].kind == 'm') gm_top[ngm_top++] = i;
  /* Ein offenes Programm-Menü bleibt bei Änderungen offen, nur nicht für ein anderes Fenster */
  if (other && menu_owner >= MB_DYN) close_menu();
  draw_menubar();
}

static void gm_clear(void) {
  if (!ngm && !gm_win) return;
  if (menu_owner >= MB_DYN) close_menu();
  ngm = ngm_top = 0;
  gm_win = None;
  gm_src = 0;
  draw_menubar();
}

static void gm_send(const char *line) {
  if (gm_fd < 0) return;
  size_t n = strlen(line);
  if (write(gm_fd, line, n) != (ssize_t)n) warn("cloakmenu: Auftrag nicht geschickt");
}

/* Automount (automount.c): hängt USB-Sticks und Datenträger unter /Volumes ein; läuft als root und gibt dem
 * angemeldeten Benutzer die Dateien. Beim Anmelden neu gestartet, damit der Besitzer stimmt. */
static pid_t am_pid;
static void automount_start(void) {
  static const char *bin = "/System/Library/CoreServices/CloakServer/clokeos-automount";
  if (recovery || access(bin, X_OK)) return;
  if (am_pid > 0) kill(am_pid, SIGTERM);
  char u[16], g[16];
  snprintf(u, sizeof u, "%u", session ? (unsigned)session->uid : 501u);
  snprintf(g, sizeof g, "%u", session ? (unsigned)session->gid : 20u);
  am_pid = fork();
  if (am_pid == 0) {
    signal(SIGCHLD, SIG_DFL);
    if (!freopen("/var/log/clokeos-automount.log", "a", stderr)) _exit(1);
    execl(bin, bin, u, g, (char *)NULL);
    _exit(1);
  }
}

static void gm_start(void) {
  if (gm_fd >= 0 || access(CLOAKMENU, X_OK)) return;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return;
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    signal(SIGCHLD, SIG_DFL);
    close(sv[0]);
    dup2(sv[1], 0);
    dup2(sv[1], 1);
    if (sv[1] > 1) close(sv[1]);
    if (!become(session)) _exit(126);
    execl(CLOAKMENU, "cloakmenu", (char *)NULL);
    _exit(127);
  }
  close(sv[1]);
  if (pid < 0) {
    close(sv[0]);
    return;
  }
  int one = 1; /* stirbt cloakmenu, soll ein Schreibversuch CloakServer nicht mitnehmen */
  setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  fcntl(sv[0], F_SETFL, O_NONBLOCK);
  fcntl(sv[0], F_SETFD, FD_CLOEXEC);
  gm_fd = sv[0];
  gm_pid = pid;
  gm_len = 0;
  gm_watch[0] = 0;
  gm_reading = false;
  if (focused) gm_focus(focused);
}

static void gm_stop(void) {
  if (gm_fd >= 0) close(gm_fd);
  gm_fd = -1;
  if (gm_pid > 0) kill(gm_pid, SIGTERM);
  gm_pid = 0;
  gm_watch[0] = 0;
  gm_clear();
}

static void gm_line(char *line) {
  if (!strncmp(line, "MENU ", 5)) {
    gm_reading = true;
    gm_reading_xid = strtoul(line + 5, NULL, 10);
    ngm_next = 0;
  } else if (!strcmp(line, "END")) {
    if (!gm_reading) return;
    gm_reading = false;
    /* Nur die Antwort zum zuletzt beobachteten Fenster zählt; MENU 0 heißt: keine Menüs */
    if (gm_reading_xid && gm_src == 1 && gm_reading_xid == gm_win) gm_set(gm_next, ngm_next, gm_win, 1);
    else if (!gm_reading_xid && gm_src == 1) gm_clear();
  } else if (gm_reading && ngm_next < GM_MAX && gm_parse(line, &gm_next[ngm_next])) {
    ngm_next++;
  }
}

static void gm_read(void) {
  if (gm_fd < 0) return;
  for (;;) {
    ssize_t k = read(gm_fd, gm_buf + gm_len, sizeof gm_buf - 1 - gm_len);
    if (k == 0) {
      warn("cloakmenu hat sich beendet");
      gm_stop();
      return;
    }
    if (k < 0) return; /* EAGAIN: alles gelesen */
    gm_len += (size_t)k;
    char *start = gm_buf, *nl;
    while ((nl = memchr(start, '\n', (size_t)(gm_buf + gm_len - start)))) {
      *nl = 0;
      gm_line(start);
      start = nl + 1;
    }
    gm_len -= (size_t)(start - gm_buf);
    memmove(gm_buf, start, gm_len);
    if (gm_len >= sizeof gm_buf - 1) gm_len = 0; /* eine Zeile länger als der Puffer: verwerfen */
  }
}

/* Textwert einer Fenster-Eigenschaft (UTF8_STRING oder STRING) */
static bool prop_text(Window w, Atom a, char *out, size_t n) {
  Atom type;
  int fmt;
  unsigned long cnt, after;
  unsigned char *data = NULL;
  out[0] = 0;
  if (XGetWindowProperty(dpy, w, a, 0, (long)(n / 4), False, AnyPropertyType, &type, &fmt, &cnt, &after, &data) !=
          Success || !data)
    return false;
  if (fmt == 8 && cnt) snprintf(out, n, "%.*s", (int)cnt, (char *)data);
  XFree(data);
  return out[0] != 0;
}

static unsigned long win_pid(Window w) {
  Atom type;
  int fmt;
  unsigned long cnt, after, pid = 0;
  unsigned char *data = NULL;
  if (XGetWindowProperty(dpy, w, NET_WM_PID, 0, 1, False, XA_CARDINAL, &type, &fmt, &cnt, &after, &data) == Success &&
      data) {
    if (cnt) pid = *(unsigned long *)data;
    XFree(data);
  }
  return pid;
}

/* Mac-Programm: Menüs aus _CLOKE_MENU */
static bool gm_load_cloke(Window w) {
  Atom type;
  int fmt;
  unsigned long cnt, after;
  unsigned char *data = NULL;
  if (XGetWindowProperty(dpy, w, CLOKE_MENU, 0, 1 << 16, False, AnyPropertyType, &type, &fmt, &cnt, &after, &data) !=
          Success || !data)
    return false;
  if (fmt != 8 || !cnt) {
    XFree(data);
    return false;
  }
  char *txt = strndup((char *)data, cnt);
  XFree(data);
  if (!txt) return false;
  ngm_next = 0;
  char *save = NULL;
  for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
    if (ngm_next < GM_MAX && gm_parse(line, &gm_next[ngm_next])) ngm_next++;
  free(txt);
  gm_set(gm_next, ngm_next, w, 2);
  return true;
}

static void gm_focus(Client *c) {
  Window w = c && !c->draw ? c->win : None;
  if (w && gm_load_cloke(w)) {
    if (gm_watch[0]) gm_send("CLEAR\n");
    gm_watch[0] = 0;
    return;
  }
  char bus[160], mb[160], un[160], app[160], win[160];
  if (w && gm_fd >= 0 && prop_text(w, GTK_UNIQUE_BUS_NAME, bus, sizeof bus) &&
      prop_text(w, GTK_MENUBAR_OBJECT_PATH, mb, sizeof mb) && !strpbrk(bus, " \t\n") && !strpbrk(mb, " \t\n")) {
    if (!prop_text(w, UNITY_OBJECT_PATH, un, sizeof un) || strpbrk(un, " \t\n")) strcpy(un, "-");
    if (!prop_text(w, GTK_APPLICATION_OBJECT_PATH, app, sizeof app) || strpbrk(app, " \t\n")) strcpy(app, "-");
    if (!prop_text(w, GTK_WINDOW_OBJECT_PATH, win, sizeof win) || strpbrk(win, " \t\n")) strcpy(win, "-");
    char cmd[sizeof gm_watch];
    snprintf(cmd, sizeof cmd, "WATCH %lu %s %s %s %s %s\n", w, bus, mb, un, app, win);
    if (gm_win != w) { /* bis cloakmenu antwortet: noch keine Programm-Menüs */
      if (menu_owner >= MB_DYN) close_menu();
      ngm = ngm_top = 0;
      gm_win = w;
      gm_src = 1;
    }
    if (strcmp(cmd, gm_watch)) {
      snprintf(gm_watch, sizeof gm_watch, "%s", cmd);
      gm_send(cmd);
    }
    return;
  }
  /* Ein Fenster ohne Menüs desselben Programms (Dialog) behält dessen Menüleiste */
  if (w && gm_win && gm_win != w && win_pid(w) && win_pid(w) == win_pid(gm_win)) return;
  if (gm_watch[0]) gm_send("CLEAR\n");
  gm_watch[0] = 0;
  gm_clear();
}

static void gm_forget(Window w) {
  if (!w || w != gm_win) return;
  if (gm_watch[0]) gm_send("CLEAR\n");
  gm_watch[0] = 0;
  gm_clear();
}

static void gm_property(Client *c, Atom a) {
  if (c != focused) return;
  if (a == CLOKE_MENU || a == GTK_UNIQUE_BUS_NAME || a == GTK_MENUBAR_OBJECT_PATH || a == UNITY_OBJECT_PATH ||
      a == GTK_APPLICATION_OBJECT_PATH || a == GTK_WINDOW_OBJECT_PATH)
    gm_focus(c);
}

static void gm_activate(long id) {
  if (gm_src == 1) {
    char b[48];
    snprintf(b, sizeof b, "ACTIVATE %ld\n", id);
    gm_send(b);
  } else if (gm_src == 2 && gm_win) {
    XClientMessageEvent ev = {.type = ClientMessage, .window = gm_win, .message_type = CLOKE_MENU_ACTIVATE, .format = 32};
    ev.data.l[0] = id;
    ev.data.l[1] = CurrentTime;
    XSendEvent(dpy, gm_win, False, NoEventMask, (XEvent *)&ev);
  }
}

/* gm[i] in das Menü, das gerade entsteht */
static void gm_add(int i) {
  const GmItem *g = &gm[i];
  if (g->kind == 's') {
    add_sep();
    return;
  }
  int before = *mbuild_n;
  add_item(g->label, g->kind == 'i' ? gm_activate : NULL, g->id, g->disabled);
  if (*mbuild_n == before) return;
  MenuItem *m = &mbuild[*mbuild_n - 1];
  m->checked = g->checked;
  snprintf(m->key, sizeof m->key, "%s", g->key);
  if (g->kind == 'm') {
    m->sub = i + 1;
    m->disabled = false;
  }
}

/* Einträge eine Ebene unter gm[parent]; flatten: die nächste Ebene als Überschrift mit Einträgen
 * statt als weiteres Untermenü (CloakServer zeigt Menü und eine Ebene Untermenü) */
static void gm_children(int parent, bool flatten) {
  int d = gm[parent].depth + 1;
  for (int i = parent + 1; i < ngm && gm[i].depth >= d; i++) {
    if (gm[i].depth != d) continue;
    if (flatten && gm[i].kind == 'm') {
      add_sep();
      add_head(gm[i].label);
      for (int k = i + 1; k < ngm && gm[k].depth > d; k++)
        if (gm[k].depth == d + 1 && gm[k].kind != 'm') gm_add(k);
      add_sep();
      continue;
    }
    gm_add(i);
  }
}

static void build_gm_menu(int t) {
  if (t < 0 || t >= ngm_top) return;
  nmenu = 0; /* neu aufbauen, nicht an die Einträge eines noch offenen Menüs anhängen */
  gm_children(gm_top[t], false);
  end_items();
}

static void build_gm_sub(int parent) {
  if (parent >= 0 && parent < ngm) gm_children(parent, true);
}

static void build_menu(int which) {
  nmenu = 0;
  int app = current_app();
  char buf[160];
  switch (which) {
  case MB_LOGO:
    add_item(L("About ClokeOS"), show_about, 0, false);
    add_sep();
    if (recovery) {
      add_item(L("Restart…"), confirm_power, 1, false);
      add_item(L("Shut Down…"), confirm_power, 0, false);
      break;
    }
    add_item(L("System Settings…"), show_settings, -1, false);
    add_item(L("Activity Monitor"), launch_app, app_by_ident("com.clokeos.aktivitaet"), app_by_ident("com.clokeos.aktivitaet") < 0);
    add_item(L("Applications"), cmd_spot, 0, false);
    add_sep();
    add_item(L("Sleep"), cmd_sleep, 0, false);
    add_item(L("Restart…"), confirm_power, 1, false);
    add_item(L("Restart in Recovery…"), confirm_power, 2, access(INSTALLER, X_OK) != 0);
    add_item(L("Shut Down…"), confirm_power, 0, false);
    add_sep();
    snprintf(buf, sizeof buf, L("Log Out “%s”…"), session_name());
    add_item(buf, cmd_logout, 0, false);
    break;
  case MB_APP: {
    if (recovery && (!focused || focused->draw)) {
      add_item(L("About ClokeOS"), show_about, 0, false);
      add_sep();
      add_item(L("Quit Recovery…"), confirm_power, 1, false);
      break;
    }
    const char *name = current_app_name();
    snprintf(buf, sizeof buf, L("About %s"), name);
    add_item(focused && app >= 0 ? buf : L("About ClokeOS"), show_about, 0, false);
    add_sep();
    bool running = false;
    for (Client *c = clients; c; c = c->next) running |= c->app == app && app >= 0;
    snprintf(buf, sizeof buf, L("Hide %s"), name);
    add_item(buf, cmd_hide_app, app, !running);
    snprintf(buf, sizeof buf, L("Quit %s"), name);
    add_item(buf, cmd_quit_app, app, !running);
    break;
  }
  case MB_FILE:
    if (recovery) { /* "Dienstprogramme" wie in macOS Recovery */
      add_item(L("Install ClokeOS…"), rec_menu_install, 0, false);
      add_sep();
      add_item(L("Disk Utility"), show_disk_utility, 0, false);
      add_item("Terminal", rec_menu_terminal, 0, false);
      add_item(L("Get Help Online"), rec_menu_help, 0, false);
      break;
    }
    add_item(L("New Window"), launch_app, app, app < 0);
    add_item(L("Close Window"), cmd_close, 0, !focused);
    break;
  case MB_WINDOW: {
    add_item(L("Minimize"), cmd_minimize, 0, !focused || !focused->th);
    add_item(L("Zoom"), cmd_zoom, 0, !focused || !focused->th);
    bool any = false;
    for (Client *c = clients; c; c = c->next) {
      if (c->draw || (focused && c->app != focused->app) || (focused && focused->app < 0 && c != focused)) continue;
      if (!any) add_sep();
      any = true;
      add_item(c->title[0] ? c->title : c->cls, cmd_activate, (long)c, false);
      menu[nmenu - 1].checked = c == focused;
    }
    break;
  }
  case MB_WIFI: {
    add_head(L("Network"));
    struct ifaddrs *ifa = NULL;
    bool any = false;
    if (!getifaddrs(&ifa)) {
      for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !strcmp(i->ifa_name, "lo0")) continue;
        char ip[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
        snprintf(buf, sizeof buf, "Ethernet (%s): %s", i->ifa_name, ip);
        add_item(buf, NULL, 0, true);
        any = true;
      }
      freeifaddrs(ifa);
    }
    if (!any) add_item(L("Not Connected"), NULL, 0, true);
    break;
  }
  case MB_CC: {
    add_head(L("Appearance"));
    const char *names[3] = {"Auto (dark in the evening)", "Light", "Dark"};
    for (int i = 0; i < 3; i++) {
      add_item(L(names[i]), set_theme_mode, i, false);
      menu[nmenu - 1].checked = theme_mode == i;
    }
    break;
  }
  case MB_CLOCK: {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char day[96];
    l10n_date_long(day, sizeof day, &tm);
    snprintf(buf, sizeof buf, l10n_24h() ? "%s %d" : "%s, %d", day, tm.tm_year + 1900);
    add_head(buf);
    add_sep();
    add_item(L("Open Calendar"), launch_app, app_by_ident("com.clokeos.kalender"), app_by_ident("com.clokeos.kalender") < 0);
    break;
  }
  }
}

/* ---------------------------------------------------------------------------------------- */
/* Menüleiste                                                                                 */

static void clock_text(char *buf, size_t n) {
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  l10n_menubar_clock(buf, n, &tm);
}

static const char *mb_label(int i) {
  if (i >= MB_DYN) return i - MB_DYN < ngm_top ? gm[gm_top[i - MB_DYN]].label : "";
  switch (i) {
  case MB_APP: return current_app_name();
  case MB_FILE: return recovery ? L("Utilities") : L("File");
  case MB_WINDOW: return L("Window");
  }
  return "";
}

/* Hat das Programm selbst ein Menü "Window"/"Fenster" (Logic Pro, GTK, Raven)? Dann kein zweites von CloakServer.
 * Kürzel-Zeichen (_ bei GTK, & bei Windows) zählen nicht. */
static bool app_has_window_menu(void) {
  for (int k = 0; k < ngm_top; k++) {
    char t[64];
    int n = 0;
    for (const char *p = gm[gm_top[k]].label; *p && n < 63; p++)
      if (*p != '_' && *p != '&') t[n++] = *p;
    t[n] = 0;
    if (!strcasecmp(t, "Window") || !strcasecmp(t, "Fenster")) return true;
  }
  return false;
}

/* Links: Logo, Programmname, dann die Menüs des Programms (globale Menüleiste) oder "Ablage",
 * und "Fenster" (vor einem "Hilfe" am Ende, wie bei macOS). Rechts: Netzwerk bis Uhr. */
static void layout_menubar(void) {
  memset(mb_w, 0, sizeof mb_w);
  char clk[64];
  clock_text(clk, sizeof clk);
  int r = SW - 8;
  mb_w[MB_CLOCK] = text_width(clk, 12.5, false) + 20;
  for (int i = MB_CLOCK; i >= MB_WIFI; i--) {
    if (i != MB_CLOCK) mb_w[i] = 36;
    r -= mb_w[i];
    mb_x[i] = r;
    r -= 1;
  }
  int order[MB_N + GM_TOP_MAX], n = 0;
  order[n++] = MB_LOGO;
  order[n++] = MB_APP;
  if (ngm_top) {
    const char *last = gm[gm_top[ngm_top - 1]].label;
    bool help = !strcasecmp(last, "Help") || !strcasecmp(last, "Hilfe"), own = !app_has_window_menu();
    for (int k = 0; k < ngm_top; k++) {
      if (own && help && k == ngm_top - 1) order[n++] = MB_WINDOW;
      order[n++] = MB_DYN + k;
    }
    if (own && !help) order[n++] = MB_WINDOW;
  } else {
    order[n++] = MB_FILE;
    order[n++] = MB_WINDOW;
  }
  int x = 8;
  for (int k = 0; k < n; k++) {
    int i = order[k];
    int w = i == MB_LOGO ? 35 : text_width(mb_label(i), 12.5, i == MB_APP) + 20;
    if (x + w > mb_x[MB_WIFI] - 12) break; /* kein Platz mehr: die übrigen Menüs fallen weg */
    mb_w[i] = w;
    mb_x[i] = x;
    x += w + 1;
  }
}

static cairo_surface_t *mb_icons[4]; /* logo, wifi, spot, cc in der Farbe der Menüleiste */

static void render_mb_icons(void) {
  char col[16];
  rgba_hex(col, T->menubar_fg);
  const char *sub[] = {"@FILL@", col, NULL};
  const char *names[4] = {"logo.svg", "wifi.svg", "spot.svg", "cc.svg"};
  int sizes[4] = {15, 16, 16, 16};
  for (int i = 0; i < 4; i++) {
    if (mb_icons[i]) cairo_surface_destroy(mb_icons[i]);
    mb_icons[i] = svg_render(names[i], sizes[i], sizes[i], sub);
  }
}

static void paint_menubar(cairo_t *cr, int w, int h, void *arg) {
  (void)arg;
  cairo_set_source_surface(cr, wall_img, 0, 0);
  cairo_paint(cr);
  /* Leichter Schleier, damit die Schrift auf jedem Hintergrund lesbar bleibt */
  cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, h);
  if (dark) { cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, .28); cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, .1); }
  else { cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, .32); cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, .12); }
  cairo_set_source(cr, g);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);
  cairo_pattern_destroy(g);

  layout_menubar();
  for (int i = 0; i < MB_N + ngm_top; i++) {
    if (!mb_w[i]) continue;
    if (menu_owner == i) {
      rrect(cr, mb_x[i], 3, mb_w[i], 22, 11);
      set(cr, T->pill);
      cairo_fill(cr);
      rim(cr, mb_x[i], 3, mb_w[i], 22, 11);
    }
    double cx = mb_x[i] + mb_w[i] / 2.0, cy = h / 2.0;
    cairo_surface_t *ic = i == MB_LOGO ? mb_icons[0] : i == MB_WIFI ? mb_icons[1] : i == MB_SPOT ? mb_icons[2]
                        : i == MB_CC ? mb_icons[3] : NULL;
    if (ic) {
      int s = cairo_image_surface_get_width(ic);
      cairo_set_source_surface(cr, ic, round(cx - s / 2.0), round(cy - s / 2.0));
      cairo_paint(cr);
    } else if (i == MB_CLOCK) {
      char clk[64];
      clock_text(clk, sizeof clk);
      text(cr, cx, cy, clk, 12.5, false, T->menubar_fg, 0, 1);
    } else {
      text(cr, cx, cy, mb_label(i), 12.5, i == MB_APP, T->menubar_fg, 0, 1);
    }
  }
}

static void draw_menubar(void) { paint_window(menubar, SW, MENUBAR_H, paint_menubar, NULL); }

static int menubar_item_at(int x) {
  for (int i = 0; i < MB_N + ngm_top; i++)
    if (mb_w[i] && x >= mb_x[i] && x < mb_x[i] + mb_w[i]) return i;
  return -1;
}

/* Das offene Menü kam durch Überfahren eines anderen Titels (nicht durch einen Klick): der Klick auf
 * diesen Titel soll es dann offen lassen. Sonst schloss er es, und erst ein zweiter Klick zeigte es. */
static bool menu_by_hover;

/* Wie bei macOS wird ein Programmfenster erst beim Loslassen der Maustaste angehoben und fokussiert, nicht
 * beim Klick: So lassen sich Dateien aus einem Hintergrundfenster in ein anderes ziehen. Der Klick geht
 * sofort an das Programm (ReplayPointer); die Leiste sieht das Loslassen nicht mehr (der Zeiger gehört dann
 * dem Programm) und fragt deshalb den Zustand der Tasten ab (pending_focus_check in der Hauptschleife). */
static Client *pending_focus;
static double pending_focus_t0;

static void menubar_open(int i) {
  if (i == MB_SPOT) {
    close_menu();
    cmd_spot(0);
    return;
  }
  if (i >= MB_DYN) build_gm_menu(i - MB_DYN);
  else build_menu(i);
  open_menu(mb_x[i], MENUBAR_H - 1, i, false);
}

/* ---------------------------------------------------------------------------------------- */
/* Dock                                                                                       */

static bool in_dock(int i) {
  for (int k = 0; k < nditems; k++)
    if (ditems[k].app == i) return true;
  return false;
}

static bool kept_in_dock(int i) {
  for (int k = 0; k < ndock_ids; k++)
    if (!strcmp(dock_ids[k], apps[i].b.ident)) return true;
  return false;
}

/* Reihenfolge wie bei macOS: Fundus, behaltene Programme, laufende andere, Trennstrich, Papierkorb.
 * pos ist die Lage entlang des Docks, vom Anfang des Glases. */
static void layout_dock(void) {
  nditems = 0;
#define DOCK_ADD(a) (nditems < 64 ? (void)(ditems[nditems++].app = (a)) : (void)0)
  if (app_fundus >= 0) DOCK_ADD(app_fundus);
  for (int k = 0; k < ndock_ids; k++) {
    int i = app_by_ident(dock_ids[k]);
    if (i >= 0 && !in_dock(i)) DOCK_ADD(i);
  }
  for (int i = 0; i < napps; i++)
    if (!apps[i].gone && !apps[i].b.hidden && !in_dock(i) && (app_running(i) || apps[i].bounce_t0 > 0)) DOCK_ADD(i);
  DOCK_ADD(DOCK_SEP);
  DOCK_ADD(DOCK_TRASH);
#undef DOCK_ADD
  int p = nt ? NT_START_W : DOCK_PAD_X;
  for (int k = 0; k < nditems; k++) {
    if (k) p += DOCK_GAP;
    ditems[k].pos = p;
    p += ditems[k].app == DOCK_SEP ? 7 : ds;
  }
  int len = p + DOCK_PAD_X;
  /* passt es nicht, wird es kleiner wie bei macOS; die Symbole zeichnet draw_dock() neu */
  int room = dock_pos == POS_BOTTOM ? SW - 2 * DOCK_MARGIN : SH - MENUBAR_H - 2 * DOCK_MARGIN;
  int icons = nditems - 1, want = ds_pref;
  if (len - icons * ds + icons * ds_pref > room && icons > 0) want = (room - (len - icons * ds)) / icons;
  if (want < 24) want = 24;
  if (want != ds) {
    ds = want;
    render_icons();
    layout_dock();
    return;
  }
  if (nt) { /* Taskleiste über die ganze Breite */
    dock_w = SW, dock_h = DOCK_H;
    dock_x = 0, dock_y = SH - DOCK_H;
  } else if (dock_pos == POS_BOTTOM) {
    dock_w = len, dock_h = DOCK_H;
    dock_x = (SW - len) / 2, dock_y = SH - DOCK_MARGIN - DOCK_H;
  } else {
    dock_w = DOCK_H, dock_h = len;
    dock_y = MENUBAR_H + (SH - MENUBAR_H - len) / 2;
    dock_x = dock_pos == POS_LEFT ? DOCK_MARGIN : SW - DOCK_MARGIN - DOCK_H;
  }
}

/* Fenster des Docks: das Glas und dock_lift Pixel zur Bildschirmmitte hin */
static void dock_window_rect(int *x, int *y, int *w, int *h) {
  *x = dock_x, *y = dock_y, *w = dock_w, *h = dock_h;
  if (dock_pos == POS_BOTTOM) *y -= dock_lift, *h += dock_lift;
  else if (dock_pos == POS_LEFT) *w += dock_lift;
  else *x -= dock_lift, *w += dock_lift;
}
static int glass_ox(void) { return dock_pos == POS_RIGHT ? dock_lift : 0; }
static int glass_oy(void) { return dock_pos == POS_BOTTOM ? dock_lift : 0; }

/* Ecke des Symbols k im Glas; off verschiebt es zur Bildschirmmitte (Überfahren, Hüpfen) */
static void item_xy(int k, double off, double *x, double *y) {
  int p = ditems[k].pos;
  if (dock_pos == POS_BOTTOM) *x = p, *y = DOCK_PAD_Y - off;
  else if (dock_pos == POS_LEFT) *x = DOCK_PAD_Y + off, *y = p;
  else *x = DOCK_PAD_Y - off, *y = p;
}

static bool app_running(int i) {
  for (Client *c = clients; c; c = c->next)
    if (c->app == i) return true;
  return false;
}

/* Hüpfen beim Starten wie .dk.bounce in clokeos.html: zweimal 18 Pixel hoch in je 0,55 s */
#define BOUNCE_S 1.1
static double bounce_offset(int i) {
  if (!dock_lift || i < 0 || apps[i].bounce_t0 <= 0) return 0;
  double t = now_s() - apps[i].bounce_t0;
  if (t >= BOUNCE_S) return 0;
  return 18 * sin(M_PI * fmod(t, BOUNCE_S / 2) / (BOUNCE_S / 2));
}

/* NT: flache Taskleiste mit Start-Schaltfläche links und Uhr rechts */
static void paint_taskbar(cairo_t *cr, int w, int h) {
  cairo_set_source_rgba(cr, dark ? .13 : .95, dark ? .13 : .95, dark ? .15 : .96, .96);
  cairo_rectangle(cr, 0, 0, w, h); /* nicht den ganzen Fensterinhalt: oben liegt der Streifen dock_lift */
  cairo_fill(cr);
  set(cr, T->line);
  cairo_rectangle(cr, 0, 0, w, 1);
  cairo_fill(cr);
  double s = 7, g = 2, x = (NT_START_W - 2 * s - g) / 2.0, y = (h - 2 * s - g) / 2.0; /* vier Felder */
  for (int k = 0; k < 4; k++) {
    set(cr, ACCENT);
    cairo_rectangle(cr, x + (k % 2) * (s + g), y + (k / 2) * (s + g), s, s);
    cairo_fill(cr);
  }
  char clk[64];
  clock_text(clk, sizeof clk);
  double cw = text_width(clk, 12, false), tx = w - 12 - cw;
  text(cr, tx, h / 2.0, clk, 12, false, T->fg, 0, 0);
  /* Tray links der Uhr: Lautsprecher und Netzwerk */
  double sx = tx - 30, cy = h / 2.0;
  set(cr, T->fg2);
  cairo_move_to(cr, sx - 6, cy - 2); cairo_line_to(cr, sx - 3, cy - 2); cairo_line_to(cr, sx + 1, cy - 6);
  cairo_line_to(cr, sx + 1, cy + 6); cairo_line_to(cr, sx - 3, cy + 2); cairo_line_to(cr, sx - 6, cy + 2);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_set_line_width(cr, 1.2);
  cairo_arc(cr, sx + 1, cy, 4, -M_PI / 3, M_PI / 3);
  cairo_stroke(cr);
  cairo_arc(cr, sx + 1, cy, 7, -M_PI / 3, M_PI / 3);
  cairo_stroke(cr);
  if (mb_icons[1]) {
    cairo_set_source_surface(cr, mb_icons[1], sx - 34, cy - 8);
    cairo_paint(cr);
  }
}

static void paint_dock(cairo_t *cr, int w, int h, void *arg) {
  (void)arg, (void)w, (void)h;
  cairo_translate(cr, glass_ox(), glass_oy());
  w = dock_w, h = dock_h;
  double r = ds * .42 + 6;
  if (nt) {
    paint_taskbar(cr, w, h);
  } else {
  rrect(cr, 0, 0, w, h, r);
  cairo_clip(cr);
  glass(cr, dock_x, dock_y, T->dock, w, h, r);
  /* Lichtkante innen oben und unten, Kante außen */
  cairo_reset_clip(cr);
  RGBA a = T->rim, b = T->rim_low;
  cairo_save(cr);
  rrect(cr, 0, 0, w, h, r);
  cairo_clip(cr);
  cairo_set_source_rgba(cr, a.r, a.g, a.b, a.a);
  cairo_rectangle(cr, r * .6, 0, w - 1.2 * r, 1);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, b.r, b.g, b.b, b.a);
  cairo_rectangle(cr, r * .6, h - 1, w - 1.2 * r, 1);
  cairo_fill(cr);
  cairo_restore(cr);
  rim(cr, 0, 0, w, h, r);
  rrect(cr, .25, .25, w - .5, h - .5, r);
  set(cr, T->edge);
  cairo_set_line_width(cr, .5);
  cairo_stroke(cr);
  }

  bool full = trash_full();
  for (int k = 0; k < nditems; k++) {
    int i = ditems[k].app, p = ditems[k].pos;
    if (i == DOCK_SEP) {
      set(cr, T->line_strong);
      if (dock_pos == POS_BOTTOM) cairo_rectangle(cr, p + 3, DOCK_PAD_Y + 2, 1, ds - 4);
      else cairo_rectangle(cr, DOCK_PAD_Y + 2, p + 3, ds - 4, 1);
      cairo_fill(cr);
      continue;
    }
    cairo_surface_t *ic = i == DOCK_TRASH ? (full ? icon_trash_full : icon_trash) : apps[i].icon;
    double x, y;
    item_xy(k, (k == dock_hover && !nt ? 2 : 0) + bounce_offset(i), &x, &y);
    if (nt && k == dock_hover) { /* NT: Taste unter dem Zeiger */
      set(cr, T->hover);
      cairo_rectangle(cr, p - 4, 2, ds + 8, h - 4);
      cairo_fill(cr);
    }
    if (ic) {
      cairo_set_source_surface(cr, ic, x, y);
      cairo_paint(cr);
    }
    if (nt && i >= 0 && app_running(i)) { /* NT: Balken unter dem Symbol */
      set(cr, ACCENT);
      cairo_rectangle(cr, p + ds / 2.0 - 8, h - 4, 16, 3);
      cairo_fill(cr);
    } else if (i >= 0 && app_running(i)) { /* Punkt außen neben dem Symbol */
      double dx = dock_pos == POS_BOTTOM ? p + ds / 2.0 : dock_pos == POS_LEFT ? DOCK_PAD_Y - 3 : DOCK_PAD_Y + ds + 3;
      double dy = dock_pos == POS_BOTTOM ? DOCK_PAD_Y + ds + 3 : p + ds / 2.0;
      cairo_new_path(cr);
      cairo_arc(cr, dx, dy, 2, 0, 2 * M_PI);
      seta(cr, T->fg, .7);
      cairo_fill(cr);
    }
  }
}

static void draw_dock(void) {
  static int lx = -1, ly, lw, lh;
  layout_dock();
  int x, y, w, h;
  dock_window_rect(&x, &y, &w, &h);
  if (x != lx || y != ly || w != lw || h != lh) { /* Programme kamen dazu, andere Lage, andere Größe */
    XMoveResizeWindow(dpy, dock, x, y, w, h);
    if (!comp_on && !nt) shape_round(dock, w, h, ds * .42 + 6, false);
    lx = x, ly = y, lw = w, lh = h;
  }
  paint_window(dock, w, h, paint_dock, NULL);
}

static int dock_item_at(int x, int y) {
  x -= glass_ox(), y -= glass_oy();
  int along = dock_pos == POS_BOTTOM ? x : y, across = dock_pos == POS_BOTTOM ? y : x;
  if (across < 0 || across > DOCK_H) return -1;
  for (int k = 0; k < nditems; k++)
    if (ditems[k].app != DOCK_SEP && along >= ditems[k].pos && along < ditems[k].pos + ds) return k;
  return -1;
}

/* Mitte des Symbols k auf dem Bildschirm */
static void item_center(int k, double *x, double *y) {
  double ix, iy;
  item_xy(k, 0, &ix, &iy);
  *x = dock_x + ix + ds / 2.0;
  *y = dock_y + iy + ds / 2.0;
}

static int tip_w;
static const char *tip_text;

static void paint_tip(cairo_t *cr, int w, int h, void *arg) {
  (void)arg;
  int sx, sy;
  Window child;
  XTranslateCoordinates(dpy, tipwin, root, 0, 0, &sx, &sy, &child);
  glass(cr, sx, sy, (RGBA){T->tint.r, T->tint.g, T->tint.b, (dark ? .86 : .8) - (comp_on ? .08 : 0)}, w, h, h / 2.0);
  rim(cr, 0, 0, w, h, h / 2.0);
  text(cr, w / 2.0, h / 2.0, tip_text, 12, false, T->fg, 0, 1);
}

/* Namensschild über dem Symbol, beim Dock links und rechts daneben */
static void show_tip(int k) {
  if (k < 0 || k >= nditems || ditems[k].app == DOCK_SEP) {
    if (tipwin) XUnmapWindow(dpy, tipwin);
    return;
  }
  tip_text = ditems[k].app == DOCK_TRASH ? L("Trash") : apps[ditems[k].app].b.name;
  tip_w = text_width(tip_text, 12, false) + 24;
  int th = 24;
  double cx, cy;
  item_center(k, &cx, &cy);
  int x = (int)cx - tip_w / 2, y = dock_y - th - 10;
  if (dock_pos == POS_LEFT) x = dock_x + dock_w + 10, y = (int)cy - th / 2;
  else if (dock_pos == POS_RIGHT) x = dock_x - 10 - tip_w, y = (int)cy - th / 2;
  if (!tipwin) tipwin = ui_window(x, y, tip_w, th, ExposureMask, true, NET_WM_WINDOW_TYPE_TOOLTIP);
  XMoveResizeWindow(dpy, tipwin, x, y, tip_w, th);
  if (!comp_on) shape_round(tipwin, tip_w, th, th / 2.0, false);
  XMapRaised(dpy, tipwin);
  paint_window(tipwin, tip_w, th, paint_tip, NULL);
}

static void open_trash(void) {
  char t[PATH_MAX];
  snprintf(t, sizeof t, "%s/.Trash", getenv("HOME") ? getenv("HOME") : "/var/root");
  if (session) snprintf(t, sizeof t, "%s/.Trash", session->home);
  mkdir(t, 0700);
  open_path(NULL, t, false);
}
static void cmd_open_trash(long unused) { (void)unused; open_trash(); }

static void dock_click(int k) {
  if (k < 0 || k >= nditems) return;
  int i = ditems[k].app;
  if (i == DOCK_TRASH) {
    open_trash();
    return;
  }
  if (i < 0) return;
  Client *top = NULL;
  bool any_min = false;
  for (Client *c = clients; c; c = c->next)
    if (c->app == i) {
      if (!top) top = c;
      any_min |= c->minimized;
    }
  if (!top) {
    launch(i);
    return;
  }
  if (any_min)
    for (Client *c = clients; c; c = c->next)
      if (c->app == i && c->minimized) restore_client(c);
  restore_client(top);
}

static void cmd_keep(long i) {
  if (ndock_ids < 48 && !kept_in_dock((int)i)) snprintf(dock_ids[ndock_ids++], 128, "%s", apps[i].b.ident);
  save_prefs();
  draw_dock();
}

static void cmd_unkeep(long i) {
  int w = 0;
  for (int k = 0; k < ndock_ids; k++)
    if (strcmp(dock_ids[k], apps[i].b.ident)) memmove(dock_ids[w++], dock_ids[k], 128);
  ndock_ids = w;
  save_prefs();
  show_tip(-1);
  draw_dock();
}

static void cmd_show_in_fundus(long i) { open_path(NULL, apps[i].b.path, true); }

static void dock_menu(int k) {
  if (k < 0 || k >= nditems) return;
  int i = ditems[k].app;
  nmenu = 0;
  if (i == DOCK_TRASH) {
    add_head(L("Trash"));
    add_item(L("Open"), cmd_open_trash, 0, false);
  } else if (i >= 0) {
    add_head(apps[i].b.name);
    bool running = app_running(i);
    add_item(running ? L("New Window") : L("Open"), launch_app, i, false);
    add_sep();
    if (i != app_fundus) {
      bool kept = kept_in_dock(i);
      add_item(kept ? L("Remove from Dock") : L("Keep in Dock"), kept ? cmd_unkeep : cmd_keep, i, false);
    }
    add_item(L("Show in Fundus"), cmd_show_in_fundus, i, false);
    if (running) {
      add_sep();
      add_item(L("Hide"), cmd_hide_app, i, false);
      add_item(L("Quit"), cmd_quit_app, i, false);
    }
  } else {
    return;
  }
  double cx, cy;
  item_center(k, &cx, &cy);
  if (dock_pos == POS_BOTTOM) open_menu((int)cx - ds / 2, dock_y - 8, -1, true);
  else if (dock_pos == POS_LEFT) open_menu(dock_x + dock_w + 8, (int)cy - ds / 2, -1, false);
  else open_menu(dock_x - 8 - menu_width(), (int)cy - ds / 2, -1, false);
}

/* NT: Start-Menü über der Start-Schaltfläche: alle Programme, Einstellungen, Ausschalten */
static void nt_start_menu(void) {
  nmenu = 0;
  add_head(L("Apps"));
  for (int i = 0; i < napps; i++)
    if (!apps[i].gone && !apps[i].b.hidden && !apps[i].b.internal[0] && nmenu < MENU_MAX - 12) add_item(apps[i].b.name, launch_app, i, false);
  add_sep();
  add_item(L("System Settings…"), show_settings, -1, false);
  add_sep();
  add_item(L("Sleep"), cmd_sleep, 0, false);
  add_item(L("Restart…"), confirm_power, 1, false);
  add_item(L("Shut Down…"), confirm_power, 0, false);
  char buf[160];
  snprintf(buf, sizeof buf, L("Log Out “%s”…"), session_name());
  add_item(buf, cmd_logout, 0, false);
  open_menu(0, dock_y - 2, -1, true);
}

/* ---------------------------------------------------------------------------------------- */
/* Einstellungen des Benutzers wie bei macOS in ~/Library/Preferences: das Dock in               */
/* com.clokeos.dock.plist (orientation, tilesize, persistent-apps wie com.apple.dock), das       */
/* Erscheinungsbild in .GlobalPreferences.plist (AppleInterfaceStyle). Grundeinstellung des      */
/* Docks aus /Library/Preferences/com.clokeos.dock.plist.                                        */

static void pref_path(char *out, size_t n, const char *file) {
  snprintf(out, n, "%s/Library/Preferences/%s", getenv("HOME") ? getenv("HOME") : "/var/root", file);
}

static void own_by_session(const char *path) {
  if (session && chown(path, session->uid, session->gid)) warn("chown %s: %s", path, strerror(errno));
}

static void load_prefs(void) {
  char p[PATH_MAX], v[64];
  size_t n = 0;
  pref_path(p, sizeof p, "com.clokeos.dock.plist");
  unsigned char *d = read_bytes(p, &n);
  if (!d) d = read_bytes("/Library/Preferences/com.clokeos.dock.plist", &n);
  int k = d ? plist_get_array(d, "persistent-apps", dock_ids, 48) : -1;
  ndock_ids = k > 0 ? k : 0;
  dock_pos = POS_BOTTOM;
  if (plist_get(d, n, "orientation", v, sizeof v)) dock_pos = !strcmp(v, "left") ? POS_LEFT : !strcmp(v, "right") ? POS_RIGHT : POS_BOTTOM;
  ds = ds_pref = 50;
  if (plist_get(d, n, "tilesize", v, sizeof v) && atoi(v) >= 32 && atoi(v) <= 96) ds = ds_pref = atoi(v);
  free(d);
  pref_path(p, sizeof p, ".GlobalPreferences.plist");
  d = read_bytes(p, &n);
  theme_mode = 0; /* wie im Prototyp: automatisch */
  wall_sel = 0;
  nt = nt_sel = false;
  if (d && plist_get(d, n, "ClokeWallpaper", v, sizeof v)) wall_sel = wall_find(v);
  if (d) {
    if (plist_get(d, n, "ClokeUI", v, sizeof v)) nt = nt_sel = !strcmp(v, "NT");
    if (plist_get(d, n, "AppleInterfaceStyle", v, sizeof v) && !strcmp(v, "Dark")) theme_mode = 2;
    else if (!plist_get(d, n, "AppleInterfaceStyleSwitchesAutomatically", v, sizeof v) || !strcmp(v, "0")) theme_mode = 1;
  }
  free(d);
  if (nt) ds = ds_pref = 32, dock_pos = POS_BOTTOM; /* Taskleiste: unten, 44 Pixel hoch */
}

static void save_prefs(void) {
  if (!session) return;
  char p[PATH_MAX];
  const char *home = getenv("HOME") ? getenv("HOME") : "/var/root";
  snprintf(p, sizeof p, "%s/Library", home);
  if (!mkdir(p, 0755)) own_by_session(p);
  snprintf(p, sizeof p, "%s/Library/Preferences", home);
  if (!mkdir(p, 0755)) own_by_session(p);
  static const char *pos_name[] = {"bottom", "left", "right"};
  const char *ids[48];
  for (int k = 0; k < ndock_ids; k++) ids[k] = dock_ids[k];
  PlistItem dk[] = {{.key = "orientation", .s = pos_name[dock_pos]}, {.key = "tilesize", .i = ds_pref},
                    {.key = "persistent-apps", .arr = ids, .narr = ndock_ids}};
  pref_path(p, sizeof p, "com.clokeos.dock.plist");
  if (plist_write(p, dk, 3)) own_by_session(p);
  else warn("kann %s nicht schreiben", p);
  PlistItem g[4];
  int ng = 0;
  if (nt_sel) g[ng++] = (PlistItem){.key = "ClokeUI", .s = "NT"};
  g[ng++] = (PlistItem){.key = "ClokeWallpaper", .s = wall_key(wall_sel)};
  if (theme_mode == 2) g[ng++] = (PlistItem){.key = "AppleInterfaceStyle", .s = "Dark"};
  g[ng++] = (PlistItem){.key = "AppleInterfaceStyleSwitchesAutomatically", .i = theme_mode == 0};
  pref_path(p, sizeof p, ".GlobalPreferences.plist");
  if (plist_write(p, g, ng)) own_by_session(p);
}

static void set_dock_pos(long pos) {
  dock_pos = (int)pos;
  show_tip(-1);
  draw_dock();
  update_workarea();
  save_prefs();
}

/* Entfernte Anmeldung (SSH), wie bei macOS unter Freigaben. sshd startet launchd über
 * /usr/libexec/clokeos-sshd, das /etc/clokeos-remote-login liest; launchctl stop startet es neu. */
#define REMOTE_LOGIN_FILE "/etc/clokeos-remote-login"
static bool remote_login_on(void) {
  char *s = read_file(REMOTE_LOGIN_FILE);
  bool on = !s || strncmp(s, "off", 3);
  free(s);
  return on;
}

static void set_remote_login(bool on) {
  FILE *f = fopen(REMOTE_LOGIN_FILE, "w");
  if (f) {
    fputs(on ? "on\n" : "off\n", f);
    fclose(f);
  }
  run_wait("/bin/launchctl stop org.puredarwin.sshd >/dev/null 2>&1");
}

/* Sprache umstellen (Systemeinstellungen, Einrichtungsassistent): CloakServer wechselt sofort,
 * die Namen der Programme kommen neu aus ihren Paketen, neue Programme starten mit dem neuen LANG */
/* Tastaturbelegung: wie macOS für das ganze System. xkbcomp setzt sie über XKB für alle Programme. Ohne
 * Auswahl folgt sie der Sprache (KEYBOARD_FILE fehlt). */
#define KEYBOARD_FILE "/etc/clokeos-keyboard"
static const struct { const char *code, *title; } KBD[] = {
    {"us", "U.S."}, {"gb", "British"}, {"de", "Deutsch"}, {"fr", "Français"}, {"es", "Español"}, {"it", "Italiano"},
    {"pt", "Português"}, {"nl", "Nederlands"}, {"ch", "Schweiz"}, {"at", "Österreich"}, {"pl", "Polski"},
    {"se", "Svenska"}, {"tr", "Türkçe"}, {"ru", "Русский"}};
#define NKBD (int)(sizeof KBD / sizeof KBD[0])
static char kbd_cur[8] = "us";

static void apply_keyboard(void) {
  FILE *f = fopen("/tmp/clokeos-xkb.src", "w");
  if (!f) return;
  fprintf(f,
          "xkb_keymap {\n xkb_keycodes { include \"xfree86+aliases(qwerty)\" };\n xkb_types { include \"complete\" };\n"
          " xkb_compat { include \"complete\" };\n xkb_symbols { include \"pc+%s%s+inet(pc105)\" };\n};\n",
          kbd_cur, !strcmp(kbd_cur, "ru") ? "+us:2+group(alt_shift_toggle)" : "");
  fclose(f);
  char cmd[200];
  snprintf(cmd, sizeof cmd, "/usr/bin/xkbcomp -w0 /tmp/clokeos-xkb.src %s >/dev/null 2>&1", getenv("DISPLAY") ? getenv("DISPLAY") : ":0");
  if (run_wait(cmd)) warn("xkbcomp für Belegung %s schlug fehl", kbd_cur);
}

static void kbd_default_for_language(void) {
  const char *lg = l10n_lang();
  snprintf(kbd_cur, sizeof kbd_cur, "%s", !strcmp(lg, "de") ? "de" : !strcmp(lg, "fr") ? "fr" : !strcmp(lg, "es") ? "es" : !strcmp(lg, "it") ? "it" : "us");
}

static void load_keyboard(void) {
  char *s = read_file(KEYBOARD_FILE);
  if (s && s[0]) {
    s[strcspn(s, " \t\r\n")] = 0;
    snprintf(kbd_cur, sizeof kbd_cur, "%s", s);
  } else {
    kbd_default_for_language();
  }
  free(s);
  apply_keyboard();
}

static void set_keyboard(int i) {
  snprintf(kbd_cur, sizeof kbd_cur, "%s", KBD[i].code);
  FILE *f = fopen(KEYBOARD_FILE, "w");
  if (f) {
    fprintf(f, "%s\n", kbd_cur);
    fclose(f);
  }
  apply_keyboard();
}

static void set_language(const char *code) {
  if (!l10n_set_lang(code)) warn("kann %s nicht schreiben", LANGUAGE_FILE);
  if (access(KEYBOARD_FILE, F_OK)) {
    kbd_default_for_language();
    apply_keyboard();
  }
  setenv("LANG", l10n_locale(), 1);
  for (int i = 0; i < napps; i++) apps[i].b.mtime = -1; /* Namen neu lesen */
  scan_apps();
  for (Client *c = clients; c; c = c->next) /* Titel der eigenen Fenster */
    if (c->draw && c->title_en) {
      const char *t = L(c->title_en);
      XStoreName(dpy, c->win, t);
      XChangeProperty(dpy, c->win, NET_WM_NAME, UTF8_STRING, 8, PropModeReplace, (const unsigned char *)t, strlen(t));
      snprintf(c->title, sizeof c->title, "%s", t);
    }
  redraw_all();
}

static void set_dock_size(long size) {
  ds = ds_pref = (int)size;
  show_tip(-1);
  render_icons();
  draw_dock();
  update_workarea();
  save_prefs();
}

/* Absatz mit Zeilenumbruch; gibt die Höhe zurück */
static int text_block(cairo_t *cr, double x, double y, const char *s, double px, bool bold, RGBA col, int width) {
  PangoLayout *l = pango_cairo_create_layout(cr);
  PangoFontDescription *f = pango_font_description_from_string(bold ? UI_FONT " SemiBold" : UI_FONT);
  pango_font_description_set_absolute_size(f, px * PANGO_SCALE);
  pango_layout_set_font_description(l, f);
  pango_layout_set_width(l, width * PANGO_SCALE);
  pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_text(l, s, -1);
  int w, h;
  pango_layout_get_pixel_size(l, &w, &h);
  cairo_move_to(cr, x, y);
  set(cr, col);
  pango_cairo_show_layout(cr, l);
  pango_font_description_free(f);
  g_object_unref(l);
  return h;
}

static void button(cairo_t *cr, double x, double y, double w, const char *label, bool primary) {
  rrect(cr, x, y, w, 28, 14);
  if (primary) {
    set(cr, ACCENT);
    cairo_fill(cr);
  } else {
    set(cr, dark ? (RGBA){1, 1, 1, .12} : (RGBA){1, 1, 1, .95});
    cairo_fill_preserve(cr);
    set(cr, T->line_strong);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
  }
  text(cr, x + w / 2, y + 14, label, 12.5, false, primary ? (RGBA){1, 1, 1, 1} : T->fg, 0, 1);
}

/* ---------------------------------------------------------------------------------------- */
/* Hinweis, wenn sich ein Programm nicht öffnen lässt, wie bei macOS. Vor allem Mac-Programme:  */
/* Sie liegen als Mach-O für x86_64 oder arm64 im Bundle und laden Frameworks wie AppKit.       */

static void problem_text(const App *a, char *buf, size_t n) {
  switch (a->b.kind) {
  case APP_APPKIT:
    snprintf(buf, n, L("This is a Mac app with a graphical user interface. It needs %s, which is not installed on "
                       "this ClokeOS (the Mac frameworks in /Library/ClokeOS/MacFrameworks are missing)."),
             a->b.missing);
    break;
  case APP_ARCH:
#if defined(__arm64__) || defined(__aarch64__)
    snprintf(buf, n, "%s", L("The app is built only for Macs with an Intel processor (x86_64). This ClokeOS runs on arm64 "
                             "and has no Rosetta; it needs a version for Apple chips or a universal version."));
#else
    snprintf(buf, n, "%s", L("The app is built only for Macs with Apple silicon (arm64). This ClokeOS runs on Intel "
                             "(x86_64); it needs an Intel or universal version."));
#endif
    break;
  case APP_ARM64E:
    snprintf(buf, n, "%s", L("The app is built only for arm64e, the variant with pointer authentication that Apple uses "
                             "for its own apps. ClokeOS does not run arm64e; it needs an arm64 or universal version."));
    break;
  case APP_MISSING:
    snprintf(buf, n, L("The Mac app needs %s, which ClokeOS does not have."), a->b.missing);
    break;
  default:
    snprintf(buf, n, L("The app bundle is incomplete: %s."), a->b.missing);
  }
}

static void paint_problem(Client *c, cairo_t *cr) {
  if (c->data < 0 || c->data >= napps) return;
  const App *a = &apps[c->data];
  cairo_surface_t *ic = bundle_icon(&a->b, 64, NULL);
  cairo_set_source_surface(cr, ic, 22, 22);
  cairo_paint(cr);
  cairo_surface_destroy(ic);
  char t[200], body[600];
  snprintf(t, sizeof t, L("“%s” can’t be opened."), a->b.name);
  text(cr, 104, 34, t, 13, true, T->fg, c->w - 124, 0);
  problem_text(a, body, sizeof body);
  text_block(cr, 104, 50, body, 11.5, false, T->fg2, c->w - 124);
  button(cr, c->w - 20 - 80, c->h - 44, 80, "OK", true);
}

static void click_problem(Client *c, int x, int y) {
  if (x >= c->w - 100 && x <= c->w - 20 && y >= c->h - 44 && y <= c->h - 16) close_client(c);
}

static void show_app_problem(int i) { own_window(500, 168, "App", paint_problem, click_problem, i); }

/* ---------------------------------------------------------------------------------------- */
/* Launchpad („Programme“) wie #launchpad in clokeos.html: alle Programme als Raster über dem   */
/* unscharfen Hintergrund, oben ein Suchfeld. Tippen sucht, die Eingabetaste startet das erste, */
/* Esc oder ein Klick daneben schließt. Die Menüleiste verschwindet, das Dock bleibt.           */

#define LP_ICON 76
static char lp_query[64];
static int lp_n, lp_app[96], lp_x[96], lp_y[96], lp_cell;

static int lp_cmp(const void *a, const void *b) {
  return strcasecmp(apps[*(const int *)a].b.name, apps[*(const int *)b].b.name);
}

static bool lp_match(const char *name) {
  if (!lp_query[0]) return true;
  size_t n = strlen(lp_query);
  for (const char *p = name; *p; p++)
    if (!strncasecmp(p, lp_query, n)) return true;
  return false;
}

static void paint_launchpad(cairo_t *cr, int w, int h, void *arg) {
  (void)arg, (void)h;
  cairo_save(cr);
  cairo_scale(cr, 4, 4);
  cairo_set_source_surface(cr, wall_blur, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_set_source_rgba(cr, 20 / 255., 12 / 255., 36 / 255., dark ? .35 : .22);
  cairo_paint(cr);

  /* Suchfeld */
  double fx = (w - 240) / 2.0, fy = 48;
  rrect(cr, fx, fy, 240, 28, 8);
  cairo_set_source_rgba(cr, 1, 1, 1, .2);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, .35);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  if (lp_query[0]) text(cr, w / 2.0, fy + 14, lp_query, 13, false, (RGBA){1, 1, 1, 1}, 220, 1);
  else text(cr, w / 2.0, fy + 14, L("Search"), 13, false, (RGBA){1, 1, 1, .7}, 0, 1);

  int list[96];
  lp_n = 0;
  for (int i = 0; i < napps && lp_n < 96; i++)
    if (!apps[i].gone && !apps[i].b.hidden && strcmp(apps[i].b.internal, "launchpad") && i != app_fundus &&
        lp_match(apps[i].b.name))
      list[lp_n++] = i;
  qsort(list, lp_n, sizeof list[0], lp_cmp);
  int gw = w - 32 < 900 ? w - 32 : 900, cols = (gw + 18) / (130 + 18);
  if (cols < 1) cols = 1;
  lp_cell = (gw - (cols - 1) * 18) / cols;
  double x0 = (w - gw) / 2.0, y0 = fy + 28 + 44;
  for (int j = 0; j < lp_n; j++) {
    App *a = &apps[list[j]];
    double cx = x0 + (j % cols) * (lp_cell + 18) + lp_cell / 2.0, y = y0 + (j / cols) * (LP_ICON + 8 + 16 + 28);
    lp_app[j] = list[j], lp_x[j] = (int)cx, lp_y[j] = (int)y;
    if (!a->icon_lp) a->icon_lp = bundle_icon(&a->b, LP_ICON, cal_subst());
    cairo_set_source_surface(cr, a->icon_lp, cx - LP_ICON / 2.0, y);
    cairo_paint(cr);
    if (a->b.kind != APP_OK) { /* lässt sich nicht öffnen: Verbotszeichen wie im Finder */
      double bx = cx + LP_ICON / 2.0 - 12, by = y + LP_ICON - 12;
      cairo_new_path(cr);
      cairo_arc(cr, bx, by, 11, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 0, 0, 0, .55);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 1, 1, 1);
      cairo_set_line_width(cr, 2.2);
      cairo_arc(cr, bx, by, 7, 0, 2 * M_PI);
      cairo_stroke(cr);
      cairo_move_to(cr, bx - 5, by + 5);
      cairo_line_to(cr, bx + 5, by - 5);
      cairo_stroke(cr);
    }
    text(cr, cx, y + LP_ICON + 8 + 9, a->b.name, 13, false, (RGBA){0, 0, 0, .35}, lp_cell + 16, 1);
    text(cr, cx, y + LP_ICON + 8 + 8, a->b.name, 13, false, (RGBA){1, 1, 1, 1}, lp_cell + 16, 1);
  }
  if (!lp_n) text(cr, w / 2.0, y0 + 40, L("No Results"), 15, false, (RGBA){1, 1, 1, .8}, 0, 1);
}

static void draw_launchpad(void) {
  if (lp_open) paint_window(lpwin, SW, SH, paint_launchpad, NULL);
}

static void show_launchpad(void) {
  if (locked) return;
  close_menu();
  show_tip(-1);
  if (!lpwin) lpwin = ui_window(0, 0, SW, SH, ExposureMask | ButtonPressMask | KeyPressMask, false, NET_WM_WINDOW_TYPE_SPLASH);
  lp_query[0] = 0;
  lp_open = true;
  scan_apps();
  XMapRaised(dpy, lpwin);
  XRaiseWindow(dpy, dock);
  XSync(dpy, False);
  XSetInputFocus(dpy, lpwin, RevertToPointerRoot, CurrentTime);
  XGrabKeyboard(dpy, lpwin, True, GrabModeAsync, GrabModeAsync, CurrentTime);
  draw_launchpad();
}

static void hide_launchpad(void) {
  if (!lp_open) return;
  lp_open = false;
  XUngrabKeyboard(dpy, CurrentTime);
  XUnmapWindow(dpy, lpwin);
  XRaiseWindow(dpy, menubar);
  focus_client(focused);
}

static void lp_click(int x, int y) {
  for (int j = 0; j < lp_n; j++)
    if (abs(x - lp_x[j]) < lp_cell / 2 && y >= lp_y[j] - 4 && y < lp_y[j] + LP_ICON + 30) {
      int i = lp_app[j];
      hide_launchpad();
      launch(i);
      return;
    }
  if (abs(x - SW / 2) < 120 && y >= 48 && y <= 76) return; /* ins Suchfeld */
  hide_launchpad();
}

static void lp_key(XKeyEvent *e) {
  char buf[16];
  KeySym ks = NoSymbol;
  int n = XLookupString(e, buf, sizeof buf, &ks, NULL);
  size_t l = strlen(lp_query);
  if (ks == XK_Escape) {
    if (!l) {
      hide_launchpad();
      return;
    }
    lp_query[0] = 0;
  } else if (ks == XK_Return || ks == XK_KP_Enter) {
    if (lp_n > 0) {
      int i = lp_app[0];
      hide_launchpad();
      launch(i);
    }
    return;
  } else if (ks == XK_BackSpace) {
    while (l > 0 && (lp_query[l - 1] & 0xc0) == 0x80) l--; /* ganzes UTF-8-Zeichen */
    if (l > 0) l--;
    lp_query[l] = 0;
  } else if (n == 1 && (unsigned char)buf[0] >= 0xa0 && l + 2 < sizeof lp_query) { /* Latin-1 -> UTF-8 */
    lp_query[l] = (char)(0xc0 | (unsigned char)buf[0] >> 6);
    lp_query[l + 1] = (char)(0x80 | ((unsigned char)buf[0] & 0x3f));
    lp_query[l + 2] = 0;
  } else if (n == 1 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] < 127 && l + 1 < sizeof lp_query) {
    lp_query[l] = buf[0];
    lp_query[l + 1] = 0;
  } else {
    return;
  }
  draw_launchpad();
}


/* ---------------------------------------------------------------------------------------- */
/* Spotlight wie #spot in clokeos.html: ein Suchfeld über dem Bildschirm, darunter Treffer: Rechnen,   */
/* Programme, Suche mit Search, Einstellungen, Dateien und Ordner im Home-Verzeichnis.                 */

#define SP_W 620
#define SP_IN 54
#define SP_ITEM 38
#define SP_HEAD 24
#define SP_MAX 24
enum { SR_CALC, SR_APP, SR_WEB, SR_SET, SR_FILE };
typedef struct {
  int type, app, sec;
  const char *kind;
  char label[128], sub[160], path[PATH_MAX];
  bool dir;
} SpRes;
static char sp_query[96];
static SpRes sp_res[SP_MAX];
static int sp_n, sp_cur;

/* kleiner Rechner für Ausdrücke wie 12*7, (3+4)^2, 15% von 80 geht nicht: nur + - * / ^ % und Klammern */
static const char *ep;
static bool eok;
static double e_expr(void);
static void e_ws(void) {
  while (*ep == ' ') ep++;
}
static double e_atom(void) {
  e_ws();
  if (*ep == '(') {
    ep++;
    double v = e_expr();
    e_ws();
    if (*ep == ')') ep++;
    else eok = false;
    return v;
  }
  if (*ep == '-') {
    ep++;
    return -e_atom();
  }
  if (*ep == '+') {
    ep++;
    return e_atom();
  }
  char *end;
  double v = strtod(ep, &end);
  if (end == ep) {
    eok = false;
    return 0;
  }
  ep = end;
  return v;
}
static double e_pow(void) {
  double b = e_atom();
  e_ws();
  if (*ep == '^') {
    ep++;
    return pow(b, e_pow());
  }
  return b;
}
static double e_term(void) {
  double v = e_pow();
  for (;;) {
    e_ws();
    if (*ep == '*' || *ep == 'x') ep++, v *= e_pow();
    else if (*ep == '/') {
      ep++;
      double d = e_pow();
      if (d == 0) eok = false;
      else v /= d;
    } else if (*ep == '%') {
      ep++;
      double d = e_pow();
      if (d == 0) eok = false;
      else v = fmod(v, d);
    } else return v;
  }
}
static double e_expr(void) {
  double v = e_term();
  for (;;) {
    e_ws();
    if (*ep == '+') ep++, v += e_term();
    else if (*ep == '-') ep++, v -= e_term();
    else return v;
  }
}
static bool calc_eval(const char *s, double *out) {
  bool op = false, digit = false;
  for (const char *p = s; *p; p++) {
    if (strchr("+-*/^%(x", *p)) op = true;
    if (isdigit((unsigned char)*p)) digit = true;
    if (!isdigit((unsigned char)*p) && !strchr("+-*/^%(). xX,", *p)) return false;
  }
  if (!op || !digit) return false;
  char t[96];
  snprintf(t, sizeof t, "%s", s);
  for (char *p = t; *p; p++)
    if (*p == ',') *p = '.';
    else if (*p == 'X') *p = 'x';
  ep = t;
  eok = true;
  *out = e_expr();
  e_ws();
  return eok && !*ep && isfinite(*out);
}

static bool ci_has(const char *hay, const char *needle) {
  if (!*needle) return true;
  size_t n = strlen(needle);
  for (const char *p = hay; *p; p++)
    if (!strncasecmp(p, needle, n)) return true;
  return false;
}

static void sp_add(SpRes *r) {
  if (sp_n < SP_MAX) sp_res[sp_n++] = *r;
}

static void sp_walk(const char *dir, int depth, int *budget) {
  DIR *d = opendir(dir);
  if (!d) return;
  for (struct dirent *e; (e = readdir(d)) && *budget > 0 && sp_n < SP_MAX;) {
    if (e->d_name[0] == '.') continue;
    (*budget)--;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st)) continue;
    bool isdir = S_ISDIR(st.st_mode);
    size_t n = strlen(e->d_name);
    if (isdir && n > 4 && !strcmp(e->d_name + n - 4, ".app")) continue;
    if (ci_has(e->d_name, sp_query)) {
      SpRes r = {.type = SR_FILE, .kind = isdir ? "Folders" : "Documents", .dir = isdir};
      snprintf(r.label, sizeof r.label, "%s", e->d_name);
      const char *home = getenv("HOME") ? getenv("HOME") : "";
      snprintf(r.sub, sizeof r.sub, "%s", strncmp(dir, home, strlen(home)) ? dir : dir + strlen(home));
      if (!r.sub[0]) snprintf(r.sub, sizeof r.sub, "~");
      snprintf(r.path, sizeof r.path, "%s", p);
      sp_add(&r);
    }
    if (isdir && depth > 0 && strcmp(e->d_name, "Library")) sp_walk(p, depth - 1, budget);
  }
  closedir(d);
}

static void sp_build(void) {
  sp_n = 0;
  sp_cur = 0;
  if (!sp_query[0]) return;
  double v;
  if (calc_eval(sp_query, &v)) {
    SpRes r = {.type = SR_CALC, .kind = "Calculator"};
    snprintf(r.label, sizeof r.label, "%s = %.10g", sp_query, v);
    snprintf(r.sub, sizeof r.sub, "%s", L("Open in Calculator"));
    r.app = app_by_ident("com.clokeos.rechner");
    sp_add(&r);
  }
  for (int i = 0; i < napps; i++) {
    if (apps[i].gone || apps[i].b.hidden || !strcmp(apps[i].b.internal, "launchpad")) continue;
    if (!ci_has(apps[i].b.name, sp_query)) continue;
    SpRes r = {.type = SR_APP, .app = i, .kind = "Applications"};
    snprintf(r.label, sizeof r.label, "%s", apps[i].b.name);
    snprintf(r.sub, sizeof r.sub, "/Applications");
    sp_add(&r);
  }
  {
    SpRes r = {.type = SR_WEB, .kind = "Search", .app = app_by_ident("com.clokeos.search")};
    snprintf(r.label, sizeof r.label, L("Search for “%s” with Search"), sp_query);
    snprintf(r.sub, sizeof r.sub, "%s", L("Browser"));
    sp_add(&r);
  }
  static const struct { const char *name; int sec; } SET[] = {
      {"General", 0}, {"Appearance", 1}, {"Wallpaper", 2}, {"Desktop & Dock", 3}, {"Keyboard", 4}, {"Language & Region", 5}, {"Sharing", 7}};
  for (size_t k = 0; k < sizeof SET / sizeof SET[0]; k++)
    if (ci_has(SET[k].name, sp_query) || ci_has(L(SET[k].name), sp_query)) {
      SpRes r = {.type = SR_SET, .kind = "System Settings", .sec = SET[k].sec};
      snprintf(r.label, sizeof r.label, "%s", L(SET[k].name));
      snprintf(r.sub, sizeof r.sub, "%s", L("System Settings"));
      sp_add(&r);
    }
  if (strlen(sp_query) >= 2) {
    int budget = 2500;
    sp_walk(getenv("HOME") ? getenv("HOME") : "/var/root", 3, &budget);
  }
}

static int sp_height(void) {
  if (!sp_n) return sp_query[0] ? SP_IN + 40 : SP_IN;
  int h = SP_IN + 12;
  const char *last = "";
  for (int i = 0; i < sp_n; i++) {
    if (strcmp(sp_res[i].kind, last)) h += SP_HEAD, last = sp_res[i].kind;
    h += SP_ITEM;
  }
  return h;
}

static void paint_spot(cairo_t *cr, int w, int h, void *arg) {
  (void)arg;
  int sx, sy;
  Window child;
  XTranslateCoordinates(dpy, spotwin, root, 0, 0, &sx, &sy, &child);
  glass(cr, sx, sy, (RGBA){T->tint.r, T->tint.g, T->tint.b, (dark ? .88 : .82) - (comp_on ? .08 : 0)}, w, h, 26);
  rim(cr, 0, 0, w, h, 26);
  /* Lupe */
  set(cr, T->fg2);
  cairo_set_line_width(cr, 1.8);
  cairo_arc(cr, 31, 26, 7.5, 0, 2 * M_PI);
  cairo_stroke(cr);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to(cr, 36.5, 31.5);
  cairo_line_to(cr, 42, 37);
  cairo_stroke(cr);
  if (sp_query[0]) text(cr, 56, SP_IN / 2.0, sp_query, 22, false, T->fg, w - 80, 0);
  else text(cr, 56, SP_IN / 2.0, L("Search"), 22, false, T->fg3, w - 80, 0);
  if (!sp_n && !sp_query[0]) return;
  cairo_rectangle(cr, 0, SP_IN, w, 1);
  set(cr, T->line);
  cairo_fill(cr);
  if (!sp_n) {
    char b[160];
    snprintf(b, sizeof b, L("No results for “%s”"), sp_query);
    text(cr, 22, SP_IN + 20, b, 12, true, T->fg3, w - 40, 0);
    return;
  }
  int y = SP_IN + 6;
  const char *last = "";
  for (int i = 0; i < sp_n; i++) {
    SpRes *r = &sp_res[i];
    if (strcmp(r->kind, last)) {
      text(cr, 22, y + SP_HEAD / 2.0 + 2, L(r->kind), 11, true, T->fg3, w - 40, 0);
      y += SP_HEAD;
      last = r->kind;
    }
    if (i == sp_cur) {
      rrect(cr, 6, y, w - 12, SP_ITEM - 2, 8);
      set(cr, ACCENT);
      cairo_fill(cr);
    }
    bool hl = i == sp_cur;
    double ix = 16, iy = y + (SP_ITEM - 2 - 28) / 2.0;
    int ai = r->type == SR_APP || r->type == SR_CALC || r->type == SR_WEB ? r->app : r->type == SR_SET ? app_by_ident("com.clokeos.systemeinstellungen") : -1;
    if (ai >= 0 && ai < napps) {
      App *a = &apps[ai];
      if (!a->icon_lp) a->icon_lp = bundle_icon(&a->b, LP_ICON, cal_subst());
      cairo_save(cr);
      cairo_translate(cr, ix, iy);
      cairo_scale(cr, 28.0 / LP_ICON, 28.0 / LP_ICON);
      cairo_set_source_surface(cr, a->icon_lp, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    } else {
      rrect(cr, ix, iy, 28, 28, 7);
      set(cr, r->dir ? (RGBA){.35, .6, 1, 1} : (RGBA){.7, .72, .78, 1});
      cairo_fill(cr);
      text(cr, ix + 14, iy + 14, r->dir ? "▭" : "≡", 14, true, (RGBA){1, 1, 1, 1}, 0, 1);
    }
    int lw = text(cr, 54, y + (SP_ITEM - 2) / 2.0, r->label, 13, true, hl ? (RGBA){1, 1, 1, 1} : T->fg, w - 80, 0);
    if (lw < w - 200) text(cr, 54 + lw + 10, y + (SP_ITEM - 2) / 2.0, r->sub, 11.5, false, hl ? (RGBA){1, 1, 1, .8} : T->fg3, w - lw - 80, 0);
    y += SP_ITEM;
  }
}

static void draw_spot(void) {
  if (!sp_open) return;
  int h = sp_height();
  XResizeWindow(dpy, spotwin, SP_W, h);
  paint_window(spotwin, SP_W, h, paint_spot, NULL);
}

static void hide_spot(void) {
  if (!sp_open) return;
  sp_open = false;
  XUngrabPointer(dpy, CurrentTime);
  XUngrabKeyboard(dpy, CurrentTime);
  XDestroyWindow(dpy, spotwin);
  spotwin = None;
  focus_client(focused);
}

static void launch_with(int i, const char *arg) {
  if (i < 0 || i >= napps || apps[i].gone || apps[i].b.kind != APP_OK) return;
  char q[2600], a[1200], cmd[3900];
  sh_quote(q, sizeof q, apps[i].b.exec);
  sh_quote(a, sizeof a, arg);
  snprintf(cmd, sizeof cmd, apps[i].b.macfw ? "%s -NSOpen %s" : "%s %s", q, a);
  apps[i].bounce_t0 = now_s();
  apps[i].pid = spawn_env(cmd, false, apps[i].b.macfw);
  draw_dock();
}

/* ---- Öffnen wie im Finder (open-Befehl, Fundus, Spotlight, Dock) ---------------------------------------
 * Programme (.app) starten, Ordner in Fundus zeigen, Dokumente mit ihrem Standardprogramm (gio open, mimeapps.list).
 * with: Kennung oder Name eines Programms, das den Pfad öffnen soll. reveal: in Fundus zeigen statt öffnen.
 * Aufträge kommen von /usr/bin/open (cloakserver/open.c) als Eigenschaft _CLOKE_OPEN am Root-Fenster:
 * Zeilen "with<TAB>flags<TAB>pfad"; CloakServer liest und löscht sie in einem Schritt. */
static Atom CLOKE_OPEN;

static int app_by_path_or_name(const char *s) {
  for (int i = 0; i < napps; i++)
    if (!apps[i].gone && (!strcmp(apps[i].b.path, s) || !strcmp(apps[i].b.ident, s) || !strcasecmp(apps[i].b.name, s))) return i;
  for (int i = 0; i < napps; i++) {     /* "Notes" für Notes.app */
    const char *b = strrchr(apps[i].b.path, '/');
    char n[256];
    snprintf(n, sizeof n, "%s", b ? b + 1 : apps[i].b.path);
    char *dot = strrchr(n, '.');
    if (dot) *dot = 0;
    if (!apps[i].gone && !strcasecmp(n, s)) return i;
  }
  return -1;
}

static void open_path(const char *with, const char *path, bool reveal) {
  if (with && *with) {
    int i = app_by_path_or_name(with);
    if (i < 0) { warn("open: kein Programm %s", with); return; }
    if (path && *path) launch_with(i, path); else launch(i);
    return;
  }
  if (!path || !*path) return;
  struct stat st;
  if (stat(path, &st) != 0) { warn("open: %s fehlt", path); return; }
  size_t n = strlen(path);
  bool isapp = S_ISDIR(st.st_mode) && n > 4 && !strcasecmp(path + n - 4, ".app");
  if (reveal || (S_ISDIR(st.st_mode) && !isapp)) {
    if (app_fundus >= 0 && !apps[app_fundus].gone) launch_with(app_fundus, path);
    return;
  }
  if (isapp) {
    int i = app_by_path_or_name(path);
    if (i < 0) { add_bundle(path); i = app_by_path_or_name(path); }
    if (i >= 0) launch(i);
    return;
  }
  char q[PATH_MAX * 2 + 8], cmd[PATH_MAX * 2 + 32];
  sh_quote(q, sizeof q, path);
  snprintf(cmd, sizeof cmd, "gio open %s", q);
  spawn(cmd);
}

static void open_requests(void) {
  Atom type; int fmt; unsigned long nitems, after;
  unsigned char *data = NULL;
  if (XGetWindowProperty(dpy, root, CLOKE_OPEN, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &nitems, &after, &data) != Success || !data)
    return;
  char *save = NULL;
  for (char *line = strtok_r((char *)data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *t1 = strchr(line, '\t'); if (!t1) continue;
    *t1 = 0;
    char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue;
    *t2 = 0;
    const char *with = strcmp(line, "-") ? line : NULL, *flags = t1 + 1, *path = t2 + 1;
    if (debug) warn("open: with=%s flags=%s %s", with ? with : "-", flags, path);
    open_path(with, *path ? path : NULL, strchr(flags, 'R') != NULL);
  }
  XFree(data);
}

static void sp_activate(int i) {
  if (i < 0 || i >= sp_n) return;
  SpRes r = sp_res[i];
  char q[96];
  snprintf(q, sizeof q, "%s", sp_query);
  hide_spot();
  switch (r.type) {
  case SR_CALC:
  case SR_APP: launch(r.app); break;
  case SR_SET: show_settings(r.sec); break;
  case SR_WEB: {
    char enc[400] = "", url[520];
    size_t k = 0;
    for (const char *p = q; *p && k + 4 < sizeof enc; p++) {
      if (isalnum((unsigned char)*p) || strchr("-._~", *p)) enc[k++] = *p;
      else k += (size_t)snprintf(enc + k, sizeof enc - k, "%%%02X", (unsigned char)*p);
    }
    snprintf(url, sizeof url, "https://duckduckgo.com/?q=%s", enc);
    launch_with(r.app, url);
    break;
  }
  case SR_FILE: {
    open_path(NULL, r.path, false);
    break;
  }
  }
}

static void show_spot(void) {
  if (locked) return;
  if (sp_open) {
    hide_spot();
    return;
  }
  close_menu();
  show_tip(-1);
  if (lp_open) hide_launchpad();
  sp_query[0] = 0;
  sp_n = 0;
  sp_cur = 0;
  sp_open = true;
  scan_apps();
  spotwin = ui_window((SW - SP_W) / 2, SH * 22 / 100, SP_W, SP_IN,
                      ExposureMask | ButtonPressMask | KeyPressMask, true, NET_WM_WINDOW_TYPE_POPUP_MENU);
  XMapRaised(dpy, spotwin);
  XGrabPointer(dpy, spotwin, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, cur_arrow, CurrentTime);
  XGrabKeyboard(dpy, spotwin, True, GrabModeAsync, GrabModeAsync, CurrentTime);
  draw_spot();
}

static void sp_click(int x, int y) {
  if (x < 0 || y < 0 || x >= SP_W || y >= sp_height()) {
    hide_spot();
    return;
  }
  int yy = SP_IN + 6;
  const char *last = "";
  for (int i = 0; i < sp_n; i++) {
    if (strcmp(sp_res[i].kind, last)) yy += SP_HEAD, last = sp_res[i].kind;
    if (y >= yy && y < yy + SP_ITEM) {
      sp_activate(i);
      return;
    }
    yy += SP_ITEM;
  }
}

static void sp_key(XKeyEvent *e) {
  char buf[16];
  KeySym ks = NoSymbol;
  int n = XLookupString(e, buf, sizeof buf, &ks, NULL);
  size_t l = strlen(sp_query);
  if (ks == XK_Escape) {
    hide_spot();
    return;
  } else if (ks == XK_Return || ks == XK_KP_Enter) {
    sp_activate(sp_cur);
    return;
  } else if (ks == XK_Down) {
    if (sp_n) sp_cur = (sp_cur + 1) % sp_n;
  } else if (ks == XK_Up) {
    if (sp_n) sp_cur = (sp_cur + sp_n - 1) % sp_n;
  } else if (ks == XK_BackSpace) {
    while (l > 0 && (sp_query[l - 1] & 0xc0) == 0x80) l--;
    if (l > 0) l--;
    sp_query[l] = 0;
    sp_build();
  } else if (n == 1 && (unsigned char)buf[0] >= 0xa0 && l + 2 < sizeof sp_query) {
    sp_query[l] = (char)(0xc0 | (unsigned char)buf[0] >> 6);
    sp_query[l + 1] = (char)(0x80 | ((unsigned char)buf[0] & 0x3f));
    sp_query[l + 2] = 0;
    sp_build();
  } else if (n == 1 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] < 127 && l + 1 < sizeof sp_query) {
    sp_query[l] = buf[0];
    sp_query[l + 1] = 0;
    sp_build();
  } else {
    return;
  }
  draw_spot();
}

/* ---------------------------------------------------------------------------------------- */
/* Systemeinstellungen wie im Prototyp: links die Bereiche, rechts die Einstellungen.           */
/* Erscheinungsbild (Hell, Dunkel, Automatisch), Schreibtisch & Dock (Lage, Größe), Programme   */
/* (wie man installiert). Alles Übrige bleibt in den Xfce-Einstellungen.                        */

enum { ST_UI, ST_SECTION, ST_THEME, ST_DOCKPOS, ST_DOCKSIZE, ST_OPENAPPS, ST_LANG, ST_SSH, ST_WALL, ST_KBD };
static struct { int x, y, w, h, act; long arg; } st_hit[48];
static int nst_hit;
enum { SC_GENERAL, SC_APPEAR, SC_WALL, SC_DOCK, SC_KEYB, SC_LANG, SC_APPS, SC_SHARE };
static const char *const ST_NAMES[] = {"General", "Appearance", "Wallpaper", "Desktop & Dock", "Keyboard", "Language & Region", "Applications", "Sharing"};
#define ST_N 8
static const int ST_ORDER[ST_N] = {SC_GENERAL, SC_APPEAR, SC_WALL, SC_DOCK, SC_KEYB, SC_LANG, SC_APPS, SC_SHARE};
static const int DOCK_SIZES[3] = {40, 50, 64};

static void st_add(double x, double y, double w, double h, int act, long arg) {
  if (nst_hit < 48) st_hit[nst_hit++] = (typeof(st_hit[0])){(int)x, (int)y, (int)w, (int)h, act, arg};
}

/* kleines Bild eines Bildschirms: Hintergrund, Menüleiste, Fenster, Dock an der Stelle pos */
static void mini_screen(cairo_t *cr, double x, double y, double w, double h, int look, int pos) {
  cairo_save(cr);
  rrect(cr, x, y, w, h, 8);
  cairo_clip(cr);
  for (int half = 0; half < 2; half++) {
    bool d = look == 1 || (look == 2 && half == 1);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + h);
    if (d) {
      cairo_pattern_add_color_stop_rgb(g, 0, .17, .13, .25);
      cairo_pattern_add_color_stop_rgb(g, 1, .38, .23, .36);
    } else {
      cairo_pattern_add_color_stop_rgb(g, 0, .55, .72, .92);
      cairo_pattern_add_color_stop_rgb(g, 1, .96, .86, .80);
    }
    cairo_rectangle(cr, x + (look == 2 ? half * w / 2 : 0), y, look == 2 ? w / 2 : w, h);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    if (look != 2) break;
  }
  bool d = look == 1;
  cairo_rectangle(cr, x, y, w, 7);
  cairo_set_source_rgba(cr, d ? 0 : 1, d ? 0 : 1, d ? 0 : 1, .45);
  cairo_fill(cr);
  double wx = x + w * .2, wy = y + 16, ww = w * .55, wh = h * .5;
  rrect(cr, wx, wy, ww, wh, 4);
  if (d) cairo_set_source_rgb(cr, .14, .13, .17);
  else cairo_set_source_rgb(cr, .97, .96, .98);
  cairo_fill(cr);
  const RGBA L[3] = {L_CLOSE, L_MIN, L_ZOOM};
  for (int i = 0; i < 3; i++) {
    cairo_new_path(cr);
    cairo_arc(cr, wx + 5 + i * 5, wy + 4.5, 1.6, 0, 2 * M_PI);
    set(cr, L[i]);
    cairo_fill(cr);
  }
  if (pos == POS_BOTTOM) rrect(cr, x + w * .3, y + h - 11, w * .4, 8, 3);
  else if (pos == POS_LEFT) rrect(cr, x + 3, y + h * .3, 8, h * .5, 3);
  else rrect(cr, x + w - 11, y + h * .3, 8, h * .5, 3);
  cairo_set_source_rgba(cr, 1, 1, 1, d ? .35 : .7);
  cairo_fill(cr);
  cairo_restore(cr);
  rrect(cr, x + .5, y + .5, w - 1, h - 1, 8);
  set(cr, T->line_strong);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
}

static void option(cairo_t *cr, double x, double y, const char *label, bool on, int look, int pos, int act, long arg) {
  mini_screen(cr, x, y, 120, 76, look, pos);
  if (on) {
    rrect(cr, x - 3, y - 3, 126, 82, 10);
    set(cr, ACCENT);
    cairo_set_line_width(cr, 3);
    cairo_stroke(cr);
  }
  text(cr, x + 60, y + 76 + 16, label, 12, on, T->fg, 0, 1);
  st_add(x - 4, y - 4, 128, 108, act, arg);
}

/* Symbole der Seitenleiste: weiße Linien auf farbigem Quadrat (bei der gewählten Zeile umgekehrt) */
static const char *const ST_GLYPH[ST_N] = {
    /* Allgemein */ "<circle cx='8' cy='8' r='6.2'/><path d='M8 7.2v4.3'/><circle cx='8' cy='4.8' r='.6' fill='%s'/>",
    /* Erscheinungsbild */ "<circle cx='8' cy='8' r='6'/><path d='M8 2a6 6 0 0 1 0 12z' fill='%s'/>",
    /* Hintergrundbild */ "<rect x='1.5' y='2.5' width='13' height='11' rx='1.5'/><path d='M1.5 11l4-4 3.5 3.5 2-2 3.5 3.5'/>",
    /* Schreibtisch & Dock */ "<rect x='1.5' y='2.5' width='13' height='9' rx='1.5'/><path d='M5 14h6'/>",
    /* Tastatur */ "<rect x='1.5' y='4' width='13' height='8' rx='1.5'/><path d='M4.5 7h.01M7 7h.01M9.5 7h.01M12 7h.01M5 10h6' stroke-linecap='round'/>",
    /* Sprache & Region */ "<circle cx='8' cy='8' r='6.2'/><path d='M8 1.8c2.2 1.8 2.2 10.6 0 12.4M8 1.8c-2.2 1.8-2.2 10.6 0 12.4M2 8h12'/>",
    /* Programme */ "<rect x='2.5' y='2.5' width='4' height='4' rx='1' fill='%s'/><rect x='9.5' y='2.5' width='4' height='4' rx='1' fill='%s'/><rect x='2.5' y='9.5' width='4' height='4' rx='1' fill='%s'/><rect x='9.5' y='9.5' width='4' height='4' rx='1' fill='%s'/>",
    /* Freigaben */ "<path d='M8 10V2.5M5 5l3-3 3 3M3 8.5v5h10v-5' stroke-linecap='round' stroke-linejoin='round'/>"};

static cairo_surface_t *st_icon_surf(int sc, bool inv, RGBA col) {
  static cairo_surface_t *cache[ST_N][2];
  if (cache[sc][inv]) return cache[sc][inv];
  char fg[16], body[900], svg[1200];
  if (inv) snprintf(fg, sizeof fg, "#%02x%02x%02x", (int)(col.r * 255), (int)(col.g * 255), (int)(col.b * 255));
  else snprintf(fg, sizeof fg, "#ffffff");
  snprintf(body, sizeof body, ST_GLYPH[sc], fg, fg, fg, fg);
  snprintf(svg, sizeof svg,
           "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20' viewBox='0 0 16 16' fill='none' stroke='%s' stroke-width='1.3' "
           "stroke-linejoin='round'>%s</svg>", fg, body);
  RsvgHandle *hd = rsvg_handle_new_from_data((const guint8 *)svg, strlen(svg), NULL);
  if (!hd) return NULL;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 20, 20);
  cairo_t *c2 = cairo_create(s);
  rsvg_handle_render_cairo(hd, c2);
  cairo_destroy(c2);
  g_object_unref(hd);
  return cache[sc][inv] = s;
}

static void st_icon(cairo_t *cr, double x, double y, int sc, RGBA col, bool selected) {
  rrect(cr, x, y, 20, 20, 5);
  if (selected) set(cr, (RGBA){1, 1, 1, .95});
  else set(cr, col);
  cairo_fill(cr);
  cairo_surface_t *g = st_icon_surf(sc, selected, col);
  if (g) {
    cairo_set_source_surface(cr, g, x, y);
    cairo_paint(cr);
  }
}

/* Allgemein: Logo, Version und die Eckdaten des Rechners wie bei macOS „Allgemein > Info“ */
static void st_row(cairo_t *cr, double x, double *y, double w, const char *k, const char *v, bool first) {
  if (!first) {
    cairo_rectangle(cr, x + 12, *y, w - 24, 1);
    set(cr, T->line);
    cairo_fill(cr);
  }
  text(cr, x + 14, *y + 19, k, 13, false, T->fg, 0, 0);
  text(cr, x + w - 14, *y + 19, v, 13, false, T->fg2, w - 120, 2);
  *y += 38;
}

static void st_general(cairo_t *cr, double x0, int w, int h) {
  if (!logo_big) logo_big = svg_render("logo-gradient.svg", 96, 96, NULL);
  double cx = x0 + (w - x0 - 20) / 2.0;
  if (logo_big) {
    cairo_save(cr);
    cairo_translate(cr, cx - 36, 50);
    cairo_scale(cr, .75, .75);
    cairo_set_source_surface(cr, logo_big, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
  }
  text(cr, cx, 138, "ClokeOS", 22, true, T->fg, 0, 1);
  char buf[160], host[64] = "", mach[64] = "";
  snprintf(buf, sizeof buf, L("“%s” · Version %s"), os_codename, os_version);
  text(cr, cx, 160, buf, 12, false, T->fg2, 0, 1);
  gethostname(host, sizeof host);
  struct utsname u;
  uname(&u);
  snprintf(mach, sizeof mach, "%s", u.machine);
  double gx = x0, gw = w - x0 - 20, y = 182;
  rrect(cr, gx, y, gw, 5 * 38 + 4, 10);
  seta(cr, T->fg, .05);
  cairo_fill(cr);
  y += 2;
  st_row(cr, gx, &y, gw, L("Computer Name"), host, true);
  snprintf(buf, sizeof buf, "Darwin %s (XNU)", u.release);
  st_row(cr, gx, &y, gw, "Kernel", buf, false);
  st_row(cr, gx, &y, gw, L("Architecture"), mach, false);
  int ncpu = 0;
  size_t sz = sizeof ncpu;
  sysctlbyname("hw.ncpu", &ncpu, &sz, NULL, 0);
  uint64_t mem = 0;
  sz = sizeof mem;
  sysctlbyname("hw.memsize", &mem, &sz, NULL, 0);
  snprintf(buf, sizeof buf, "%d", ncpu);
  st_row(cr, gx, &y, gw, L("Cores"), buf, false);
  snprintf(buf, sizeof buf, "%.1f GB · %d × %d", mem / 1073741824.0, SW, SH);
  st_row(cr, gx, &y, gw, L("Memory"), buf, false);
}

/* Tastatur: Belegung für das ganze System, auch für die Anmeldung */
static void st_keyboard(cairo_t *cr, double x0, int w, int h) {
  text(cr, x0, 66, L("Keyboard layout"), 12.5, true, T->fg, 0, 0);
  for (int i = 0; i < NKBD; i++) {
    double bx = x0 + (i % 3) * 112, by = 82 + (i / 3) * 34;
    button(cr, bx, by, 104, KBD[i].title, !strcmp(kbd_cur, KBD[i].code));
    st_add(bx, by, 104, 28, ST_KBD, i);
  }
  text_block(cr, x0, 82 + ((NKBD + 2) / 3) * 34 + 10,
             L("The keyboard layout applies right away in all apps and at the login window. It follows the language until "
               "you choose one here."),
             11.5, false, T->fg2, w - x0 - 24);
}

static void paint_settings(Client *c, cairo_t *cr) {
  int w = c->w, h = c->h, sec = (int)c->data;
  nst_hit = 0;
  /* Seitenleiste */
  cairo_rectangle(cr, 0, 0, 196, h);
  seta(cr, T->fg, .04);
  cairo_fill(cr);
  cairo_rectangle(cr, 196, 0, 1, h);
  set(cr, T->line);
  cairo_fill(cr);
  static const RGBA ICO[ST_N] = {{.45, .47, .55, 1}, {.49, .36, 1, 1}, {.9, .35, .6, 1}, {.2, .55, .95, 1},
                                 {.5, .5, .58, 1}, {.95, .55, .2, 1}, {.2, .72, .45, 1}, {.25, .68, .9, 1}};
  /* Benutzerkarte wie im Prototyp (st-user): Avatar mit Anfangsbuchstabe, Name, „Lokaler Benutzer“ */
  {
    const char *nm = session && session->full[0] ? session->full : session && session->name[0] ? session->name : L("Guest");
    char ini[8] = {0};
    g_utf8_strncpy(ini, nm, 1);
    char *up = g_utf8_strup(ini, -1);
    cairo_arc(cr, 32, 36, 17, 0, 2 * M_PI);
    cairo_pattern_t *ap = cairo_pattern_create_linear(15, 19, 49, 53);
    cairo_pattern_add_color_stop_rgb(ap, 0, .49, .36, 1);
    cairo_pattern_add_color_stop_rgb(ap, 1, .27, .2, .56);
    cairo_set_source(cr, ap);
    cairo_fill(cr);
    cairo_pattern_destroy(ap);
    text(cr, 32, 37, up, 15, true, (RGBA){1, 1, 1, 1}, 0, 1);
    g_free(up);
    text(cr, 58, 30, nm, 13, true, T->fg, 120, 0);
    text(cr, 58, 46, L("Local user"), 11, false, T->fg2, 120, 0);
    cairo_rectangle(cr, 12, 62, 172, 1);
    set(cr, T->line);
    cairo_fill(cr);
  }
  for (int i = 0; i < ST_N; i++) {
    double y = 68 + i * 31;
    if (i == sec) {
      rrect(cr, 10, y, 176, 28, 7);
      set(cr, ACCENT);
      cairo_fill(cr);
    }
    st_icon(cr, 18, y + 4, ST_ORDER[i], ICO[i], i == sec);
    text(cr, 46, y + 14, L(ST_NAMES[i]), 12.5, false, i == sec ? (RGBA){1, 1, 1, 1} : T->fg, 136, 0);
    st_add(10, y, 176, 28, ST_SECTION, i);
  }

  double x0 = 216;
  text(cr, x0, 34, L(ST_NAMES[sec]), 19, true, T->fg, w - x0 - 20, 0);
  int sc = ST_ORDER[sec];
  if (sc == SC_GENERAL) {
    st_general(cr, x0, w, h);
  } else if (sc == SC_APPEAR) {
    static const char *const names[3] = {"Light", "Dark", "Auto"};
    static const int mode[3] = {1, 2, 0};
    for (int i = 0; i < 3; i++)
      option(cr, x0 + i * 140, 70, L(names[i]), theme_mode == mode[i], i, POS_BOTTOM, ST_THEME, mode[i]);
    text(cr, x0, 262, L("Interface"), 12.5, true, T->fg, 0, 0);
    for (int i = 0; i < 2; i++) {
      double bx = x0 + i * 100, by = 278;
      button(cr, bx, by, 90, i ? "NT" : "ClokeOS", nt_sel == (i == 1));
      st_add(bx, by, 90, 28, ST_UI, i);
    }
    if (nt_sel != nt) text_block(cr, x0, 316, L("The new interface appears at the next login."), 11.5, false, T->fg2, w - x0 - 24);
    text_block(cr, x0, 192, L("Auto: light during the day, dark from 7 PM to 7 AM. Windows, the Dock and menus switch "
                              "right away, and so do Fundus, Terminal and the other GTK apps."),
               11.5, false, T->fg2, w - x0 - 24);
  } else if (sc == SC_DOCK) {
    text(cr, x0, 66, L("Position on screen"), 12.5, true, T->fg, 0, 0);
    static const char *const names[3] = {"Left", "Bottom", "Right"};
    static const int pos[3] = {POS_LEFT, POS_BOTTOM, POS_RIGHT};
    for (int i = 0; i < 3; i++)
      option(cr, x0 + i * 140, 84, L(names[i]), dock_pos == pos[i], dark ? 1 : 0, pos[i], ST_DOCKPOS, pos[i]);
    text(cr, x0, 222, L("Size"), 12.5, true, T->fg, 0, 0);
    static const char *const sizes[3] = {"Small", "Medium", "Large"};
    for (int i = 0; i < 3; i++) {
      double bx = x0 + i * 100, by = 238;
      button(cr, bx, by, 90, L(sizes[i]), ds_pref == DOCK_SIZES[i]);
      st_add(bx, by, 90, 28, ST_DOCKSIZE, DOCK_SIZES[i]);
    }
    text_block(cr, x0, 286, L("Right-click an icon: “Keep in Dock” or “Remove from Dock”. ClokeOS remembers the Dock "
                              "for each user in ~/Library/Preferences."),
               11.5, false, T->fg2, w - x0 - 24);
  } else if (sc == SC_APPS) {
    int nmac = 0, nall = 0;
    for (int i = 0; i < napps; i++)
      if (!apps[i].gone && !apps[i].b.internal[0]) nall++, nmac += apps[i].b.mac;
    char buf[700];
    snprintf(buf, sizeof buf,
             L("Apps are bundles with the extension .app, as on macOS: folders with Contents/Info.plist, "
               "Contents/MacOS and Contents/Resources. To install one, drag it to the Applications folder "
               "(/Applications), or to ~/Applications for yourself only; to remove it, move it to the Trash. "
               "Launchpad and the Dock show new apps after a second or two.\n\n"
               "ClokeOS recognizes Mac apps by their Info.plist and shows their icon. Apps with windows run on "
               "the Mac frameworks from Darling (AppKit), which are still incomplete.\n\n"
               "Found: %d apps, %d of them Mac apps."),
             nall, nmac);
    text_block(cr, x0, 58, buf, 11.5, false, T->fg2, w - x0 - 24);
    button(cr, x0, h - 52, 200, L("Open Applications Folder"), false);
    st_add(x0, h - 52, 200, 28, ST_OPENAPPS, 0);
  } else if (sc == SC_LANG) {
    text(cr, x0, 66, L("Preferred language"), 12.5, true, T->fg, 0, 0);
    for (int i = 0; i < l10n_count(); i++) {
      double bx = x0 + (i % 3) * 112, by = 82 + (i / 3) * 34;
      button(cr, bx, by, 104, l10n_name(i), !strcmp(l10n_lang(), l10n_code(i)));
      st_add(bx, by, 104, 28, ST_LANG, i);
    }
    text_block(cr, x0, 160,
               L("ClokeOS, the login window and the menu bar switch right away. Apps that are already open "
                 "switch the next time they open."),
               11.5, false, T->fg2, w - x0 - 24);
    text(cr, x0, 226, L("Date and time"), 12.5, true, T->fg, 0, 0);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char d1[96], d2[96], buf[220];
    l10n_date_long(d1, sizeof d1, &tm);
    l10n_menubar_clock(d2, sizeof d2, &tm);
    snprintf(buf, sizeof buf, "%s\n%s", d1, d2);
    text_block(cr, x0, 244, buf, 12, false, T->fg2, w - x0 - 24);
    text_block(cr, x0, 298, L("The region follows the language: English uses the US format with AM and PM, the other "
                              "languages their usual format with a 24-hour clock."),
               11.5, false, T->fg3, w - x0 - 24);
  } else if (sc == SC_KEYB) {
    st_keyboard(cr, x0, w, h);
  } else if (sc == SC_WALL) {
    int cols = 5;
    double tw = 92, th = 58, gx = 100, gy = 86;
    for (int i = 0; i < wall_count(); i++) {
      double bx = x0 + (i % cols) * gx, by = 62 + (i / cols) * gy;
      cairo_save(cr);
      rrect(cr, bx, by, tw, th, 6);
      cairo_clip(cr);
      wall_thumb(cr, i, bx, by, tw, th, dark);
      cairo_restore(cr);
      if (i == wall_sel) {
        rrect(cr, bx - 2, by - 2, tw + 4, th + 4, 8);
        set(cr, ACCENT);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
      }
      text(cr, bx + tw / 2, by + th + 12, L(wall_title(i)), 10, i == wall_sel, T->fg, 84, 1);
      st_add(bx - 3, by - 3, tw + 6, th + 20, ST_WALL, i);
    }
    text(cr, x0, h - 22, L("Wallpapers by Pixel Makes Art (instagram.com/pixelmakesart), used with permission."), 10, false, T->fg3, w - x0 - 20, 0);
  } else if (sc == SC_SHARE) {
    bool on = remote_login_on();
    text(cr, x0, 66, L("Remote Login"), 12.5, true, T->fg, 0, 0);
    button(cr, x0, 82, 90, L("On"), on);
    st_add(x0, 82, 90, 28, ST_SSH, 1);
    button(cr, x0 + 100, 82, 90, L("Off"), !on);
    st_add(x0 + 100, 82, 90, 28, ST_SSH, 0);
    int y = 128;
    y += text_block(cr, x0, y,
                    L("Lets users with a password log in to this computer over the network with SSH (port 22), for "
                      "example from Terminal on a Mac. Accounts without a password cannot log in."),
                    11.5, false, T->fg2, w - x0 - 24) + 18;
    if (on) {
      text(cr, x0, y + 8, L("To log in from another computer:"), 12.5, true, T->fg, 0, 0);
      y += 26;
      const char *user = session && session->uid ? session->name : "<account>";
      struct ifaddrs *ifa = NULL;
      bool any = false;
      if (!getifaddrs(&ifa)) {
        for (struct ifaddrs *i = ifa; i && y < h - 40; i = i->ifa_next) {
          if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !strcmp(i->ifa_name, "lo0")) continue;
          char ip[64], buf[160];
          inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
          snprintf(buf, sizeof buf, "ssh %s@%s", user, ip);
          text(cr, x0, y + 8, buf, 12.5, false, ACCENT, w - x0 - 24, 0);
          y += 22;
          any = true;
        }
        freeifaddrs(ifa);
      }
      if (!any)
        text_block(cr, x0, y,
                   L("No network connection. ClokeOS supports Realtek RTL8111/8168 and Intel 82545EM network cards "
                     "and the virtual cards of QEMU."),
                   11.5, false, T->fg3, w - x0 - 24);
    }
  }
}

static void click_settings(Client *c, int x, int y) {
  for (int k = 0; k < nst_hit; k++) {
    if (x < st_hit[k].x || y < st_hit[k].y || x >= st_hit[k].x + st_hit[k].w || y >= st_hit[k].y + st_hit[k].h) continue;
    long arg = st_hit[k].arg;
    switch (st_hit[k].act) {
    case ST_SECTION: c->data = arg; break;
    case ST_UI: nt_sel = arg == 1; save_prefs(); break;
    case ST_THEME: set_theme_mode(arg); break;
    case ST_WALL: set_wallpaper(arg); break;
    case ST_DOCKPOS: set_dock_pos(arg); break;
    case ST_DOCKSIZE: set_dock_size(arg); break;
    case ST_OPENAPPS: open_path(NULL, "/Applications", false); break;
    case ST_LANG: set_language(l10n_code((int)arg)); break;
    case ST_KBD: set_keyboard((int)arg); break;
    case ST_SSH: set_remote_login(arg != 0); break;
    }
    draw_own(c);
    return;
  }
}

static void show_settings(long section) {
  static long last;
  if (section >= 0) last = section;
  Client *c = own_window(740, 500, "System Settings", paint_settings, click_settings, last);
  if (c) c->app = app_by_ident("com.clokeos.systemeinstellungen");
  draw_menubar();
  draw_dock();
}

static void redraw_all(void) {
  render_mb_icons();
  draw_menubar();
  draw_dock();
  for (Client *c = clients; c; c = c->next) {
    draw_frame(c);
    if (c->draw) draw_own(c);
  }
  if (logo_big) {
    cairo_surface_destroy(logo_big);
    logo_big = NULL;
  }
}

#include "setup.c"

/* ---------------------------------------------------------------------------------------- */
/* Ereignisse                                                                                 */

static void on_button_press(XButtonEvent *e) {
  /* Klicks in ein Programmfenster kommen über den synchronen Griff aus grab_buttons(): Bis
   * XAllowEvents steht der Zeiger still. Deshalb immer zuerst freigeben, auch bei offenem Menü. */
  Client *gc = find_client(e->window);
  if (setup_active) {
    if (gc && e->window == gc->win) XAllowEvents(dpy, AsyncPointer, CurrentTime);
    if (e->window == setupwin && e->button == Button1) setup_click(e->x, e->y);
    return;
  }
  if (locked) {
    if (gc && e->window == gc->win) XAllowEvents(dpy, AsyncPointer, CurrentTime);
    if (e->window == loginwin && e->button == Button1) login_click(e->x, e->y);
    return;
  }
  if (sp_open) {
    if (e->window == spotwin && e->button == Button1) sp_click(e->x, e->y);
    else if (e->window != spotwin || e->button != Button1) hide_spot();
    return;
  }
  if (lp_open && e->window != dock) {
    if (gc && e->window == gc->win) XAllowEvents(dpy, AsyncPointer, CurrentTime);
    if (e->window == lpwin && e->button == Button1) lp_click(e->x, e->y);
    else hide_launchpad();
    return;
  }
  if (lp_open) hide_launchpad(); /* Klick ins Dock */
  if (gc && e->window == gc->win) {
    if (menuwin) {
      close_menu();
      XAllowEvents(dpy, AsyncPointer, CurrentTime); /* der Klick schließt nur das Menü */
      return;
    }
    if (gc->draw || gc == focused) { /* eigene Fenster und das aktive: gleich */
      raise_client(gc);
      focus_client(gc);
    } else {
      pending_focus = gc; /* erst beim Loslassen */
      pending_focus_t0 = now_s();
    }
    XAllowEvents(dpy, ReplayPointer, CurrentTime);
    if (debug) warn("Klick in Programmfenster 0x%lx weitergegeben", gc->win);
    if (gc->draw && e->button == Button1) gc->click(gc, e->x, e->y);
    return;
  }
  if (menuwin) {
    if (e->window == menuwin && e->x >= 0 && e->y >= 0 && e->x < menu_w && e->y < menu_h) return;
    if (subwin && e->window == subwin && e->x >= 0 && e->y >= 0 && e->x < sub_w && e->y < sub_h) return;
    if (e->window == menubar) {
      int i = menubar_item_at(e->x);
      if (i == menu_owner && menu_by_hover) menu_by_hover = false; /* der Klick bestätigt das Menü */
      else if (i == menu_owner || i < 0) close_menu();
      else {
        menubar_open(i);
        menu_by_hover = false;
      }
      return;
    }
    close_menu();
    return;
  }
  if (e->window == menubar) {
    int i = menubar_item_at(e->x);
    if (i >= 0) {
      menubar_open(i);
      menu_by_hover = false;
    }
    return;
  }
  if (e->window == dock) {
    if (nt && e->button == Button1 && e->x < NT_START_W) { /* Start: Menü */
      show_tip(-1);
      nt_start_menu();
      return;
    }
    int i = dock_item_at(e->x, e->y);
    show_tip(-1);
    if (i < 0) return;
    if (e->button == Button3) dock_menu(i);
    else if (e->button == Button1) dock_click(i);
    return;
  }
  if (e->window == root) {
    if (nt && e->button == Button1) { /* Desktop-Symbole: Doppelklick startet */
      static int last_k = -1;
      static double last_t;
      int k = nt_desk_at(e->x, e->y);
      double t = now_s();
      if (k >= 0 && k == last_k && t - last_t < .5) {
        int i = nt_desk_app(k);
        if (i >= 0) launch(i);
        else open_trash();
        last_k = -1;
      } else {
        last_k = k, last_t = t;
      }
    }
    focus_client(NULL);
    return;
  }
  Client *c = find_client(e->window);
  if (!c) return;
  if (c != focused || clients != c) {
    raise_client(c);
    focus_client(c);
  }
  if (e->button != Button1) return;
  if (e->window == c->grip) {
    drag = c;
    drag_mode = 2;
  } else if (c->lights && e->window == c->lights) {
    int x, y, w, h;
    lights_rect(c, &x, &y, &w, &h);
    int l = light_at(c, e->x + x, e->y + y);
    if (l) c->pressed = l;
    return;
  } else {
    int l = light_at(c, e->x, e->y);
    if (l) {
      c->pressed = l;
      return;
    }
    int nv = nav_at(c, e->x, e->y);
    if (nv) {
      send_nav_key(c, nv);
      return;
    }
    if (e->y > c->th + c->bw) return;
    if (last_title_client == c && e->time - last_title_click < 400) {
      last_title_client = NULL;
      zoom_client(c);
      return;
    }
    last_title_client = c;
    last_title_click = e->time;
    drag = c;
    drag_mode = 1;
  }
  drag_px = e->x_root;
  drag_py = e->y_root;
  drag_x = c->x;
  drag_y = c->y;
  drag_w = c->w;
  drag_h = c->h;
  XGrabPointer(dpy, e->window, False, ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None,
               drag_mode == 2 ? cur_resize : cur_move, CurrentTime);
}

static void on_button_release(XButtonEvent *e) {
  if (menuwin) {
    if (e->window == menuwin) {
      int i = menu_item_at(e->x, e->y);
      if (i >= 0) activate_item(i);
    } else if (subwin && e->window == subwin) {
      int i = sub_item_at(e->x, e->y);
      if (i >= 0) activate_sub(i);
    }
    return;
  }
  if (drag) {
    XUngrabPointer(dpy, CurrentTime);
    Client *c = drag;
    drag = NULL;
    if (c->zoomed && drag_mode == 1) c->zoomed = false;
    send_configure(c);
    draw_frame(c);
    return;
  }
  Client *c = find_client(e->window);
  if (c && c->pressed) {
    int ox = 0, oy = 0, w, h;
    if (c->lights && e->window == c->lights) lights_rect(c, &ox, &oy, &w, &h);
    int l = light_at(c, e->x + ox, e->y + oy);
    int p = c->pressed;
    c->pressed = 0;
    if (l == p) {
      if (p == 1) close_client(c);
      else if (p == 2) minimize_client(c);
      else zoom_client(c);
    }
  }
}

static void on_motion(XMotionEvent *e) {
  XEvent next;
  while (XCheckTypedWindowEvent(dpy, e->window, MotionNotify, &next)) e = &next.xmotion; /* nur die letzte Lage */
  if (menuwin) {
    if (e->window == menuwin) {
      int i = menu_item_at(e->x, e->y);
      if (i != menu_hover) {
        menu_hover = i;
        draw_menu();
        if (i >= 0 && menu[i].sub) open_sub(i);
        else if (i >= 0) close_sub();
      }
    } else if (subwin && e->window == subwin) {
      int i = sub_item_at(e->x, e->y);
      if (i != sub_hover) {
        sub_hover = i;
        draw_sub();
      }
      if (menu_hover != sub_of) { /* Eintrag des Untermenüs hervorgehoben lassen */
        menu_hover = sub_of;
        draw_menu();
      }
    } else if (e->window == menubar && menu_owner >= 0) {
      int i = menubar_item_at(e->x);
      if (i >= 0 && i != menu_owner && i != MB_SPOT) {
        menubar_open(i);
        menu_by_hover = true;
      }
    }
    return;
  }
  if (drag) {
    Client *c = drag;
    int dx = e->x_root - drag_px, dy = e->y_root - drag_py;
    if (drag_mode == 1) {
      c->x = drag_x + dx;
      c->y = drag_y + dy;
      if (c->y - c->th < MENUBAR_H) c->y = MENUBAR_H + c->th;
      XMoveWindow(dpy, c->frame, c->x - c->bw, c->y - c->th - c->bw);
    } else {
      c->w = drag_w + dx < c->minw ? c->minw : drag_w + dx;
      c->h = drag_h + dy < c->minh ? c->minh : drag_h + dy;
      apply_geometry(c);
      draw_frame(c);
    }
    return;
  }
  if (e->window == dock) {
    int i = dock_item_at(e->x, e->y);
    if (i != dock_hover) {
      dock_hover = i;
      show_tip(i);
      draw_dock();
    }
    return;
  }
  Client *c = find_client(e->window);
  if (c && c->lights && e->window == c->lights) { /* wie bei macOS: Symbole über der ganzen Gruppe */
    if (!c->hover) {
      c->hover = 1;
      draw_frame(c);
    }
    return;
  }
  if (c && e->window == c->frame) {
    int h = nt ? light_at(c, e->x, e->y) : (e->y < c->th && e->x < LIGHT_X0 + 2 * LIGHT_GAP + LIGHT_R + 4);
    if (h != c->hover) {
      c->hover = h;
      draw_frame(c);
    }
  }
}

static void on_leave(XCrossingEvent *e) {
  if (e->window == dock && dock_hover >= 0) {
    dock_hover = -1;
    show_tip(-1);
    draw_dock();
  }
  if (e->window == menuwin && menu_hover >= 0 && !subwin) {
    menu_hover = -1;
    draw_menu();
  }
  if (subwin && e->window == subwin && sub_hover >= 0) {
    sub_hover = -1;
    draw_sub();
  }
  Client *c = find_client(e->window);
  if (c && (e->window == c->frame || (c->lights && e->window == c->lights)) && c->hover) {
    c->hover = 0;
    draw_frame(c);
  }
}

static void on_expose(XExposeEvent *e) {
  if (e->count) return;
  if (e->window == menubar) draw_menubar();
  else if (e->window == dock) draw_dock();
  else if (e->window == menuwin) draw_menu();
  else if (subwin && e->window == subwin) draw_sub();
  else if (e->window == tipwin) paint_window(tipwin, tip_w, 24, paint_tip, NULL);
  else if (e->window == lpwin) draw_launchpad();
  else if (sp_open && e->window == spotwin) draw_spot();
  else if (setupwin && e->window == setupwin) draw_setup();
  else {
    Client *c = find_client(e->window);
    if (!c) return;
    if (e->window == c->frame || (c->lights && e->window == c->lights)) draw_frame(c);
    else if (c->draw && e->window == c->win) draw_own(c);
  }
}

static void on_configure_request(XConfigureRequestEvent *e) {
  Client *c = find_client(e->window);
  if (c && e->window == c->win) {
    if (e->value_mask & CWX) c->x = e->x;
    if (e->value_mask & CWY) c->y = c->uni ? e->y + c->th : e->y;
    if (e->value_mask & CWWidth) c->w = e->width;
    if (e->value_mask & CWHeight) c->h = c->uni && e->height > c->th ? e->height - c->th : e->height;
    if (c->y - c->th < MENUBAR_H) c->y = MENUBAR_H + c->th + c->bw;
    apply_geometry(c);
    draw_frame(c);
    return;
  }
  XWindowChanges wc = {.x = e->x, .y = e->y, .width = e->width, .height = e->height, .border_width = e->border_width,
                       .sibling = e->above, .stack_mode = e->detail};
  XConfigureWindow(dpy, e->window, e->value_mask, &wc);
}

static void on_client_message(XClientMessageEvent *e) {
  Client *c = find_client(e->window);
  if (!c) return;
  if (e->message_type == NET_ACTIVE_WINDOW) restore_client(c);
  else if (e->message_type == NET_CLOSE_WINDOW) close_client(c);
  else if (e->message_type == WM_CHANGE_STATE && e->data.l[0] == IconicState) minimize_client(c);
  else if (e->message_type == NET_WM_MOVERESIZE) {
    /* EWMH: Programme ohne Titelleiste von CloakServer (GTK-Kopfleisten, Mac-Fenster mit eigener Fläche oben) verschieben
     * das Fenster so; 8 = verschieben, 4 = Größe unten rechts, 11 = abbrechen */
    long dir = e->data.l[2];
    if (dir == 11) {
      if (drag == c) {
        XUngrabPointer(dpy, CurrentTime);
        drag = NULL;
      }
      return;
    }
    if (dir != 8 && dir != 4) return;
    if (c->zoomed && dir == 8) c->zoomed = false;
    drag = c;
    drag_mode = dir == 8 ? 1 : 2;
    drag_px = (int)e->data.l[0];
    drag_py = (int)e->data.l[1];
    drag_x = c->x, drag_y = c->y, drag_w = c->w, drag_h = c->h;
    raise_client(c);
    focus_client(c);
    if (XGrabPointer(dpy, c->frame, False, ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None,
                     drag_mode == 2 ? cur_resize : cur_move, CurrentTime) != GrabSuccess)
      drag = NULL;
  }
}

static void on_key(XKeyEvent *e) {
  if (setup_active) {
    setup_key(e);
    return;
  }
  if (locked) {
    login_key(e);
    return;
  }
  if (sp_open) {
    sp_key(e);
    return;
  }
  if (lp_open && !menuwin) {
    lp_key(e);
    return;
  }
  KeySym ks = XLookupKeysym(e, 0);
  if (menuwin) {
    if (subwin && sub_hover >= 0 && (ks == XK_Down || ks == XK_Up)) { /* im Untermenü */
      int d = ks == XK_Down ? 1 : -1, i = sub_hover;
      for (int k = 0; k < nsub; k++) {
        i = (i + d + nsub) % nsub;
        if (submenu[i].kind == 0 && !submenu[i].disabled) break;
      }
      sub_hover = i;
      draw_sub();
    } else if (subwin && sub_hover >= 0 && ks == XK_Return) {
      activate_sub(sub_hover);
    } else if (subwin && sub_hover >= 0 && (ks == XK_Left || ks == XK_Escape)) {
      sub_hover = -1;
      draw_sub();
    } else if (ks == XK_Escape) close_menu();
    else if (ks == XK_Right && menu_hover >= 0 && menu[menu_hover].sub) {
      open_sub(menu_hover);
      for (int i = 0; i < nsub; i++)
        if (submenu[i].kind == 0 && !submenu[i].disabled) {
          sub_hover = i;
          break;
        }
      draw_sub();
    } else if (ks == XK_Down || ks == XK_Up) {
      int d = ks == XK_Down ? 1 : -1, i = menu_hover;
      for (int k = 0; k < nmenu; k++) {
        i = (i + d + nmenu) % nmenu;
        if (menu[i].kind == 0 && !menu[i].disabled) break;
      }
      menu_hover = i;
      draw_menu();
      if (menu[i].sub) open_sub(i);
      else close_sub();
    } else if (ks == XK_Return && menu_hover >= 0) {
      activate_item(menu_hover);
    }
    return;
  }
  if (focused && focused->draw == rec_paint && !(e->state & Mod1Mask)) { /* Wiederherstellung per Tastatur */
    rec_key(ks);
    return;
  }
  if (focused && focused->draw == du_paint && !(e->state & Mod1Mask)) { /* Festplattendienstprogramm */
    du_key(focused, e);
    return;
  }
  if (ks == XK_Tab && (e->state & Mod1Mask)) { /* Alt+Tab: nächstes Fenster */
    Client *last = NULL;
    for (Client *c = clients; c; c = c->next)
      if (!c->minimized) last = c;
    if (last && last != focused) restore_client(last);
  } else if (ks == XK_F4 && (e->state & Mod1Mask)) {
    close_client(focused);
  } else if (ks == XK_space && (e->state & ControlMask)) { /* Strg+Leertaste: Spotlight wie im Prototyp */
    show_spot();
  }
}

/* Ein Bild aller laufenden Animationen; true, solange noch eine läuft */
static bool animate(void) {
  bool any = false;
  double t = now_s();
  if (debug)
    for (Client *c = clients; c; c = c->next)
      if (c->anim && c->anim_p == 0) warn("Animation %d beginnt um %.3f s", c->anim, fmod(c->anim_t0, 1000));
  for (Client *c = clients; c; c = c->next) {
    if (!c->anim) continue;
    int fx = c->x - c->bw, fy = c->y - c->th - c->bw, fw = c->w + 2 * c->bw, fh = c->h + c->th + 2 * c->bw;
    int x0 = fx, y0 = fy, x1 = fx + fw, y1 = fy + fh;
    if (c->anim != ANIM_OPEN) { /* der Weg bis zum Dock */
      double tx, ty;
      dock_target(c, &tx, &ty);
      x0 = fmin(x0, tx - ds);
      x1 = fmax(x1, tx + ds);
      y0 = fmin(y0, ty - ds);
      y1 = fmax(y1, ty + ds);
    }
    comp_dirty(x0 - 56, y0 - 56, x1 - x0 + 112, y1 - y0 + 112);
    /* nach der Zeit wie im Prototyp, aber höchstens ein Viertel pro Bild: Auch wenn ein Bild
     * lange dauert (Emulation), sind es mindestens vier Zwischenbilder */
    c->anim_p = fmin((t - c->anim_t0) / ANIM_S[c->anim], c->anim_p + .25);
    if (c->anim_p < 1) {
      any = true;
      continue;
    }
    if (c->anim == ANIM_MIN) XUnmapWindow(dpy, c->frame);
    c->anim = ANIM_NONE;
  }
  for (int i = 0; i < napps; i++) {
    if (apps[i].bounce_t0 <= 0) continue;
    if (t - apps[i].bounce_t0 >= BOUNCE_S) apps[i].bounce_t0 = 0;
    else any = true;
    draw_dock();
  }
  return any;
}

static void tick(void) {
  static int last_min = -1;
  static double last_scan;
  setup_tick();
  if (now_s() - last_scan > 2) { /* ein Bundle nach /Applications kopiert oder gelöscht */
    last_scan = now_s();
    if (apps_signature() != apps_sig) {
      scan_apps();
      draw_dock();
      draw_launchpad();
    }
  }
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  if (shake_t0 >= 0) { /* Wackeln läuft: nächstes Bild */
    if (now_s() - shake_t0 >= .4) shake_t0 = -1;
    draw_login();
  }
  if (tm.tm_min == last_min) return;
  last_min = tm.tm_min;
  if (locked) draw_login();
  if (theme_mode == 0 && want_dark() != dark) {
    apply_theme();
    redraw_all();
    return;
  }
  if (tm.tm_mday != cal_day) {
    render_icons();
    draw_dock();
  } else if (nt) {
    draw_dock(); /* Uhr der Taskleiste */
  }
  draw_menubar();
}

/* ---------------------------------------------------------------------------------------- */

static Atom atom(const char *name) { return XInternAtom(dpy, name, False); }

static void setup_atoms(void) {
  WM_PROTOCOLS = atom("WM_PROTOCOLS");
  WM_WINDOW_ROLE = atom("WM_WINDOW_ROLE");
  WM_DELETE_WINDOW = atom("WM_DELETE_WINDOW");
  WM_TAKE_FOCUS = atom("WM_TAKE_FOCUS");
  WM_STATE = atom("WM_STATE");
  WM_CHANGE_STATE = atom("WM_CHANGE_STATE");
  UTF8_STRING = atom("UTF8_STRING");
  NET_SUPPORTED = atom("_NET_SUPPORTED");
  NET_WM_NAME = atom("_NET_WM_NAME");
  NET_ACTIVE_WINDOW = atom("_NET_ACTIVE_WINDOW");
  NET_CLIENT_LIST = atom("_NET_CLIENT_LIST");
  NET_WORKAREA = atom("_NET_WORKAREA");
  NET_SUPPORTING_WM_CHECK = atom("_NET_SUPPORTING_WM_CHECK");
  NET_WM_WINDOW_TYPE = atom("_NET_WM_WINDOW_TYPE");
  NET_WM_WINDOW_TYPE_DOCK = atom("_NET_WM_WINDOW_TYPE_DOCK");
  NET_WM_WINDOW_TYPE_DESKTOP = atom("_NET_WM_WINDOW_TYPE_DESKTOP");
  NET_WM_WINDOW_TYPE_SPLASH = atom("_NET_WM_WINDOW_TYPE_SPLASH");
  NET_WM_WINDOW_TYPE_NOTIFICATION = atom("_NET_WM_WINDOW_TYPE_NOTIFICATION");
  NET_WM_WINDOW_TYPE_TOOLTIP = atom("_NET_WM_WINDOW_TYPE_TOOLTIP");
  NET_WM_WINDOW_TYPE_MENU = atom("_NET_WM_WINDOW_TYPE_MENU");
  NET_WM_WINDOW_TYPE_POPUP_MENU = atom("_NET_WM_WINDOW_TYPE_POPUP_MENU");
  NET_WM_WINDOW_TYPE_DROPDOWN_MENU = atom("_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
  NET_WM_WINDOW_TYPE_COMBO = atom("_NET_WM_WINDOW_TYPE_COMBO");
  NET_WM_WINDOW_TYPE_DND = atom("_NET_WM_WINDOW_TYPE_DND");
  NET_CLOSE_WINDOW = atom("_NET_CLOSE_WINDOW");
  NET_WM_PID = atom("_NET_WM_PID");
  NET_FRAME_EXTENTS = atom("_NET_FRAME_EXTENTS");
  NET_NUMBER_OF_DESKTOPS = atom("_NET_NUMBER_OF_DESKTOPS");
  NET_CURRENT_DESKTOP = atom("_NET_CURRENT_DESKTOP");
  MOTIF_WM_HINTS = atom("_MOTIF_WM_HINTS");
  CLOKE_TITLEBAR = atom("_CLOKE_TITLEBAR");
  NET_WM_MOVERESIZE = atom("_NET_WM_MOVERESIZE");
  XROOTPMAP_ID = atom("_XROOTPMAP_ID");
  ESETROOT_PMAP_ID = atom("ESETROOT_PMAP_ID");
  GTK_UNIQUE_BUS_NAME = atom("_GTK_UNIQUE_BUS_NAME");
  GTK_MENUBAR_OBJECT_PATH = atom("_GTK_MENUBAR_OBJECT_PATH");
  UNITY_OBJECT_PATH = atom("_UNITY_OBJECT_PATH");
  GTK_APPLICATION_OBJECT_PATH = atom("_GTK_APPLICATION_OBJECT_PATH");
  GTK_WINDOW_OBJECT_PATH = atom("_GTK_WINDOW_OBJECT_PATH");
  CLOKE_MENU = atom("_CLOKE_MENU");
  CLOKE_MENU_ACTIVATE = atom("_CLOKE_MENU_ACTIVATE");
  CLOKE_OPEN = atom("_CLOKE_OPEN");
}

static void setup_ewmh(void) {
  Atom supported[] = {NET_SUPPORTED, NET_WM_NAME, NET_ACTIVE_WINDOW, NET_CLIENT_LIST, NET_WORKAREA,
                      NET_SUPPORTING_WM_CHECK, NET_WM_WINDOW_TYPE, NET_CLOSE_WINDOW, NET_FRAME_EXTENTS,
                      NET_NUMBER_OF_DESKTOPS, NET_CURRENT_DESKTOP, NET_WM_MOVERESIZE};
  XChangeProperty(dpy, root, NET_SUPPORTED, XA_ATOM, 32, PropModeReplace, (unsigned char *)supported,
                  sizeof supported / sizeof supported[0]);
  checkwin = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
  XChangeProperty(dpy, checkwin, NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&checkwin, 1);
  XChangeProperty(dpy, checkwin, NET_WM_NAME, UTF8_STRING, 8, PropModeReplace, (unsigned char *)"CloakServer", 11);
  XChangeProperty(dpy, root, NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&checkwin, 1);
  long one = 1, zero = 0;
  XChangeProperty(dpy, root, NET_NUMBER_OF_DESKTOPS, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
  XChangeProperty(dpy, root, NET_CURRENT_DESKTOP, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&zero, 1);
  update_workarea();
}

static void update_workarea(void) {
  int x0, y0, x1, y1;
  work_area(&x0, &y0, &x1, &y1);
  long wa[4] = {x0, y0, x1 - x0, y1 - y0};
  XChangeProperty(dpy, root, NET_WORKAREA, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)wa, 4);
}

/* Eigene Fenster der Oberfläche; argb: mit Compositor ein 32-Bit-Fenster mit Durchsicht */
static Window ui_window(int x, int y, int w, int h, long mask, bool argb, Atom type) {
  XSetWindowAttributes sa = {0};
  sa.override_redirect = True;
  sa.event_mask = mask;
  sa.cursor = cur_arrow;
  unsigned long vm = CWOverrideRedirect | CWEventMask | CWCursor;
  int depth = CopyFromParent;
  Visual *v = CopyFromParent;
  if (argb && comp_on) {
    sa.colormap = comp_argb_cmap;
    sa.border_pixel = sa.background_pixel = 0;
    vm |= CWColormap | CWBorderPixel | CWBackPixel;
    depth = 32;
    v = comp_argb;
  }
  Window win = XCreateWindow(dpy, root, x, y, w, h, 0, depth, InputOutput, v, vm, &sa);
  XChangeProperty(dpy, win, NET_WM_WINDOW_TYPE, XA_ATOM, 32, PropModeReplace, (unsigned char *)&type, 1);
  return win;
}

/* Wohin ein Fenster ins Dock fliegt: zu seinem Symbol, sonst zur Mitte des Docks */
static void dock_target(Client *c, double *x, double *y) {
  layout_dock();
  *x = dock_x + dock_w / 2.0;
  *y = dock_y + dock_h / 2.0;
  for (int k = 0; k < nditems; k++)
    if (c->app >= 0 && ditems[k].app == c->app) item_center(k, x, y);
}

static void anim_style(Client *c, WinStyle *st) {
  double p = c->anim_p;
  if (c->anim == ANIM_OPEN) { /* aus 94 % Größe einblenden, ease-out */
    double e = 1 - pow(1 - p, 3);
    st->shrink = .06 * (1 - e);
    st->fade = 1 - e;
    return;
  }
  double e = c->anim == ANIM_MIN ? p * p : pow(1 - p, 3); /* hin: beschleunigen, zurück: abbremsen */
  double fw = c->w + 2 * c->bw, fh = c->h + c->th + 2 * c->bw;
  double cx = c->x - c->bw + fw / 2, cy = c->y - c->th - c->bw + fh / 2, tx, ty;
  dock_target(c, &tx, &ty);
  st->shrink = (1 - ds / fmax(fw, fh)) * e;
  st->dx = (tx - cx) * e;
  st->dy = (ty - cy) * e;
  st->fade = e;
}

/* Für den Compositor: Schatten, runde Ecken, Glas (Schatten wie --shadow, --pop-shadow und der
 * Dock-Schatten in clokeos.html, verkleinert, weil Unschärfe unter Emulation teuer ist) */
WinStyle cs_window_style(Window w) {
  WinStyle st = {WK_PLAIN, 0, 0, 0, 0};
  if (w == dock)
    return (WinStyle){WK_GLASS, ds * .42 + 6, 22, 10, .28, .it = dock_pos == POS_BOTTOM ? dock_lift : 0,
                      .ir = dock_pos == POS_LEFT ? dock_lift : 0, .il = dock_pos == POS_RIGHT ? dock_lift : 0};
  if (w && (w == menuwin || w == subwin)) return (WinStyle){WK_GLASS, 14, 16, 8, .26};
  if (w && w == spotwin) return (WinStyle){WK_GLASS, 26, 20, 10, .26};
  if (w && w == tipwin) return (WinStyle){WK_GLASS, 12, 10, 4, .2};
  for (Client *c = clients; c; c = c->next)
    if (c->frame == w) {
      if (c->bw) {
        bool a = c == focused;
        st = (WinStyle){WK_FRAME, RADIUS, a ? 30 : 18, a ? 16 : 8, a ? (dark ? .55 : .36) : (dark ? .4 : .22)};
      }
      if (c->anim) anim_style(c, &st);
      return st;
    }
  return st;
}

Pixmap cs_wallpaper(void) { return wall_pm; }

/* Bis das Hintergrundbild fertig ist, steht der Umhang da, wo ihn der Loader gezeichnet hat
 * (weiß auf Schwarz, 12 % der kürzeren Bildschirmseite, mittig); Xorg hat ihn gerade gelöscht. */
static void splash(void) {
  int s = (SW < SH ? SW : SH) * 12 / 100; /* wie SPLASH_SIZE_PERCENT im Loader */
  const char *white[] = {"@FILL@", "#ffffff", NULL};
  cairo_surface_t *lg = svg_render("logo.svg", s, s, white);
  XSetWindowBackground(dpy, root, BlackPixel(dpy, scr));
  XClearWindow(dpy, root);
  if (lg) {
    cairo_surface_t *rs = cairo_xlib_surface_create(dpy, root, vis, SW, SH);
    cairo_t *cr = cairo_create(rs);
    cairo_set_source_surface(cr, lg, (SW - s) / 2, (SH - s) / 2);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(rs);
    cairo_surface_destroy(lg);
  }
  XFlush(dpy);
}

static void scan_existing(void) {
  Window r, p, *kids = NULL;
  unsigned n = 0;
  if (!XQueryTree(dpy, root, &r, &p, &kids, &n)) return;
  for (unsigned i = 0; i < n; i++) {
    XWindowAttributes wa;
    if (kids[i] == menubar || kids[i] == dock || kids[i] == checkwin) continue;
    if (XGetWindowAttributes(dpy, kids[i], &wa) && !wa.override_redirect && wa.map_state == IsViewable)
      manage(kids[i], true, false);
  }
  if (kids) XFree(kids);
}

int main(int argc, char **argv) {
  bool use_comp = true;
  debug = getenv("CLOAKSERVER_DEBUG") != NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
      i++;
      theme_mode = !strcmp(argv[i], "hell") ? 1 : !strcmp(argv[i], "dunkel") ? 2 : 0;
    } else if (!strcmp(argv[i], "--compositor") && i + 1 < argc) {
      use_comp = strcmp(argv[++i], "aus") != 0;
    } else {
      fprintf(stderr, "Aufruf: %s [--theme auto|hell|dunkel] [--compositor an|aus]\n", argv[0]);
      return 2;
    }
  }
  char self[PATH_MAX];
  if (!realpath(argv[0], self)) snprintf(self, sizeof self, "%s", argv[0]);
  snprintf(resdir, sizeof resdir, "%s/Resources", dirname(self));
  signal(SIGCHLD, SIG_IGN); /* gestartete Programme nicht als Zombies behalten */
  /* PureDarwins arm64-Kernel hat kein System-V-Shared-Memory: shmget, das cairo und Xlib für
   * MIT-SHM ausprobieren, beendet das Programm sonst mit SIGSYS statt mit ENOSYS. Ignoriert, gibt
   * es nur einen Fehler, und alle zeichnen ohne Shared Memory. Gestartete Programme erben das. */
  signal(SIGSYS, SIG_IGN);
  signal(SIGSEGV, crash_log);
  signal(SIGBUS, crash_log);
  signal(SIGILL, crash_log);
  signal(SIGFPE, crash_log);
  signal(SIGABRT, crash_log);
  rec_init();
  l10n_init();
  setenv("LANG", l10n_locale(), 1); /* für die Programme der Sitzung */

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    warn("kein X-Server (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "");
    return 1;
  }
  fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);
  scr = DefaultScreen(dpy);
  root = RootWindow(dpy, scr);
  SW = DisplayWidth(dpy, scr);
  SH = DisplayHeight(dpy, scr);
  vis = DefaultVisual(dpy, scr);
  cmap = DefaultColormap(dpy, scr);

  XSetErrorHandler(xerror_start);
  XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask | PropertyChangeMask);
  XSync(dpy, False);
  if (other_wm) {
    warn("es läuft schon ein Fenstermanager");
    return 1;
  }
  XSetErrorHandler(xerror);
  setup_atoms();

  cur_arrow = XcursorLibraryLoadCursor(dpy, "left_ptr");
  cur_resize = XcursorLibraryLoadCursor(dpy, "bottom_right_corner");
  cur_move = XcursorLibraryLoadCursor(dpy, "fleur");
  if (!cur_move) cur_move = cur_arrow;
  if (cur_arrow) XDefineCursor(dpy, root, cur_arrow);

  read_release();
  splash();
  dark = want_dark();
  xsettings_init();
  apply_theme();
  load_keyboard();
  if (use_comp && !comp_init(dpy, scr, root, SW, SH)) warn("ohne Compositor: keine Schatten, Glas zeigt nur das Hintergrundbild");
  setup_ewmh();
  menubar = ui_window(0, 0, SW, MENUBAR_H, ExposureMask | ButtonPressMask | PointerMotionMask, false,
                      NET_WM_WINDOW_TYPE_DOCK);
  dock_lift = comp_on ? 20 : 0;
  scan_apps();
  load_prefs();
  render_icons();
  if (want_dark() != dark || wall_sel) apply_theme();
  layout_dock();
  update_workarea();
  int dwx, dwy, dww, dwh;
  dock_window_rect(&dwx, &dwy, &dww, &dwh);
  dock = ui_window(dwx, dwy, dww, dwh, ExposureMask | ButtonPressMask | PointerMotionMask | LeaveWindowMask,
                   true, NET_WM_WINDOW_TYPE_DOCK);
  XMapRaised(dpy, menubar);
  if (!recovery) XMapRaised(dpy, dock); /* die Wiederherstellung hat kein Dock */
  XGrabKey(dpy, XKeysymToKeycode(dpy, XK_Tab), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
  XGrabKey(dpy, XKeysymToKeycode(dpy, XK_F4), Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
  XGrabKey(dpy, XKeysymToKeycode(dpy, XK_space), ControlMask, root, True, GrabModeAsync, GrabModeAsync);
  render_mb_icons();
  scan_existing();
  save_root_env();
  load_accounts();
  if (recovery) { /* als root, ohne Anmeldung, gleich das Fenster der Wiederherstellung */
    for (int i = 0; i < naccounts; i++)
      if (accounts[i].uid == 0) start_session(&accounts[i]);
    if (!session) start_session(&accounts[0]);
    rec_show(RP_CHOOSE);
  } else if (setup_needed()) show_setup(); /* erster Start nach der Installation */
  else if (strcmp(os_login, "nein")) show_login(false);
  else start_session(&accounts[0]);
  focus_client(clients);
  redraw_all();
  warn("läuft auf %dx%d, Thema %s", SW, SH, dark ? "dunkel" : "hell");

  int fd = ConnectionNumber(dpy);
  for (;;) {
    for (;;) {
      /* Manche Bibliotheken setzen zwischendurch eigene Fehlerhandler; der Standardhandler von
       * Xlib beendet das Programm schon bei harmlosen Fehlern. */
      XSetErrorHandler(xerror);
      while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (debug && (ev.type == ButtonPress || ev.type == ButtonRelease))
          warn("%s Taste %u an 0x%lx (%d,%d) Zeit %lu", ev.type == ButtonPress ? "drückt" : "lässt los",
               ev.xbutton.button, ev.xbutton.window, ev.xbutton.x_root, ev.xbutton.y_root, ev.xbutton.time);
        comp_handle(&ev);
        switch (ev.type) {
        case MapRequest: {
          Client *c = find_client(ev.xmaprequest.window);
          if (c) restore_client(c);
          else manage(ev.xmaprequest.window, false, false);
          draw_dock();
          break;
        }
        case UnmapNotify: {
          Client *c = find_client(ev.xunmap.window);
          if (c && ev.xunmap.window == c->win) {
            if (c->ignore_unmaps) c->ignore_unmaps--;
            else unmanage(c, false);
          }
          break;
        }
        case DestroyNotify: {
          Client *c = find_client(ev.xdestroywindow.window);
          if (c && ev.xdestroywindow.window == c->win) unmanage(c, true);
          break;
        }
        case ConfigureRequest: on_configure_request(&ev.xconfigurerequest); break;
        case PropertyNotify: {
          if (ev.xproperty.window == root && ev.xproperty.atom == CLOKE_OPEN && ev.xproperty.state == PropertyNewValue) {
            open_requests();
            break;
          }
          Client *c = find_client(ev.xproperty.window);
          if (c && (ev.xproperty.atom == XA_WM_NAME || ev.xproperty.atom == NET_WM_NAME)) {
            update_title(c);
            draw_frame(c);
          }
          if (c && ev.xproperty.window == c->win && ev.xproperty.atom == CLOKE_TITLEBAR) titlebar_changed(c);
          if (c && ev.xproperty.window == c->win) gm_property(c, ev.xproperty.atom);
          break;
        }
        case ButtonPress: on_button_press(&ev.xbutton); break;
        case ButtonRelease: on_button_release(&ev.xbutton); break;
        case MotionNotify: on_motion(&ev.xmotion); break;
        case LeaveNotify: on_leave(&ev.xcrossing); break;
        case Expose: on_expose(&ev.xexpose); break;
        case ClientMessage: on_client_message(&ev.xclient); break;
        case KeyPress: on_key(&ev.xkey); break;
        }
      }
      /* Mit Compositor: erst zeichnen, wenn auch die Ereignisse zu den eigenen Anfragen (Menü zu,
       * Namensschild weg) da sind, sonst zeigt das Bild Fenster, die es nicht mehr gibt. */
      if (!comp_on) break;
      XSync(dpy, False);
      if (!XPending(dpy)) break;
    }
    tick();
    if (pending_focus) {
      Window rw, cw;
      int rx, ry, wx, wy;
      unsigned mask = 0;
      XQueryPointer(dpy, root, &rw, &cw, &rx, &ry, &wx, &wy, &mask);
      if (!(mask & (Button1Mask | Button2Mask | Button3Mask)) || now_s() - pending_focus_t0 > 60) {
        if (client_alive(pending_focus) && !pending_focus->minimized) {
          raise_client(pending_focus);
          focus_client(pending_focus);
        }
        pending_focus = NULL;
      }
    }
    if (recovery && rp == RP_RUN) rec_redraw(); /* Restzeit und Balken laufen auch ohne Meldung des Installers weiter */
    bool anim = animate();
    /* Ein Klick in ein inaktives Programmfenster friert den Zeiger ein, bis XAllowEvents kommt
     * (grab_buttons). Geht das einmal schief, steht der Zeiger für immer: hier noch einmal lösen. */
    if (!menuwin && !drag) XAllowEvents(dpy, AsyncPointer, CurrentTime);
    comp_repaint();
    XFlush(dpy);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    if (gm_fd >= 0) FD_SET(gm_fd, &fds);
    if (rec_fd >= 0) FD_SET(rec_fd, &fds);
    int maxfd = fd > gm_fd ? fd : gm_fd;
    if (rec_fd > maxfd) maxfd = rec_fd;
    struct timeval tv = {1, 0};
    if (shake_t0 >= 0 || anim) tv = (struct timeval){0, 16000}; /* Animation: so oft wie möglich */
    if (pending_focus) tv = (struct timeval){0, 20000}; /* auf das Loslassen warten */
    if (setup_anim()) tv = (struct timeval){0, 50000};     /* "hello" im Einrichtungsassistenten */
    select(maxfd + 1, &fds, NULL, NULL, &tv);
    if (gm_fd >= 0 && FD_ISSET(gm_fd, &fds)) gm_read();
    if (rec_fd >= 0 && FD_ISSET(rec_fd, &fds)) rec_read();
  }
}
