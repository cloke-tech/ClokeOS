/*
 * clokeos-pkginstall: installiert ein flaches Mac-Installationspaket (.pkg, xar) wie macOS' installer.
 *
 *   clokeos-pkginstall [-target /] [-n] [-v] [-appledouble] <paket.pkg>
 *
 *   -target <wurzel>   Zielvolume (Standard /); die Dateien landen unter <wurzel><install-location>
 *   -n                 nur auflisten ("Modus Größe Pfad"), nichts schreiben
 *   -v                 jeden Pfad ausgeben
 *   -appledouble       ._name-Dateien (AppleDouble: xattrs) als Dateien anlegen; sonst werden sie übersprungen
 *
 * Ablauf: xar-Kopf und TOC (zlib-gepacktes XML) lesen, jede Payload eines Pakets (auch in
 * Produktarchiven mit mehreren Paketen) als Strom durch gzip -> cpio (odc oder newc) schicken und
 * beim Lesen anlegen. Es gibt keine Zwischendatei: gelesen wird mit pread aus dem Paket, der
 * Speicherbedarf ist unabhängig von der Paketgröße. Ein 1,7-GB-Paket braucht also nur den Platz der
 * entpackten Dateien.
 *
 * Besitzer ist root:wheel (wenn als root gestartet), Modi, Zeiten, Symlinks und Hardlinks kommen aus
 * dem cpio. Bestehende Verzeichnisse behalten ihre Rechte (overwrite-permissions="false").
 * Danach schreibt es <wurzel>/var/db/receipts/<kennung>.plist und <kennung>.files (Pfad, Modus,
 * Größe je Zeile; ein BOM erzeugt es nicht).
 * Nicht unterstützt: pbzx/xz- oder bzip2-Payloads, Skripte (Scripts wird nicht ausgeführt, es ist
 * bei Logic leer), Erweiterte Attribute/Resource Forks.
 *
 * Bauen: macapps/pkginstall.sh (nur libSystem und libz).
 */
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define CHUNK (1 << 20)

static int opt_list, opt_verbose, opt_keep_ad;
static char target[PATH_MAX] = "";
static int is_root;

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#include <stdarg.h>
static void die(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fputs("clokeos-pkginstall: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
  exit(1);
}
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("kein Speicher"); return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die("kein Speicher"); return p; }

/* ---------- Stromkette: Sink-Funktionen ---------- */

typedef int (*sinkfn)(void *ctx, const unsigned char *p, size_t n);

struct zs {                       /* inflate-Stufe */
  z_stream z; int done;
  sinkfn out; void *octx;
  unsigned char *buf;
};
static void zs_init(struct zs *s, sinkfn out, void *octx) {
  memset(s, 0, sizeof *s);
  if (inflateInit2(&s->z, 15 + 32) != Z_OK) die("zlib: inflateInit2");   /* zlib oder gzip erkennen */
  s->out = out; s->octx = octx; s->buf = xmalloc(CHUNK);
}
static int zs_feed(void *ctx, const unsigned char *p, size_t n) {
  struct zs *s = ctx;
  if (s->done) return 0;
  s->z.next_in = (Bytef *)p; s->z.avail_in = (uInt)n;
  while (s->z.avail_in > 0) {
    s->z.next_out = s->buf; s->z.avail_out = CHUNK;
    int r = inflate(&s->z, Z_NO_FLUSH);
    if (r != Z_OK && r != Z_STREAM_END) { fprintf(stderr, "zlib: %s\n", s->z.msg ? s->z.msg : "Fehler"); return -1; }
    size_t have = CHUNK - s->z.avail_out;
    if (have && s->out(s->octx, s->buf, have) < 0) return -1;
    if (r == Z_STREAM_END) { s->done = 1; break; }
  }
  return 0;
}
static void zs_free(struct zs *s) { inflateEnd(&s->z); free(s->buf); }

