// FundusCore: Dateisystem-Kern von Fundus (docs/fundus.md). Reines C ohne Cocoa-Typen, damit die Oberfläche
// (FundusKit, Objective-C) und später andere Sprachen (Rust, Swift) oder GTK/Raven denselben Kern benutzen können.
// Pfade sind UTF-8, Fehler sind errno-Werte (0 = gut).
#ifndef FCORE_H
#define FCORE_H
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	FC_FOLDER, FC_APP, FC_IMAGE, FC_AUDIO, FC_VIDEO, FC_TEXT, FC_CODE, FC_PDF, FC_ARCHIVE, FC_DISKIMAGE,
	FC_EXE, FC_DOCUMENT, FC_PACKAGE, FC_ALIAS, FC_EXECUTABLE, FC_FILE, FC_KIND_COUNT
} fc_kind;

typedef enum {
	FC_HOME, FC_DESKTOP, FC_DOCUMENTS, FC_DOWNLOADS, FC_MOVIES, FC_MUSIC, FC_PICTURES, FC_PUBLIC, FC_LIBRARY,
	FC_APPLICATIONS, FC_TRASH, FC_ROOT, FC_VOLUMES, FC_NT, FC_TUX, FC_SPECIAL_COUNT
} fc_special;

typedef struct {
	char *name;         // Dateiname
	char *path;         // voller Pfad
	fc_kind kind;
	int is_dir;         // Ordner (auch .app und andere Pakete)
	int is_hidden;      // beginnt mit '.'
	int is_link;
	int64_t size;       // Bytes (bei Ordnern -1)
	time_t mtime;
	int nitems;         // Ordner: Anzahl der Einträge (-1 unbekannt)
} fc_entry;

typedef struct { fc_entry *v; int n; } fc_list;

typedef enum { FC_SORT_NAME, FC_SORT_DATE, FC_SORT_SIZE, FC_SORT_KIND } fc_sort;

// Ordner lesen; hidden = auch Punkt-Dateien. Ergebnis mit fc_list_free freigeben.
int fc_read_dir(const char *dir, int hidden, fc_list *out);
void fc_list_free(fc_list *l);
void fc_sort_list(fc_list *l, fc_sort by, int descending, int folders_first);
// nur Einträge, deren Name text enthält (ohne Groß/Klein)
void fc_filter(fc_list *l, const char *text);

fc_kind fc_kind_of(const char *path, int is_dir);
const char *fc_kind_key(fc_kind k);          // Schlüssel für die Übersetzung ("kind.folder" …)
const char *fc_special_path(fc_special s);   // statischer Puffer je Ordner
const char *fc_special_key(fc_special s);    // "sidebar.home" …
int fc_is_package(const char *path);         // .app, .bundle, .logicx … als Datei behandeln
int fc_exists(const char *path, int *is_dir);
int fc_same_volume(const char *a, const char *b);   // 1 = gleiches Volume (Ziehen: bewegen statt kopieren, wie der Finder)  // 1 = vorhanden; is_dir (darf NULL sein) = Ordner (Links verfolgt)

// Dateioperationen
int fc_mkdir_unique(const char *dir, const char *base, char *outpath, int outlen);
int fc_rename(const char *path, const char *newname, char *outpath, int outlen);
int fc_trash(const char *path);              // nach ~/.Trash (eindeutiger Name)
int fc_empty_trash(void);
int fc_copy(const char *src, const char *dstdir, char *outpath, int outlen);   // rekursiv, eindeutiger Name bei Kollision
int fc_move(const char *src, const char *dstdir, char *outpath, int outlen);
int fc_trash_count(void);

// Speicherplatz des Volumes
int fc_free_space(const char *path, int64_t *free_bytes, int64_t *total_bytes);
// "12 KB", "3,4 MB" (dezimal wie der Finder); decimal_comma für de/fr/es/it
void fc_format_size(int64_t bytes, int decimal_comma, char *out, int outlen);

// Öffnen wie ein Doppelklick: /usr/bin/open (CloakServer entscheidet: App, Ordner, Dokument). with = Bundle-ID oder NULL
int fc_open(const char *path, const char *with);

#ifdef __cplusplus
}
#endif
#endif
