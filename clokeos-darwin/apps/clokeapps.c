/*
 * clokeapps: die kleinen Programme aus dem Dock als echte Fenster statt im Terminal.
 *
 *   clokeapps notes | calc | calendar | activity
 *
 * Notizen (Liste links, Text rechts, speichert selbst), Rechner, Kalender (Monatsansicht) und
 * Aktivitätsanzeige (Prozessliste, alle 2 s neu). Aussehen und Maße wie in clokeos.html. Die Titelleiste
 * zeichnet CloakServer; die Fensterrolle ist die Bundle-Kennung, damit das Dock das Fenster dem
 * Programm zuordnet (cloakserver/bundles.py). Sprache nach /etc/clokeos-language.
 */
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static gboolean de;
static char lang2[3] = "en";
#define T(en, deu) (de ? (deu) : (en))

static const char *CSS =
    ".cl-side { background: @theme_bg_color; border-right: 1px solid alpha(currentColor, 0.12); }"
    ".cl-note-row { padding: 8px 12px; border-radius: 8px; margin: 2px 8px; }"
    ".cl-note-row:selected { background: #fbd34f; color: #2b2300; }"
    ".cl-note-title { font-weight: 600; }"
    ".cl-note-sub { font-size: 11px; opacity: 0.6; }"
    ".cl-text { font-size: 14px; padding: 18px 22px; }"
    ".cl-bar { padding: 6px 10px; border-bottom: 1px solid alpha(currentColor, 0.12); }"
    ".cl-calc { background: #1c1c1e; }"
    ".cl-calc-display { color: white; font-size: 44px; font-weight: 300; padding: 14px 16px 6px 16px; }"
    ".cl-calc button { border-radius: 999px; border: none; box-shadow: none; font-size: 20px; margin: 3px; padding: 0;"
    "  background: #333336; color: white; min-height: 52px; min-width: 52px; }"
    ".cl-calc button.fn { background: #a5a5a8; color: black; }"
    ".cl-calc button.op { background: #ff9f0a; color: white; }"
    ".cl-calc button.op.on { background: white; color: #ff9f0a; }"
    ".cl-calc button:active { opacity: 0.7; }"
    ".cl-month { font-size: 22px; font-weight: 700; }"
    ".cl-dow { font-size: 11px; font-weight: 600; opacity: 0.55; }"
    ".cl-day { font-size: 15px; padding: 8px 0; }"
    ".cl-day.out { opacity: 0.3; }"
    ".cl-day.today { background: #ff3b30; color: white; border-radius: 999px; font-weight: 700; }"
    ".cl-stat { font-size: 12px; opacity: 0.75; }"
    ".cl-tb { min-height: 38px; padding: 0 10px 0 0; border: none; box-shadow: none; }"
    ".cl-tbtitle { font-size: 13px; font-weight: 700; }"
    ".cl-pill { background: alpha(currentColor, 0.09); border-radius: 999px; padding: 0 2px; margin: 6px 0; }"
    ".cl-pill button { border-radius: 999px; min-height: 26px; min-width: 30px; padding: 0 8px; background: none; border: none; box-shadow: none; }"
    ".cl-date { font-size: 11px; opacity: 0.55; }"
    ".cl-cell { border-right: 1px solid alpha(currentColor, 0.12); border-bottom: 1px solid alpha(currentColor, 0.12); }"
    ".cl-cell.we { background: alpha(currentColor, 0.045); }"
    ".cl-cell.sel { box-shadow: inset 0 0 0 2px #7c5cff; }"
    ".cl-cell .cl-dn { font-size: 13px; margin: 6px 8px 0 0; min-width: 22px; }"
    ".cl-cell.out .cl-dn { opacity: 0.3; }"
    ".cl-cell .cl-dn.today { background: #ff453a; color: white; border-radius: 999px; font-weight: 700; min-height: 22px; }"
    ".cl-ev { font-size: 10.5px; padding: 0 6px; opacity: 0.8; }"
    ".cl-side2 { border-left: 1px solid alpha(currentColor, 0.12); padding: 16px 14px; }"
    ".cl-grid-wrap { padding: 18px 18px 0 18px; }"
    ".cl-year { opacity: 0.45; font-weight: 400; }";

static void load_css(void) {
  GtkCssProvider *p = gtk_css_provider_new();
  gtk_css_provider_load_from_data(p, CSS, -1, NULL);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static GtkWidget *new_window(const char *role, const char *title, int w, int h, gboolean fixed) {
  GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(win), title);
  gtk_window_set_role(GTK_WINDOW(win), role);
  gtk_window_set_default_size(GTK_WINDOW(win), w, h);
  if (fixed) gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
  g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
  return win;
}

/* Zurück/Vor aus der Fensterleiste: CloakServer setzt _CLOKEOS_NAV (1 = zurück, 2 = vor) am Fenster. Das ist
 * zuverlässiger als gesendete Tasten; Thunar und NetSurf bekommen weiter Alt+Links/Rechts. */
static void (*nav_cb)(gpointer, int);

static gboolean on_nav_prop(GtkWidget *w, GdkEventProperty *e, gpointer data) {
  if (e->state != GDK_PROPERTY_NEW_VALUE || strcmp(gdk_atom_name(e->atom), "_CLOKEOS_NAV")) return FALSE;
  GdkAtom type;
  gint fmt, len;
  guchar *d = NULL;
  if (gdk_property_get(gtk_widget_get_window(w), e->atom, GDK_NONE, 0, 4, FALSE, &type, &fmt, &len, &d) && d && len >= 4) {
    int v = *(gint32 *)d == 1 || *(long *)d == 1 ? -1 : 1;
    if (len >= (int)sizeof(long)) v = *(long *)d == 1 ? -1 : 1;
    nav_cb(data, v);
  }
  g_free(d);
  return FALSE;
}

static void add_nav(GtkWidget *win, void (*cb)(gpointer, int), gpointer data) {
  nav_cb = cb;
  gtk_widget_add_events(win, GDK_PROPERTY_CHANGE_MASK);
  g_signal_connect(win, "property-notify-event", G_CALLBACK(on_nav_prop), data);
}

/* Werkzeugleiste in der Fensterleiste (Finder/Safari-Art): CloakServer zeichnet links 146 px Ampel und
 * Zurück/Vor und lässt dort kein Programm durch. Der Titel steht in der Mitte des Fensters, die Bedienknöpfe
 * rechts in einer Pille wie im Prototyp (.titlebar, .tb-actions). */
typedef struct { GtkWidget *title, *right; } UBar;

static UBar unified_bar(GtkWidget *root, const char *title) {
  GtkWidget *ov = gtk_overlay_new();
  GtkWidget *bg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkStyleContext *sc = gtk_widget_get_style_context(bg);
  gtk_style_context_add_class(sc, "toolbar");
  gtk_style_context_add_class(sc, "cl-tb");
  gtk_widget_set_size_request(bg, -1, 38);
  gtk_container_add(GTK_CONTAINER(ov), bg);
  GtkWidget *l = gtk_label_new(title);
  gtk_style_context_add_class(gtk_widget_get_style_context(l), "cl-tbtitle");
  gtk_widget_set_halign(l, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
  gtk_overlay_add_overlay(GTK_OVERLAY(ov), l);
  GtkWidget *right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(right), "cl-pill");
  gtk_widget_set_halign(right, GTK_ALIGN_END);
  gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end(right, 10);
  gtk_overlay_add_overlay(GTK_OVERLAY(ov), right);
  gtk_box_pack_start(GTK_BOX(root), ov, FALSE, FALSE, 0);
  return (UBar){l, right};
}

