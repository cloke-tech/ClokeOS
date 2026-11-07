/* Einrichtungsassistent wie bei macOS. Wird in cloakserver.c eingebunden (#include) und nutzt
 * dessen Zeichenhelfer, Konten und Sitzungen.
 *
 * Liegt /var/db/.RunClokeOSSetup im System (recovery/build.sh legt es ins System des
 * Online-Releases, also auf jede frisch installierte Platte), zeigt CloakServer beim ersten Start
 * statt der Anmeldung den Assistenten: „Hello“ in wechselnden Sprachen, Sprache, Land oder Region
 * (Zeitzone), Benutzerkonto, Erscheinungsbild. Danach meldet er das neue Konto an und löscht die
 * Markierung; /var/db/.ClokeOSSetupDone hält fest, dass die Einrichtung gelaufen ist (wie
 * /var/db/.AppleSetupDone bei macOS). */

#define SETUP_MARK "/var/db/.RunClokeOSSetup"
#define SETUP_DONE "/var/db/.ClokeOSSetupDone"

enum { SP_HELLO, SP_LANG, SP_REGION, SP_ACCOUNT, SP_LOOK, SP_N };

static int sp, sp_region = -1, sp_look = 0;
static double sp_t0; /* Beginn der Hallo-Animation */
static char sp_error[200];

/* Konto: Felder und welches gerade Eingaben bekommt */
enum { SF_FULL, SF_NAME, SF_PW, SF_PW2, SF_N };
static char sf[SF_N][64];
static int sf_focus;
static bool sf_name_edited; /* Kontoname nicht mehr aus dem vollen Namen ableiten */

/* Länder und Regionen mit Zeitzone als POSIX-Regel (PureDarwin hat keine Zeitzonen-Datenbank) */
static const struct { const char *name, *tz; } REGIONS[] = {
  {"United States (Eastern)", "EST5EDT,M3.2.0,M11.1.0"},
  {"United States (Pacific)", "PST8PDT,M3.2.0,M11.1.0"},
  {"United Kingdom", "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Germany", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Austria", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Switzerland", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"France", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Spain", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Italy", "CET-1CEST,M3.5.0,M10.5.0/3"},
};
#define NREGIONS (int)(sizeof REGIONS / sizeof REGIONS[0])

static const char *const HELLO[] = {"hello", "hallo", "bonjour", "hola", "ciao", "olá", "hej", "привет", "こんにちは", "你好"};
#define NHELLO (int)(sizeof HELLO / sizeof HELLO[0])

/* Kartenmaße */
#define SC_W 760
#define SC_H 540

static bool setup_needed(void) { return !recovery && access(SETUP_MARK, F_OK) == 0; }

static bool setup_anim(void) { return setup_active && sp == SP_HELLO; }

static void card_rect(double *x, double *y) {
  *x = (SW - SC_W) / 2.0;
  *y = (SH - SC_H) / 2.0;
}

/* ---- Symbole der Seiten, mit cairo gezeichnet ------------------------------------------- */

static void icon_globe(cairo_t *cr, double cx, double cy, double r) {
  cairo_set_line_width(cr, 2.5);
  set(cr, ACCENT);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  cairo_stroke(cr);
  cairo_save(cr);
  cairo_translate(cr, cx, cy);
  cairo_scale(cr, .45, 1);
  cairo_new_path(cr);
  cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
  cairo_restore(cr);
  cairo_stroke(cr);
  cairo_move_to(cr, cx, cy - r);
  cairo_line_to(cr, cx, cy + r);
  cairo_move_to(cr, cx - r, cy);
  cairo_line_to(cr, cx + r, cy);
  for (int s = -1; s <= 1; s += 2) {
    double yy = cy + s * r * .5, hw = sqrt(r * r - (r * .5) * (r * .5));
    cairo_move_to(cr, cx - hw, yy);
    cairo_line_to(cr, cx + hw, yy);
  }
  cairo_stroke(cr);
}

static void icon_pin(cairo_t *cr, double cx, double cy, double r) {
  set(cr, ACCENT);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy - r * .25, r * .62, M_PI * .85, M_PI * .15);
  cairo_line_to(cr, cx, cy + r);
  cairo_close_path(cr);
  cairo_fill(cr);
  set(cr, T->win);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy - r * .25, r * .25, 0, 2 * M_PI);
  cairo_fill(cr);
}