struct det {                      /* erkennt gzip oder rohes cpio an den ersten Bytes */
  unsigned char head[8]; size_t have; int decided;
  struct zs z; int gz;
  sinkfn out; void *octx;
};
static int det_feed(void *ctx, const unsigned char *p, size_t n) {
  struct det *d = ctx;
  if (!d->decided) {
    while (n > 0 && d->have < 6) d->head[d->have++] = *p++, n--;
    if (d->have < 6) return 0;
    d->decided = 1;
    if (d->head[0] == 0x1f && d->head[1] == 0x8b) { d->gz = 1; zs_init(&d->z, d->out, d->octx); }
    else if (!memcmp(d->head, "pbzx", 4)) { fprintf(stderr, "Payload ist pbzx (xz), nicht unterstützt\n"); return -1; }
    else if (!memcmp(d->head, "BZh", 3)) { fprintf(stderr, "Payload ist bzip2, nicht unterstützt\n"); return -1; }
    else if (memcmp(d->head, "070707", 6) && memcmp(d->head, "070701", 6) && memcmp(d->head, "070702", 6)) {
      fprintf(stderr, "Payload hat kein bekanntes Format (Anfang %02x%02x%02x%02x)\n", d->head[0], d->head[1], d->head[2], d->head[3]);
      return -1;
    }
    if (d->gz) { if (zs_feed(&d->z, d->head, 6) < 0) return -1; }
    else if (d->out(d->octx, d->head, 6) < 0) return -1;
  }
  if (n == 0) return 0;
  return d->gz ? zs_feed(&d->z, p, n) : d->out(d->octx, p, n);
}

/* ---------- xar ---------- */

struct ent {
  char *name; int parent;
  int dir, has_data, enc;          /* enc: 0 roh, 1 zlib/gzip, 2 sonstiges */
  uint64_t off, len, size;
};
static struct ent *ents; static int nents, capents;
static int pkgfd = -1; static uint64_t heap;

static char *ent_path(int i) {
  char tmp[2048]; size_t pos = sizeof tmp - 1; tmp[pos] = 0;
  for (; i >= 0; i = ents[i].parent) {
    size_t l = strlen(ents[i].name);
    if (l + 1 > pos) break;
    pos -= l; memcpy(tmp + pos, ents[i].name, l);
    if (ents[i].parent >= 0) tmp[--pos] = '/';
  }
  return strdup(tmp + pos);
}

static void xml_text(char *dst, size_t cap, const char *s, size_t n) {   /* Entitäten auflösen */
  size_t o = 0;
  for (size_t i = 0; i < n && o + 4 < cap; i++) {
    if (s[i] == '&') {
      if (!strncmp(s + i, "&amp;", 5)) { dst[o++] = '&'; i += 4; }
      else if (!strncmp(s + i, "&lt;", 4)) { dst[o++] = '<'; i += 3; }
      else if (!strncmp(s + i, "&gt;", 4)) { dst[o++] = '>'; i += 3; }
      else if (!strncmp(s + i, "&quot;", 6)) { dst[o++] = '"'; i += 5; }
      else if (!strncmp(s + i, "&apos;", 6)) { dst[o++] = '\''; i += 5; }
      else if (s[i + 1] == '#') {
        unsigned long c = (s[i + 2] == 'x') ? strtoul(s + i + 3, NULL, 16) : strtoul(s + i + 2, NULL, 10);
        const char *semi = memchr(s + i, ';', n - i);
        if (c < 0x80) dst[o++] = (char)c;
        else if (c < 0x800) { dst[o++] = 0xc0 | (c >> 6); dst[o++] = 0x80 | (c & 63); }
        else if (c < 0x10000) { dst[o++] = 0xe0 | (c >> 12); dst[o++] = 0x80 | ((c >> 6) & 63); dst[o++] = 0x80 | (c & 63); }
        else { dst[o++] = 0xf0 | (c >> 18); dst[o++] = 0x80 | ((c >> 12) & 63); dst[o++] = 0x80 | ((c >> 6) & 63); dst[o++] = 0x80 | (c & 63); }
        if (semi) i = semi - s;
      } else dst[o++] = s[i];
    } else dst[o++] = s[i];
  }
  dst[o] = 0;
}

