/* Search für ClokeOS auf WebKitGTK: Tabs, Adressfeld, Zurück/Vor, Neu laden (Safari-ähnlich).
 *   clokesearch [URL]      ohne URL die Startseite aus /share/clokeos
 * Tasten: Strg+L Adresse, Strg+T neuer Tab, Strg+W Tab schließen, Strg+R neu laden, Alt+Links/Rechts, Strg+Tab nächster Tab. */
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <string.h>

static GtkWidget *window, *notebook, *entry, *btn_back, *btn_fwd, *btn_reload;

static WebKitWebView *current_view(void)
{
    int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    return n < 0 ? NULL : WEBKIT_WEB_VIEW(gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), n));
}

/* "wikipedia.org" -> https://wikipedia.org, "zwei wörter" -> Suche */
static char *normalize(const char *in)
{
    char *s = g_strstrip(g_strdup(in));
    if (strstr(s, "://") || g_str_has_prefix(s, "about:") || g_str_has_prefix(s, "data:"))
        return s;
    if (!strchr(s, ' ') && (strchr(s, '.') || g_str_has_prefix(s, "localhost"))) {
        char *u = g_strconcat("https://", s, NULL);
        g_free(s);
        return u;
    }
    char *q = g_uri_escape_string(s, NULL, FALSE);
    char *u = g_strconcat("https://duckduckgo.com/?q=", q, NULL);
    g_free(q);
    g_free(s);
    return u;
}

static void update_chrome(void)
{
    WebKitWebView *v = current_view();
    if (!v)
        return;
    gtk_widget_set_sensitive(btn_back, webkit_web_view_can_go_back(v));
    gtk_widget_set_sensitive(btn_fwd, webkit_web_view_can_go_forward(v));
    const char *uri = webkit_web_view_get_uri(v);
    if (uri && !gtk_widget_has_focus(entry))
        gtk_entry_set_text(GTK_ENTRY(entry), g_str_has_prefix(uri, "file://") ? "" : uri);
    const char *title = webkit_web_view_get_title(v);
    gtk_window_set_title(GTK_WINDOW(window), title && *title ? title : "Search");
    gtk_button_set_label(GTK_BUTTON(btn_reload), webkit_web_view_is_loading(v) ? "✕" : "⟳");
}

static void on_notify(GObject *o, GParamSpec *p, gpointer d) { if (WEBKIT_WEB_VIEW(o) == current_view()) update_chrome(); }

static GtkWidget *tab_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 22);
    return l;
}

static void on_title(GObject *o, GParamSpec *p, gpointer d)
{
    WebKitWebView *v = WEBKIT_WEB_VIEW(o);
    const char *t = webkit_web_view_get_title(v);
    gtk_notebook_set_tab_label(GTK_NOTEBOOK(notebook), GTK_WIDGET(v), tab_label(t && *t ? t : "…"));
    gtk_widget_show_all(notebook);
    on_notify(o, p, d);
}

static WebKitWebView *new_tab(const char *url, WebKitWebView *related);

static GtkWidget *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer d)
{
    return GTK_WIDGET(new_tab(NULL, v));
}

static WebKitWebView *new_tab(const char *url, WebKitWebView *related)
{
    GtkWidget *v = related ? webkit_web_view_new_with_related_view(related) : webkit_web_view_new();
    WebKitSettings *st = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(v));
    webkit_settings_set_hardware_acceleration_policy(st, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    webkit_settings_set_enable_developer_extras(st, FALSE);
    g_signal_connect(v, "notify::uri", G_CALLBACK(on_notify), NULL);
    g_signal_connect(v, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(v, "notify::is-loading", G_CALLBACK(on_notify), NULL);
    g_signal_connect(v, "create", G_CALLBACK(on_create), NULL);
    int n = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), v, tab_label("Neuer Tab"));
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook), v, TRUE);
    gtk_widget_show_all(notebook);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), n);
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) > 1);
    if (url) {
        char *u = normalize(url);
        webkit_web_view_load_uri(WEBKIT_WEB_VIEW(v), u);
        g_free(u);
    }
    return WEBKIT_WEB_VIEW(v);
}