static void icon_person(cairo_t *cr, double cx, double cy, double r) {
  set(cr, ACCENT);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  cairo_fill(cr);
  set(cr, (RGBA){1, 1, 1, .95});
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy - r * .22, r * .3, 0, 2 * M_PI);
  cairo_fill(cr);
  cairo_save(cr);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  cairo_clip(cr);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy + r * .78, r * .58, M_PI, 2 * M_PI);
  cairo_fill(cr);
  cairo_restore(cr);
}

static void icon_look(cairo_t *cr, double cx, double cy, double r) {
  set(cr, ACCENT);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  cairo_fill(cr);
  set(cr, T->win);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, r * .78, -M_PI / 2, M_PI / 2);
  cairo_close_path(cr);
  cairo_fill(cr);
}

/* ---- Zeichnen ---------------------------------------------------------------------------- */

static void sp_background(cairo_t *cr) {
  cairo_save(cr);
  cairo_scale(cr, 4, 4);
  cairo_set_source_surface(cr, wall_blur, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_set_source_rgba(cr, 10 / 255., 6 / 255., 20 / 255., sp == SP_HELLO ? .35 : .25);
  cairo_paint(cr);
}

/* Liste wählbarer Zeilen (Sprache, Region): gibt die Höhe einer Zeile zurück */
#define SP_ROW 30
static void sp_list(cairo_t *cr, double x, double y, double w, const char *const *items, int n, int sel) {
  rrect(cr, x, y, w, n * SP_ROW + 8, 10);
  seta(cr, T->fg, .05);
  cairo_fill(cr);
  for (int i = 0; i < n; i++) {
    double ry = y + 4 + i * SP_ROW;
    if (i == sel) {
      rrect(cr, x + 4, ry, w - 8, SP_ROW, 7);
      set(cr, ACCENT);
      cairo_fill(cr);
    }
    text(cr, x + 18, ry + SP_ROW / 2.0, items[i], 13.5, false, i == sel ? (RGBA){1, 1, 1, 1} : T->fg, w - 36, 0);
  }
}

static const char *sp_field_label(int f) {
  static const char *const labels[SF_N] = {"Full name", "Account name", "Password", "Verify"};
  return L(labels[f]);
}

static void sp_field(cairo_t *cr, double x, double y, double w, int f) {
  text(cr, x, y + 15, sp_field_label(f), 12.5, false, T->fg2, 0, 2);
  double fx = x + 14;
  rrect(cr, fx, y, w, 30, 7);
  set(cr, dark ? (RGBA){1, 1, 1, .08} : (RGBA){1, 1, 1, 1});
  cairo_fill_preserve(cr);
  if (f == sf_focus) {
    set(cr, ACCENT);
    cairo_set_line_width(cr, 2);
  } else {
    set(cr, T->line_strong);
    cairo_set_line_width(cr, 1);
  }
  cairo_stroke(cr);
  char shown[3 * 64 + 1] = "";
  if (f == SF_PW || f == SF_PW2) {
    int n = (int)g_utf8_strlen(sf[f], -1);
    for (int i = 0; i < n && i < 40; i++) strcat(shown, "•");
  } else {
    snprintf(shown, sizeof shown, "%s", sf[f]);
  }
  int tw = text(cr, fx + 10, y + 15, shown, 13, false, T->fg, (int)w - 20, 0);
  if (f == sf_focus) { /* Schreibmarke */
    set(cr, ACCENT);
    cairo_rectangle(cr, fx + 10 + fmin(tw, w - 22) + 1, y + 7, 1.5, 16);
    cairo_fill(cr);
  }
  if (f == SF_FULL && !sf[f][0] && f != sf_focus)
    text(cr, fx + 10, y + 15, L("Required"), 13, false, T->fg3, 0, 0);
}

/* Knöpfe unten: Zurück links (ab Seite 2), Weiter rechts */
static void sp_buttons(cairo_t *cr, double cx, double cy, bool can_continue, const char *next) {
  if (sp > SP_LANG) button(cr, cx + 28, cy + SC_H - 52, 110, L("Back"), false);
  double nw = text_width(next, 12.5, false) + 44;
  if (nw < 110) nw = 110;
  rrect(cr, cx + SC_W - 28 - nw, cy + SC_H - 52, nw, 28, 14);
  RGBA a = ACCENT;
  if (!can_continue) a.a = .4;
  set(cr, a);
  cairo_fill(cr);
  text(cr, cx + SC_W - 28 - nw / 2, cy + SC_H - 38, next, 12.5, false, (RGBA){1, 1, 1, can_continue ? 1 : .7}, 0, 1);
}

static bool sp_account_ok(void);

static void paint_setup(cairo_t *cr, int w, int h, void *arg) {
  (void)arg;
  sp_background(cr);
  if (sp == SP_HELLO) {
    double t = now_s() - sp_t0, per = 2.4;
    int k = (int)(t / per) % NHELLO;
    double f = fmod(t, per) / per, a = f < .2 ? f / .2 : f > .8 ? (1 - f) / .2 : 1;
    if (logo_big || (logo_big = svg_render("logo-gradient.svg", 96, 96, NULL))) {
      cairo_set_source_surface(cr, logo_big, (w - 96) / 2.0, h * .24);
      cairo_paint(cr);
    }
    double y = h * .5;
    text(cr, w / 2.0, y + 3, HELLO[k], 84, true, (RGBA){0, 0, 0, .18 * a}, 0, 1);
    text(cr, w / 2.0, y, HELLO[k], 84, true, (RGBA){1, 1, 1, a}, 0, 1);
    /* Knopf "Get Started" mit Pfeil */
    const char *go = L("Get Started");
    double bw = text_width(go, 15, false) + 70, bx = (w - bw) / 2.0, by = h * .72;
    rrect(cr, bx, by, bw, 40, 20);
    cairo_set_source_rgba(cr, 1, 1, 1, .22);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, .45);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    text(cr, bx + 24, by + 20, go, 15, false, (RGBA){1, 1, 1, 1}, 0, 0);
    double ax = bx + bw - 30, ay = by + 20;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, ax - 6, ay);
    cairo_line_to(cr, ax + 6, ay);
    cairo_move_to(cr, ax + 1, ay - 5);
    cairo_line_to(cr, ax + 6, ay);
    cairo_line_to(cr, ax + 1, ay + 5);
    cairo_stroke(cr);
    return;
  }
  double cx, cy;
  card_rect(&cx, &cy);
  /* Karte mit Schatten */
  for (int i = 6; i > 0; i--) {
    rrect(cr, cx - i * 2, cy - i * 2 + 6, SC_W + i * 4, SC_H + i * 4, 20 + i * 2);
    cairo_set_source_rgba(cr, 0, 0, 0, .025);
    cairo_fill(cr);
  }
  rrect(cr, cx, cy, SC_W, SC_H, 18);
  set(cr, T->win);
  cairo_fill(cr);
  double icx = cx + SC_W / 2.0, icy = cy + 62;
  const char *title = "", *sub = "";
  switch (sp) {
  case SP_LANG:
    icon_globe(cr, icx, icy, 26);
    title = L("Select Your Language");
    sub = L("ClokeOS, the login window and the apps use this language. You can change it later in System Settings.");
    break;
  case SP_REGION:
    icon_pin(cr, icx, icy + 2, 28);
    title = L("Select Your Country or Region");
    sub = L("Sets the time zone of this computer.");
    break;
  case SP_ACCOUNT:
    icon_person(cr, icx, icy, 28);
    title = L("Create a Computer Account");
    sub = L("Fill out the following information to create your computer account. Without a password, pressing "
            "Return logs you in.");
    break;
  case SP_LOOK:
    icon_look(cr, icx, icy, 26);
    title = L("Choose Your Look");
    sub = L("Select an appearance. You can change it later in System Settings or in the menu bar.");
    break;
  }
  text(cr, icx, cy + 120, title, 22, true, T->fg, SC_W - 60, 1);
  para(cr, cx + 90, cy + 140, SC_W - 180, sub, 12.5, T->fg2, true);
  double lx = cx + 200, lw = SC_W - 400, ly = cy + 200;
  bool can = true;
  const char *next = L("Continue");
  switch (sp) {
  case SP_LANG: {
    const char *names[16];
    int n = l10n_count(), sel = 0;
    for (int i = 0; i < n && i < 16; i++) {
      names[i] = l10n_name(i);
      if (!strcmp(l10n_code(i), l10n_lang())) sel = i;
    }
    sp_list(cr, lx, ly, lw, names, n, sel);
    break;
  }
  case SP_REGION: {
    const char *names[NREGIONS];
    for (int i = 0; i < NREGIONS; i++) names[i] = L(REGIONS[i].name);
    sp_list(cr, lx, ly - 10, lw, names, NREGIONS, sp_region);
    can = sp_region >= 0;
    break;
  }
  case SP_ACCOUNT: {
    double fx = cx + 260, fw = 300;
    for (int f = 0; f < SF_N; f++) sp_field(cr, fx - 14, cy + 200 + f * 46, fw, f);
    if (sp_error[0]) text_block(cr, fx, cy + 200 + SF_N * 46 + 4, sp_error, 11.5, false, (RGBA){.86, .24, .26, 1}, fw);
    else
      text_block(cr, fx, cy + 200 + SF_N * 46 + 4,
                 L("The account name is the name of your home folder and cannot be changed later."), 11, false, T->fg3,
                 fw);
    can = sp_account_ok();
    break;
  }
  case SP_LOOK: {
    static const char *const names[3] = {"Light", "Dark", "Auto"};
    static const int look[3] = {0, 1, 2}, mode[3] = {1, 2, 0};
    for (int i = 0; i < 3; i++) {
      double bx = cx + 110 + i * 190, by = cy + 205;
      if (sp_look == mode[i]) {
        rrect(cr, bx - 5, by - 5, 170, 116, 12);
        set(cr, ACCENT);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
      }
      mini_screen(cr, bx, by, 160, 106, look[i], POS_BOTTOM);
      text(cr, bx + 80, by + 132, L(names[i]), 13, sp_look == mode[i], T->fg, 0, 1);
    }
    next = L("Start Using ClokeOS");
    break;
  }
  }
  sp_buttons(cr, cx, cy, can, next);
  /* Seitenpunkte */
  for (int i = SP_LANG; i < SP_N; i++) {
    cairo_new_path(cr);
    cairo_arc(cr, icx + (i - (SP_LANG + SP_N - 1) / 2.0) * 14, cy + SC_H - 38, 3.5, 0, 2 * M_PI);
    i == sp ? set(cr, ACCENT) : seta(cr, T->fg, .2);
    cairo_fill(cr);
  }
}