static GtkWidget *pill_button(const char *icon, const char *label, const char *tip) {
  GtkWidget *b = icon ? gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU) : gtk_button_new_with_label(label);
  gtk_widget_set_tooltip_text(b, tip);
  gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
  return b;
}

/* ---- Notizen ----------------------------------------------------------------------------------- */

typedef struct {
  GtkWidget *list, *view, *date, *wtitle;
  GtkTextBuffer *buf;
  char *dir;
  char *cur;      /* Dateipfad der geöffneten Notiz */
  guint save_id;
  gboolean loading;
} Notes;

static char *first_line(const char *text) {
  const char *nl = strchr(text, '\n');
  char *l = nl ? g_strndup(text, (gsize)(nl - text)) : g_strdup(text);
  g_strstrip(l);
  if (!*l) {
    g_free(l);
    return g_strdup(T("New Note", "Neue Notiz"));
  }
  return l;
}

static void notes_save(Notes *n) {
  if (!n->cur) return;
  GtkTextIter a, b;
  gtk_text_buffer_get_bounds(n->buf, &a, &b);
  char *t = gtk_text_buffer_get_text(n->buf, &a, &b, FALSE);
  g_file_set_contents(n->cur, t, -1, NULL);
  g_free(t);
}

static void notes_fill(Notes *n, const char *select);

static gboolean save_timeout(gpointer d) {
  Notes *n = d;
  n->save_id = 0;
  notes_save(n);
  notes_fill(n, n->cur);
  return G_SOURCE_REMOVE;
}

static void notes_changed(GtkTextBuffer *b, gpointer d) {
  (void)b;
  Notes *n = d;
  if (n->loading) return;
  if (n->save_id) g_source_remove(n->save_id);
  n->save_id = g_timeout_add(700, save_timeout, n);
}

static void notes_open(Notes *n, const char *path) {
  if (n->save_id) {
    g_source_remove(n->save_id);
    n->save_id = 0;
    notes_save(n);
  }
  char *t = NULL;
  g_file_get_contents(path, &t, NULL, NULL);
  n->loading = TRUE;
  gtk_text_buffer_set_text(n->buf, t ? t : "", -1);
  n->loading = FALSE;
  g_free(t);
  g_free(n->cur);
  n->cur = g_strdup(path);
  GStatBuf st;
  if (!g_stat(path, &st) && n->date) {
    GDateTime *dt = g_date_time_new_from_unix_local(st.st_mtime);
    char *when = g_date_time_format(dt, "%d.%m.%Y, %H:%M");
    gtk_label_set_text(GTK_LABEL(n->date), when);
    g_free(when);
    g_date_time_unref(dt);
  }
  if (n->wtitle) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(n->buf, &s, &e);
    char *all = gtk_text_buffer_get_text(n->buf, &s, &e, FALSE), *fl = first_line(all);
    char *tt = g_strdup_printf("%s — %s", *fl ? fl : T("New note", "Neue Notiz"), T("Notes", "Notizen"));
    gtk_label_set_text(GTK_LABEL(n->wtitle), tt);
    g_free(tt);
    g_free(fl);
    g_free(all);
  }
}

static void on_row_selected(GtkListBox *l, GtkListBoxRow *r, gpointer d) {
  (void)l;
  Notes *n = d;
  if (!r) return;
  const char *path = g_object_get_data(G_OBJECT(r), "path");
  if (path && (!n->cur || strcmp(path, n->cur))) notes_open(n, path);
}

static gint newest_first(gconstpointer a, gconstpointer b) {
  GStatBuf sa, sb;
  g_stat(a, &sa);
  g_stat(b, &sb);
  return sa.st_mtime < sb.st_mtime ? 1 : sa.st_mtime > sb.st_mtime ? -1 : 0;
}

static void notes_fill(Notes *n, const char *select) {
  g_signal_handlers_block_by_func(n->list, on_row_selected, n);
  GList *rows = gtk_container_get_children(GTK_CONTAINER(n->list));
  for (GList *r = rows; r; r = r->next) gtk_widget_destroy(r->data);
  g_list_free(rows);
  GDir *d = g_dir_open(n->dir, 0, NULL);
  GList *files = NULL;
  for (const char *f; d && (f = g_dir_read_name(d));)
    if (g_str_has_suffix(f, ".txt")) files = g_list_prepend(files, g_build_filename(n->dir, f, NULL));
  if (d) g_dir_close(d);
  files = g_list_sort(files, newest_first);
  GtkWidget *sel = NULL;
  for (GList *f = files; f; f = f->next) {
    char *t = NULL;
    g_file_get_contents(f->data, &t, NULL, NULL);
    char *title = first_line(t ? t : "");
    GStatBuf st;
    g_stat(f->data, &st);
    GDateTime *dt = g_date_time_new_from_unix_local(st.st_mtime);
    char *when = g_date_time_format(dt, "%d.%m.%Y %H:%M");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *l1 = gtk_label_new(title), *l2 = gtk_label_new(when);
    gtk_label_set_xalign(GTK_LABEL(l1), 0);
    gtk_label_set_xalign(GTK_LABEL(l2), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l1), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(l1), "cl-note-title");
    gtk_style_context_add_class(gtk_widget_get_style_context(l2), "cl-note-sub");
    gtk_box_pack_start(GTK_BOX(box), l1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), l2, FALSE, FALSE, 0);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "cl-note-row");
    gtk_container_add(GTK_CONTAINER(row), box);
    g_object_set_data_full(G_OBJECT(row), "path", g_strdup(f->data), g_free);
    gtk_list_box_insert(GTK_LIST_BOX(n->list), row, -1);
    if (select && !strcmp(select, f->data)) sel = row;
    g_free(t);
    g_free(title);
    g_free(when);
    g_date_time_unref(dt);
  }
  g_list_free_full(files, g_free);
  gtk_widget_show_all(n->list);
  if (sel) gtk_list_box_select_row(GTK_LIST_BOX(n->list), GTK_LIST_BOX_ROW(sel));
  g_signal_handlers_unblock_by_func(n->list, on_row_selected, n);
}

static void notes_new(GtkButton *b, gpointer d) {
  (void)b;
  Notes *n = d;
  notes_save(n);
  char *name = g_strdup_printf("note-%ld.txt", (long)time(NULL));
  char *path = g_build_filename(n->dir, name, NULL);
  g_file_set_contents(path, "", 0, NULL);
  notes_open(n, path);
  notes_fill(n, path);
  gtk_widget_grab_focus(n->view);
  g_free(name);
  g_free(path);
}

