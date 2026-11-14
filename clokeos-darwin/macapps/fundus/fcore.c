// FundusCore, siehe fcore.h
#include "fcore.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char *home(void) { const char *h = getenv("HOME"); return h && *h ? h : "/var/root"; }

const char *fc_special_path(fc_special s) {
	static char buf[FC_SPECIAL_COUNT][PATH_MAX];
	const char *sub = NULL;
	switch (s) {
	case FC_HOME: snprintf(buf[s], PATH_MAX, "%s", home()); return buf[s];
	case FC_DESKTOP: sub = "Desktop"; break;
	case FC_DOCUMENTS: sub = "Documents"; break;
	case FC_DOWNLOADS: sub = "Downloads"; break;
	case FC_MOVIES: sub = "Movies"; break;
	case FC_MUSIC: sub = "Music"; break;
	case FC_PICTURES: sub = "Pictures"; break;
	case FC_PUBLIC: sub = "Public"; break;
	case FC_LIBRARY: sub = "Library"; break;
	case FC_TRASH: sub = ".Trash"; break;
	case FC_APPLICATIONS: return "/Applications";
	case FC_ROOT: return "/";
	case FC_VOLUMES: return "/Volumes";
	case FC_NT: return "/NT";
	case FC_TUX: return "/TUX";
	default: return "/";
	}
	snprintf(buf[s], PATH_MAX, "%s/%s", home(), sub);
	return buf[s];
}

const char *fc_special_key(fc_special s) {
	static const char *k[FC_SPECIAL_COUNT] = { "sidebar.home", "sidebar.desktop", "sidebar.documents", "sidebar.downloads",
		"sidebar.movies", "sidebar.music", "sidebar.pictures", "sidebar.public", "sidebar.library", "sidebar.applications",
		"sidebar.trash", "sidebar.root", "sidebar.volumes", "sidebar.nt", "sidebar.tux" };
	return s < FC_SPECIAL_COUNT ? k[s] : "";
}

const char *fc_kind_key(fc_kind k) {
	static const char *n[FC_KIND_COUNT] = { "kind.folder", "kind.app", "kind.image", "kind.audio", "kind.video", "kind.text",
		"kind.code", "kind.pdf", "kind.archive", "kind.diskimage", "kind.exe", "kind.document", "kind.package", "kind.alias",
		"kind.executable", "kind.file" };
	return k < FC_KIND_COUNT ? n[k] : "kind.file";
}

static const char *ext_of(const char *path) {
	const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
	if (!dot || (slash && dot < slash) || dot == path || (slash && dot == slash + 1)) return "";
	return dot + 1;
}

static int ext_in(const char *e, const char *list) {   // list: "png jpg jpeg"
	size_t n = strlen(e);
	if (!n) return 0;
	for (const char *p = list; *p; ) {
		const char *q = strchr(p, ' '); size_t l = q ? (size_t)(q - p) : strlen(p);
		if (l == n && !strncasecmp(p, e, n)) return 1;
		if (!q) break;
		p = q + 1;
	}
	return 0;
}

int fc_is_package(const char *path) {
	return ext_in(ext_of(path), "app bundle framework plugin component logicx band kext pkg rtfd xcodeproj photoslibrary musiclibrary");
}

