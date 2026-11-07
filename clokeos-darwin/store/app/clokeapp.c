/* clokeapp: ein WebKit-Fenster ohne Adressleiste und Tabs, für Programme mit Weboberfläche (Cloke Store).
 *   clokeapp [--title T] [--size BxH] [--class NAME] [--allow PRAEFIX] URL
 * Es lädt nur Adressen, die mit PRAEFIX beginnen (Vorgabe: die Start-Adresse bis zum Pfad); alles andere,
 * auch neue Fenster und Links nach außen, wird verworfen. Kein Kontextmenü, keine Entwicklerwerkzeuge.
 * Strg+R lädt neu, Strg+W und Strg+Q schließen.
 *
 * Die WebKit-Köpfe liegen auf dem Mac nur nach einem vollen WebKit-Bau vor; hier wird deshalb nur das
 * Wenige, was gebraucht wird, von Hand deklariert und gegen libwebkit2gtk-4.1 gelinkt. */
#include <gtk/gtk.h>
#include <string.h>

typedef struct _WV WV;
GtkWidget *webkit_web_view_new(void);
void webkit_web_view_load_uri(WV *v, const char *uri);
void webkit_web_view_reload(WV *v);
const char *webkit_web_view_get_title(WV *v);
void *webkit_web_view_get_settings(WV *v);
void webkit_settings_set_hardware_acceleration_policy(void *s, int policy); /* 2 = NEVER */
void webkit_settings_set_enable_developer_extras(void *s, gboolean on);
void webkit_settings_set_enable_write_console_messages_to_stdout(void *s, gboolean on);
void *webkit_navigation_policy_decision_get_navigation_action(void *d);
void *webkit_navigation_action_get_request(void *a);
const char *webkit_uri_request_get_uri(void *r);
void webkit_policy_decision_use(void *d);
void webkit_policy_decision_ignore(void *d);

static GtkWidget *window;
static const char *fixed_title, *allow;

static void on_title(GObject *o, GParamSpec *p, gpointer d)
{
    if (fixed_title) return;
    const char *t = webkit_web_view_get_title((WV *)o);
    if (t && *t) gtk_window_set_title(GTK_WINDOW(window), t);
}

/* 0 = Navigation, 1 = neues Fenster, 2 = Antwort (Standard) */
static gboolean on_policy(WV *v, void *dec, int type, gpointer d)
{
    if (type != 0 && type != 1) return FALSE;
    const char *u = NULL;
    if (type == 0) {
        void *a = webkit_navigation_policy_decision_get_navigation_action(dec);
        void *r = a ? webkit_navigation_action_get_request(a) : NULL;
        u = r ? webkit_uri_request_get_uri(r) : NULL;
    }
    if (type == 0 && u && g_str_has_prefix(u, allow)) webkit_policy_decision_use(dec);
    else webkit_policy_decision_ignore(dec);
    return TRUE;
}

static gboolean on_context(void) { return TRUE; }
static GtkWidget *on_create(void) { return NULL; }

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer v)
{
    if (e->state & GDK_CONTROL_MASK) {
        if (e->keyval == GDK_KEY_r) { webkit_web_view_reload((WV *)v); return TRUE; }
        if (e->keyval == GDK_KEY_w || e->keyval == GDK_KEY_q) { gtk_widget_destroy(window); return TRUE; }
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    const char *cls = "ClokeApp", *url = NULL;
    int w = 1000, h = 680;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--title") && i + 1 < argc) fixed_title = argv[++i];
        else if (!strcmp(argv[i], "--class") && i + 1 < argc) cls = argv[++i];
        else if (!strcmp(argv[i], "--allow") && i + 1 < argc) allow = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else url = argv[i];
    }
    if (!url) { fprintf(stderr, "Aufruf: clokeapp [--title T] [--size BxH] [--class NAME] [--allow PRAEFIX] URL\n"); return 2; }
    gtk_init(&argc, &argv);
    if (!allow) {                       /* Vorgabe: Ursprung der Start-Adresse (Schema und Rechner) */
        const char *e = strstr(url, "://");
        e = e ? strchr(e + 3, '/') : NULL;
        allow = e ? g_strndup(url, e - url + 1) : g_strconcat(url, "/", NULL);
    }
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), w, h);
    gtk_window_set_title(GTK_WINDOW(window), fixed_title ? fixed_title : cls);
    gtk_window_set_wmclass(GTK_WINDOW(window), cls, cls);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GtkWidget *v = webkit_web_view_new();
    void *st = webkit_web_view_get_settings((WV *)v);
    webkit_settings_set_hardware_acceleration_policy(st, 2);
    webkit_settings_set_enable_developer_extras(st, FALSE);
    g_signal_connect(v, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(v, "decide-policy", G_CALLBACK(on_policy), NULL);
    g_signal_connect(v, "context-menu", G_CALLBACK(on_context), NULL);
    g_signal_connect(v, "create", G_CALLBACK(on_create), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key), v);
    gtk_container_add(GTK_CONTAINER(window), v);
    gtk_widget_show_all(window);
    webkit_web_view_load_uri((WV *)v, url);
    gtk_main();
    return 0;
}