static void notes_delete(GtkButton *b, gpointer d) {
  (void)b;
  Notes *n = d;
  if (!n->cur) return;
  if (n->save_id) g_source_remove(n->save_id), n->save_id = 0;
  g_remove(n->cur);
  g_free(n->cur);
  n->cur = NULL;
  n->loading = TRUE;
  gtk_text_buffer_set_text(n->buf, "", -1);
  n->loading = FALSE;
  notes_fill(n, NULL);
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(n->list), 0);
  if (first) gtk_list_box_select_row(GTK_LIST_BOX(n->list), first);
}

/* Textknopf statt Symbol: PureDarwins Symbolsatz hat nicht alle Zeichen */

static void notes_nav(gpointer data, int dir) {
  Notes *n = data;
  GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(n->list));
  int i = cur ? gtk_list_box_row_get_index(cur) : 0;
  GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(n->list), i + dir);
  if (r) gtk_list_box_select_row(GTK_LIST_BOX(n->list), r);
}

static void run_notes(void) {
  Notes *n = g_new0(Notes, 1);
  n->dir = g_build_filename(g_get_home_dir(), "Notes", NULL);
  g_mkdir_with_parents(n->dir, 0755);
  /* die alte einzelne Datei aus der Terminal-Version übernehmen */
  char *old = g_build_filename(g_get_home_dir(), "Notes.txt", NULL);
  char *old_t = NULL;
  if (g_file_get_contents(old, &old_t, NULL, NULL) && *old_t) {
    char *p = g_build_filename(n->dir, "note-imported.txt", NULL);
    if (!g_file_test(p, G_FILE_TEST_EXISTS)) g_file_set_contents(p, old_t, -1, NULL);
    g_free(p);
    g_remove(old);
  }
  g_free(old_t);
  g_free(old);

  GtkWidget *win = new_window("com.clokeos.notizen", T("Notes", "Notizen"), 740, 470, FALSE);
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(side), "cl-side");
  gtk_widget_set_size_request(side, 230, -1);
  UBar bar = unified_bar(root, T("Notes", "Notizen"));
  n->wtitle = bar.title;
  GtkWidget *bnew = pill_button("document-edit-symbolic", NULL, T("New note", "Neue Notiz"));
  GtkWidget *bdel = pill_button("edit-delete-symbolic", NULL, T("Delete note", "Notiz löschen"));
  gtk_box_pack_start(GTK_BOX(bar.right), bnew, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar.right), bdel, FALSE, FALSE, 0);
  n->list = gtk_list_box_new();
  GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(sc), n->list);
  gtk_box_pack_start(GTK_BOX(side), sc, TRUE, TRUE, 0);
  n->view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(n->view), GTK_WRAP_WORD_CHAR);
  gtk_style_context_add_class(gtk_widget_get_style_context(n->view), "cl-text");
  n->buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(n->view));
  GtkWidget *sc2 = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(sc2), n->view);
  n->date = gtk_label_new("");
  gtk_style_context_add_class(gtk_widget_get_style_context(n->date), "cl-date");
  gtk_widget_set_margin_top(n->date, 12);
  GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(rbox), n->date, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(rbox), sc2, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), side, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), rbox, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(root), hbox, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(win), root);
  add_nav(win, notes_nav, n);

  g_signal_connect(n->list, "row-selected", G_CALLBACK(on_row_selected), n);
  g_signal_connect(n->buf, "changed", G_CALLBACK(notes_changed), n);
  g_signal_connect(bnew, "clicked", G_CALLBACK(notes_new), n);
  g_signal_connect(bdel, "clicked", G_CALLBACK(notes_delete), n);

  notes_fill(n, NULL);
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(n->list), 0);
  if (first) gtk_list_box_select_row(GTK_LIST_BOX(n->list), first);
  else notes_new(NULL, n);
  gtk_widget_show_all(win);
  gtk_main();
  notes_save(n);
}

/* ---- Rechner ------------------------------------------------------------------------------------ */

typedef struct {
  GtkWidget *disp;
  double acc, cur;
  char op;           /* + - * / oder 0 */
  gboolean typing;   /* cur wird gerade eingetippt */
  char buf[32];
  GtkWidget *opbtn[4];
} Calc;

static void calc_show(Calc *c, double v) {
  char t[48];
  if (isnan(v) || isinf(v)) snprintf(t, sizeof t, "%s", T("Error", "Fehler"));
  else if (fabs(v) >= 1e12 || (v != 0 && fabs(v) < 1e-9)) snprintf(t, sizeof t, "%.6g", v);
  else {
    snprintf(t, sizeof t, "%.10f", v);
    char *e = t + strlen(t) - 1;
    while (*e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
  }
  if (de) {
    char *dot = strchr(t, '.');
    if (dot) *dot = ',';
  }
  gtk_label_set_text(GTK_LABEL(c->disp), t);
}

static double apply(double a, char op, double b) {
  switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b == 0 ? NAN : a / b;
  }
  return b;
}

static void calc_mark(Calc *c, char op) {
  const char ops[4] = {'/', '*', '-', '+'};
  for (int i = 0; i < 4; i++) {
    GtkStyleContext *sc = gtk_widget_get_style_context(c->opbtn[i]);
    if (op == ops[i]) gtk_style_context_add_class(sc, "on");
    else gtk_style_context_remove_class(sc, "on");
  }
}

static void calc_key(GtkButton *b, gpointer d) {
  Calc *c = d;
  const char *k = g_object_get_data(G_OBJECT(b), "k");
  if ((*k >= '0' && *k <= '9') || *k == '.') {
    if (!c->typing) {
      c->buf[0] = 0;
      c->typing = TRUE;
    }
    size_t l = strlen(c->buf);
    if (*k == '.' && strchr(c->buf, '.')) return;
    if (l < 14) {
      if (*k == '.' && !l) strcpy(c->buf, "0");
      strncat(c->buf, k, 1);
    }
    if (!strcmp(c->buf, "0") || !strcmp(c->buf, "00")) strcpy(c->buf, "0");
    if (c->buf[0] == '0' && c->buf[1] >= '0' && c->buf[1] <= '9') memmove(c->buf, c->buf + 1, strlen(c->buf));
    c->cur = atof(c->buf);
    char t[40];
    snprintf(t, sizeof t, "%s", c->buf);
    if (de) {
      char *dot = strchr(t, '.');
      if (dot) *dot = ',';
    }
    gtk_label_set_text(GTK_LABEL(c->disp), t);
    calc_mark(c, 0);
  } else if (!strcmp(k, "AC")) {
    c->acc = c->cur = 0;
    c->op = 0;
    c->typing = FALSE;
    calc_show(c, 0);
    calc_mark(c, 0);
  } else if (!strcmp(k, "±")) {
    c->cur = -c->cur;
    c->typing = FALSE;
    calc_show(c, c->cur);
  } else if (!strcmp(k, "%")) {
    c->cur /= 100;
    c->typing = FALSE;
    calc_show(c, c->cur);
  } else {
    char op = !strcmp(k, "÷") ? '/' : !strcmp(k, "×") ? '*' : !strcmp(k, "−") ? '-' : !strcmp(k, "+") ? '+' : '=';
    if (c->op && (c->typing || op == '=')) c->acc = apply(c->acc, c->op, c->cur);
    else if (!c->op) c->acc = c->cur;
    c->cur = c->acc;
    calc_show(c, c->acc);
    c->typing = FALSE;
    c->op = op == '=' ? 0 : op;
    calc_mark(c, c->op);
  }
}