static void parse_toc(const char *x, size_t n) {
  int stack[256], sp = 0, in_ea = 0, in_data = 0;
  const char *p = x, *end = x + n;
  while (p < end && (p = memchr(p, '<', end - p))) {
    const char *gt = memchr(p, '>', end - p);
    if (!gt) break;
    const char *tag = p + 1; size_t tl = gt - tag;
    p = gt + 1;
    if (tl == 0 || *tag == '?' || *tag == '!') continue;
    int top = sp ? stack[sp - 1] : -1;
    int selfclose = tag[tl - 1] == '/';
    if (!strncmp(tag, "file", 4) && (tag[4] == ' ' || tag[4] == '>')) {
      if (nents == capents) { capents = capents ? capents * 2 : 64; ents = xrealloc(ents, capents * sizeof *ents); }
      memset(&ents[nents], 0, sizeof ents[0]);
      ents[nents].parent = top; ents[nents].name = strdup("");
      if (sp < 256) stack[sp++] = nents;
      nents++;
    } else if (tl == 5 && !strncmp(tag, "/file", 5)) { if (sp) sp--; }
    else if (!strncmp(tag, "ea", 2) && (tag[2] == ' ' || tag[2] == '>')) in_ea = 1;
    else if (tl == 3 && !strncmp(tag, "/ea", 3)) in_ea = 0;
    else if (tl == 4 && !strncmp(tag, "data", 4)) { in_data = 1; if (top >= 0) ents[top].has_data = 1; }
    else if (tl == 5 && !strncmp(tag, "/data", 5)) in_data = 0;
    else if (!strncmp(tag, "encoding", 8) && in_data && !in_ea && top >= 0) {
      const char *st = strstr(tag, "style=\"");
      if (st && st < gt) {
        st += 7;
        if (!strncmp(st, "application/octet-stream", 24)) ents[top].enc = 0;
        else if (!strncmp(st, "application/x-gzip", 18) || !strncmp(st, "application/zlib", 16)) ents[top].enc = 1;
        else ents[top].enc = 2;
      }
    } else if (!selfclose && top >= 0 && *tag != '/') {
      /* Textknoten: <name>, <type>, <offset>, <length>, <size> */
      const char *tx = p, *lt = memchr(tx, '<', end - tx);
      if (!lt) break;
      size_t txl = lt - tx;
      if (tl == 4 && !strncmp(tag, "name", 4) && !in_ea) {
        char buf[1024]; xml_text(buf, sizeof buf, tx, txl);
        free(ents[top].name); ents[top].name = strdup(buf);
      } else if (tl == 4 && !strncmp(tag, "type", 4) && !in_ea) ents[top].dir = txl == 9 && !strncmp(tx, "directory", 9);
      else if (in_data && !in_ea) {
        char buf[32]; size_t m = txl < 31 ? txl : 31; memcpy(buf, tx, m); buf[m] = 0;
        if (tl == 6 && !strncmp(tag, "offset", 6)) ents[top].off = strtoull(buf, NULL, 10);
        else if (tl == 6 && !strncmp(tag, "length", 6)) ents[top].len = strtoull(buf, NULL, 10);
        else if (tl == 4 && !strncmp(tag, "size", 4)) ents[top].size = strtoull(buf, NULL, 10);
      }
    }
  }
}

static void pread_full(void *buf, size_t n, uint64_t off) {
  unsigned char *b = buf;
  while (n) {
    ssize_t r = pread(pkgfd, b, n, (off_t)off);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) die("Paket zu kurz oder Lesefehler bei Offset %llu", (unsigned long long)off);
    b += r; n -= r; off += r;
  }
}

/* Eintrag als Strom aus dem Paket lesen, xar-Kompression (falls vorhanden) entfernen */
static int ent_stream(const struct ent *e, sinkfn out, void *ctx) {
  unsigned char *buf = xmalloc(CHUNK);
  struct zs z; int rc = 0;
  if (e->enc == 2) { fprintf(stderr, "xar-Kodierung nicht unterstützt\n"); free(buf); return -1; }
  if (e->enc == 1) zs_init(&z, out, ctx);
  for (uint64_t done = 0; done < e->len && rc == 0;) {
    size_t n = e->len - done < CHUNK ? (size_t)(e->len - done) : CHUNK;
    pread_full(buf, n, heap + e->off + done);
    done += n;
    rc = e->enc == 1 ? zs_feed(&z, buf, n) : out(ctx, buf, n);
  }
  if (e->enc == 1) zs_free(&z);
  free(buf);
  return rc;
}

struct mem { char *p; size_t n, cap; };
static int mem_sink(void *ctx, const unsigned char *p, size_t n) {
  struct mem *m = ctx;
  if (m->n + n + 1 > m->cap) { m->cap = (m->n + n + 1) * 2; m->p = xrealloc(m->p, m->cap); }
  memcpy(m->p + m->n, p, n); m->n += n; m->p[m->n] = 0;
  return 0;
}

/* ---------- cpio -> Dateisystem ---------- */

static uint64_t g_appledouble, g_files, g_bytes, g_dirs, g_links, g_next_report = 100ull << 20;
static FILE *flist;           /* Dateiliste für die Quittung */