fc_kind fc_kind_of(const char *path, int is_dir) {
	const char *e = ext_of(path);
	if (is_dir) {
		if (ext_in(e, "app")) return FC_APP;
		if (fc_is_package(path)) return FC_PACKAGE;
		return FC_FOLDER;
	}
	if (ext_in(e, "png jpg jpeg gif tif tiff bmp heic webp svg icns ico psd")) return FC_IMAGE;
	if (ext_in(e, "mp3 wav aif aiff aifc caf m4a flac ogg opus mid midi")) return FC_AUDIO;
	if (ext_in(e, "mp4 mov m4v mkv avi webm mpg mpeg")) return FC_VIDEO;
	if (ext_in(e, "pdf")) return FC_PDF;
	if (ext_in(e, "zip tar gz tgz bz2 xz 7z rar sit sitx z lz zst")) return FC_ARCHIVE;
	if (ext_in(e, "dmg iso img")) return FC_DISKIMAGE;
	if (ext_in(e, "exe msi bat com")) return FC_EXE;
	if (ext_in(e, "txt md rtf log csv strings plist")) return FC_TEXT;
	if (ext_in(e, "c h m mm cpp hpp cc swift py sh zsh js ts json xml html css rs go java kt rb pl lua sql yaml yml toml")) return FC_CODE;
	if (ext_in(e, "doc docx odt pages key numbers xls xlsx ods ppt pptx odp epub")) return FC_DOCUMENT;
	struct stat st;
	if (!*e && stat(path, &st) == 0 && (st.st_mode & 0111)) return FC_EXECUTABLE;
	return FC_FILE;
}

static int count_entries(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) return -1;
	int n = 0; struct dirent *e;
	while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
	closedir(d);
	return n;
}

int fc_read_dir(const char *dir, int hidden, fc_list *out) {
	out->v = NULL; out->n = 0;
	DIR *d = opendir(dir);
	if (!d) return errno;
	int cap = 64;
	out->v = calloc(cap, sizeof(fc_entry));
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (!hidden && e->d_name[0] == '.') continue;
		if (out->n == cap) { cap *= 2; out->v = realloc(out->v, cap * sizeof(fc_entry)); }
		fc_entry *x = &out->v[out->n];
		memset(x, 0, sizeof *x);
		size_t pl = strlen(dir) + strlen(e->d_name) + 2;
		x->path = malloc(pl);
		snprintf(x->path, pl, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
		x->name = strdup(e->d_name);
		x->is_hidden = e->d_name[0] == '.';
		struct stat st, lst;
		int ok = stat(x->path, &st) == 0;
		x->is_link = lstat(x->path, &lst) == 0 && S_ISLNK(lst.st_mode);
		if (!ok && x->is_link) { st = lst; ok = 1; }
		x->is_dir = ok && S_ISDIR(st.st_mode);
		x->mtime = ok ? st.st_mtime : 0;
		x->size = x->is_dir ? -1 : (ok ? st.st_size : 0);
		x->kind = fc_kind_of(x->path, x->is_dir);
		x->nitems = -1;
		out->n++;
	}
	closedir(d);
	return 0;
}

void fc_list_free(fc_list *l) {
	for (int i = 0; i < l->n; i++) { free(l->v[i].name); free(l->v[i].path); }
	free(l->v); l->v = NULL; l->n = 0;
}

// natürlicher Vergleich wie im Finder: "Datei 2" vor "Datei 10", ohne Groß/Klein
static int natcmp(const char *a, const char *b) {
	while (*a && *b) {
		if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
			char *ea, *eb; unsigned long long x = strtoull(a, &ea, 10), y = strtoull(b, &eb, 10);
			if (x != y) return x < y ? -1 : 1;
			a = ea; b = eb; continue;
		}
		int ca = (unsigned char)*a, cb = (unsigned char)*b;
		if (ca < 128) ca = tolower(ca);
		if (cb < 128) cb = tolower(cb);
		if (ca != cb) return ca < cb ? -1 : 1;
		a++; b++;
	}
	return *a ? 1 : *b ? -1 : 0;
}

static fc_sort g_by; static int g_desc, g_ff;
static int cmp(const void *pa, const void *pb) {
	const fc_entry *a = pa, *b = pb;
	int fa = a->is_dir && a->kind == FC_FOLDER, fb = b->is_dir && b->kind == FC_FOLDER;
	if (g_ff && fa != fb) return fa ? -1 : 1;
	int r = 0;
	switch (g_by) {
	case FC_SORT_DATE: r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime; break;
	case FC_SORT_SIZE: r = a->size < b->size ? -1 : a->size > b->size; break;
	case FC_SORT_KIND: r = (int)a->kind - (int)b->kind; break;
	default: break;
	}
	if (!r) r = natcmp(a->name, b->name);
	return g_desc ? -r : r;
}