static void run_calc(void) {
  Calc *c = g_new0(Calc, 1);
  GtkWidget *win = new_window("com.clokeos.rechner", T("Calculator", "Rechner"), 252, 400, TRUE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(box), "cl-calc");
  c->disp = gtk_label_new("0");
  gtk_label_set_xalign(GTK_LABEL(c->disp), 1);
  gtk_style_context_add_class(gtk_widget_get_style_context(c->disp), "cl-calc-display");
  gtk_box_pack_start(GTK_BOX(box), c->disp, TRUE, TRUE, 0);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
  gtk_widget_set_margin_start(grid, 6);
  gtk_widget_set_margin_end(grid, 6);
  gtk_widget_set_margin_bottom(grid, 8);
  static const char *keys[5][4] = {{"AC", "±", "%", "÷"}, {"7", "8", "9", "×"}, {"4", "5", "6", "−"},
                                   {"1", "2", "3", "+"}, {"0", ".", "=", NULL}};
  for (int r = 0; r < 5; r++)
    for (int col = 0; col < 4; col++) {
      const char *k = keys[r][col];
      if (!k) continue;
      const char *label = !strcmp(k, ".") ? T(".", ",") : k;
      GtkWidget *b = gtk_button_new_with_label(label);
      g_object_set_data(G_OBJECT(b), "k", (gpointer)k);
      GtkStyleContext *sc = gtk_widget_get_style_context(b);
      if (r == 0 && col < 3) gtk_style_context_add_class(sc, "fn");
      if (col == 3 || !strcmp(k, "=")) gtk_style_context_add_class(sc, "op");
      if (col == 3 && r < 4) c->opbtn[r == 0 ? 0 : r == 1 ? 1 : r == 2 ? 2 : 3] = b;
      if (!strcmp(k, "=")) gtk_style_context_add_class(sc, "op");
      g_signal_connect(b, "clicked", G_CALLBACK(calc_key), c);
      gtk_grid_attach(GTK_GRID(grid), b, col, r, 1, 1);
    }
  /* 0 über zwei Spalten wie beim Vorbild: "0" und "." bleiben einzeln, = rechts daneben */
  gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(win), box);
  gtk_widget_show_all(win);
  gtk_main();
}

/* ---- Kalender ------------------------------------------------------------------------------------- */

typedef struct { int y, m, d; char tm[8]; char *text; } Event;

typedef struct {
  GtkWidget *month, *grid, *prev, *next, *dayhead, *evbox, *entry, *wtitle;
  int y, m, sel_y, sel_m, sel_d;
  GList *events;
  char *file;
} Cal;