struct dirrec { char *path; mode_t mode; time_t mtime; };
static struct dirrec *dirs; static size_t ndirs, capdirs;

struct hl { uint64_t dev, ino; char *data_path; char **empties; int ne, left; struct hl *next; };
static struct hl *hltab[65536];

static char prefix[PATH_MAX];  /* Zielwurzel + install-location */

enum { S_HDR1, S_HDR2, S_NAME, S_DATA, S_SKIP };
struct cp {
  int st, after_skip, fmt;      /* fmt: 0 odc, 1 newc */
  unsigned char hdr[128]; size_t need, have;
  char *name; size_t namecap;
  uint64_t dev, ino, mode, uid, gid, nlink, mtime, namesize, filesize, remain;
  char *path; int fd; int skipent; char linkbuf[4100]; size_t linklen;
  int trailer;
};

static uint64_t parse_num(const unsigned char *p, int len, int base) {
  uint64_t v = 0;
  for (int i = 0; i < len; i++) {
    int c = p[i], d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else die("cpio: ungültige Ziffer im Kopf");
    v = v * base + d;
  }
  return v;
}

static void mkdirs(char *path) {   /* mkdir -p für alles oberhalb von path */
  for (char *s = path + 1; (s = strchr(s, '/')); s++) {
    *s = 0;
    if (mkdir(path, 0755) < 0 && errno != EEXIST) { int e = errno; *s = '/'; die("mkdir %s: %s", path, strerror(e)); }
    *s = '/';
  }
}

static int safe_name(const char *n) {
  for (const char *p = n; *p;) {
    const char *e = strchr(p, '/'); size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l == 2 && p[0] == '.' && p[1] == '.') return 0;
    if (!e) break; p = e + 1;
  }
  return 1;
}

