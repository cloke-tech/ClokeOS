/* CloakServer: der Fensterserver von ClokeOS. Gemeinsame Deklarationen. */
#ifndef CLOAKSERVER_H
#define CLOAKSERVER_H

#include <X11/Xlib.h>
#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

/* wall.c: Hintergrundbild "Goldenes Tor" aus clokeos.html */
void wall_draw(cairo_t *cr, double W, double H, bool dark);
extern int wall_sel;
int wall_count(void);
const char *wall_title(int i);
const char *wall_key(int i);
int wall_find(const char *key);
void wall_thumb(cairo_t *cr, int idx, double x, double y, double w, double h, bool dark);

/* cloakserver.c */
void rrect(cairo_t *cr, double x, double y, double w, double h, double r);

/* Wie der Compositor ein Fenster zeigt: Rahmen und Glas mit runden Ecken und Schatten */
enum { WK_PLAIN, WK_FRAME, WK_GLASS };
typedef struct {
  int kind;
  double radius;
  int blur, offset; /* Schatten: Unschärfe und Versatz nach unten in Pixeln, 0 = keiner */
  double alpha;
  int it, ir, ib, il; /* Glas und Schatten nur innerhalb dieser Ränder (Dock: Platz zum Hüpfen) */
  /* Animation, 0 = unverändert: verkleinern um shrink (0..1) zur Mitte, ausblenden um fade
   * (0..1), verschieben um dx, dy */
  double shrink, fade, dx, dy;
} WinStyle;
WinStyle cs_window_style(Window w);
/* nach dem Bild eines Fensterrahmens: was CloakServer darüber malt (Ampelknöpfe bei _CLOKE_TITLEBAR = Overlay) */
void cs_window_overlay(Window frame, cairo_t *cr, double alpha);
Pixmap cs_wallpaper(void);

/* apps.c: Programme als .app-Bundles wie bei macOS */
/* APP_ARCH: nur für die andere Architektur gebaut (arm64 auf x86_64 oder umgekehrt);
 * APP_ARM64E: nur arm64e (Apples eigene Programme), das ClokeOS auf arm64 nicht ausführt */
enum { APP_OK, APP_APPKIT, APP_ARCH, APP_MISSING, APP_NOEXEC, APP_ARM64E };
/* Darlings Mac-Frameworks (AppKit, Foundation …, macapps/), gefunden über DYLD_FRAMEWORK_PATH */
#define MAC_FRAMEWORKS "/Library/ClokeOS/MacFrameworks"
typedef struct {
  char path[1024];      /* /System/Applications/Rechner.app */
  char name[96];        /* CFBundleDisplayName, CFBundleName oder Ordnername */
  char ident[128];      /* CFBundleIdentifier */
  char exec_name[96];   /* CFBundleExecutable */
  char exec[1200];      /* Contents/MacOS/<CFBundleExecutable> */
  char classes[192];    /* CLWindowClasses: WM_CLASS der Fenster */
  char internal[24];    /* CLInternal: zeichnet CloakServer selbst (launchpad, settings) */
  char icon[1200];      /* Contents/Resources/…: .svg, .png oder .icns */
  char missing[160];    /* kind != APP_OK: was fehlt */
  int kind;             /* APP_*: lässt es sich starten? */
  bool mac, hidden;     /* Mach-O wie bei macOS; LSUIElement */
  bool macfw;           /* braucht Frameworks aus MAC_FRAMEWORKS: Start mit DYLD_FRAMEWORK_PATH */
  long mtime;
} Bundle;
typedef struct {
  const char *key, *s;  /* s = Text, sonst Zahl i, oder Liste arr */
  long i;
  const char *const *arr;
  int narr;
} PlistItem;
unsigned char *read_bytes(const char *path, size_t *len);
bool plist_get(const unsigned char *data, size_t len, const char *key, char *out, size_t n);
int plist_get_array(const unsigned char *data, const char *key, char out[][128], int max);
bool plist_write(const char *path, const PlistItem *items, int n);
bool bundle_read(const char *path, Bundle *b);
long bundle_mtime(const char *path);
unsigned char *icns_png(const char *path, int min, size_t *len);

/* l10n.c: Sprache. Texte stehen englisch im Code, L() übersetzt in die eingestellte Sprache */
#define LANGUAGE_FILE "/etc/clokeos-language"
struct tm;
void l10n_init(void);
const char *L(const char *en);
const char *l10n_lang(void);            /* "en" oder "de" */
bool l10n_set_lang(const char *code);   /* schreibt LANGUAGE_FILE (root) */
const char *l10n_locale(void);
int l10n_count(void);
const char *l10n_code(int i);
const char *l10n_name(int i);
bool l10n_24h(void);          /* LANG für die Programme der Sitzung */
const char *l10n_month(int m);
const char *l10n_month_short(int m);
void l10n_date_long(char *out, size_t n, const struct tm *t);
void l10n_menubar_clock(char *out, size_t n, const struct tm *t);
void l10n_big_clock(char *out, size_t n, const struct tm *t);

/* comp.c: Compositor (Composite, Damage, XFixes) */
extern bool comp_on;
extern Visual *comp_argb;
extern Colormap comp_argb_cmap;
bool comp_init(Display *dpy, int scr, Window root, int sw, int sh);
void comp_handle(XEvent *ev);
void comp_dirty(int x, int y, int w, int h);
void comp_dirty_all(void);
void comp_repaint(void);
bool comp_quiet_error(XErrorEvent *e);

#endif