static void draw_setup(void) {
  if (setup_active && setupwin) paint_window(setupwin, SW, SH, paint_setup, NULL);
}

/* ---- Konto ------------------------------------------------------------------------------- */

/* Kontoname aus dem vollen Namen: klein, ohne Leerzeichen und Umlaute, wie macOS es vorschlägt */
static void sp_derive_name(void) {
  char out[32];
  size_t k = 0;
  for (const unsigned char *p = (const unsigned char *)sf[SF_FULL]; *p && k + 2 < sizeof out; p++) {
    unsigned char ch = *p;
    if (ch == 0xc3 && p[1]) { /* Umlaute aus UTF-8 */
      unsigned char n = *++p;
      const char *r = n == 0xa4 ? "ae" : n == 0xb6 ? "oe" : n == 0xbc ? "ue" : n == 0x9f ? "ss" : n == 0x84 ? "ae"
                      : n == 0x96 ? "oe" : n == 0x9c ? "ue" : "";
      for (; *r && k + 2 < sizeof out; r++) out[k++] = *r;
    } else if (isalnum(ch)) {
      out[k++] = (char)tolower(ch);
    }
  }
  out[k] = 0;
  snprintf(sf[SF_NAME], sizeof sf[SF_NAME], "%s", out);
}

static bool sp_valid_name(const char *n) {
  if (!n[0] || strlen(n) > 31 || !(islower((unsigned char)n[0]) || n[0] == '_')) return false;
  for (const char *p = n; *p; p++)
    if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) return false;
  static const char *const reserved[] = {"root", "daemon", "nobody", "guest", "admin", "wheel", "staff", "operator"};
  for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++)
    if (!strcmp(n, reserved[i])) return false;
  return true;
}