static const char *const MON_L[5][12] = {
    {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"},
    {"Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"},
    {"janvier", "février", "mars", "avril", "mai", "juin", "juillet", "août", "septembre", "octobre", "novembre", "décembre"},
    {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"},
    {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"}};
static const char *const DOW_L[5][7] = {{"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"},  /* Sonntag zuerst, wie in den USA */
                                        {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"},
                                        {"Lu", "Ma", "Me", "Je", "Ve", "Sa", "Di"},
                                        {"Lu", "Ma", "Mi", "Ju", "Vi", "Sá", "Do"},
                                        {"Lu", "Ma", "Me", "Gi", "Ve", "Sa", "Do"}};
static const char *const WD_L[5][7] = {{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
                                       {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"},
                                       {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"},
                                       {"domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"},
                                       {"domenica", "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato"}};
static int lang_idx(void) {
  static const char *c[] = {"en", "de", "fr", "es", "it"};
  for (int i = 1; i < 5; i++)
    if (!strcmp(lang2, c[i])) return i;
  return 0;
}

static void cal_fill(Cal *c);
static void cal_side(Cal *c);

static void cal_load(Cal *c) {
  char *txt = NULL;
  if (!g_file_get_contents(c->file, &txt, NULL, NULL)) return;
  char **lines = g_strsplit(txt, "\n", -1);
  for (char **l = lines; *l; l++) {
    int y, m, d;
    char tm[8] = "", *tab1 = strchr(*l, '\t');
    if (!tab1 || sscanf(*l, "%d-%d-%d", &y, &m, &d) != 3) continue;
    char *tab2 = strchr(tab1 + 1, '\t');
    if (!tab2) continue;
    g_strlcpy(tm, tab1 + 1, MIN((gsize)(tab2 - tab1), sizeof tm));
    Event *e = g_new0(Event, 1);
    e->y = y, e->m = m, e->d = d;
    g_strlcpy(e->tm, tm, sizeof e->tm);
    e->text = g_strdup(tab2 + 1);
    c->events = g_list_append(c->events, e);
  }
  g_strfreev(lines);
  g_free(txt);
}

static void cal_save(Cal *c) {
  GString *s = g_string_new("");
  for (GList *l = c->events; l; l = l->next) {
    Event *e = l->data;
    g_string_append_printf(s, "%04d-%02d-%02d\t%s\t%s\n", e->y, e->m, e->d, e->tm, e->text);
  }
  char *dir = g_path_get_dirname(c->file);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  g_file_set_contents(c->file, s->str, -1, NULL);
  g_string_free(s, TRUE);
}

static int dow_of(int y, int m, int d) { /* 0 = Sonntag */
  GDateTime *dt = g_date_time_new_local(y, m, d, 12, 0, 0);
  int w = g_date_time_get_day_of_week(dt) % 7;
  g_date_time_unref(dt);
  return w;
}

static void cal_pick(GtkWidget *w, GdkEventButton *e, gpointer d) {
  (void)e;
  Cal *c = d;
  c->sel_y = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "y"));
  c->sel_m = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "m"));
  c->sel_d = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "d"));
  if (c->sel_m != c->m || c->sel_y != c->y) c->y = c->sel_y, c->m = c->sel_m;
  cal_fill(c);
}

static void cal_fill(Cal *c) {
  int li = lang_idx();
  char t[160];
  snprintf(t, sizeof t, "<b>%s</b> <span alpha=\"55%%\" weight=\"normal\">%d</span>", MON_L[li][c->m - 1], c->y);
  gtk_label_set_markup(GTK_LABEL(c->month), t);
  GList *kids = gtk_container_get_children(GTK_CONTAINER(c->grid));
  for (GList *k = kids; k; k = k->next) gtk_widget_destroy(k->data);
  g_list_free(kids);
  int mon_first = li != 0; /* Montag zuerst außer im Englischen */
  for (int i = 0; i < 7; i++) {
    GtkWidget *h = gtk_label_new(DOW_L[li][(i + mon_first) % 7]);
    gtk_style_context_add_class(gtk_widget_get_style_context(h), "cl-dow");
    gtk_widget_set_halign(h, GTK_ALIGN_END);
    gtk_widget_set_margin_end(h, 8);
    gtk_widget_set_margin_top(h, 8);
    gtk_widget_set_margin_bottom(h, 8);
    gtk_grid_attach(GTK_GRID(c->grid), h, i, 0, 1, 1);
  }
  GDateTime *now = g_date_time_new_now_local();
  int ty = g_date_time_get_year(now), tm_ = g_date_time_get_month(now), td = g_date_time_get_day_of_month(now);
  g_date_time_unref(now);
  int first = (dow_of(c->y, c->m, 1) - mon_first + 7) % 7;
  GDateTime *f1 = g_date_time_new_local(c->y, c->m, 1, 12, 0, 0);
  GDateTime *start = g_date_time_add_days(f1, -first);
  g_date_time_unref(f1);
  for (int i = 0; i < 42; i++) {
    GDateTime *dd = g_date_time_add_days(start, i);
    int y = g_date_time_get_year(dd), m = g_date_time_get_month(dd), d = g_date_time_get_day_of_month(dd);
    int w = g_date_time_get_day_of_week(dd) % 7;
    g_date_time_unref(dd);
    GtkWidget *eb = gtk_event_box_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    char dn[8];
    snprintf(dn, sizeof dn, "%d", d);
    GtkWidget *l = gtk_label_new(dn);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "cl-dn");
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    int shown = 0;
    for (GList *ev = c->events; ev; ev = ev->next) {
      Event *e = ev->data;
      if (e->y != y || e->m != m || e->d != d || shown >= 2) continue;
      GtkWidget *el = gtk_label_new(e->text);
      gtk_label_set_xalign(GTK_LABEL(el), 0);
      gtk_label_set_ellipsize(GTK_LABEL(el), PANGO_ELLIPSIZE_END);
      gtk_style_context_add_class(gtk_widget_get_style_context(el), "cl-ev");
      gtk_box_pack_start(GTK_BOX(box), el, FALSE, FALSE, 0);
      shown++;
    }
    gtk_container_add(GTK_CONTAINER(eb), box);
    GtkStyleContext *sc = gtk_widget_get_style_context(eb);
    gtk_style_context_add_class(sc, "cl-cell");
    if (w == 0 || w == 6) gtk_style_context_add_class(sc, "we");
    if (m != c->m) gtk_style_context_add_class(sc, "out");
    if (y == c->sel_y && m == c->sel_m && d == c->sel_d) gtk_style_context_add_class(sc, "sel");
    if (y == ty && m == tm_ && d == td) gtk_style_context_add_class(gtk_widget_get_style_context(l), "today");
    gtk_widget_set_size_request(eb, 64, 56);
    gtk_widget_set_hexpand(eb, TRUE);
    gtk_widget_set_vexpand(eb, TRUE);
    g_object_set_data(G_OBJECT(eb), "y", GINT_TO_POINTER(y));
    g_object_set_data(G_OBJECT(eb), "m", GINT_TO_POINTER(m));
    g_object_set_data(G_OBJECT(eb), "d", GINT_TO_POINTER(d));
    g_signal_connect(eb, "button-press-event", G_CALLBACK(cal_pick), c);
    gtk_grid_attach(GTK_GRID(c->grid), eb, i % 7, 1 + i / 7, 1, 1);
  }
  g_date_time_unref(start);
  gtk_widget_show_all(c->grid);
  cal_side(c);
}

static void cal_del(GtkButton *b, gpointer d) {
  Cal *c = d;
  Event *e = g_object_get_data(G_OBJECT(b), "ev");
  c->events = g_list_remove(c->events, e);
  g_free(e->text);
  g_free(e);
  cal_save(c);
  cal_fill(c);
}

static void cal_side(Cal *c) {
  int li = lang_idx();
  char t[160];
  const char *wd = WD_L[li][dow_of(c->sel_y, c->sel_m, c->sel_d)], *mo = MON_L[li][c->sel_m - 1];
  if (li == 0) snprintf(t, sizeof t, "%s, %s %d", wd, mo, c->sel_d);
  else if (li == 1) snprintf(t, sizeof t, "%s, %d. %s", wd, c->sel_d, mo);
  else snprintf(t, sizeof t, "%s %d %s", wd, c->sel_d, mo);
  gtk_label_set_text(GTK_LABEL(c->dayhead), t);
  GList *kids = gtk_container_get_children(GTK_CONTAINER(c->evbox));
  for (GList *k = kids; k; k = k->next) gtk_widget_destroy(k->data);
  g_list_free(kids);
  int n = 0;
  for (GList *ev = c->events; ev; ev = ev->next) {
    Event *e = ev->data;
    if (e->y != c->sel_y || e->m != c->sel_m || e->d != c->sel_d) continue;
    char line[300];
    snprintf(line, sizeof line, "%s%s%s", e->tm, e->tm[0] ? "  " : "", e->text);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(line);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_set_hexpand(l, TRUE);
    GtkWidget *x = gtk_button_new_from_icon_name("edit-delete-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
    g_object_set_data(G_OBJECT(x), "ev", e);
    g_signal_connect(x, "clicked", G_CALLBACK(cal_del), c);
    gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row), x, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c->evbox), row, FALSE, FALSE, 0);
    n++;
  }
  if (!n) {
    GtkWidget *l = gtk_label_new(T("No events on this day.", "Keine Termine an diesem Tag."));
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), "cl-date");
    gtk_box_pack_start(GTK_BOX(c->evbox), l, FALSE, FALSE, 0);
  }
  gtk_widget_show_all(c->evbox);
}

static void cal_add(GtkEntry *en, gpointer d) {
  Cal *c = d;
  const char *s = gtk_entry_get_text(en);
  while (*s == ' ') s++;
  if (!*s) return;
  Event *e = g_new0(Event, 1);
  e->y = c->sel_y, e->m = c->sel_m, e->d = c->sel_d;
  int h, mi, used = 0;
  if (sscanf(s, "%d:%d%n", &h, &mi, &used) >= 2 && h >= 0 && h < 24 && mi >= 0 && mi < 60) {
    snprintf(e->tm, sizeof e->tm, "%02d:%02d", h, mi);
    s += used;
    while (*s == ' ') s++;
  }
  e->text = g_strdup(*s ? s : T("Event", "Termin"));
  c->events = g_list_append(c->events, e);
  gtk_entry_set_text(en, "");
  cal_save(c);
  cal_fill(c);
}

static void cal_today(GtkButton *b, gpointer d) {
  (void)b;
  Cal *c = d;
  GDateTime *now = g_date_time_new_now_local();
  c->y = c->sel_y = g_date_time_get_year(now);
  c->m = c->sel_m = g_date_time_get_month(now);
  c->sel_d = g_date_time_get_day_of_month(now);
  g_date_time_unref(now);
  cal_fill(c);
}

static void cal_step(GtkButton *b, gpointer d) {
  Cal *c = d;
  int dm = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "dm"));
  c->m += dm;
  if (c->m < 1) c->m = 12, c->y--;
  if (c->m > 12) c->m = 1, c->y++;
  cal_fill(c);
}