static void set_times(const char *path, int fd, time_t mt, int nofollow) {
  struct timespec ts[2] = {{mt, 0}, {mt, 0}};
  if (fd >= 0) futimens(fd, ts);
  else utimensat(AT_FDCWD, path, ts, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
}

static struct hl *hl_find(uint64_t dev, uint64_t ino, int create, int nlink) {
  unsigned h = (unsigned)((ino * 2654435761u) ^ dev) & 0xffff;
  for (struct hl *g = hltab[h]; g; g = g->next) if (g->ino == ino && g->dev == dev) return g;
  if (!create) return NULL;
  struct hl *g = xmalloc(sizeof *g); memset(g, 0, sizeof *g);
  g->dev = dev; g->ino = ino; g->left = nlink; g->next = hltab[h]; hltab[h] = g;
  return g;
}

static void record(const char *rel, mode_t mode, uint64_t size) {
  g_files++;
  if (flist) fprintf(flist, "%s\t%o\t%llu\n", rel, (unsigned)mode, (unsigned long long)size);
  if (opt_verbose || opt_list) printf("%06o %llu %s\n", (unsigned)mode, (unsigned long long)size, rel);
}

static const char *relname(struct cp *c) { return c->name[0] ? c->name : "."; }

static void cp_entry_begin(struct cp *c) {      /* Name steht in c->name, Modus etc. in c */
  c->fd = -1; c->skipent = 0; c->linklen = 0;
  char *nm = c->name;
  while (nm[0] == '.' && nm[1] == '/') nm += 2;
  if (!strcmp(nm, ".")) nm = nm + 1;                 /* "." = Installationsort selbst */
  if (nm != c->name) memmove(c->name, nm, strlen(nm) + 1);
  size_t l = strlen(c->name);
  while (l > 1 && c->name[l - 1] == '/') c->name[--l] = 0;
  if (!safe_name(c->name)) { fprintf(stderr, "Pfad mit .. übersprungen: %s\n", c->name); c->skipent = 1; return; }
  mode_t fmt = c->mode & S_IFMT;
  if (fmt == S_IFREG && !opt_keep_ad) {          /* AppleDouble (._name) trägt xattrs/Resource Forks von name */
    const char *bn = strrchr(c->name, '/'); bn = bn ? bn + 1 : c->name;
    if (bn[0] == '.' && bn[1] == '_' && bn[2]) { g_appledouble++; c->skipent = 1; return; }
  }
  if (opt_list) {
    if (fmt == S_IFREG || fmt == S_IFDIR || fmt == S_IFLNK) record(relname(c), c->mode & 07777 | (fmt == S_IFDIR ? 040000 : fmt == S_IFLNK ? 0120000 : 0100000), fmt == S_IFREG ? c->filesize : 0);
    c->skipent = fmt != S_IFLNK; return;             /* Symlink-Ziel überspringt cp_entry_end */
  }
  free(c->path);
  size_t pl = strlen(prefix) + 1 + strlen(c->name) + 1;
  c->path = xmalloc(pl);
  snprintf(c->path, pl, "%s%s%s", prefix, c->name[0] ? "/" : "", c->name);
  if (fmt == S_IFDIR) {
    struct stat st;
    if (stat(c->path, &st) == 0 && S_ISDIR(st.st_mode)) { c->skipent = 1; return; }   /* bleibt wie es ist */
    if (mkdir(c->path, 0700) < 0) {
      if (errno == ENOENT) { mkdirs(c->path); if (mkdir(c->path, 0700) < 0 && errno != EEXIST) die("mkdir %s: %s", c->path, strerror(errno)); }
      else if (errno != EEXIST) die("mkdir %s: %s", c->path, strerror(errno));
    }
    if (is_root) lchown(c->path, 0, 0);
    if (ndirs == capdirs) { capdirs = capdirs ? capdirs * 2 : 1024; dirs = xrealloc(dirs, capdirs * sizeof *dirs); }
    dirs[ndirs++] = (struct dirrec){strdup(c->path), c->mode & 07777, (time_t)c->mtime};
    record(relname(c), (c->mode & 07777) | 040000, 0);
    g_dirs++; c->skipent = 1; return;
  }
  if (fmt == S_IFLNK) return;                         /* Ziel kommt als Daten */
  if (fmt != S_IFREG) { c->skipent = 1; return; }     /* Geräte, Sockets, FIFOs: keine in Paketen */
  if (c->nlink > 1) {
    struct hl *g = hl_find(c->dev, c->ino, 1, (int)c->nlink);
    if (c->filesize == 0 && g->data_path) {
      unlink(c->path);
      if (link(g->data_path, c->path) < 0 && errno == ENOENT) { mkdirs(c->path); link(g->data_path, c->path); }
      record(relname(c), c->mode & 07777 | 0100000, 0); g_links++; c->skipent = 1; return;
    }
  }
  unlink(c->path);
  c->fd = open(c->path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (c->fd < 0 && errno == ENOENT) { mkdirs(c->path); c->fd = open(c->path, O_WRONLY | O_CREAT | O_EXCL, 0600); }
  if (c->fd < 0) die("%s: %s", c->path, strerror(errno));
}

static void cp_entry_end(struct cp *c) {
  mode_t fmt = c->mode & S_IFMT;
  if (c->fd >= 0) {
    if (is_root) fchown(c->fd, 0, 0);
    fchmod(c->fd, c->mode & 07777);
    set_times(NULL, c->fd, (time_t)c->mtime, 0);
    close(c->fd); c->fd = -1;
    record(relname(c), (c->mode & 07777) | 0100000, c->filesize);
    if (c->nlink > 1) {
      struct hl *g = hl_find(c->dev, c->ino, 1, (int)c->nlink);
      if (c->filesize > 0) {
        free(g->data_path); g->data_path = strdup(c->path);
        for (int i = 0; i < g->ne; i++) {       /* früher leer angelegte Verweise auf die Datei mit Daten umlenken */
          unlink(g->empties[i]);
          if (link(g->data_path, g->empties[i]) < 0) fprintf(stderr, "link %s: %s\n", g->empties[i], strerror(errno));
          free(g->empties[i]);
        }
        free(g->empties); g->empties = NULL; g->ne = 0;
      } else {
        g->empties = xrealloc(g->empties, (g->ne + 1) * sizeof *g->empties);
        g->empties[g->ne++] = strdup(c->path);
        if (!g->data_path && g->ne == 1) { /* erste leere Datei dient bis zur Datenfassung als Platzhalter */ }
      }
    }
  } else if (fmt == S_IFLNK && !c->skipent) {
    c->linkbuf[c->linklen] = 0;
    if (opt_list) { return; }
    unlink(c->path);
    if (symlink(c->linkbuf, c->path) < 0 && errno == ENOENT) { mkdirs(c->path); symlink(c->linkbuf, c->path); }
    if (is_root) lchown(c->path, 0, 0);
    set_times(c->path, -1, (time_t)c->mtime, 1);
    record(relname(c), 0120000 | (c->mode & 07777), 0);
  }
}

static int cp_data_chunk(struct cp *c, const unsigned char *p, size_t n) {
  mode_t fmt = c->mode & S_IFMT;
  if (fmt == S_IFLNK) {
    if (c->linklen + n < sizeof c->linkbuf - 1) { memcpy(c->linkbuf + c->linklen, p, n); c->linklen += n; }
  } else if (c->fd >= 0) {
    while (n) {
      ssize_t w = write(c->fd, p, n);
      if (w < 0) { if (errno == EINTR) continue; fprintf(stderr, "Schreibfehler %s: %s\n", c->path, strerror(errno)); return -1; }
      p += w; n -= w;
    }
  }
  return 0;
}

static int cp_feed(void *ctx, const unsigned char *p, size_t n) {
  struct cp *c = ctx;
  while (n > 0 && !c->trailer) {
    switch (c->st) {
    case S_HDR1:                                  /* 6 Byte Magic */
    case S_HDR2: {
      size_t take = c->need - c->have; if (take > n) take = n;
      memcpy(c->hdr + c->have, p, take); c->have += take; p += take; n -= take;
      if (c->have < c->need) break;
      if (c->st == S_HDR1) {
        if (!memcmp(c->hdr, "070707", 6)) { c->fmt = 0; c->need = 76; }
        else if (!memcmp(c->hdr, "070701", 6) || !memcmp(c->hdr, "070702", 6)) { c->fmt = 1; c->need = 110; }
        else { fprintf(stderr, "cpio: unbekannte Magic %.6s\n", c->hdr); return -1; }
        c->st = S_HDR2; break;
      }
      const unsigned char *h = c->hdr;
      if (c->fmt == 0) {
        c->dev = parse_num(h + 6, 6, 8); c->ino = parse_num(h + 12, 6, 8); c->mode = parse_num(h + 18, 6, 8);
        c->uid = parse_num(h + 24, 6, 8); c->gid = parse_num(h + 30, 6, 8); c->nlink = parse_num(h + 36, 6, 8);
        c->mtime = parse_num(h + 48, 11, 8); c->namesize = parse_num(h + 59, 6, 8); c->filesize = parse_num(h + 65, 11, 8);
      } else {
        c->ino = parse_num(h + 6, 8, 16); c->mode = parse_num(h + 14, 8, 16); c->uid = parse_num(h + 22, 8, 16);
        c->gid = parse_num(h + 30, 8, 16); c->nlink = parse_num(h + 38, 8, 16); c->mtime = parse_num(h + 46, 8, 16);
        c->filesize = parse_num(h + 54, 8, 16);
        c->dev = (parse_num(h + 62, 8, 16) << 20) ^ parse_num(h + 70, 8, 16);
        c->namesize = parse_num(h + 94, 8, 16);
      }
      if (c->namesize == 0 || c->namesize > 65536) { fprintf(stderr, "cpio: Namenslänge %llu\n", (unsigned long long)c->namesize); return -1; }
      if (c->namesize > c->namecap) { c->namecap = c->namesize + 64; c->name = xrealloc(c->name, c->namecap); }
      c->st = S_NAME; c->need = c->namesize; c->have = 0;
      break;
    }
    case S_NAME: {
      size_t take = c->need - c->have; if (take > n) take = n;
      memcpy(c->name + c->have, p, take); c->have += take; p += take; n -= take;
      if (c->have < c->need) break;
      c->name[c->namesize - 1] = 0;
      if (!strcmp(c->name, "TRAILER!!!")) { c->trailer = 1; break; }
      cp_entry_begin(c);
      c->remain = c->filesize;
      size_t pad = c->fmt ? (4 - ((110 + c->namesize) & 3)) & 3 : 0;
      if (pad) { c->st = S_SKIP; c->need = pad; c->after_skip = S_DATA; }
      else c->st = S_DATA;
      c->have = 0;
      if (c->st == S_DATA && c->remain == 0) { cp_entry_end(c); c->st = S_HDR1; c->need = 6; c->have = 0; }
      break;
    }
    case S_DATA: {
      size_t take = c->remain < n ? (size_t)c->remain : n;
      if (!c->skipent && cp_data_chunk(c, p, take) < 0) return -1;
      g_bytes += take;
      p += take; n -= take; c->remain -= take;
      if (c->remain == 0) {
        if (!c->skipent) cp_entry_end(c);
        size_t pad = c->fmt ? (4 - (c->filesize & 3)) & 3 : 0;
        if (pad) { c->st = S_SKIP; c->need = pad; c->after_skip = S_HDR1; }
        else c->st = S_HDR1, c->need = 6;
        c->have = 0;
      }
      break;
    }
    case S_SKIP: {
      size_t take = c->need - c->have; if (take > n) take = n;
      c->have += take; p += take; n -= take;
      if (c->have < c->need) break;
      c->have = 0;
      if (c->after_skip == S_DATA) {
        c->st = S_DATA;
        if (c->remain == 0) { if (!c->skipent) cp_entry_end(c); c->st = S_HDR1; c->need = 6; }
      } else { c->st = S_HDR1; c->need = 6; }
      break;
    }
    }
    if (g_bytes >= g_next_report) { fprintf(stderr, "  %llu MB, %llu Dateien\r", (unsigned long long)(g_bytes >> 20), (unsigned long long)g_files); g_next_report += 100ull << 20; }
  }
  return 0;
}

static int install_payload(const struct ent *e) {
  struct cp *c = xmalloc(sizeof *c); memset(c, 0, sizeof *c);
  c->st = S_HDR1; c->need = 6; c->fd = -1;
  struct det d; memset(&d, 0, sizeof d); d.out = cp_feed; d.octx = c;
  int rc = ent_stream(e, det_feed, &d);
  if (d.gz) zs_free(&d.z);
  int incomplete = !c->trailer;
  if (rc == 0 && incomplete) { fprintf(stderr, "Payload endet ohne cpio-Trailer\n"); rc = -1; }
  /* Verzeichnisse zuletzt: Modi und Zeiten, innerste zuerst */
  for (size_t i = ndirs; i-- > 0;) {
    chmod(dirs[i].path, dirs[i].mode);
    set_times(dirs[i].path, -1, dirs[i].mtime, 0);
    free(dirs[i].path);
  }
  ndirs = 0;
  free(c->name); free(c->path); free(c);
  return rc;
}

/* ---------- Quittung ---------- */

static char *attr(const char *tag, const char *key) {      /* Attribut aus <pkg-info …> */
  char pat[64]; snprintf(pat, sizeof pat, " %s=\"", key);
  const char *p = strstr(tag, pat); if (!p) return NULL;
  p += strlen(pat);
  const char *q = strchr(p, '"'); if (!q) return NULL;
  char *out = xmalloc(q - p + 8); xml_text(out, q - p + 8, p, q - p);
  return out;
}
static void plist_str(FILE *f, const char *s) {
  for (; *s; s++) {
    if (*s == '&') fputs("&amp;", f); else if (*s == '<') fputs("&lt;", f); else if (*s == '>') fputs("&gt;", f); else fputc(*s, f);
  }
}

static int find_ent(const char *path) {
  for (int i = 0; i < nents; i++) {
    char *p = ent_path(i); int m = !strcmp(p, path); free(p);
    if (m) return i;
  }
  return -1;
}

int main(int argc, char **argv) {
  const char *pkg = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-target") && i + 1 < argc) snprintf(target, sizeof target, "%s", argv[++i]);
    else if (!strcmp(argv[i], "-n")) opt_list = 1;
    else if (!strcmp(argv[i], "-v")) opt_verbose = 1;
    else if (!strcmp(argv[i], "-appledouble")) opt_keep_ad = 1;
    else if (argv[i][0] == '-' && argv[i][1]) die("unbekannte Option %s", argv[i]);
    else pkg = argv[i];
  }
  if (!pkg) { fprintf(stderr, "Aufruf: clokeos-pkginstall [-target /] [-n] [-v] [-appledouble] <paket.pkg>\n"); return 2; }
  size_t tl = strlen(target); while (tl > 0 && target[tl - 1] == '/') target[--tl] = 0;   /* "/" wird "" */
  is_root = geteuid() == 0;

  pkgfd = open(pkg, O_RDONLY);
  if (pkgfd < 0) die("%s: %s", pkg, strerror(errno));
  unsigned char h[28];
  pread_full(h, 28, 0);
  if (memcmp(h, "xar!", 4)) die("%s ist kein xar-Paket", pkg);
  uint32_t hsz = h[4] << 8 | h[5];
  uint64_t clen = 0, ulen = 0;
  for (int i = 0; i < 8; i++) { clen = clen << 8 | h[8 + i]; ulen = ulen << 8 | h[16 + i]; }
  if (hsz < 28 || clen > (1u << 30) || ulen > (1u << 30)) die("xar-Kopf unplausibel");
  heap = hsz + clen;
  unsigned char *ctoc = xmalloc(clen);
  pread_full(ctoc, clen, hsz);
  struct mem toc = {0};
  struct zs z; zs_init(&z, mem_sink, &toc);
  if (zs_feed(&z, ctoc, clen) < 0) die("TOC lässt sich nicht entpacken");
  zs_free(&z); free(ctoc);
  parse_toc(toc.p, toc.n);

  int npay = 0;
  for (int i = 0; i < nents; i++) {
    if (ents[i].dir || !ents[i].has_data || strcmp(ents[i].name, "Payload")) continue;
    npay++;
    char *pp = ent_path(i);
    char *slash = strrchr(pp, '/');
    char infopath[2048];
    if (slash) { *slash = 0; snprintf(infopath, sizeof infopath, "%s/PackageInfo", pp); *slash = '/'; }
    else snprintf(infopath, sizeof infopath, "PackageInfo");
    char *id = NULL, *ver = NULL, *loc = NULL;
    int ii = find_ent(infopath);
    if (ii >= 0) {
      struct mem m = {0};
      if (ent_stream(&ents[ii], mem_sink, &m) == 0 && m.p) {
        char *t = strstr(m.p, "<pkg-info"); char *te = t ? strchr(t, '>') : NULL;
        if (te) { *te = 0; id = attr(t, "identifier"); ver = attr(t, "version"); loc = attr(t, "install-location"); }
      }
      free(m.p);
    }
    if (!loc) loc = strdup("/");
    snprintf(prefix, sizeof prefix, "%s%s", target, strcmp(loc, "/") ? loc : "");
    size_t pl = strlen(prefix); while (pl > 0 && prefix[pl - 1] == '/') prefix[--pl] = 0;
    fprintf(stderr, "Paket %s %s -> %s (%llu MB Payload)\n", id ? id : "(ohne Kennung)", ver ? ver : "", pl ? prefix : "/", (unsigned long long)(ents[i].size >> 20));
    if (!opt_list && pl) { char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s/x", prefix); mkdirs(tmp); }   /* Installationsort anlegen */

    char recpath[PATH_MAX]; flist = NULL;
    if (!opt_list && id) {
      snprintf(recpath, sizeof recpath, "%s/var/db/receipts/%s.files", target, id);
      char *tmp = strdup(recpath); mkdirs(tmp); free(tmp);
      flist = fopen(recpath, "w");
    }
    g_files = g_bytes = g_dirs = g_links = g_appledouble = 0;
    int rc = install_payload(&ents[i]);
    if (flist) fclose(flist);
    fprintf(stderr, "\n%llu Einträge (%llu Verzeichnisse, %llu Hardlinks, %llu AppleDouble-Dateien übersprungen), %llu MB\n", (unsigned long long)g_files, (unsigned long long)g_dirs, (unsigned long long)g_links, (unsigned long long)g_appledouble, (unsigned long long)(g_bytes >> 20));
    if (rc < 0) die("Installation von %s fehlgeschlagen", id ? id : pp);
    if (!opt_list && id) {
      snprintf(recpath, sizeof recpath, "%s/var/db/receipts/%s.plist", target, id);
      FILE *f = fopen(recpath, "w");
      if (!f) die("%s: %s", recpath, strerror(errno));
      char date[40]; time_t now = time(NULL); strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
      const char *base = strrchr(pkg, '/'); base = base ? base + 1 : pkg;
      fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n", f);
      fputs("\t<key>InstallDate</key>\n\t<date>", f); fputs(date, f); fputs("</date>\n", f);
      fputs("\t<key>InstallPrefixPath</key>\n\t<string>", f); plist_str(f, loc); fputs("</string>\n", f);
      fputs("\t<key>InstallProcessName</key>\n\t<string>clokeos-pkginstall</string>\n", f);
      fputs("\t<key>PackageFileName</key>\n\t<string>", f); plist_str(f, base); fputs("</string>\n", f);
      fputs("\t<key>PackageIdentifier</key>\n\t<string>", f); plist_str(f, id); fputs("</string>\n", f);
      fputs("\t<key>PackageVersion</key>\n\t<string>", f); plist_str(f, ver ? ver : ""); fputs("</string>\n</dict>\n</plist>\n", f);
      fclose(f);
    }
    free(pp); free(id); free(ver); free(loc);
  }
  if (!npay) die("keine Payload im Paket");
  return 0;
}