static bool sp_account_ok(void) {
  return sf[SF_FULL][0] && sp_valid_name(sf[SF_NAME]) && !getpwnam(sf[SF_NAME]) && !strcmp(sf[SF_PW], sf[SF_PW2]);
}

static void sp_account_error(void) {
  sp_error[0] = 0;
  if (!sf[SF_FULL][0]) snprintf(sp_error, sizeof sp_error, "%s", L("Enter your full name."));
  else if (!sp_valid_name(sf[SF_NAME]))
    snprintf(sp_error, sizeof sp_error, "%s", L("The account name may contain only lowercase letters, digits and . _ -"));
  else if (getpwnam(sf[SF_NAME])) snprintf(sp_error, sizeof sp_error, "%s", L("This account name is already in use."));
  else if (strcmp(sf[SF_PW], sf[SF_PW2])) snprintf(sp_error, sizeof sp_error, "%s", L("The passwords don’t match."));
}

/* Passwort-Hash: MD5-crypt ($1$), wenn PureDarwins crypt() es kann, sonst DES wie früher */
static void sp_hash(const char *pw, char *out, size_t n) {
  out[0] = 0;
  if (!pw[0]) return;
  static const char cs[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  char salt[16] = "$1$";
  for (int i = 0; i < 8; i++) salt[3 + i] = cs[arc4random_uniform(64)];
  salt[11] = '$';
  salt[12] = 0;
  const char *h = crypt(pw, salt);
  if (h && !strncmp(h, "$1$", 3)) {
    snprintf(out, n, "%s", h);
    return;
  }
  char s2[3] = {cs[arc4random_uniform(64)], cs[arc4random_uniform(64)], 0};
  h = crypt(pw, s2);
  snprintf(out, n, "%s", h ? h : "*");
}

static bool sp_append(const char *path, const char *line) {
  FILE *f = fopen(path, "a");
  if (!f) return false;
  bool ok = fputs(line, f) >= 0;
  return fclose(f) == 0 && ok;
}

/* Mitglied in einer Gruppe von /etc/group: die Zeile "name:*:gid:a,b" um ",user" ergänzen */
static void sp_group_add(const char *group, const char *user) {
  char *s = read_file("/etc/group");
  if (!s) return;
  size_t n = strlen(s), gl = strlen(group);
  char *out = malloc(n + strlen(user) + 4);
  if (!out) {
    free(s);
    return;
  }
  size_t k = 0;
  for (char *line = s; *line;) {
    char *nl = strchr(line, '\n');
    size_t len = nl ? (size_t)(nl - line) : strlen(line);
    memcpy(out + k, line, len);
    k += len;
    if (!strncmp(line, group, gl) && line[gl] == ':') {
      bool empty = len > 0 && line[len - 1] == ':';
      k += sprintf(out + k, "%s%s", empty ? "" : ",", user);
    }
    if (nl) out[k++] = '\n';
    line += len + (nl ? 1 : 0);
  }
  out[k] = 0;
  FILE *f = fopen("/etc/group", "w");
  if (f) {
    fputs(out, f);
    fclose(f);
  }
  free(out);
  free(s);
}

static void sp_write_owned(const char *path, const char *text_, uid_t uid, mode_t mode) {
  FILE *f = fopen(path, "w");
  if (!f) return;
  fputs(text_, f);
  fclose(f);
  chmod(path, mode);
  if (chown(path, uid, 20)) warn("chown %s: %s", path, strerror(errno));
}

static void sp_mkdir_owned(const char *path, uid_t uid, mode_t mode) {
  mkdir(path, mode);
  chmod(path, mode);
  if (chown(path, uid, 20)) warn("chown %s: %s", path, strerror(errno));
}

/* Konto anlegen wie brand.sh das Gastkonto: master.passwd, passwd, Gruppe staff (und admin),
 * Benutzerordner mit den Ordnern von macOS und einer Begrüßung in der gewählten Sprache */
static bool sp_create_account(Account *out) {
  uid_t uid = 501;
  setpwent();
  for (struct passwd *p; (p = getpwent());)
    if (p->pw_uid >= uid && p->pw_uid < 60000) uid = p->pw_uid + 1;
  endpwent();
  const char *name = sf[SF_NAME];
  char home[300], hash[128], line[700];
  snprintf(home, sizeof home, "/Users/%s", name);
  sp_hash(sf[SF_PW], hash, sizeof hash);
  /* ':' im vollen Namen würde die Zeile zerlegen */
  char full[64];
  snprintf(full, sizeof full, "%s", sf[SF_FULL]);
  for (char *p = full; *p; p++)
    if (*p == ':') *p = ' ';
  snprintf(line, sizeof line, "%s:%s:%u:20::0:0:%s:%s:/bin/zsh\n", name, hash, (unsigned)uid, full, home);
  if (!sp_append("/etc/master.passwd", line)) {
    snprintf(sp_error, sizeof sp_error, L("Could not create the account: %s"), strerror(errno));
    return false;
  }
  snprintf(line, sizeof line, "%s:*:%u:20:%s:%s:/bin/zsh\n", name, (unsigned)uid, full, home);
  sp_append("/etc/passwd", line);
  sp_group_add("staff", name);
  if (getgrnam("admin")) sp_group_add("admin", name);

  mkdir("/Users", 0755);
  sp_mkdir_owned(home, uid, 0755);
  static const char *const dirs[] = {"Desktop", "Documents", "Downloads", "Pictures", "Music", "Movies", "Library",
                                     "Library/Preferences", ".config", ".config/gtk-3.0"};
  char p[600];
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
    snprintf(p, sizeof p, "%s/%s", home, dirs[i]);
    sp_mkdir_owned(p, uid, 0755);
  }
  snprintf(p, sizeof p, "%s/.Trash", home);
  sp_mkdir_owned(p, uid, 0700);
  if (!access("/NT/Users", W_OK)) {
    /* Windows-Ordner des Kontos in /NT/Users (Raven): AppData, und Documents, Desktop ... sind die Ordner des Benutzers */
    char nt[400], link[600];
    snprintf(nt, sizeof nt, "/NT/Users/%s", name);
    sp_mkdir_owned(nt, uid, 0755);
    static const char *const sub[] = {"AppData", "AppData/Local", "AppData/Roaming"};
    for (size_t i = 0; i < sizeof sub / sizeof sub[0]; i++) {
      snprintf(link, sizeof link, "%s/%s", nt, sub[i]);
      sp_mkdir_owned(link, uid, 0755);
    }
    static const char *const pair[][2] = {{"Documents", "Documents"}, {"Desktop", "Desktop"}, {"Downloads", "Downloads"},
                                          {"Pictures", "Pictures"}, {"Music", "Music"}, {"Videos", "Movies"}};
    for (size_t i = 0; i < sizeof pair / sizeof pair[0]; i++) {
      char target[600];
      snprintf(link, sizeof link, "%s/%s", nt, pair[i][0]);
      snprintf(target, sizeof target, "%s/%s", home, pair[i][1]);
      if (!symlink(target, link) && lchown(link, uid, 20)) warn("lchown %s: %s", link, strerror(errno));
    }
  }
  {
    /* Seitenleiste von Fundus (Thunar): Favoriten wie im Finder */
    static const char *const fav[] = {"Documents", "Downloads", "Pictures", "Music", "Movies"};
    char bm[1024];
    size_t bl = 0;
    for (size_t i = 0; i < sizeof fav / sizeof fav[0] && bl < sizeof bm; i++)
      bl += (size_t)snprintf(bm + bl, sizeof bm - bl, "file://%s/%s %s\n", home, fav[i], fav[i]);
    bl += (size_t)snprintf(bm + bl, sizeof bm - bl, "file:///Volumes Volumes\n");
    snprintf(p, sizeof p, "%s/.config/gtk-3.0/bookmarks", home);
    sp_write_owned(p, bm, uid, 0644);
  }
  char *css = read_file("/var/root/.config/gtk-3.0/gtk.css");
  if (css) {
    snprintf(p, sizeof p, "%s/.config/gtk-3.0/gtk.css", home);
    sp_write_owned(p, css, uid, 0644);
    free(css);
  }
  static const char *const dot[] = {"profile", "zprofile", "zshrc"};
  for (int i = 0; i < 3; i++) {
    char body[200];
    snprintf(body, sizeof body, "test -r /etc/%s && . /etc/%s\n%s", dot[i], dot[i], i == 2 ? "alias ll='ls -la'\n" : "");
    snprintf(p, sizeof p, "%s/.%s", home, dot[i]);
    sp_write_owned(p, body, uid, 0644);
  }
  char welcome[1200];
  snprintf(welcome, sizeof welcome,
           L("Welcome to ClokeOS %s, %s!\n\n"
             "This is your home folder. Documents, Downloads, Pictures and Music are\n"
             "here, the Trash is in the Dock.\n\n"
             "• In Terminal: clokefetch, uname -a, sw_vers\n"
             "• System Settings: appearance, Dock, language\n"
             "• Restart in Recovery in the ClokeOS menu reinstalls ClokeOS\n"),
           os_version, full);
  snprintf(p, sizeof p, "%s/Desktop/%s", home, L("Welcome.txt"));
  sp_write_owned(p, welcome, uid, 0644);

  memset(out, 0, sizeof *out);
  out->uid = uid;
  out->gid = 20;
  snprintf(out->name, sizeof out->name, "%s", name);
  snprintf(out->full, sizeof out->full, "%s", full);
  snprintf(out->home, sizeof out->home, "%s", home);
  snprintf(out->shell, sizeof out->shell, "/bin/zsh");
  snprintf(out->passwd, sizeof out->passwd, "%s", hash);
  return true;
}