void fc_sort_list(fc_list *l, fc_sort by, int descending, int folders_first) {
	g_by = by; g_desc = descending; g_ff = folders_first;
	qsort(l->v, l->n, sizeof(fc_entry), cmp);
}

void fc_filter(fc_list *l, const char *text) {
	if (!text || !*text) return;
	int k = 0;
	for (int i = 0; i < l->n; i++) {
		if (strcasestr(l->v[i].name, text)) l->v[k++] = l->v[i];
		else { free(l->v[i].name); free(l->v[i].path); }
	}
	l->n = k;
}

static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

int fc_exists(const char *path, int *is_dir) {
	struct stat st;
	if (stat(path, &st) != 0) { if (is_dir) *is_dir = 0; return 0; }
	if (is_dir) *is_dir = S_ISDIR(st.st_mode);
	return 1;
}

// "Name", "Name 2", "Name 3" … bzw. "Name.txt", "Name 2.txt"
static void unique_path(const char *dir, const char *name, char *out, int outlen) {
	snprintf(out, outlen, "%s/%s", dir, name);
	if (!exists(out)) return;
	char base[NAME_MAX + 1]; snprintf(base, sizeof base, "%s", name);
	const char *e = ext_of(name); char ext[64] = "";
	if (*e && strlen(e) < sizeof ext - 1) { snprintf(ext, sizeof ext, ".%s", e); base[strlen(base) - strlen(ext)] = 0; }
	for (int i = 2; i < 10000; i++) {
		snprintf(out, outlen, "%s/%s %d%s", dir, base, i, ext);
		if (!exists(out)) return;
	}
}

int fc_mkdir_unique(const char *dir, const char *base, char *outpath, int outlen) {
	char p[PATH_MAX]; unique_path(dir, base, p, sizeof p);
	if (mkdir(p, 0755)) return errno;
	if (outpath) snprintf(outpath, outlen, "%s", p);
	return 0;
}

int fc_rename(const char *path, const char *newname, char *outpath, int outlen) {
	if (!*newname || strchr(newname, '/')) return EINVAL;
	char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", path);
	char *s = strrchr(dir, '/'); if (!s) return EINVAL;
	if (s == dir) s[1] = 0; else *s = 0;
	char np[PATH_MAX]; snprintf(np, sizeof np, "%s/%s", strcmp(dir, "/") ? dir : "", newname);
	if (strcmp(np, path) && exists(np)) return EEXIST;
	if (rename(path, np)) return errno;
	if (outpath) snprintf(outpath, outlen, "%s", np);
	return 0;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
	int in = open(src, O_RDONLY), out = -1, r = 0;
	if (in < 0) return errno;
	out = open(dst, O_WRONLY | O_CREAT | O_EXCL, mode & 07777);
	if (out < 0) { r = errno; close(in); return r; }
	char *buf = malloc(1 << 20); ssize_t n;
	while ((n = read(in, buf, 1 << 20)) > 0) {
		for (ssize_t w = 0; w < n; ) { ssize_t k = write(out, buf + w, n - w); if (k < 0) { if (errno == EINTR) continue; r = errno; goto done; } w += k; }
	}
	if (n < 0) r = errno;
done:
	free(buf); close(in); if (close(out) && !r) r = errno;
	return r;
}

static int copy_tree(const char *src, const char *dst) {
	struct stat st;
	if (lstat(src, &st)) return errno;
	if (S_ISLNK(st.st_mode)) {
		char t[PATH_MAX]; ssize_t n = readlink(src, t, sizeof t - 1);
		if (n < 0) return errno;
		t[n] = 0;
		return symlink(t, dst) ? errno : 0;
	}
	if (!S_ISDIR(st.st_mode)) return copy_file(src, dst, st.st_mode);
	if (mkdir(dst, st.st_mode & 07777)) return errno;
	DIR *d = opendir(src);
	if (!d) return errno;
	struct dirent *e; int r = 0;
	while (!r && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char s[PATH_MAX], t[PATH_MAX];
		snprintf(s, sizeof s, "%s/%s", src, e->d_name);
		snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
		r = copy_tree(s, t);
	}
	closedir(d);
	return r;
}