static void cal_nav(gpointer data, int dir) {
  Cal *c = data;
  gtk_button_clicked(GTK_BUTTON(dir < 0 ? c->prev : c->next));
}

static void run_calendar(void) {
  Cal *c = g_new0(Cal, 1);
  c->file = g_build_filename(g_get_home_dir(), "Library", "Calendars", "events.tsv", NULL);
  cal_load(c);
  GtkWidget *win = new_window("com.clokeos.kalender", T("Calendar", "Kalender"), 760, 520, FALSE);
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  UBar bar = unified_bar(root, T("Calendar", "Kalender"));
  c->prev = gtk_button_new();
  c->next = gtk_button_new();
  g_object_ref_sink(c->prev);
  g_object_ref_sink(c->next);
  g_object_set_data(G_OBJECT(c->prev), "dm", GINT_TO_POINTER(-1));
  g_object_set_data(G_OBJECT(c->next), "dm", GINT_TO_POINTER(1));
  g_signal_connect(c->prev, "clicked", G_CALLBACK(cal_step), c);
  g_signal_connect(c->next, "clicked", G_CALLBACK(cal_step), c);
  GtkWidget *today = pill_button(NULL, T("Today", "Heute"), T("Go to today", "Zu heute"));
  g_signal_connect(today, "clicked", G_CALLBACK(cal_today), c);
  gtk_box_pack_start(GTK_BOX(bar.right), today, FALSE, FALSE, 0);

  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(left), "cl-grid-wrap");
  c->month = gtk_label_new("");
  gtk_style_context_add_class(gtk_widget_get_style_context(c->month), "cl-month");
  gtk_label_set_xalign(GTK_LABEL(c->month), 0);
  c->grid = gtk_grid_new();
  gtk_grid_set_column_homogeneous(GTK_GRID(c->grid), TRUE);
  gtk_grid_set_row_homogeneous(GTK_GRID(c->grid), FALSE);
  gtk_widget_set_vexpand(c->grid, TRUE);
  gtk_box_pack_start(GTK_BOX(left), c->month, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(left), c->grid, TRUE, TRUE, 0);

  GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_style_context_add_class(gtk_widget_get_style_context(side), "cl-side2");
  gtk_widget_set_size_request(side, 230, -1);
  c->dayhead = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(c->dayhead), 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(c->dayhead), "cl-tbtitle");
  c->evbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  c->entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(c->entry), T("New event, e.g. 14:00 Dentist", "Neuer Termin, z. B. 14:00 Zahnarzt"));
  g_signal_connect(c->entry, "activate", G_CALLBACK(cal_add), c);
  GtkWidget *note = gtk_label_new(T("Events are saved in ~/Library/Calendars.", "Termine werden in ~/Library/Calendars gespeichert."));
  gtk_label_set_xalign(GTK_LABEL(note), 0);
  gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(note), "cl-date");
  gtk_box_pack_start(GTK_BOX(side), c->dayhead, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(side), c->evbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(side), c->entry, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(side), note, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(body), left, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(body), side, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), body, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(win), root);
  add_nav(win, cal_nav, c);
  cal_today(NULL, c);
  gtk_widget_show_all(win);
  gtk_main();
}

/* ---- Aktivitätsanzeige ------------------------------------------------------------------------------ */

enum { A_PID, A_NAME, A_CPU, A_MEM, A_COLS };

typedef struct {
  GtkListStore *store;
  GtkWidget *stat;
} Act;

static gboolean act_refresh(gpointer d) {
  Act *a = d;
  gchar *out = NULL;
  if (!g_spawn_command_line_sync("/bin/ps -axo pid=,pcpu=,rss=,comm=", &out, NULL, NULL, NULL) || !out) return G_SOURCE_CONTINUE;
  gtk_list_store_clear(a->store);
  double total = 0;
  int count = 0;
  gchar **lines = g_strsplit(out, "\n", -1);
  for (gchar **l = lines; *l; l++) {
    int pid = 0, off = 0;
    double cpu = 0;
    long rss = 0;
    if (sscanf(*l, " %d %lf %ld %n", &pid, &cpu, &rss, &off) < 3 || !off) continue;
    const char *name = strrchr(*l + off, '/');
    name = name ? name + 1 : *l + off;
    GtkTreeIter it;
    gtk_list_store_append(a->store, &it);
    gtk_list_store_set(a->store, &it, A_PID, pid, A_NAME, name, A_CPU, cpu, A_MEM, rss / 1024.0, -1);
    total += cpu;
    count++;
  }
  g_strfreev(lines);
  g_free(out);
  double load[3] = {0};
  getloadavg(load, 3);
  char t[160];
  snprintf(t, sizeof t, "%s %.1f %%   ·   %d %s   ·   %s %.2f", T("CPU", "CPU"), total, count, T("processes", "Prozesse"),
           T("Load", "Last"), load[0]);
  gtk_label_set_text(GTK_LABEL(a->stat), t);
  return G_SOURCE_CONTINUE;
}

static void cell_num(GtkTreeViewColumn *c, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *i, gpointer col) {
  (void)c;
  double v;
  gtk_tree_model_get(m, i, GPOINTER_TO_INT(col), &v, -1);
  char t[32];
  if (GPOINTER_TO_INT(col) == A_CPU) snprintf(t, sizeof t, "%.1f", v);
  else snprintf(t, sizeof t, "%.1f MB", v);
  g_object_set(r, "text", t, "xalign", 1.0f, NULL);
}

static void run_activity(void) {
  Act *a = g_new0(Act, 1);
  GtkWidget *win = new_window("com.clokeos.aktivitaet", T("Activity Monitor", "Aktivitätsanzeige"), 680, 460, FALSE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  unified_bar(box, T("Activity Monitor", "Aktivitätsanzeige"));
  a->stat = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(a->stat), 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(a->stat), "cl-stat");
  gtk_style_context_add_class(gtk_widget_get_style_context(a->stat), "cl-bar");
  a->store = gtk_list_store_new(A_COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(a->store), A_CPU, GTK_SORT_DESCENDING);
  GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));
  gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(tv), TRUE);
  const char *heads_en[] = {"PID", "Process Name", "% CPU", "Memory"}, *heads_de[] = {"PID", "Prozessname", "% CPU", "Speicher"};
  for (int i = 0; i < A_COLS; i++) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes((de ? heads_de : heads_en)[i], r, NULL);
    if (i == A_PID) gtk_tree_view_column_add_attribute(col, r, "text", A_PID);
    else if (i == A_NAME) gtk_tree_view_column_add_attribute(col, r, "text", A_NAME);
    else gtk_tree_view_column_set_cell_data_func(col, r, cell_num, GINT_TO_POINTER(i), NULL);
    gtk_tree_view_column_set_sort_column_id(col, i);
    gtk_tree_view_column_set_resizable(col, TRUE);
    if (i == A_NAME) gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
  }
  GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(sc), tv);
  gtk_box_pack_start(GTK_BOX(box), a->stat, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(win), box);
  act_refresh(a);
  g_timeout_add_seconds(2, act_refresh, a);
  gtk_widget_show_all(win);
  gtk_main();
}