/* Zeitzone in /etc/clokeos-release (TIMEZONE="…") und sofort für CloakServer */
static void sp_set_timezone(const char *tz) {
  char *s = read_file("/etc/clokeos-release");
  FILE *f = fopen("/etc/clokeos-release", "w");
  if (f) {
    bool done = false;
    for (char *line = s; line && *line;) {
      char *nl = strchr(line, '\n');
      size_t len = nl ? (size_t)(nl - line) : strlen(line);
      if (!strncmp(line, "TIMEZONE=", 9)) {
        fprintf(f, "TIMEZONE=\"%s\"\n", tz);
        done = true;
      } else {
        fwrite(line, 1, len, f);
        fputc('\n', f);
      }
      line += len + (nl ? 1 : 0);
    }
    if (!done) fprintf(f, "TIMEZONE=\"%s\"\n", tz);
    fclose(f);
  }
  free(s);
  setenv("TZ", tz, 1);
  tzset();
}

/* ---- Ablauf ------------------------------------------------------------------------------ */

static void show_setup(void) {
  setup_active = true;
  sp = SP_HELLO;
  sp_t0 = now_s();
  sp_region = -1;
  sp_look = theme_mode;
  memset(sf, 0, sizeof sf);
  sf_focus = SF_FULL;
  sf_name_edited = false;
  sp_error[0] = 0;
  if (!setupwin)
    setupwin = ui_window(0, 0, SW, SH, ExposureMask | ButtonPressMask | KeyPressMask, false, NET_WM_WINDOW_TYPE_SPLASH);
  XUnmapWindow(dpy, dock);
  XUnmapWindow(dpy, menubar);
  XMapRaised(dpy, setupwin);
  XSync(dpy, False);
  XSetInputFocus(dpy, setupwin, RevertToPointerRoot, CurrentTime);
  XGrabKeyboard(dpy, setupwin, True, GrabModeAsync, GrabModeAsync, CurrentTime);
  draw_setup();
}