static void close_tab(void)
{
    int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) <= 1) { gtk_widget_destroy(window); return; }
    gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), n);
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) > 1);
    update_chrome();
}

static void on_back(GtkWidget *w, gpointer d) { WebKitWebView *v = current_view(); if (v) webkit_web_view_go_back(v); }
static void on_fwd(GtkWidget *w, gpointer d) { WebKitWebView *v = current_view(); if (v) webkit_web_view_go_forward(v); }
static void on_reload(GtkWidget *w, gpointer d)
{
    WebKitWebView *v = current_view();
    if (!v) return;
    if (webkit_web_view_is_loading(v)) webkit_web_view_stop_loading(v); else webkit_web_view_reload(v);
}
static void on_new_tab(GtkWidget *w, gpointer d) { new_tab("file:///share/clokeos/search-en-hell.html", NULL); gtk_widget_grab_focus(entry); }
static void on_enter(GtkWidget *w, gpointer d)
{
    WebKitWebView *v = current_view();
    char *u = normalize(gtk_entry_get_text(GTK_ENTRY(entry)));
    if (v) webkit_web_view_load_uri(v, u);
    g_free(u);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}
static void on_switch(GtkNotebook *nb, GtkWidget *page, guint n, gpointer d) { update_chrome(); }

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    gboolean ctrl = e->state & GDK_CONTROL_MASK, alt = e->state & GDK_MOD1_MASK;
    if (ctrl && e->keyval == GDK_KEY_l) { gtk_widget_grab_focus(entry); gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1); return TRUE; }
    if (ctrl && e->keyval == GDK_KEY_t) { on_new_tab(NULL, NULL); return TRUE; }
    if (ctrl && e->keyval == GDK_KEY_w) { close_tab(); return TRUE; }
    if (ctrl && e->keyval == GDK_KEY_r) { WebKitWebView *v = current_view(); if (v) webkit_web_view_reload(v); return TRUE; }
    if (ctrl && e->keyval == GDK_KEY_Tab) { int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)); gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), (gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook)) + 1) % n); return TRUE; }
    if (alt && e->keyval == GDK_KEY_Left) { on_back(NULL, NULL); return TRUE; }
    if (alt && e->keyval == GDK_KEY_Right) { on_fwd(NULL, NULL); return TRUE; }
    return FALSE;
}

static GtkWidget *tool_button(const char *label, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "flat");
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "entry{border-radius:9px;padding:4px 12px;min-height:22px}"
        "button.flat{border-radius:8px;padding:2px 10px}"
        "notebook tab{padding:4px 10px}", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 980, 640);
    gtk_window_set_title(GTK_WINDOW(window), "Search");
    gtk_window_set_wmclass(GTK_WINDOW(window), "clokesearch", "ClokeSearch");
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 6);
    btn_back = tool_button("‹", G_CALLBACK(on_back));
    btn_fwd = tool_button("›", G_CALLBACK(on_fwd));
    btn_reload = tool_button("⟳", G_CALLBACK(on_reload));
    GtkWidget *btn_new = tool_button("+", G_CALLBACK(on_new_tab));
    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Suchen oder Adresse eingeben");
    gtk_entry_set_alignment(GTK_ENTRY(entry), 0.5);
    g_signal_connect(entry, "activate", G_CALLBACK(on_enter), NULL);
    gtk_box_pack_start(GTK_BOX(bar), btn_back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), btn_fwd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), entry, TRUE, TRUE, 6);
    gtk_box_pack_start(GTK_BOX(bar), btn_reload, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), btn_new, FALSE, FALSE, 0);

    notebook = gtk_notebook_new();
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch), NULL);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), notebook, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(window), box);
    gtk_widget_show_all(window);

    new_tab(argc > 1 ? argv[1] : "file:///share/clokeos/search-en-hell.html", NULL);
    gtk_main();
    return 0;
}