/* ---- MIDI-Tastatur ----------------------------------------------------------------------------------- */
/* Zwei Oktaven zum Spielen mit Maus oder Computertastatur (A W S E D F T G Y H U J K O L P wie "Musical Typing"
 * in Logic, Z/X Oktave, C/V Anschlag). Die Noten gehen als virtuelle MIDI-Quelle "ClokeOS Keyboard" über den
 * MIDI-Dienst clokemidid (macapps/coremidi/clokemidi-proto.h) an alle Programme, die sie abonnieren. */
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "clokemidi-proto.h"

typedef struct {
  GtkWidget *area, *status, *octlabel, *vel;
  int fd, octave, uid, down[128], mousenote;
} Kbd;

static void kbd_send(Kbd *k, const unsigned char *b, int n) {
  if (k->fd < 0) return;
  cm_hdr h = { (uint32_t)n, CM_DATA, CM_SOURCE, k->uid, CM_FMT_BYTES };
  unsigned char m[sizeof h + 8];
  memcpy(m, &h, sizeof h); memcpy(m + sizeof h, b, n);
  if (write(k->fd, m, sizeof h + n) != (ssize_t)(sizeof h + n)) { close(k->fd); k->fd = -1; }
}

static void kbd_status(Kbd *k) {
  gtk_label_set_text(GTK_LABEL(k->status), k->fd >= 0 ? T("Connected to the MIDI service as “ClokeOS Keyboard”",
                                                          "Mit dem MIDI-Dienst verbunden als „ClokeOS Keyboard“")
                                                     : T("MIDI service not running (clokemidid)", "MIDI-Dienst läuft nicht (clokemidid)"));
}

static gboolean kbd_connect(gpointer data) {
  Kbd *k = data;
  if (k->fd >= 0) return G_SOURCE_CONTINUE;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = { .sun_family = AF_UNIX };
  const char *path = g_getenv("CLOKE_MIDI_SOCKET");
  g_strlcpy(a.sun_path, path ? path : CLOKEMIDI_SOCKET, sizeof a.sun_path);
  if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
    int on = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    k->fd = fd;
    const char *name = "ClokeOS Keyboard";
    cm_hdr h = { (uint32_t)strlen(name), CM_ADD, CM_SOURCE, k->uid, 0 };
    if (write(fd, &h, sizeof h) != sizeof h || write(fd, name, strlen(name)) != (ssize_t)strlen(name)) { close(fd); k->fd = -1; }
  } else if (fd >= 0) close(fd);
  kbd_status(k);
  return G_SOURCE_CONTINUE;
}

/* Lage der Tasten: 14 weiße Tasten, schwarze dazwischen (relativ zur Fläche) */
static const int kbd_white[7] = { 0, 2, 4, 5, 7, 9, 11 };
static int kbd_is_black(int n) { int m = n % 12; return m == 1 || m == 3 || m == 6 || m == 8 || m == 10; }

static void kbd_note(Kbd *k, int note, int on) {
  if (note < 0 || note > 127 || k->down[note] == on) return;
  k->down[note] = on;
  int vel = (int)gtk_range_get_value(GTK_RANGE(k->vel));
  unsigned char b[3] = { (unsigned char)(on ? 0x90 : 0x80), (unsigned char)note, (unsigned char)(on ? vel : 0) };
  kbd_send(k, b, 3);
  gtk_widget_queue_draw(k->area);
}

static int kbd_hit(Kbd *k, double x, double y) {
  int w = gtk_widget_get_allocated_width(k->area), h = gtk_widget_get_allocated_height(k->area);
  double ww = w / 14.0;
  int base = 12 * (k->octave + 1);
  if (y < h * 0.62) {            /* zuerst schwarze Tasten */
    for (int i = 0; i < 14; i++) {
      int n = base + 12 * (i / 7) + kbd_white[i % 7] + 1;
      if (!kbd_is_black(n)) continue;
      double bx = (i + 1) * ww - ww * 0.3;
      if (x >= bx && x < bx + ww * 0.6) return n;
    }
  }
  int i = (int)(x / ww); if (i < 0) i = 0; if (i > 13) i = 13;
  return base + 12 * (i / 7) + kbd_white[i % 7];
}

static gboolean kbd_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
  Kbd *k = data;
  int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
  double ww = W / 14.0;
  int base = 12 * (k->octave + 1);
  for (int i = 0; i < 14; i++) {
    int n = base + 12 * (i / 7) + kbd_white[i % 7];
    double x = i * ww + 1, r = 6;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + ww - 2 - r, H - r - 1, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, H - r - 1, r, M_PI / 2, M_PI);
    cairo_line_to(cr, x, 0); cairo_line_to(cr, x + ww - 2, 0); cairo_close_path(cr);
    if (k->down[n]) cairo_set_source_rgb(cr, 0.70, 0.62, 1.0); else cairo_set_source_rgb(cr, 0.98, 0.98, 0.99);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.18); cairo_set_line_width(cr, 1); cairo_stroke(cr);
    if (n % 12 == 0) {
      char lab[8]; g_snprintf(lab, sizeof lab, "C%d", n / 12 - 1);
      cairo_set_source_rgba(cr, 0, 0, 0, 0.45); cairo_select_font_face(cr, "Cloke Sans", 0, 0); cairo_set_font_size(cr, 11);
      cairo_text_extents_t te; cairo_text_extents(cr, lab, &te);
      cairo_move_to(cr, x + (ww - 2 - te.width) / 2, H - 10); cairo_show_text(cr, lab);
    }
  }
  for (int i = 0; i < 14; i++) {
    int n = base + 12 * (i / 7) + kbd_white[i % 7] + 1;
    if (!kbd_is_black(n) || i == 13) continue;
    double bx = (i + 1) * ww - ww * 0.3, bw = ww * 0.6, bh = H * 0.62, r = 4;
    cairo_new_sub_path(cr);
    cairo_arc(cr, bx + bw - r, bh - r, r, 0, M_PI / 2);
    cairo_arc(cr, bx + r, bh - r, r, M_PI / 2, M_PI);
    cairo_line_to(cr, bx, 0); cairo_line_to(cr, bx + bw, 0); cairo_close_path(cr);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, bh);
    if (k->down[n]) { cairo_pattern_add_color_stop_rgb(g, 0, 0.55, 0.42, 1.0); cairo_pattern_add_color_stop_rgb(g, 1, 0.35, 0.22, 0.85); }
    else { cairo_pattern_add_color_stop_rgb(g, 0, 0.20, 0.17, 0.27); cairo_pattern_add_color_stop_rgb(g, 1, 0.06, 0.05, 0.10); }
    cairo_set_source(cr, g); cairo_fill(cr); cairo_pattern_destroy(g);
  }
  return FALSE;
}