static void setup_finish(void) {
  Account a;
  if (!sp_create_account(&a)) {
    sp = SP_ACCOUNT;
    draw_setup();
    return;
  }
  unlink(SETUP_MARK);
  int fd = open(SETUP_DONE, O_WRONLY | O_CREAT, 0644);
  if (fd >= 0) close(fd);
  warn("Einrichtung: Konto %s (UID %u) angelegt", a.name, (unsigned)a.uid);
  memset(sf, 0, sizeof sf);
  setup_active = false;
  XUngrabKeyboard(dpy, CurrentTime);
  XUnmapWindow(dpy, setupwin);
  load_accounts();
  for (int i = 0; i < naccounts; i++)
    if (accounts[i].uid == a.uid) sel_account = i;
  start_session(&a);
  set_theme_mode(sp_look); /* jetzt in den Einstellungen des neuen Kontos */
  XMapRaised(dpy, menubar);
  XMapRaised(dpy, dock);
  focus_client(clients);
  redraw_all();
}

static void sp_next(void) {
  switch (sp) {
  case SP_HELLO: sp = SP_LANG; break;
  case SP_LANG: sp = SP_REGION; break;
  case SP_REGION:
    if (sp_region < 0) return;
    sp_set_timezone(REGIONS[sp_region].tz);
    sp = SP_ACCOUNT;
    break;
  case SP_ACCOUNT:
    if (!sp_account_ok()) {
      sp_account_error();
      break;
    }
    sp_error[0] = 0;
    sp = SP_LOOK;
    break;
  case SP_LOOK: setup_finish(); return;
  }
  if (sp == SP_REGION && sp_region < 0) {
    const char *lg = l10n_lang();
    sp_region = !strcmp(lg, "de") ? 3 : !strcmp(lg, "fr") ? 6 : !strcmp(lg, "es") ? 7 : !strcmp(lg, "it") ? 8 : 0;
  }
  draw_setup();
}