static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s && s[1] ? s + 1 : p; }

int fc_copy(const char *src, const char *dstdir, char *outpath, int outlen) {
	char d[PATH_MAX]; unique_path(dstdir, base_name(src), d, sizeof d);
	int r = copy_tree(src, d);
	if (!r && outpath) snprintf(outpath, outlen, "%s", d);
	return r;
}

static int remove_tree(const char *p) {
	struct stat st;
	if (lstat(p, &st)) return errno;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(p);
		if (!d) return errno;
		struct dirent *e; int r = 0;
		while (!r && (e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
			char s[PATH_MAX]; snprintf(s, sizeof s, "%s/%s", p, e->d_name);
			r = remove_tree(s);
		}
		closedir(d);
		if (r) return r;
		return rmdir(p) ? errno : 0;
	}
	return unlink(p) ? errno : 0;
}

int fc_move(const char *src, const char *dstdir, char *outpath, int outlen) {
	char d[PATH_MAX]; unique_path(dstdir, base_name(src), d, sizeof d);
	if (rename(src, d) == 0) { if (outpath) snprintf(outpath, outlen, "%s", d); return 0; }
	if (errno != EXDEV) return errno;
	int r = copy_tree(src, d);              // über Volume-Grenzen: kopieren, dann löschen
	if (!r) r = remove_tree(src);
	if (!r && outpath) snprintf(outpath, outlen, "%s", d);
	return r;
}

int fc_trash(const char *path) {
	const char *t = fc_special_path(FC_TRASH);
	mkdir(t, 0700);
	char out[PATH_MAX];
	return fc_move(path, t, out, sizeof out);
}

int fc_empty_trash(void) {
	fc_list l; int r = fc_read_dir(fc_special_path(FC_TRASH), 1, &l);
	if (r) return r;
	for (int i = 0; i < l.n && !r; i++) r = remove_tree(l.v[i].path);
	fc_list_free(&l);
	return r;
}

int fc_same_volume(const char *a, const char *b) {
	struct stat sa, sb;
	if (stat(a, &sa) || stat(b, &sb)) return 0;
	return sa.st_dev == sb.st_dev;
}

int fc_trash_count(void) { return count_entries(fc_special_path(FC_TRASH)); }

int fc_free_space(const char *path, int64_t *free_bytes, int64_t *total_bytes) {
	struct statfs s;
	if (statfs(path, &s)) return errno;
	if (free_bytes) *free_bytes = (int64_t)s.f_bavail * s.f_bsize;
	if (total_bytes) *total_bytes = (int64_t)s.f_blocks * s.f_bsize;
	return 0;
}

void fc_format_size(int64_t b, int comma, char *out, int outlen) {
	static const char *u[] = { "bytes", "KB", "MB", "GB", "TB" };
	if (b < 0) { snprintf(out, outlen, "--"); return; }
	if (b < 1000) { snprintf(out, outlen, "%lld %s", (long long)b, b == 1 ? "byte" : "bytes"); return; }
	double v = (double)b; int i = 0;
	while (v >= 1000 && i < 4) { v /= 1000; i++; }
	snprintf(out, outlen, v < 10 ? "%.1f %s" : "%.0f %s", v, u[i]);
	if (comma) for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

int fc_open(const char *path, const char *with) {
	const char *argv[6]; int k = 0;
	argv[k++] = "/usr/bin/open";
	if (with) { argv[k++] = "-b"; argv[k++] = with; }
	argv[k++] = path; argv[k] = NULL;
	pid_t pid;
	int r = posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ);
	if (r) return r;
	int st; waitpid(pid, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : EIO;
}