static gboolean kbd_press(GtkWidget *w, GdkEventButton *e, gpointer data) {
  Kbd *k = data;
  if (e->button != 1) return FALSE;
  k->mousenote = kbd_hit(k, e->x, e->y);
  kbd_note(k, k->mousenote, 1);
  return TRUE;
}
static gboolean kbd_release(GtkWidget *w, GdkEventButton *e, gpointer data) {
  Kbd *k = data;
  if (k->mousenote >= 0) kbd_note(k, k->mousenote, 0);
  k->mousenote = -1;
  return TRUE;
}
static gboolean kbd_motion(GtkWidget *w, GdkEventMotion *e, gpointer data) {
  Kbd *k = data;
  if (k->mousenote < 0 || !(e->state & GDK_BUTTON1_MASK)) return FALSE;
  int n = kbd_hit(k, e->x, e->y);
  if (n != k->mousenote) { kbd_note(k, k->mousenote, 0); k->mousenote = n; kbd_note(k, n, 1); }
  return TRUE;
}

static void kbd_octave(Kbd *k, int d) {
  int o = k->octave + d;
  if (o < 0 || o > 7) return;
  for (int n = 0; n < 128; n++) if (k->down[n]) kbd_note(k, n, 0);
  k->octave = o;
  char t[32]; g_snprintf(t, sizeof t, "C%d – B%d", o, o + 1);
  gtk_label_set_text(GTK_LABEL(k->octlabel), t);
  gtk_widget_queue_draw(k->area);
}
static void kbd_oct_down(GtkButton *b, gpointer d) { kbd_octave(d, -1); }
static void kbd_oct_up(GtkButton *b, gpointer d) { kbd_octave(d, 1); }

static int kbd_keynote(Kbd *k, guint kv) {
  static const char map[] = "awsedftgyhujkolp;";   /* C C# D D# E F F# G G# A A# B C C# D D# E */
  const char *p = kv < 128 ? strchr(map, (int)g_ascii_tolower((gchar)kv)) : NULL;
  if (!p || !*p) return -1;
  return 12 * (k->octave + 1) + (int)(p - map);
}
static gboolean kbd_key(GtkWidget *w, GdkEventKey *e, gpointer data) {
  Kbd *k = data;
  if (e->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_META_MASK)) return FALSE;
  int on = e->type == GDK_KEY_PRESS;
  if (on && (e->keyval == 'z' || e->keyval == 'Z')) { kbd_octave(k, -1); return TRUE; }
  if (on && (e->keyval == 'x' || e->keyval == 'X')) { kbd_octave(k, 1); return TRUE; }
  if (on && (e->keyval == 'c' || e->keyval == 'C')) { gtk_range_set_value(GTK_RANGE(k->vel), gtk_range_get_value(GTK_RANGE(k->vel)) - 16); return TRUE; }
  if (on && (e->keyval == 'v' || e->keyval == 'V')) { gtk_range_set_value(GTK_RANGE(k->vel), gtk_range_get_value(GTK_RANGE(k->vel)) + 16); return TRUE; }
  int n = kbd_keynote(k, e->keyval);
  if (n < 0) return FALSE;
  kbd_note(k, n, on);   /* Tastenwiederholung: kbd_note sendet nur bei Wechsel */
  return TRUE;
}

static void run_keyboard(void) {
  Kbd *k = g_new0(Kbd, 1);
  k->fd = -1; k->octave = 3; k->mousenote = -1;
  k->uid = (int)(g_random_int() | 0x10000000u);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGSYS, SIG_IGN);   /* arm64: kein SysV-Shm, Cairos XShm-Versuch soll scheitern statt das Programm zu beenden */
  GtkWidget *win = new_window("com.clokeos.miditastatur", T("MIDI Keyboard", "MIDI-Tastatur"), 760, 250, FALSE);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 12);
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *down = gtk_button_new_with_label("−"), *up = gtk_button_new_with_label("+");
  k->octlabel = gtk_label_new("");
  gtk_widget_set_size_request(k->octlabel, 80, -1);
  gtk_box_pack_start(GTK_BOX(bar), gtk_label_new(T("Octave", "Oktave")), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), down, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), k->octlabel, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), up, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), gtk_label_new(T("Velocity", "Anschlag")), FALSE, FALSE, 12);
  k->vel = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 127, 1);
  gtk_range_set_value(GTK_RANGE(k->vel), 100);
  gtk_widget_set_size_request(k->vel, 160, -1);
  gtk_box_pack_start(GTK_BOX(bar), k->vel, FALSE, FALSE, 0);
  k->status = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(k->status), 1);
  gtk_style_context_add_class(gtk_widget_get_style_context(k->status), "dim-label");
  gtk_box_pack_end(GTK_BOX(bar), k->status, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
  k->area = gtk_drawing_area_new();
  gtk_widget_set_size_request(k->area, 560, 150);
  gtk_widget_add_events(k->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK);
  g_signal_connect(k->area, "draw", G_CALLBACK(kbd_draw), k);
  g_signal_connect(k->area, "button-press-event", G_CALLBACK(kbd_press), k);
  g_signal_connect(k->area, "button-release-event", G_CALLBACK(kbd_release), k);
  g_signal_connect(k->area, "motion-notify-event", G_CALLBACK(kbd_motion), k);
  gtk_box_pack_start(GTK_BOX(box), k->area, TRUE, TRUE, 0);
  GtkWidget *hint = gtk_label_new(T("Keys A W S E D F T G Y H U J K: notes · Z/X: octave · C/V: velocity",
                                    "Tasten A W S E D F T G Y H U J K: Noten · Z/X: Oktave · C/V: Anschlag"));
  gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
  gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
  g_signal_connect(down, "clicked", G_CALLBACK(kbd_oct_down), k);
  g_signal_connect(up, "clicked", G_CALLBACK(kbd_oct_up), k);
  g_signal_connect(win, "key-press-event", G_CALLBACK(kbd_key), k);
  g_signal_connect(win, "key-release-event", G_CALLBACK(kbd_key), k);
  gtk_container_add(GTK_CONTAINER(win), box);
  kbd_octave(k, 0);
  kbd_connect(k);
  g_timeout_add_seconds(2, kbd_connect, k);
  gtk_widget_show_all(win);
  gtk_main();
}

int main(int argc, char **argv) {
  char *lang = NULL;
  de = g_file_get_contents("/etc/clokeos-language", &lang, NULL, NULL) && g_str_has_prefix(lang, "de");
  if (lang && strlen(lang) >= 2) g_strlcpy(lang2, lang, 3);
  g_free(lang);
  if (argc < 2) {
    g_printerr("usage: clokeapps notes|calc|calendar|activity|keyboard\n");
    return 2;
  }
  gtk_init(&argc, &argv);
  load_css();
  if (!strcmp(argv[1], "notes")) run_notes();
  else if (!strcmp(argv[1], "calc")) run_calc();
  else if (!strcmp(argv[1], "calendar")) run_calendar();
  else if (!strcmp(argv[1], "activity")) run_activity();
  else if (!strcmp(argv[1], "keyboard")) run_keyboard();
  else {
    g_printerr("unknown app %s\n", argv[1]);
    return 2;
  }
  return 0;
}