static void sp_back(void) {
  if (sp > SP_LANG) sp--;
  sp_error[0] = 0;
  draw_setup();
}

static void setup_click(int x, int y) {
  if (sp == SP_HELLO) {
    sp_next();
    return;
  }
  double cx, cy;
  card_rect(&cx, &cy);
  double by = cy + SC_H - 52;
  if (y >= by && y <= by + 28) {
    if (x >= cx + SC_W - 28 - 200 && x <= cx + SC_W - 28) sp_next();
    else if (sp > SP_LANG && x >= cx + 28 && x <= cx + 138) sp_back();
    return;
  }
  double lx = cx + 200, lw = SC_W - 400, ly = cy + 200;
  if (sp == SP_LANG && x >= lx && x <= lx + lw) {
    int i = (int)((y - ly - 4) / SP_ROW);
    if (i >= 0 && i < l10n_count()) {
      set_language(l10n_code(i));
      if (setupwin) XRaiseWindow(dpy, setupwin);
    }
  } else if (sp == SP_REGION && x >= lx && x <= lx + lw) {
    int i = (int)((y - (ly - 10) - 4) / SP_ROW);
    if (i >= 0 && i < NREGIONS) sp_region = i;
  } else if (sp == SP_ACCOUNT) {
    for (int f = 0; f < SF_N; f++) {
      double fy = cy + 200 + f * 46;
      if (y >= fy && y <= fy + 30 && x >= cx + 246 && x <= cx + 260 + 300) sf_focus = f;
    }
  } else if (sp == SP_LOOK) {
    static const int mode[3] = {1, 2, 0};
    for (int i = 0; i < 3; i++) {
      double bx = cx + 110 + i * 190, byy = cy + 205;
      if (x >= bx && x <= bx + 160 && y >= byy && y <= byy + 140) {
        sp_look = mode[i];
        theme_mode = sp_look; /* gleich zeigen, gespeichert wird beim neuen Konto */
        if (want_dark() != dark) apply_theme();
      }
    }
  }
  draw_setup();
}

/* Ein Zeichen (UTF-8) an das Feld hängen; XLookupString liefert Latin-1 */
static void sp_type(int f, const char *buf, int n) {
  char add[8];
  int k = 0;
  for (int i = 0; i < n && k < 6; i++) {
    unsigned char ch = (unsigned char)buf[i];
    if (ch < 0x20 || ch == 0x7f) continue;
    if (ch >= 0x80) {
      add[k++] = (char)(0xc0 | ch >> 6);
      add[k++] = (char)(0x80 | (ch & 0x3f));
    } else {
      add[k++] = (char)ch;
    }
  }
  add[k] = 0;
  if (f == SF_NAME) { /* Kontoname: nur, was erlaubt ist, klein */
    for (int i = 0; i < k; i++) add[i] = (char)tolower((unsigned char)add[i]);
    if (k != 1 || !(islower((unsigned char)add[0]) || isdigit((unsigned char)add[0]) || strchr("._-", add[0]))) return;
    sf_name_edited = true;
  }
  if (strlen(sf[f]) + strlen(add) < sizeof sf[f]) strcat(sf[f], add);
  if (f == SF_FULL && !sf_name_edited) sp_derive_name();
}

static void setup_key(XKeyEvent *e) {
  char buf[16];
  KeySym ks = NoSymbol;
  int n = XLookupString(e, buf, sizeof buf, &ks, NULL);
  if (sp == SP_HELLO) {
    if (ks == XK_Return || ks == XK_KP_Enter || ks == XK_space || ks == XK_Right) sp_next();
    return;
  }
  if (ks == XK_Return || ks == XK_KP_Enter) {
    if (sp == SP_ACCOUNT && sf_focus < SF_N - 1 && !sp_account_ok()) sf_focus++;
    else sp_next();
    draw_setup();
    return;
  }
  if (sp == SP_LANG && (ks == XK_Up || ks == XK_Down)) {
    int cur = 0, n = l10n_count();
    for (int i = 0; i < n; i++)
      if (!strcmp(l10n_code(i), l10n_lang())) cur = i;
    set_language(l10n_code((cur + (ks == XK_Down ? 1 : n - 1)) % n));
    if (setupwin) XRaiseWindow(dpy, setupwin);
  } else if (sp == SP_REGION && (ks == XK_Up || ks == XK_Down)) {
    sp_region = (sp_region + (ks == XK_Down ? 1 : NREGIONS - 1)) % NREGIONS;
  } else if (sp == SP_ACCOUNT) {
    if (ks == XK_Tab || ks == XK_ISO_Left_Tab || ks == XK_Down || ks == XK_Up) {
      bool back = ks == XK_ISO_Left_Tab || ks == XK_Up || (e->state & ShiftMask);
      sf_focus = (sf_focus + (back ? SF_N - 1 : 1)) % SF_N;
    } else if (ks == XK_BackSpace) {
      size_t l = strlen(sf[sf_focus]);
      while (l > 0 && ((unsigned char)sf[sf_focus][l - 1] & 0xc0) == 0x80) l--; /* ganzes UTF-8-Zeichen */
      if (l > 0) l--;
      sf[sf_focus][l] = 0;
      if (sf_focus == SF_NAME) sf_name_edited = sf[SF_NAME][0] != 0;
      if (sf_focus == SF_FULL && !sf_name_edited) sp_derive_name();
    } else if (n > 0) {
      sp_type(sf_focus, buf, n);
    }
    sp_error[0] = 0;
  } else if (ks == XK_Escape || ks == XK_Left) {
    sp_back();
    return;
  }
  draw_setup();
}

static void setup_tick(void) {
  if (setup_active && sp == SP_HELLO) draw_setup();
}
