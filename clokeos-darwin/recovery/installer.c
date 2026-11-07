/*
 * clokeos-installer: installiert ClokeOS aus dem Internet, wie macOS Recovery.
 *
 *   clokeos-installer disks                      Festplatten mit Größe und Partitionen
 *   clokeos-installer install <diskN> <url>      ClokeOS aus <url>/clokeos-<arch>.txt installieren
 *   clokeos-installer erase <diskN>              Festplatte löschen (leere GPT)
 *   clokeos-installer layout <diskN>             Partitionen und freie Bereiche (für das Festplattendienstprogramm)
 *   clokeos-installer add <diskN> <lba> <bytes> <fat32|ext4|none> <name>   Partition anlegen und formatieren
 *   clokeos-installer delete <diskN> <slot>      Partition entfernen
 *   clokeos-installer format <diskN> <slot> <fat32|ext4> <name>   Partition löschen (neues Dateisystem)
 *   clokeos-installer resize <diskN> <slot> <bytes>   Partition ändern (ext4 samt Dateisystem)
 *   clokeos-installer bootnext recovery|system   nächsten Start in Wiederherstellung oder System
 *   clokeos-installer stick                      Offline-USB-Stick mit Abbildern? ("STICK <version>")
 *   clokeos-installer stick-write <name> <datei>  \ClokeOS\<name> auf dem Stick an Ort und Stelle überschreiben
 *   <url> stick:                                 die Abbilder von diesem Stick lesen (Abschnitt unten)
 *
 * Ein Release (recovery/release.sh) besteht aus einer Beschreibung clokeos-<arch>.txt und drei mit
 * xz gepackten Partitionsabbildern: EFI-Partition (Loader, Kernel, Boot-Argumente), System (ext4
 * mit dem vollen Desktop) und Wiederherstellung (ext4, das abgespeckte System, in dem dieses
 * Programm läuft). install legt auf der Festplatte eine GPT mit diesen drei Partitionen an und
 * schreibt die Abbilder beim Herunterladen direkt an ihre Stelle, ohne Zwischendatei: libcurl
 * lädt, liblzma entpackt, SHA-256 prüft das Geladene gegen die Beschreibung. Läuft das Programm
 * selbst von der Wiederherstellungspartition dieser Festplatte, bleiben GPT und
 * Wiederherstellung, wie bei macOS; nur EFI-Partition und System werden neu geschrieben.
 *
 * Für CloakServer schreibt es zeilenweise auf stdout:
 *   DISK <name> <bytes> <merkmale> <bezeichnung>    merkmale: r Startplatte, i Installationsmedium,
 *   PART <name> <index> <bytes> <name in der GPT>              c ClokeOS darauf, - nichts davon
 *   STEP <text> | PROGRESS <fertig> <gesamt> | ERROR <text> | DONE <version>
 */
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <lzma.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#define CA_FILE "/etc/ssl/cert.pem"
#define CHUNK (1 << 20) /* in diesen Schritten auf die Platte, ein Vielfaches jeder Blockgröße */

static void out(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
}

/* Sprache wie in CloakServer (cloakserver/l10n.c): Texte englisch, Deutsch aus derselben Tabelle */
static const char *const DE[][2] = {
#include "../cloakserver/strings-de.h"
};
static bool lang_de;

static void lang_init(void) {
  const char *env = getenv("CLOKE_LANG");
  char buf[16] = "";
  if (env && *env) snprintf(buf, sizeof buf, "%s", env);
  else {
    FILE *f = fopen("/etc/clokeos-language", "r");
    if (f) {
      if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
      fclose(f);
    }
  }
  lang_de = !strncmp(buf, "de", 2);
}

static const char *L(const char *en) {
  if (lang_de)
    for (size_t i = 0; i < sizeof DE / sizeof DE[0]; i++)
      if (!strcmp(DE[i][0], en)) return DE[i][1];
  return en;
}

static void fail(const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, L(fmt), ap);
  va_end(ap);
  out("ERROR %s", msg);
  exit(1);
}

/* ---- SHA-256 (FIPS 180-4) ---------------------------------------------------------------- */

typedef struct {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t n;
} Sha256;

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
  0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
  0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
  0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
  0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const uint8_t *p) {
  uint32_t w[64], a[8];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  memcpy(a, s->h, sizeof a);
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
    uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
    memmove(a + 1, a, 7 * sizeof a[0]);
    a[4] += t1;
    a[0] = t1 + t2;
  }
  for (int i = 0; i < 8; i++) s->h[i] += a[i];
}

static void sha_init(Sha256 *s) {
  static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, h0, sizeof h0);
  s->len = 0;
  s->n = 0;
}

static void sha_update(Sha256 *s, const uint8_t *p, size_t n) {
  s->len += n;
  while (n) {
    size_t k = 64 - s->n < n ? 64 - s->n : n;
    memcpy(s->buf + s->n, p, k);
    s->n += k, p += k, n -= k;
    if (s->n == 64) {
      sha_block(s, s->buf);
      s->n = 0;
    }
  }
}

static void sha_hex(Sha256 *s, char out[65]) {
  uint64_t bits = s->len * 8;
  uint8_t pad = 0x80, zero = 0, len[8];
  sha_update(s, &pad, 1);
  while (s->n != 56) sha_update(s, &zero, 1);
  for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha_update(s, len, 8);
  for (int i = 0; i < 8; i++) sprintf(out + 8 * i, "%08x", s->h[i]);
}

/* ---- Festplatten ------------------------------------------------------------------------- */

typedef struct {
  char name[32];   /* disk1 */
  uint64_t bytes;
  uint32_t bsize;
  bool root, installer, clokeos;
  int nparts;
  struct { uint64_t first, count; char name[40]; uint8_t type[16], guid[16]; int slot; } part[16];
} Disk;

static const uint8_t GUID_ESP[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
static const uint8_t GUID_LINUX[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};

static uint32_t crc32(const void *data, size_t n) {
  const uint8_t *p = data;
  uint32_t c = 0xffffffff;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xedb88320 : c >> 1;
  }
  return ~c;
}

static int open_raw(const char *name, int flags) {
  char path[64];
  snprintf(path, sizeof path, "/dev/r%s", name);
  return open(path, flags);
}

/* Partitionsliste aus der GPT (Blockgröße 512 oder 4096) */
static void read_gpt(int fd, Disk *d) {
  uint8_t *hdr = malloc(d->bsize);
  if (!hdr) return;
  if (pread(fd, hdr, d->bsize, d->bsize) != (ssize_t)d->bsize || memcmp(hdr, "EFI PART", 8)) {
    free(hdr);
    return;
  }
  uint64_t lba;
  uint32_t count, esize;
  memcpy(&lba, hdr + 72, 8);
  memcpy(&count, hdr + 80, 4);
  memcpy(&esize, hdr + 84, 4);
  free(hdr);
  if (esize < 128 || count > 256) return;
  size_t len = (size_t)count * esize;
  len = (len + d->bsize - 1) / d->bsize * d->bsize;
  uint8_t *tab = malloc(len);
  if (!tab || pread(fd, tab, len, (off_t)(lba * d->bsize)) != (ssize_t)len) {
    free(tab);
    return;
  }
  for (uint32_t i = 0; i < count && d->nparts < 16; i++) {
    uint8_t *e = tab + (size_t)i * esize;
    static const uint8_t zero[16];
    if (!memcmp(e, zero, 16)) continue;
    uint64_t first, last;
    memcpy(&first, e + 32, 8);
    memcpy(&last, e + 40, 8);
    typeof(d->part[0]) *p = &d->part[d->nparts++];
    p->slot = (int)i;
    p->first = first;
    p->count = last - first + 1;
    memcpy(p->type, e, 16);
    memcpy(p->guid, e + 16, 16);
    size_t k = 0;
    for (int c = 0; c < 36 && k + 1 < sizeof p->name; c++) { /* UTF-16LE, hier nur ASCII */
      uint16_t ch = e[56 + 2 * c] | e[57 + 2 * c] << 8;
      if (!ch) break;
      p->name[k++] = ch < 128 ? (char)ch : '?';
    }
    p->name[k] = 0;
    if (!strcmp(p->name, "ClokeOS Installer")) d->installer = true;
    if (!strncmp(p->name, "ClokeOS", 7)) d->clokeos = true;
  }
  free(tab);
}

static bool probe_disk(const char *name, Disk *d) {
  memset(d, 0, sizeof *d);
  snprintf(d->name, sizeof d->name, "%s", name);
  int fd = open_raw(name, O_RDONLY);
  if (fd < 0) return false;
  uint64_t blocks = 0;
  if (ioctl(fd, DKIOCGETBLOCKSIZE, &d->bsize) || ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) || !d->bsize) {
    close(fd);
    return false;
  }
  d->bytes = blocks * d->bsize;
  read_gpt(fd, d);
  close(fd);
  /* Die Platte, von der dieses System läuft: st_dev von / ist das Gerät der Wurzelpartition */
  struct stat rs, ps;
  if (!stat("/", &rs))
    for (int i = 1; i <= 16; i++) {
      char path[64];
      snprintf(path, sizeof path, "/dev/%ss%d", name, i);
      if (!stat(path, &ps) && ps.st_rdev == rs.st_dev) d->root = true;
    }
  return true;
}

/* Index (1, 2, …) der Partition, von der das System läuft, oder 0 */
static int root_part(const char *disk) {
  struct stat rs, ps;
  if (stat("/", &rs)) return 0;
  for (int i = 1; i <= 16; i++) {
    char path[64];
    snprintf(path, sizeof path, "/dev/%ss%d", disk, i);
    if (!stat(path, &ps) && ps.st_rdev == rs.st_dev) return i;
  }
  return 0;
}

/* Eingebundene Partitionen der Platte lösen, bevor darunter geschrieben wird (diskarbitrationd
 * bindet erkannte Dateisysteme ein; sonst schriebe der Kernel womöglich alte Puffer darüber) */
static void unmount_disk(const char *disk, int keep_part) {
  struct statfs *m;
  int n = getmntinfo(&m, MNT_NOWAIT);
  char prefix[40];
  snprintf(prefix, sizeof prefix, "/dev/%ss", disk);
  for (int i = 0; i < n; i++) {
    if (strncmp(m[i].f_mntfromname, prefix, strlen(prefix))) continue;
    if (keep_part && atoi(m[i].f_mntfromname + strlen(prefix)) == keep_part) continue;
    if (unmount(m[i].f_mntonname, MNT_FORCE)) fail("%s is still mounted: %s", m[i].f_mntonname, strerror(errno));
  }
}

static void cmd_disks(void) {
  for (int i = 0; i < 16; i++) {
    char name[16];
    snprintf(name, sizeof name, "disk%d", i);
    Disk d;
    if (!probe_disk(name, &d)) continue;
    char flags[8] = "";
    size_t f = 0;
    if (d.root) flags[f++] = 'r';
    if (d.installer) flags[f++] = 'i';
    if (d.clokeos) flags[f++] = 'c';
    if (!f) flags[f++] = '-';
    const char *label = L(d.installer ? "ClokeOS Installer" : d.clokeos ? "ClokeOS Disk" : "Disk");
    out("DISK %s %llu %s %s", d.name, (unsigned long long)d.bytes, flags, label);
    for (int k = 0; k < d.nparts; k++)
      out("PART %s %d %llu %s", d.name, k + 1, (unsigned long long)(d.part[k].count * d.bsize),
          d.part[k].name[0] ? d.part[k].name : "-");
  }
}

/* ---- GPT schreiben ----------------------------------------------------------------------- */

typedef struct {
  const uint8_t *type;
  const char *name;
  uint64_t first, count; /* in Blöcken */
  uint8_t guid[16];      /* Kennung der Partition, von write_gpt gewürfelt */
} NewPart;

/* Kennung als Text, wie IOMedia sie als UUID führt und der Loader sie in boot-uuid= erwartet */
static void guid_str(const uint8_t g[16], char out[37]) {
  snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", g[3], g[2], g[1], g[0],
           g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static void put16name(uint8_t *e, const char *name) {
  for (int c = 0; c < 36 && name[c]; c++) e[56 + 2 * c] = (uint8_t)name[c];
}

static void random_guid(uint8_t g[16]) {
  arc4random_buf(g, 16);
  g[7] = (g[7] & 0x0f) | 0x40; /* Version 4 */
  g[8] = (g[8] & 0x3f) | 0x80;
}

/* Schützende MBR, primäre GPT am Anfang, Sicherung am Ende; tab = die 128 Einträge zu je 128 Byte */
static void gpt_commit(int fd, const Disk *d, const uint8_t *tab_in, const uint8_t disk_guid[16]) {
  uint32_t bs = d->bsize;
  uint64_t last = d->bytes / bs - 1;
  uint32_t tab_blocks = (128 * 128 + bs - 1) / bs;
  uint8_t *tab = calloc(1, (size_t)tab_blocks * bs), *hdr = calloc(1, bs), *mbr = calloc(1, bs);
  if (!tab || !hdr || !mbr) fail("Out of memory");
  memcpy(tab, tab_in, 128 * 128);
  uint32_t tab_crc = crc32(tab, 128 * 128);

  mbr[446 + 4] = 0xee; /* eine Partition vom Typ GPT-Schutz über die ganze Platte */
  uint32_t one = 1, cnt = last > 0xffffffff ? 0xffffffff : (uint32_t)last;
  memcpy(mbr + 446 + 8, &one, 4);
  memcpy(mbr + 446 + 12, &cnt, 4);
  mbr[446 + 2] = 2;
  mbr[446 + 5] = mbr[446 + 6] = mbr[446 + 7] = 0xff;
  mbr[510] = 0x55, mbr[511] = 0xaa;

  for (int backup = 0; backup <= 1; backup++) {
    memset(hdr, 0, bs);
    memcpy(hdr, "EFI PART", 8);
    uint32_t rev = 0x00010000, hsize = 92, zero = 0, nent = 128, esize = 128;
    uint64_t my = backup ? last : 1, alt = backup ? 1 : last;
    uint64_t first_use = 2 + tab_blocks, last_use = last - 1 - tab_blocks;
    uint64_t tab_lba = backup ? last - tab_blocks : 2;
    memcpy(hdr + 8, &rev, 4);
    memcpy(hdr + 12, &hsize, 4);
    memcpy(hdr + 16, &zero, 4);
    memcpy(hdr + 24, &my, 8);
    memcpy(hdr + 32, &alt, 8);
    memcpy(hdr + 40, &first_use, 8);
    memcpy(hdr + 48, &last_use, 8);
    memcpy(hdr + 56, disk_guid, 16);
    memcpy(hdr + 72, &tab_lba, 8);
    memcpy(hdr + 80, &nent, 4);
    memcpy(hdr + 84, &esize, 4);
    memcpy(hdr + 88, &tab_crc, 4);
    uint32_t hcrc = crc32(hdr, 92);
    memcpy(hdr + 16, &hcrc, 4);
    if (pwrite(fd, tab, (size_t)tab_blocks * bs, (off_t)(tab_lba * bs)) != (ssize_t)((size_t)tab_blocks * bs) ||
        pwrite(fd, hdr, bs, (off_t)(my * bs)) != (ssize_t)bs)
      fail("Could not write the GPT: %s", strerror(errno));
  }
  if (pwrite(fd, mbr, bs, 0) != (ssize_t)bs) fail("Could not write the MBR: %s", strerror(errno));
  free(tab);
  free(hdr);
  free(mbr);
}

/* Neue GPT aus einer Liste von Partitionen (Kennungen werden gewürfelt) */
static void write_gpt(int fd, const Disk *d, NewPart *p, int n) {
  uint8_t tab[128 * 128] = {0}, disk_guid[16];
  for (int i = 0; i < n; i++) {
    uint8_t *e = tab + 128 * i;
    memcpy(e, p[i].type, 16);
    random_guid(p[i].guid);
    memcpy(e + 16, p[i].guid, 16);
    uint64_t f = p[i].first, l = p[i].first + p[i].count - 1;
    memcpy(e + 32, &f, 8);
    memcpy(e + 40, &l, 8);
    put16name(e, p[i].name);
  }
  random_guid(disk_guid);
  gpt_commit(fd, d, tab, disk_guid);
}

/* Während eines Eingriffs bindet clokeos-automount nichts ein (sonst hängt es die Partition gleich wieder
 * ein, und die ganze Platte lässt sich nicht mehr schreiben); die Sperre verschwindet beim Beenden */
#define DU_LOCK "/var/run/clokeos-diskutil.lock"

static void unlock_disks(void) { unlink(DU_LOCK); }

static void lock_disks(void) {
  int fd = open(DU_LOCK, O_CREAT | O_WRONLY, 0644);
  if (fd >= 0) close(fd);
  atexit(unlock_disks);
}

static void cmd_erase(const char *disk) {
  Disk d;
  if (!probe_disk(disk, &d)) fail("No disk %s", disk);
  if (d.root) fail("The system is running from this disk, so it can’t be erased.");
  lock_disks();
  unmount_disk(disk, 0);
  int fd = open_raw(disk, O_RDWR);
  if (fd < 0) fail("Could not open %s: %s", disk, strerror(errno));
  out("STEP %s", L("Erasing the disk"));
  uint8_t *z = calloc(1, CHUNK);
  /* alte Dateisysteme am Anfang unkenntlich machen, dann eine leere GPT */
  for (int i = 0; i < 8 && z; i++) (void)pwrite(fd, z, CHUNK, (off_t)i * CHUNK);
  free(z);
  write_gpt(fd, &d, NULL, 0);
  fsync(fd);
  close(fd);
  out("DONE gelöscht");
}

/* ---- Beschreibung des Releases ----------------------------------------------------------- */

typedef struct {
  char file[128], sha[65];
  uint64_t raw, xz;
} Asset;

typedef struct {
  char version[32], build[32], codename[64], arch[16];
  Asset esp, system, recovery;
} Manifest;

static size_t mem_write(char *p, size_t s, size_t n, void *arg) {
  char **buf = arg;
  size_t len = *buf ? strlen(*buf) : 0, add = s * n;
  if (len + add > 1 << 16) return 0;
  char *b = realloc(*buf, len + add + 1);
  if (!b) return 0;
  memcpy(b + len, p, add);
  b[len + add] = 0;
  *buf = b;
  return add;
}

static void curl_common(CURL *c, const char *url) {
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "ClokeOS-Installer");
  if (!access(CA_FILE, R_OK)) curl_easy_setopt(c, CURLOPT_CAINFO, CA_FILE);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
  if (strncmp(url, "file:", 5)) { /* steht ein Download eine Minute, Abbruch; vom Stick (file:) nie: */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L); /* dort bremst nur das Entpacken und Schreiben */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
  }
}

/* Gibt es <base>/<file>? Kurze HEAD-Anfrage, ohne abzubrechen */
static bool url_reachable(const char *base, const char *file) {
  char url[1024];
  snprintf(url, sizeof url, "%s/%s", base, file);
  CURL *c = curl_easy_init();
  if (!c) return false;
  curl_common(c, url);
  curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  bool ok = curl_easy_perform(c) == CURLE_OK;
  curl_easy_cleanup(c);
  return ok;
}

static bool parse_asset(const char *v, Asset *a) {
  unsigned long long raw, xz;
  return sscanf(v, "%127s %llu %llu %64s", a->file, &raw, &xz, a->sha) == 4 && (a->raw = raw, a->xz = xz, true);
}

static void parse_manifest(char *text, const char *arch, Manifest *m);
static bool is_stick(const char *base) { return !strcmp(base, "stick:"); }
static char *stick_text(const char *name);
static bool stick_has(const char *name);

static void fetch_manifest(const char *base, const char *arch, Manifest *m) {
  if (is_stick(base)) { /* Offline-USB-Stick: Abbilder unter \ClokeOS\Install auf seiner EFI-Partition */
    char name[64];
    snprintf(name, sizeof name, "clokeos-%s.txt", arch);
    char *text = stick_text(name);
    if (!text) fail("The ClokeOS files were not found on the USB stick (\\ClokeOS\\Install\\%s).", name);
    parse_manifest(text, arch, m);
    free(text);
    return;
  }
  char url[1024], *text = NULL, err[CURL_ERROR_SIZE] = "";
  snprintf(url, sizeof url, "%s/clokeos-%s.txt", base, arch);
  CURL *c = curl_easy_init();
  curl_common(c, url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &text);
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
  CURLcode rc = curl_easy_perform(c);
  long status = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(c);
  if (rc == CURLE_HTTP_RETURNED_ERROR && status == 404)
    fail("The ClokeOS server has no release for %s yet (%s).", arch, url);
  if (rc == CURLE_HTTP_RETURNED_ERROR)
    fail("The ClokeOS server answered with error %ld. Please try again later.", status);
  if (rc != CURLE_OK || !text)
    fail("Could not connect to the ClokeOS server (%s). Is the network connected?", err[0] ? err : curl_easy_strerror(rc));
  parse_manifest(text, arch, m);
  free(text);
}

static void parse_manifest(char *text, const char *arch, Manifest *m) {
  memset(m, 0, sizeof *m);
  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const char *k = line, *v = eq + 1;
    if (!strcmp(k, "version")) snprintf(m->version, sizeof m->version, "%s", v);
    else if (!strcmp(k, "build")) snprintf(m->build, sizeof m->build, "%s", v);
    else if (!strcmp(k, "codename")) snprintf(m->codename, sizeof m->codename, "%s", v);
    else if (!strcmp(k, "arch")) snprintf(m->arch, sizeof m->arch, "%s", v);
    else if (!strcmp(k, "esp")) parse_asset(v, &m->esp);
    else if (!strcmp(k, "system")) parse_asset(v, &m->system);
    else if (!strcmp(k, "recovery")) parse_asset(v, &m->recovery);
  }
  if (strcmp(m->arch, arch) || !m->esp.raw || !m->system.raw || !m->recovery.raw)
    fail("The release description is incomplete.");
}

/* ---- Herunterladen, entpacken, schreiben -------------------------------------------------- */

typedef struct {
  int fd;
  uint64_t off, limit, written;
  lzma_stream z;
  Sha256 sha;
  uint8_t *buf;
  size_t fill;
  uint64_t done_before, total; /* Fortschritt über alle Downloads */
  uint64_t got;
  double last;
  bool bad;
  int werr; /* errno des letzten fehlgeschlagenen Schreibens */
} Writer;

static double now(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Schreiben mit Wiederholung, danach in kleineren Stücken: manche NVMe-Treiber lehnen große Aufträge
 * mit "Input/output error" ab, und USB-Sticks machen kurze Aussetzer */
static bool pwrite_robust(int fd, const uint8_t *buf, size_t len, off_t off) {
  for (int t = 0; t < 3; t++) {
    if (pwrite(fd, buf, len, off) == (ssize_t)len) return true;
    fprintf(stderr, "write of %zu bytes at %lld failed (%s), retrying\n", len, (long long)off, strerror(errno));
    usleep(200000);
  }
  static const size_t pieces[] = {131072, 16384, 4096};
  for (size_t k = 0; k < sizeof pieces / sizeof pieces[0]; k++) {
    bool ok = true;
    for (size_t o = 0; o < len && ok; o += pieces[k]) {
      size_t n = len - o < pieces[k] ? len - o : pieces[k];
      ok = pwrite(fd, buf + o, n, off + (off_t)o) == (ssize_t)n || pwrite(fd, buf + o, n, off + (off_t)o) == (ssize_t)n;
    }
    if (ok) return true;
  }
  return false;
}

static bool flush_buf(Writer *w, bool final) {
  if (!w->fill) return true;
  size_t len = w->fill;
  if (final) {
    len = (w->fill + 4095) / 4096 * 4096; /* Rest auf ganze Blöcke mit Nullen auffüllen */
    memset(w->buf + w->fill, 0, len - w->fill);
  }
  if (w->written + len > w->limit) return false;
  if (!pwrite_robust(w->fd, w->buf, len, (off_t)(w->off + w->written))) {
    w->werr = errno;
    return false;
  }
  w->written += len;
  w->fill = 0;
  return true;
}

static size_t on_data(char *p, size_t s, size_t n, void *arg) {
  Writer *w = arg;
  size_t len = s * n;
  sha_update(&w->sha, (const uint8_t *)p, len);
  w->got += len;
  w->z.next_in = (const uint8_t *)p;
  w->z.avail_in = len;
  while (w->z.avail_in) {
    w->z.next_out = w->buf + w->fill;
    w->z.avail_out = CHUNK - w->fill;
    lzma_ret r = lzma_code(&w->z, LZMA_RUN);
    w->fill = CHUNK - w->z.avail_out;
    if (r != LZMA_OK && r != LZMA_STREAM_END) {
      w->bad = true;
      return 0;
    }
    if (w->fill == CHUNK && !flush_buf(w, false)) {
      w->bad = true;
      return 0;
    }
    if (r == LZMA_STREAM_END) break;
  }
  double t = now();
  if (t - w->last > .25) {
    w->last = t;
    out("PROGRESS %llu %llu", (unsigned long long)(w->done_before + w->got), (unsigned long long)w->total);
  }
  return len;
}

static void stick_feed(const char *name, uint64_t size, Writer *w);

/* Abbild a von base herunterladen und ab Byte off auf fd schreiben, höchstens limit Bytes */
static void install_asset(const char *base, const Asset *a, int fd, uint64_t off, uint64_t limit, uint64_t done_before,
                          uint64_t total) {
  if (a->raw > limit) fail("%s does not fit into the partition.", a->file);
  Writer w = {.fd = fd, .off = off, .limit = limit, .done_before = done_before, .total = total};
  w.buf = malloc(CHUNK + 4096);
  if (!w.buf) fail("Out of memory");
  lzma_stream init = LZMA_STREAM_INIT;
  w.z = init;
  if (lzma_stream_decoder(&w.z, UINT64_MAX, 0) != LZMA_OK) fail("Could not start the xz decompressor");
  sha_init(&w.sha);
  char url[1024], err[CURL_ERROR_SIZE] = "";
  snprintf(url, sizeof url, "%s/%s", base, a->file);
  if (is_stick(base)) stick_feed(a->file, a->xz, &w);
  /* Bricht die Verbindung ab (GitHub setzt lange Downloads gelegentlich zurück, und solange der
   * Installer Nullbereiche auf eine langsame Platte schreibt, liest er nichts), geht es mit einer
   * Range-Anfrage ab dem letzten empfangenen Byte weiter: Entpacker, Prüfsumme und Schreibstelle
   * laufen einfach weiter. Antwortet der Server ohne Range (200 statt 206), meldet libcurl
   * CURLE_RANGE_ERROR, statt Daten doppelt zu liefern. */
  for (int attempt = 1; !is_stick(base); attempt++) {
    CURL *c = curl_easy_init();
    if (!c) fail("Out of memory");
    curl_common(c, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &w);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    if (w.got) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)w.got);
    err[0] = 0;
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (w.bad && w.werr)
      fail("Writing to the disk failed at %llu MB: %s", (unsigned long long)((w.off + w.written) >> 20), strerror(w.werr));
    if (w.bad) fail("%s is damaged or too large.", a->file);
    if (rc == CURLE_OK) break;
    bool again = strncmp(url, "file:", 5) && attempt < 10 && rc != CURLE_RANGE_ERROR && rc != CURLE_WRITE_ERROR &&
                 !(rc == CURLE_HTTP_RETURNED_ERROR && code >= 400 && code < 500);
    if (!again) fail("Download of %s failed: %s", a->file, err[0] ? err : curl_easy_strerror(rc));
    fprintf(stderr, "%s: %s, continuing at byte %llu (attempt %d)\n", a->file, err[0] ? err : curl_easy_strerror(rc),
            (unsigned long long)w.got, attempt + 1);
    sleep(attempt < 5 ? 2u * attempt : 15u);
  }
  for (;;) { /* was der Entpacker noch zurückhält */
    w.z.next_in = NULL;
    w.z.avail_in = 0;
    w.z.next_out = w.buf + w.fill;
    w.z.avail_out = CHUNK - w.fill;
    lzma_ret r = lzma_code(&w.z, LZMA_FINISH);
    w.fill = CHUNK - w.z.avail_out;
    if (w.fill == CHUNK && !flush_buf(&w, false))
      fail("Writing to the disk failed at %llu MB: %s", (unsigned long long)((w.off + w.written) >> 20), strerror(w.werr));
    if (r == LZMA_STREAM_END) break;
    if (r != LZMA_OK) fail("%s is damaged.", a->file);
  }
  if (!flush_buf(&w, true))
    fail("Writing to the disk failed at %llu MB: %s", (unsigned long long)((w.off + w.written) >> 20), strerror(w.werr));
  lzma_end(&w.z);
  free(w.buf);
  char hex[65];
  sha_hex(&w.sha, hex);
  if (w.got != a->xz || strcmp(hex, a->sha)) fail("%s did not arrive completely (wrong checksum).", a->file);
  if (w.written < a->raw) fail("%s is shorter than stated.", a->file);
  out("PROGRESS %llu %llu", (unsigned long long)(done_before + a->xz), (unsigned long long)total);
}

static void patch_esp(int fd, uint32_t dbs, uint64_t esp_off, const uint8_t sys[16], const uint8_t rec[16], bool have_rec);

static const char *machine_arch(void) {
  struct utsname u;
  if (uname(&u)) return "x86_64";
  return strncmp(u.machine, "arm64", 5) && strncmp(u.machine, "aarch64", 7) ? "x86_64" : "arm64";
}

/* Dateisystem des Systems auf die ganze Partition vergrößern (e2fsprogs der Wiederherstellung). Das Abbild
 * ist nur so groß wie sein Inhalt; ohne diesen Schritt bliebe der Rest der Platte unbenutzt. Schlägt es fehl,
 * läuft das System mit dem kleinen Dateisystem weiter. */
static void grow_ext4(const char *disk, bool keep, uint64_t off, uint64_t bytes) {
  if (access("/usr/sbin/resize2fs", X_OK) || access("/usr/sbin/e2fsck", X_OK)) return;
  char dev[96], cmd[512];
  if (keep) snprintf(dev, sizeof dev, "/dev/r%ss2", disk);
  else snprintf(dev, sizeof dev, "/dev/r%s?offset=%llu", disk, (unsigned long long)off);
  out("STEP %s", L("Using the whole disk"));
  snprintf(cmd, sizeof cmd,
           "/usr/sbin/e2fsck -fp '%s' >>/var/tmp/installer.log 2>&1; [ $? -lt 4 ] && "
           "/usr/sbin/resize2fs -f '%s' %llus >>/var/tmp/installer.log 2>&1",
           dev, dev, (unsigned long long)(bytes / 512));
  if (system(cmd) != 0) fprintf(stderr, "could not grow the file system on %s\n", dev);
}

static void cmd_install(const char *disk, const char *base) {
  Disk d;
  if (!probe_disk(disk, &d)) fail("No disk %s", disk);
  if (d.installer) fail("This is the installer disk itself.");
  int rootp = d.root ? root_part(disk) : 0;
  /* Neu installieren aus der Wiederherstellung derselben Platte: GPT und Wiederherstellung bleiben */
  bool keep = d.root && rootp == 3 && d.nparts >= 3 && !strcmp(d.part[2].name, "ClokeOS Recovery");
  if (d.root && !keep) fail("The system is running from this disk.");

  out("STEP %s", L("Loading information about ClokeOS"));
  curl_global_init(CURL_GLOBAL_DEFAULT);
  Manifest m;
  fetch_manifest(base, machine_arch(), &m);
  uint32_t bs = d.bsize;
  uint64_t mib = 1 << 20;
  uint64_t esp_b = (m.esp.raw + mib - 1) / mib * mib, sys_b = (m.system.raw + mib - 1) / mib * mib,
           rec_b = (m.recovery.raw + mib - 1) / mib * mib;
  NewPart p[3] = {{GUID_ESP, "EFI System Partition", mib / bs, esp_b / bs, {0}},
                  {GUID_LINUX, "ClokeOS", 0, sys_b / bs, {0}},
                  {GUID_LINUX, "ClokeOS Recovery", 0, rec_b / bs, {0}}};
  if (keep) {
    /* vorhandene Lage übernehmen */
    if (d.part[0].count * bs < m.esp.raw || d.part[1].count * bs < m.system.raw)
      fail("This version needs more space than the partitions have. Please start from the installer "
           "disk and set up the disk again.");
    for (int i = 0; i < 2; i++) p[i].first = d.part[i].first, p[i].count = d.part[i].count;
  } else {
    p[1].first = p[0].first + p[0].count;
    p[2].first = p[1].first + p[1].count;
    uint64_t need = (p[2].first + p[2].count) * bs + mib;
    if (d.bytes < need)
      fail("The disk is too small: ClokeOS %s needs %.1f GB, it has %.1f GB.", m.version, need / 1e9,
           d.bytes / 1e9);
    /* Das System bekommt den Rest der Platte (Wiederherstellung dahinter, 1 MiB Luft für die Sicherungs-GPT);
     * sein Dateisystem wird nach dem Schreiben darauf vergrößert (grow_ext4) */
    uint64_t mibs = mib / bs, end = (d.bytes / bs - mibs) / mibs * mibs;
    if (end > p[1].first + p[2].count) {
      uint64_t sys = (end - p[1].first - p[2].count) / mibs * mibs;
      if (sys > p[1].count) p[1].count = sys, p[2].first = p[1].first + sys;
    }
  }
  unmount_disk(disk, keep ? 3 : 0);
  /* fd[i]/off[i]: wohin Partition i geschrieben wird. Sonst die ganze Platte; beim Neuinstallieren
   * aus der Wiederherstellung derselben Platte lässt sich die nicht zum Schreiben öffnen (ihre
   * Partition 3 ist die laufende Wurzel), dann die Partitionsgeräte selbst */
  int fd[2];
  uint64_t off[2];
  if (keep) {
    for (int i = 0; i < 2; i++) {
      char part[24];
      snprintf(part, sizeof part, "%ss%d", disk, i + 1);
      fd[i] = open_raw(part, O_RDWR);
      if (fd[i] < 0) fail("Could not write to %s: %s", part, strerror(errno));
      off[i] = 0;
    }
  } else {
    fd[0] = fd[1] = open_raw(disk, O_RDWR);
    if (fd[0] < 0) fail("Could not write to %s: %s", disk, strerror(errno));
    off[0] = p[0].first * bs, off[1] = p[1].first * bs;
    out("STEP %s", L("Preparing the disk"));
    write_gpt(fd[0], &d, p, 3);
  }
  uint64_t total = m.system.xz + m.esp.xz + (keep ? 0 : m.recovery.xz), done = 0;
  char step[256];
  snprintf(step, sizeof step, L("Downloading and installing ClokeOS %s"), m.version);
  out("STEP %s", step);
  install_asset(base, &m.system, fd[1], off[1], p[1].count * bs, done, total);
  done += m.system.xz;
  bool have_rec = true;
  if (!keep) {
    out("STEP %s", L("Setting up the recovery"));
    /* Der USB-Stick bringt die Wiederherstellung nicht mit (zu groß für die RAM-Disk): dann genau
     * diese Version aus dem Netz; ohne Internet bleibt die Partition leer */
    const char *rbase = base;
    char online[512];
    if (!strncmp(base, "file://", 7)) {
      char local[700];
      snprintf(local, sizeof local, "%s/%s", base + 7, m.recovery.file);
      if (access(local, R_OK) && stick_has(m.recovery.file)) {
        rbase = "stick:"; /* Offline-Stick, auf dem die große RAM-Disk passte */
      } else if (access(local, R_OK)) {
        snprintf(online, sizeof online, "https://github.com/%s/releases/download/v%s", RELEASES_REPO, m.version);
        rbase = url_reachable(online, m.recovery.file) ? online : NULL;
      }
    }
    if (rbase) install_asset(rbase, &m.recovery, fd[0], p[2].first * bs, p[2].count * bs, done, total);
    else {
      have_rec = false;
      out("STEP %s", L("No internet connection: ClokeOS is installed without a recovery partition"));
    }
    done += m.recovery.xz;
  }
  out("STEP %s", L("Setting up the startup volume"));
  install_asset(base, &m.esp, fd[0], off[0], p[0].count * bs, done, total);
  fsync(fd[1]);
  grow_ext4(disk, keep, off[1], p[1].count * bs);
  if (keep) { /* Kennungen der vorhandenen Partitionen */
    memcpy(p[1].guid, d.part[1].guid, 16);
    memcpy(p[2].guid, d.part[2].guid, 16);
  }
  patch_esp(fd[0], bs, off[0], p[1].guid, p[2].guid, keep || have_rec);
  fsync(fd[0]);
  close(fd[0]);
  if (fd[1] != fd[0]) close(fd[1]);
  out("DONE %s", m.version);
}

/* Lesen mit Wiederholung: PureDarwins einziger Treiber für USB-Massenspeicher
 * (RavynXHCIMassStorageDisk) bricht bei langen Lesevorgängen gelegentlich mit E/A-Fehlern ab */
static ssize_t pread_retry(int fd, void *buf, size_t n, off_t off) {
  for (int t = 0;; t++) {
    ssize_t r = pread(fd, buf, n, off);
    if (r == (ssize_t)n) return r;
    if (t == 8) return -1;
    fprintf(stderr, "read of %zu bytes at %lld failed (%s), retrying\n", n, (long long)off,
            r < 0 ? strerror(errno) : "short read");
    sleep(t < 3 ? 1 : 3);
  }
}

/* ---- EFI-Partition (FAT32): Boot-Argumente lesen und schreiben ----------------------------- */
/* Der Loader liest bei jedem Start \EFI\BOOT\boot-args.txt; boot-uuid= darin nennt die GPT-Partition,
 * die er als Wurzel nimmt. Das Release legt daneben boot-args-system.txt und boot-args-recovery.txt
 * ab. Diese Dateien sind kurz (ein Cluster); sie werden nur gelesen und an genau ihrer Stelle
 * überschrieben, ohne den Dateisystemaufbau anzufassen. */

typedef struct {
  int fd;
  uint32_t dbs;  /* Blockgröße der Platte */
  uint64_t base; /* Byte-Anfang der Partition */
  uint32_t bps, spc, rsvd, nfats, fatsz, root;
  uint64_t data;
  uint32_t *table; /* ganze FAT im Speicher (USB-Stick), sonst NULL */
  uint64_t nent;
} Fat;

static uint64_t clus_off(Fat *f, uint32_t c) { return f->base + f->data + (uint64_t)(c - 2) * f->spc * f->bps; }

static uint32_t fat_next(Fat *f, uint32_t c) {
  if (f->table) return c < f->nent ? f->table[c] & 0x0fffffff : 0x0fffffff;
  uint32_t blk[1024]; /* ein Block der FAT (Rohgerät: nur ganze Blöcke lesen) */
  uint64_t pos = f->base + (uint64_t)f->rsvd * f->bps + 4ull * c, b = pos / f->dbs * f->dbs;
  if (f->dbs > sizeof blk || pread_retry(f->fd, blk, f->dbs, (off_t)b) != (ssize_t)f->dbs) return 0x0fffffff;
  return blk[(pos - b) / 4] & 0x0fffffff;
}

static bool fat_init(Fat *f, int fd, uint32_t dbs, uint64_t base) {
  memset(f, 0, sizeof *f);
  f->fd = fd, f->dbs = dbs, f->base = base;
  uint8_t bpb[4096];
  if (dbs > sizeof bpb || pread_retry(fd, bpb, dbs, (off_t)base) != (ssize_t)dbs) return false;
  f->bps = bpb[11] | bpb[12] << 8;
  f->spc = bpb[13];
  f->rsvd = bpb[14] | bpb[15] << 8;
  f->nfats = bpb[16];
  f->fatsz = bpb[36] | bpb[37] << 8 | bpb[38] << 16 | (uint32_t)bpb[39] << 24;
  f->root = bpb[44] | bpb[45] << 8 | bpb[46] << 16 | (uint32_t)bpb[47] << 24;
  if (!f->bps || !f->spc || !f->fatsz || (bpb[17] | bpb[18] << 8)) return false; /* nur FAT32 */
  f->data = (uint64_t)(f->rsvd + f->nfats * f->fatsz) * f->bps;
  return true;
}

/* Eintrag name (lange oder kurze Form, ohne Groß/klein) im Ordner ab Cluster dir */
static bool fat_find(Fat *f, uint32_t dir, const char *name, uint64_t *entry_off, uint8_t ent[32]) {
  size_t csize = (size_t)f->spc * f->bps;
  uint8_t *buf = malloc(csize);
  char lfn[256] = "";
  bool found = false;
  for (uint32_t c = dir; buf && c >= 2 && c < 0x0ffffff8 && !found; c = fat_next(f, c)) {
    if (pread_retry(f->fd, buf, csize, (off_t)clus_off(f, c)) != (ssize_t)csize) break;
    for (size_t o = 0; o < csize && !found; o += 32) {
      uint8_t *e = buf + o;
      if (e[0] == 0) goto done;
      if (e[0] == 0xe5) {
        lfn[0] = 0;
        continue;
      }
      if (e[11] == 0x0f) { /* langer Name: 13 Zeichen je Eintrag, rückwärts */
        int seq = (e[0] & 0x1f) - 1;
        static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        if (e[0] & 0x40) memset(lfn, 0, sizeof lfn);
        for (int k = 0; k < 13 && seq * 13 + k < 255; k++) {
          uint16_t ch = e[pos[k]] | e[pos[k] + 1] << 8;
          if (ch == 0 || ch == 0xffff) break;
          lfn[seq * 13 + k] = ch < 128 ? (char)ch : '?';
        }
        continue;
      }
      if (e[11] & 0x08) { /* Datenträgername ("EFI"), kein Ordner */
        lfn[0] = 0;
        continue;
      }
      char shortn[13];
      size_t k = 0;
      for (int i = 0; i < 8 && e[i] != ' '; i++) shortn[k++] = (char)e[i];
      if (e[8] != ' ') shortn[k++] = '.';
      for (int i = 8; i < 11 && e[i] != ' '; i++) shortn[k++] = (char)e[i];
      shortn[k] = 0;
      if (!strcasecmp(lfn, name) || !strcasecmp(shortn, name)) {
        *entry_off = clus_off(f, c) + o;
        memcpy(ent, e, 32);
        found = true;
      }
      lfn[0] = 0;
    }
  }
done:
  free(buf);
  return found;
}

static uint32_t ent_clus(const uint8_t e[32]) { return (uint32_t)(e[26] | e[27] << 8 | e[20] << 16 | e[21] << 24); }

/* Datei \EFI\BOOT\<name>: Lage des Verzeichniseintrags */
static bool fat_boot_file(Fat *f, const char *name, uint64_t *eoff, uint8_t ent[32]) {
  uint8_t e[32];
  uint64_t o;
  if (!fat_find(f, f->root, "EFI", &o, e)) return false;
  if (!fat_find(f, ent_clus(e), "BOOT", &o, e)) return false;
  return fat_find(f, ent_clus(e), name, eoff, ent);
}

/* Inhalt einer kurzen Datei (höchstens ein Cluster) als Text */
static bool fat_read(Fat *f, const char *name, char *out, size_t cap) {
  uint8_t e[32];
  uint64_t eo;
  if (!fat_boot_file(f, name, &eo, e)) return false;
  uint32_t len = e[28] | e[29] << 8 | e[30] << 16 | (uint32_t)e[31] << 24;
  size_t csize = (size_t)f->spc * f->bps;
  if (len >= cap || len > csize || ent_clus(e) < 2) return false;
  uint8_t *buf = malloc(csize);
  bool ok = buf && pread(f->fd, buf, csize, (off_t)clus_off(f, ent_clus(e))) == (ssize_t)csize;
  if (ok) {
    memcpy(out, buf, len);
    out[len] = 0;
  }
  free(buf);
  return ok;
}

/* Kurze Datei an ihrer Stelle überschreiben, Größe im Verzeichniseintrag anpassen */
static bool fat_write(Fat *f, const char *name, const char *data) {
  uint8_t e[32];
  uint64_t eo;
  size_t len = strlen(data), csize = (size_t)f->spc * f->bps;
  if (!fat_boot_file(f, name, &eo, e) || len > csize || ent_clus(e) < 2) return false;
  uint8_t *buf = calloc(1, csize), *b = malloc(f->dbs);
  bool ok = buf && b;
  if (ok) {
    memcpy(buf, data, len);
    ok = pwrite(f->fd, buf, csize, (off_t)clus_off(f, ent_clus(e))) == (ssize_t)csize;
  }
  uint64_t blk = eo / f->dbs * f->dbs;
  if (ok) ok = pread(f->fd, b, f->dbs, (off_t)blk) == (ssize_t)f->dbs;
  if (ok) {
    uint8_t *de = b + (eo - blk);
    de[28] = len & 0xff, de[29] = len >> 8 & 0xff, de[30] = len >> 16 & 0xff, de[31] = len >> 24 & 0xff;
    ok = pwrite(f->fd, b, f->dbs, (off_t)blk) == (ssize_t)f->dbs;
  }
  free(buf);
  free(b);
  return ok;
}

/* boot-uuid= in einer Boot-Argument-Datei auf guid setzen */
static bool set_boot_uuid(Fat *f, const char *name, const char *guid) {
  char args[1024], out[1100];
  if (!fat_read(f, name, args, sizeof args)) return false;
  args[strcspn(args, "\r\n")] = 0;
  char *p = strstr(args, "boot-uuid=");
  if (p) {
    char *end = p + strcspn(p, " ");
    memmove(p, end, strlen(end) + 1);
  }
  size_t n = strlen(args);
  while (n && args[n - 1] == ' ') args[--n] = 0;
  snprintf(out, sizeof out, "%s boot-uuid=%s\n", args, guid);
  return fat_write(f, name, out);
}

/* Nach dem Schreiben der EFI-Partition: Kennungen von System und Wiederherstellung einsetzen */
/* have_rec false: die Wiederherstellung fehlt (USB-Stick ohne Internet). boot-args-recovery.txt
 * beginnt dann mit #, bootnext und die Taste R im Loader lassen es stehen. */
static void patch_esp(int fd, uint32_t dbs, uint64_t esp_off, const uint8_t sys[16], const uint8_t rec[16], bool have_rec) {
  Fat f;
  char sg[37], rg[37];
  guid_str(sys, sg);
  guid_str(rec, rg);
  if (!fat_init(&f, fd, dbs, esp_off)) fail("The EFI partition is not FAT32.");
  bool rec_ok = have_rec ? set_boot_uuid(&f, "boot-args-recovery.txt", rg)
                         : fat_write(&f, "boot-args-recovery.txt", "# no recovery partition\n");
  if (!set_boot_uuid(&f, "boot-args-system.txt", sg) || !rec_ok || !set_boot_uuid(&f, "boot-args.txt", sg))
    fail("Could not set the boot arguments in the EFI partition.");
}

/* ---- Abbilder vom Offline-USB-Stick ------------------------------------------------------ */
/* Passt die große RAM-Disk des Offline-Sticks nicht unter die Grenze von XNU (siehe
 * loader/xnu-loader.patch), startet die schlanke ohne Abbilder. Die liegen dann als Dateien unter
 * \ClokeOS\Install auf der EFI-Partition des Sticks (recovery/usb.sh offline) und werden über den
 * USB-Treiber gelesen: die FAT einmal ganz, die Dateien in zusammenhängenden Stücken bis 1 MB, jeder
 * Lesevorgang mit Wiederholung, bei Fehlern in Stücken von 64 KB. */

/* Datei aus \EFI\BOOT der EFI-Partition der Platte, von der das System läuft, auf die Standardausgabe
 * (für große Dateien: Cluster für Cluster). Damit holt clokeos-apply-update beim Start die Dateien, die ein
 * Update in der EFI-Partition mitgebracht hat; ein Einbinden der Partitionen wäre in der Wiederherstellung
 * ein Absturz des Kernels (ext4) beziehungsweise gibt es für FAT keinen mount-Helfer. */
static int cmd_esp_cat(const char *name) {
  int disk = -1;
  for (int i = 0; i < 16 && disk < 0; i++) {
    char n[16];
    snprintf(n, sizeof n, "disk%d", i);
    if (root_part(n)) disk = i;
  }
  if (disk < 0) fail("Startup disk not found");
  char dn[16], esp[24];
  snprintf(dn, sizeof dn, "disk%d", disk);
  Disk d;
  if (!probe_disk(dn, &d) || d.nparts < 1 || memcmp(d.part[0].type, GUID_ESP, 16)) fail("No EFI partition");
  snprintf(esp, sizeof esp, "%ss1", dn);
  int fd = open_raw(esp, O_RDONLY);
  if (fd < 0) fail("Could not open the EFI partition (%s): %s", esp, strerror(errno));
  Fat f;
  if (!fat_init(&f, fd, d.bsize, 0)) fail("The EFI partition is not FAT32");
  uint8_t e[32];
  uint64_t eo;
  if (!fat_boot_file(&f, name, &eo, e)) return 1;
  uint64_t left = e[28] | e[29] << 8 | e[30] << 16 | (uint64_t)e[31] << 24;
  size_t csize = (size_t)f.spc * f.bps;
  uint8_t *buf = malloc(csize);
  uint32_t c = ent_clus(e);
  while (buf && left && c >= 2 && c < 0x0ffffff8) {
    size_t n = left < csize ? (size_t)left : csize;
    if (pread_retry(fd, buf, csize, (off_t)clus_off(&f, c)) != (ssize_t)csize) return 2;
    if (fwrite(buf, 1, n, stdout) != n) return 2;
    left -= n;
    c = fat_next(&f, c);
  }
  free(buf);
  close(fd);
  return left ? 2 : 0;
}

/* Aktualisieren: nur die EFI-Partition (Loader, Kernel, Boot-Argumente) einer installierten Platte aus dem
 * Abbild neu schreiben; System und Wiederherstellung bleiben, die Kennungen der Partitionen werden wieder
 * in boot-args-*.txt eingesetzt. Die Dateien des Systems kopiert clokeos-update. */
static void cmd_update_esp(const char *disk, const char *base) {
  Disk d;
  if (!probe_disk(disk, &d)) fail("No disk %s", disk);
  if (d.installer || d.root) fail("Choose the disk that ClokeOS is installed on, and start from the installer.");
  if (!d.clokeos || d.nparts < 2) fail("There is no ClokeOS on this disk.");
  out("STEP %s", L("Loading information about ClokeOS"));
  curl_global_init(CURL_GLOBAL_DEFAULT);
  Manifest m;
  fetch_manifest(base, machine_arch(), &m);
  uint32_t bs = d.bsize;
  if (d.part[0].count * bs < m.esp.raw) fail("The EFI partition is too small for this version.");
  unmount_disk(disk, 0);
  char part[24];
  snprintf(part, sizeof part, "%ss1", disk);
  int fd = open_raw(part, O_RDWR);
  if (fd < 0) fail("Could not write to %s: %s", part, strerror(errno));
  out("STEP %s", L("Setting up the startup volume"));
  install_asset(base, &m.esp, fd, 0, d.part[0].count * bs, 0, m.esp.xz);
  bool have_rec = d.nparts >= 3 && !strcmp(d.part[2].name, "ClokeOS Recovery");
  patch_esp(fd, bs, 0, d.part[1].guid, have_rec ? d.part[2].guid : d.part[1].guid, have_rec);
  fsync(fd);
  close(fd);
  out("DONE %s", m.version);
}

static bool read_robust(int fd, void *buf, size_t n, off_t off, uint32_t dbs) {
  if (pread_retry(fd, buf, n, off) == (ssize_t)n) return true;
  static const size_t tiers[] = {65536, 8192};
  for (size_t k = 0; k < sizeof tiers / sizeof tiers[0]; k++) {
    size_t piece = tiers[k] % dbs ? dbs : tiers[k];
    bool ok = true;
    for (size_t o = 0; o < n && ok; o += piece) {
      size_t len = n - o < piece ? n - o : piece;
      ok = pread_retry(fd, (uint8_t *)buf + o, len, off + (off_t)o) == (ssize_t)len;
    }
    if (ok) return true;
  }
  return false;
}

/* Die Abbilder im Speicher: der Loader hat sie in reservierten Speicher gelegt (kext/ClokePayload),
 * /dev/clokeos_payload gibt sie her. Das umgeht PureDarwins USB-Treiber für Massenspeicher, der auf
 * echter Hardware nach einigen Dutzend MB mit E/A-Fehlern aufgibt. */
static struct {
  bool tried, ok;
  int fd;
  int n;
  struct { char name[100]; uint64_t off, size; } f[8];
} blob;

static bool blob_open(void) {
  if (blob.tried) return blob.ok;
  blob.tried = true;
  int fd = open("/dev/clokeos_payload", O_RDONLY);
  if (fd < 0) return false;
  char hdr[4096];
  ssize_t n = pread(fd, hdr, sizeof hdr - 1, 0);
  if (n < 20 || strncmp(hdr, "CLOKEOS-PAYLOAD 1\n", 18)) {
    close(fd);
    return false;
  }
  hdr[n] = 0;
  char *save = NULL;
  for (char *line = strtok_r(hdr, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char name[100];
    unsigned long long off, size;
    if (blob.n < 8 && sscanf(line, "%99s %llu %llu", name, &off, &size) == 3 && strchr(name, '.')) {
      snprintf(blob.f[blob.n].name, sizeof blob.f[blob.n].name, "%s", name);
      blob.f[blob.n].off = off;
      blob.f[blob.n].size = size;
      blob.n++;
    }
  }
  blob.fd = fd;
  blob.ok = blob.n > 0;
  if (!blob.ok) close(fd);
  return blob.ok;
}

static bool blob_find(const char *name, uint64_t *off, uint64_t *size) {
  if (!blob_open()) return false;
  for (int i = 0; i < blob.n; i++)
    if (!strcmp(blob.f[i].name, name)) {
      *off = blob.f[i].off;
      *size = blob.f[i].size;
      return true;
    }
  return false;
}

static struct {
  bool tried, ok;
  int fd;
  Fat f;
  uint32_t install; /* erster Cluster von \ClokeOS\Install */
} stick;

static bool stick_open(void) {
  if (stick.tried) return stick.ok;
  stick.tried = true;
  for (int i = 0; i < 16; i++) {
    char name[16], part[24];
    snprintf(name, sizeof name, "disk%d", i);
    Disk d;
    if (!probe_disk(name, &d) || !d.installer) continue;
    snprintf(part, sizeof part, "%ss1", name);
    int fd = open_raw(part, O_RDONLY);
    if (fd < 0) continue;
    uint32_t dbs = 0;
    Fat *f = &stick.f;
    bool ok = !ioctl(fd, DKIOCGETBLOCKSIZE, &dbs) && dbs && fat_init(f, fd, dbs, 0);
    size_t bytes = ok ? (size_t)f->fatsz * f->bps : 0;
    uint32_t *table = ok ? malloc(bytes) : NULL;
    ok = table && read_robust(fd, table, bytes, (off_t)f->rsvd * f->bps, dbs);
    if (ok) f->table = table, f->nent = bytes / 4;
    uint8_t e[32];
    uint64_t o;
    ok = ok && fat_find(f, f->root, "ClokeOS", &o, e) && fat_find(f, ent_clus(e), "Install", &o, e);
    if (!ok) {
      free(table);
      f->table = NULL;
      close(fd);
      continue;
    }
    stick.install = ent_clus(e);
    stick.fd = fd;
    stick.ok = true;
    return true;
  }
  return false;
}

static bool stick_lookup(const char *name, uint32_t *clus, uint64_t *size) {
  uint8_t e[32];
  uint64_t o;
  if (!stick_open() || !fat_find(&stick.f, stick.install, name, &o, e)) return false;
  *clus = ent_clus(e);
  *size = e[28] | e[29] << 8 | e[30] << 16 | (uint64_t)e[31] << 24;
  return true;
}

static bool stick_has(const char *name) {
  uint32_t c;
  uint64_t off, size;
  if (blob_open()) return blob_find(name, &off, &size);
  return stick_lookup(name, &c, &size);
}

/* Datei name vom Stick lesen und stückweise an cb geben (wie die Schreibfunktion von libcurl);
 * want != 0: so groß muss sie sein. false: nicht da, oder cb hat abgebrochen */
static bool stick_read(const char *name, uint64_t want, size_t (*cb)(char *, size_t, size_t, void *), void *arg) {
  uint32_t c;
  uint64_t left;
  if (blob_open()) { /* aus dem Speicher */
    uint64_t off;
    if (!blob_find(name, &off, &left)) return false;
    if (want && left != want) fail("%s has the wrong size in memory.", name);
    static uint8_t mbuf[1 << 20];
    for (uint64_t done = 0; done < left;) {
      size_t n = left - done < sizeof mbuf ? (size_t)(left - done) : sizeof mbuf;
      if (pread_retry(blob.fd, mbuf, n, (off_t)(off + done)) != (ssize_t)n)
        fail("Reading %s from memory failed at %llu MB: %s.", name, (unsigned long long)(done >> 20), strerror(errno));
      if (cb((char *)mbuf, 1, n, arg) != n) return false;
      done += n;
    }
    return true;
  }
  if (!stick_lookup(name, &c, &left)) return false;
  uint64_t total = left;
  if (want && left != want) fail("%s on the USB stick has the wrong size.", name);
  Fat *f = &stick.f;
  size_t csize = (size_t)f->spc * f->bps, maxrun = (1u << 20) / csize ? (1u << 20) / csize : 1;
  uint8_t *buf = malloc(maxrun * csize);
  if (!buf) fail("Out of memory");
  while (left) {
    if (c < 2 || c >= 0x0ffffff8) fail("%s on the USB stick is damaged.", name);
    uint32_t first = c, next = fat_next(f, c);
    size_t run = 1;
    while (run < maxrun && next == c + 1 && (uint64_t)run * csize < left) {
      c = next;
      next = fat_next(f, c);
      run++;
    }
    size_t len = run * csize, use = left < len ? (size_t)left : len;
    if (!read_robust(stick.fd, buf, len, (off_t)clus_off(f, first), f->dbs))
      fail("Reading %s from the USB stick failed at %llu MB: %s. Please try another USB port.", name,
           (unsigned long long)((total - left) >> 20), strerror(errno));
    if (cb((char *)buf, 1, use, arg) != use) {
      free(buf);
      return false;
    }
    left -= use;
    c = next;
  }
  free(buf);
  return true;
}

static char *stick_text(const char *name) {
  char *text = NULL;
  if (!stick_read(name, 0, mem_write, &text)) {
    free(text);
    return NULL;
  }
  return text;
}

static void stick_feed(const char *name, uint64_t size, Writer *w) {
  if (stick_read(name, size, on_data, w)) return;
  if (w->bad && w->werr)
    fail("Writing to the disk failed at %llu MB: %s", (unsigned long long)((w->off + w->written) >> 20), strerror(w->werr));
  if (w->bad) fail("%s is damaged or too large.", name);
  fail("%s is missing on the USB stick.", name);
}

/* Offline-Stick mit Abbildern für diese Architektur? Gibt "STICK <version>" aus */
static int cmd_stick(void) {
  char name[64];
  snprintf(name, sizeof name, "clokeos-%s.txt", machine_arch());
  char *text = stick_text(name);
  if (!text) return 1;
  Manifest m;
  parse_manifest(text, machine_arch(), &m);
  free(text);
  out("STICK %s", m.version);
  return 0;
}

/* Datei \EFI\BOOT\<name> (Kernel) auf dem Live-Stick (recovery/usb.sh live) durch src ersetzen: an Ort und Stelle,
 * Cluster für Cluster, ohne die Datei zu vergrößern. Der Rest bis zur alten Größe wird mit Nullen gefüllt;
 * ist src größer, passiert nichts. Für die Treiberarbeit: neuer Kernel per scp, dann stick-write-boot, dann
 * Neustart. Bei einem schlechten Kernel startet U beim Einschalten kernel-up (bleibt unangetastet). */
static int cmd_stick_write_boot(const char *name, const char *src) {
  FILE *in = fopen(src, "rb");
  if (!in) fail("Cannot read %s", src);
  fseek(in, 0, SEEK_END);
  long len = ftell(in);
  fseek(in, 0, SEEK_SET);
  for (int i = 0; i < 16; i++) {
    char dname[16], part[24];
    snprintf(dname, sizeof dname, "disk%d", i);
    Disk d;
    if (!probe_disk(dname, &d) || !d.installer) continue;
    snprintf(part, sizeof part, "%ss1", dname);
    int fd = open_raw(part, O_RDWR);
    if (fd < 0) fail("Could not open %s for writing: %s", part, strerror(errno));
    uint32_t dbs = 0;
    Fat f;
    uint8_t e[32];
    uint64_t eo;
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &dbs) || !dbs || !fat_init(&f, fd, dbs, 0) || !fat_boot_file(&f, name, &eo, e)) {
      close(fd);
      continue;
    }
    uint64_t size = e[28] | e[29] << 8 | e[30] << 16 | (uint64_t)e[31] << 24, done = 0;
    if ((uint64_t)len > size) {
      close(fd);
      fail("%s is %lld bytes, the file on the stick only %llu.", src, (long long)len, (unsigned long long)size);
    }
    size_t csize = (size_t)f.spc * f.bps;
    uint8_t *buf = malloc(csize);
    if (!buf) fail("Out of memory");
    for (uint32_t c = ent_clus(e); c >= 2 && c < 0x0ffffff8 && done < size; c = fat_next(&f, c)) {
      memset(buf, 0, csize);
      size_t n = fread(buf, 1, csize, in);
      (void)n;
      if (pwrite(fd, buf, csize, (off_t)clus_off(&f, c)) != (ssize_t)csize) {
        free(buf);
        close(fd);
        fail("Writing to the USB stick failed: %s", strerror(errno));
      }
      done += csize;
    }
    free(buf);
    fsync(fd);
    close(fd);
    fclose(in);
    out("DONE %lld of %llu bytes", (long long)len, (unsigned long long)size);
    return 0;
  }
  fail("%s is missing on the USB stick.", name);
  return 1;
}

/* Datei \ClokeOS\<name> auf dem Stick mit dem Inhalt von src überschreiben: an Ort und Stelle,
 * Cluster für Cluster, Größe bleibt (recovery/usb.sh legt die Datei mit Leerzeichen gefüllt an).
 * Für die Diagnose (cloakserver-session): sie ist dann am Mac lesbar, auch ohne Foto. */
static int cmd_stick_write(const char *name, const char *src) {
  FILE *in = fopen(src, "rb");
  if (!in) fail("Cannot read %s", src);
  static char data[1 << 20];
  size_t len = fread(data, 1, sizeof data, in);
  fclose(in);
  for (int i = 0; i < 16; i++) {
    char dname[16], part[24];
    snprintf(dname, sizeof dname, "disk%d", i);
    Disk d;
    if (!probe_disk(dname, &d) || !d.installer) continue;
    snprintf(part, sizeof part, "%ss1", dname);
    int fd = open_raw(part, O_RDWR);
    if (fd < 0) fail("Could not open %s for writing: %s", part, strerror(errno));
    uint32_t dbs = 0;
    Fat f;
    uint8_t e[32];
    uint64_t eo;
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &dbs) || !dbs || !fat_init(&f, fd, dbs, 0) ||
        !fat_find(&f, f.root, "ClokeOS", &eo, e) || !fat_find(&f, ent_clus(e), name, &eo, e)) {
      close(fd);
      continue;
    }
    uint64_t size = e[28] | e[29] << 8 | e[30] << 16 | (uint64_t)e[31] << 24, done = 0;
    size_t csize = (size_t)f.spc * f.bps;
    uint8_t *buf = malloc(csize);
    if (!buf) fail("Out of memory");
    for (uint32_t c = ent_clus(e); c >= 2 && c < 0x0ffffff8 && done < size; c = fat_next(&f, c)) {
      memset(buf, ' ', csize);
      size_t n = csize;
      if (done + n > size) n = (size_t)(size - done);
      if (done < len) memcpy(buf, data + done, len - done < n ? len - done : n);
      if (pwrite(fd, buf, csize, (off_t)clus_off(&f, c)) != (ssize_t)csize) {
        free(buf);
        close(fd);
        fail("Writing to the USB stick failed: %s", strerror(errno));
      }
      done += csize;
    }
    free(buf);
    fsync(fd);
    close(fd);
    out("DONE %llu of %llu bytes", (unsigned long long)(len < size ? len : size), (unsigned long long)size);
    return 0;
  }
  fail("%s is missing on the USB stick.", name);
  return 1;
}

static void cmd_bootnext(const char *which) {
  /* EFI-Partition der Platte, von der das System läuft */
  int disk = -1;
  for (int i = 0; i < 16 && disk < 0; i++) {
    char name[16];
    snprintf(name, sizeof name, "disk%d", i);
    if (root_part(name)) disk = i;
  }
  if (disk < 0) fail("Startup disk not found");
  char name[16];
  snprintf(name, sizeof name, "disk%d", disk);
  Disk d;
  if (!probe_disk(name, &d) || d.nparts < 1 || memcmp(d.part[0].type, GUID_ESP, 16)) fail("No EFI partition");
  /* Die ganze Platte lässt sich nicht zum Schreiben öffnen, solange eine ihrer Partitionen (die
   * laufende Wurzel) eingebunden ist; die EFI-Partition selbst ist frei */
  char esp[24];
  snprintf(esp, sizeof esp, "%ss1", name);
  int fd = open_raw(esp, O_RDWR);
  if (fd < 0) fail("Could not open the EFI partition (%s): %s", esp, strerror(errno));
  Fat f;
  if (!fat_init(&f, fd, d.bsize, 0)) fail("The EFI partition is not FAT32");
  char src[64], args[1024];
  snprintf(src, sizeof src, "boot-args-%s.txt", which);
  if (!fat_read(&f, src, args, sizeof args)) fail("%s is missing from the EFI partition", src);
  if (args[0] == '#') fail("This disk has no recovery partition.");
  if (!fat_write(&f, "boot-args.txt", args)) fail("Could not write boot-args.txt");
  fsync(fd);
  close(fd);
  out("DONE %s", which);
}

/* ---- Partitionieren (Festplattendienstprogramm) -------------------------------------------- */

static const uint8_t GUID_BASIC[16] = {0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7};
#define MIB (1ull << 20)
#define DU_LOG "/var/tmp/diskutil.log"

typedef struct {
  Disk d;
  bool gpt;
  uint8_t tab[128 * 128];
  uint8_t disk_guid[16];
  uint64_t first_use, last_use; /* nutzbarer Bereich in Blöcken */
} Layout;

static void load_layout(const char *disk, Layout *l) {
  memset(l, 0, sizeof *l);
  if (!probe_disk(disk, &l->d)) fail("No disk %s", disk);
  int fd = open_raw(disk, O_RDONLY);
  if (fd < 0) fail("Could not open %s: %s", disk, strerror(errno));
  uint32_t bs = l->d.bsize;
  uint8_t *hdr = malloc(bs);
  if (!hdr) fail("Out of memory");
  if (pread(fd, hdr, bs, bs) == (ssize_t)bs && !memcmp(hdr, "EFI PART", 8)) {
    uint64_t lba;
    uint32_t count, esize;
    memcpy(&lba, hdr + 72, 8);
    memcpy(&count, hdr + 80, 4);
    memcpy(&esize, hdr + 84, 4);
    memcpy(&l->first_use, hdr + 40, 8);
    memcpy(&l->last_use, hdr + 48, 8);
    memcpy(l->disk_guid, hdr + 56, 16);
    if (esize != 128 || count > 128) fail("This partition table is not supported.");
    size_t len = ((size_t)count * esize + bs - 1) / bs * bs;
    uint8_t *tab = malloc(len);
    if (!tab || pread(fd, tab, len, (off_t)(lba * bs)) != (ssize_t)len) fail("Could not read the partition table.");
    memcpy(l->tab, tab, (size_t)count * esize);
    free(tab);
    l->gpt = true;
  }
  free(hdr);
  close(fd);
}

static const char *detect_fs(const char *disk, uint64_t off) {
  int fd = open_raw(disk, O_RDONLY);
  if (fd < 0) return "-";
  uint8_t b[8192];
  ssize_t got = pread(fd, b, sizeof b, (off_t)off);
  close(fd);
  if (got < 2048) return "-";
  if (b[1080] == 0x53 && b[1081] == 0xEF) return "ext4";
  if (b[1024] == 'H' && (b[1025] == '+' || b[1025] == 'X')) return "hfs";
  if (!memcmp(b + 32, "NXSB", 4)) return "apfs";
  if (!memcmp(b + 3, "EXFAT   ", 8)) return "exfat";
  if (!memcmp(b + 3, "NTFS    ", 8)) return "ntfs";
  if (b[510] == 0x55 && b[511] == 0xAA) {
    if (!memcmp(b + 82, "FAT32   ", 8)) return "fat32";
    if (!memcmp(b + 54, "FAT", 3)) return "fat";
  }
  return "-";
}

static bool part_mounted(const char *disk, int slot) {
  struct statfs *m;
  int n = getmntinfo(&m, MNT_NOWAIT);
  char dev[48];
  snprintf(dev, sizeof dev, "/dev/%ss%d", disk, slot + 1);
  for (int i = 0; i < n; i++)
    if (!strcmp(m[i].f_mntfromname, dev)) return true;
  return false;
}

static void unmount_slot(const char *disk, int slot) {
  struct statfs *m;
  int n = getmntinfo(&m, MNT_NOWAIT);
  char dev[48];
  snprintf(dev, sizeof dev, "/dev/%ss%d", disk, slot + 1);
  for (int i = 0; i < n; i++)
    if (!strcmp(m[i].f_mntfromname, dev) && unmount(m[i].f_mntonname, MNT_FORCE))
      fail("%s is still mounted: %s", m[i].f_mntonname, strerror(errno));
}

typedef struct { uint64_t first, count; } Run;

static bool slot_used(const Layout *l, int s) {
  static const uint8_t zero[16];
  return memcmp(l->tab + 128 * s, zero, 16) != 0;
}

static uint64_t slot_first(const Layout *l, int s) {
  uint64_t v;
  memcpy(&v, l->tab + 128 * s + 32, 8);
  return v;
}

static uint64_t slot_last(const Layout *l, int s) {
  uint64_t v;
  memcpy(&v, l->tab + 128 * s + 40, 8);
  return v;
}

/* Freie Bereiche, auf ganze MiB gekürzt */
static int free_runs(const Layout *l, Run *r, int max) {
  uint64_t unit = MIB / l->d.bsize;
  int order[128], n = 0, nr = 0;
  for (int s = 0; s < 128; s++)
    if (slot_used(l, s)) order[n++] = s;
  for (int i = 1; i < n; i++) /* nach Anfang sortieren */
    for (int k = i; k > 0 && slot_first(l, order[k]) < slot_first(l, order[k - 1]); k--) {
      int t = order[k];
      order[k] = order[k - 1];
      order[k - 1] = t;
    }
  uint64_t cur = l->first_use;
  for (int i = 0; i <= n; i++) {
    uint64_t end = i < n ? slot_first(l, order[i]) : l->last_use + 1; /* ausschließlich */
    uint64_t a = (cur + unit - 1) / unit * unit, b = end / unit * unit;
    if (b > a && b - a >= unit && nr < max) r[nr++] = (Run){a, b - a};
    if (i < n && slot_last(l, order[i]) + 1 > cur) cur = slot_last(l, order[i]) + 1;
  }
  return nr;
}

static void cmd_layout(const char *disk) {
  Layout l;
  load_layout(disk, &l);
  Disk *d = &l.d;
  char flags[8] = "";
  size_t f = 0;
  if (d->root) flags[f++] = 'r';
  if (d->installer) flags[f++] = 'i';
  if (d->clokeos) flags[f++] = 'c';
  if (l.gpt) flags[f++] = 'g';
  if (!f) flags[f++] = '-';
  out("DISKX %s %llu %u %s %s", d->name, (unsigned long long)d->bytes, d->bsize, flags,
      L(d->installer ? "ClokeOS Installer" : d->clokeos ? "ClokeOS Disk" : "Disk"));
  if (!l.gpt) return;
  int rootp = d->root ? root_part(disk) : 0;
  for (int s = 0; s < 128; s++) {
    if (!slot_used(&l, s)) continue;
    uint64_t first = slot_first(&l, s), last = slot_last(&l, s);
    char name[40] = "";
    size_t k = 0;
    for (int c = 0; c < 36 && k + 1 < sizeof name; c++) {
      uint16_t ch = l.tab[128 * s + 56 + 2 * c] | l.tab[128 * s + 57 + 2 * c] << 8;
      if (!ch) break;
      name[k++] = ch < 128 && ch >= 32 ? (char)ch : '?'; /* der Name steht am Zeilenende */
    }
    char pf[8] = "";
    size_t pn = 0;
    if (part_mounted(disk, s)) pf[pn++] = 'm';
    if (rootp == s + 1) pf[pn++] = 'r';
    if (!memcmp(l.tab + 128 * s, GUID_ESP, 16)) pf[pn++] = 'e';
    if (!pn) pf[pn++] = '-';
    out("PARTX %d %llu %llu %s %s %s", s + 1, (unsigned long long)first, (unsigned long long)(last - first + 1),
        detect_fs(disk, first * d->bsize), pf, name[0] ? name : "-");
  }
  Run r[130];
  int nr = free_runs(&l, r, 130);
  for (int i = 0; i < nr; i++) out("FREEX %llu %llu", (unsigned long long)r[i].first, (unsigned long long)r[i].count);
}

/* ---- Dateisysteme anlegen ---- */

static void zero_range(int fd, uint64_t off, uint64_t len) {
  uint8_t *z = calloc(1, CHUNK);
  if (!z) fail("Out of memory");
  for (uint64_t o = 0; o < len; o += CHUNK) {
    size_t n = len - o < CHUNK ? (size_t)(len - o) : CHUNK;
    if (!pwrite_robust(fd, z, n, (off_t)(off + o))) fail("Could not write to the disk: %s", strerror(errno));
  }
  free(z);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

/* FAT32 wie mkfs.fat: 32 reservierte Blöcke, zwei FATs, Wurzel in Cluster 2 mit Datenträgernamen */
static void mkfs_fat32(const char *disk, uint64_t off, uint64_t bytes, uint32_t bs, const char *label) {
  uint64_t total = bytes / bs;
  uint32_t reserved = 32;
  uint64_t cb = bytes <= 260 * MIB ? 512 : bytes <= 8192 * MIB ? 4096 : bytes <= 16384 * MIB ? 8192
                : bytes <= 32768 * MIB ? 16384 : 32768;
  uint32_t spc = cb / bs ? (uint32_t)(cb / bs) : 1;
  uint64_t fatsz = 0, clusters = 0;
  for (;;) {
    fatsz = ((total / spc + 2) * 4 + bs - 1) / bs + 1;
    clusters = (total - reserved - 2 * fatsz) / spc;
    if (clusters >= 65525 || spc == 1) break;
    spc /= 2;
  }
  if (clusters < 65525 || clusters > 0x0ffffff5) fail("This partition can’t be formatted as FAT32 (it needs 33 MB to 2 TB).");
  int fd = open_raw(disk, O_RDWR);
  if (fd < 0) fail("Could not write to %s: %s", disk, strerror(errno));
  uint64_t meta = (reserved + 2 * fatsz + spc) * bs;
  zero_range(fd, off, (meta + MIB - 1) / MIB * MIB); /* alte FATs und Wurzel */
  uint8_t *b = calloc(1, bs), *fi = calloc(1, bs), *fat = calloc(1, bs), *root = calloc(spc, bs);
  if (!b || !fi || !fat || !root) fail("Out of memory");
  b[0] = 0xeb, b[1] = 0x58, b[2] = 0x90;
  memcpy(b + 3, "CLOKEOS ", 8);
  put16(b + 11, (uint16_t)bs);
  b[13] = (uint8_t)spc;
  put16(b + 14, (uint16_t)reserved);
  b[16] = 2;
  b[21] = 0xf8;
  put16(b + 24, 63);
  put16(b + 26, 255);
  put32(b + 28, (uint32_t)(off / bs));
  put32(b + 32, (uint32_t)total);
  put32(b + 36, (uint32_t)fatsz);
  put32(b + 44, 2);
  put16(b + 48, 1);
  put16(b + 50, 6);
  b[64] = 0x80;
  b[66] = 0x29;
  put32(b + 67, arc4random());
  char lab[12];
  snprintf(lab, sizeof lab, "%-11.11s", label[0] ? label : "NO NAME");
  for (int i = 0; i < 11; i++) lab[i] = lab[i] >= 'a' && lab[i] <= 'z' ? lab[i] - 32 : lab[i];
  memcpy(b + 71, lab, 11);
  memcpy(b + 82, "FAT32   ", 8);
  b[510] = 0x55, b[511] = 0xaa;
  put32(fi, 0x41615252);
  put32(fi + 484, 0x61417272);
  put32(fi + 488, (uint32_t)(clusters - 1));
  put32(fi + 492, 3);
  put32(fi + 508, 0xaa550000);
  bool ok = pwrite_robust(fd, b, bs, off) && pwrite_robust(fd, fi, bs, off + bs) &&
            pwrite_robust(fd, b, bs, off + 6ull * bs) && pwrite_robust(fd, fi, bs, off + 7ull * bs);
  put32(fat, 0x0ffffff8);
  put32(fat + 4, 0x0fffffff);
  put32(fat + 8, 0x0fffffff);
  for (int k = 0; k < 2 && ok; k++) ok = pwrite_robust(fd, fat, bs, off + (reserved + (uint64_t)k * fatsz) * bs);
  memcpy(root, lab, 11);
  root[11] = 0x08;
  ok = ok && pwrite_robust(fd, root, (size_t)spc * bs, off + (reserved + 2 * fatsz) * bs);
  if (!ok) fail("Could not write to the disk: %s", strerror(errno));
  fsync(fd);
  close(fd);
  free(b), free(fi), free(fat), free(root);
}

/* Programm der Wiederherstellung ausführen; die letzte Zeile seiner Meldungen kommt in last */
static int run_cmd(const char *cmd, char *last, size_t n) {
  char full[1024], line[256];
  snprintf(full, sizeof full, "(%s) 2>&1", cmd);
  last[0] = 0;
  FILE *p = popen(full, "r");
  if (!p) return -1;
  FILE *log = fopen(DU_LOG, "a");
  if (log) fprintf(log, "$ %s\n", cmd);
  while (fgets(line, sizeof line, p)) {
    if (log) fputs(line, log);
    line[strcspn(line, "\r\n")] = 0;
    if (line[0]) snprintf(last, n, "%s", line);
  }
  if (log) fclose(log);
  return pclose(p);
}

static void shq(char *dst, size_t n, const char *s) { /* in einfache Anführungszeichen */
  size_t k = 0;
  dst[k++] = '\'';
  for (; *s && k + 5 < n; s++) {
    if (*s == '\'') { memcpy(dst + k, "'\\''", 4); k += 4; }
    else dst[k++] = *s;
  }
  dst[k++] = '\'';
  dst[k] = 0;
}

static void mkfs_ext4(const char *disk, uint64_t off, uint64_t bytes, const char *label) {
  if (access("/usr/sbin/mke2fs", X_OK)) fail("mke2fs is missing, ext4 can’t be created here.");
  char q[160], cmd[640], last[256];
  shq(q, sizeof q, label[0] ? label : "ext4");
  /* dieselben Merkmale wie PureDarwins ext4 (ohne orphan_file), Rest wie die eingebauten Voreinstellungen */
  snprintf(cmd, sizeof cmd, "/usr/sbin/mke2fs -q -F -t ext4 -O ^orphan_file,metadata_csum_seed -L %s -E offset=%llu '/dev/r%s' %lluk",
           q, (unsigned long long)off, disk, (unsigned long long)(bytes / 1024));
  if (run_cmd(cmd, last, sizeof last)) fail("mke2fs failed: %s", last);
}

static void format_fs(const char *disk, uint64_t off, uint64_t bytes, uint32_t bs, const char *fs, const char *label) {
  /* alte Kennungen (auch von ext4 und HFS+ hinter dem ersten Block) vor dem Anlegen tilgen */
  int fd = open_raw(disk, O_RDWR);
  if (fd < 0) fail("Could not write to %s: %s", disk, strerror(errno));
  zero_range(fd, off, bytes < 4 * MIB ? bytes : 4 * MIB);
  fsync(fd);
  close(fd);
  out("STEP %s", L("Creating the file system"));
  if (!strcmp(fs, "fat32")) mkfs_fat32(disk, off, bytes, bs, label);
  else if (!strcmp(fs, "ext4")) mkfs_ext4(disk, off, bytes, label);
}

/* ---- Eingriffe ---- */

static void commit_layout(const char *disk, Layout *l) {
  int fd = open_raw(disk, O_RDWR);
  if (fd < 0) fail("Could not open %s: %s", disk, strerror(errno));
  gpt_commit(fd, &l->d, l->tab, l->disk_guid);
  fsync(fd);
  close(fd);
}

static void check_name(const char *name) {
  if (strlen(name) > 35) fail("The name is too long (35 characters at most).");
  for (const char *p = name; *p; p++)
    if ((unsigned char)*p < 32 || (unsigned char)*p > 126 || *p == '\'' || *p == '\\')
      fail("The name may only contain letters, digits and simple punctuation.");
}

static void set_entry(Layout *l, int s, const char *fs, uint64_t first, uint64_t count, const char *name, bool fresh) {
  uint8_t *e = l->tab + 128 * s;
  if (fresh) {
    memset(e, 0, 128);
    random_guid(e + 16);
  } else {
    memset(e + 56, 0, 72);
  }
  memcpy(e, !strcmp(fs, "fat32") ? GUID_BASIC : GUID_LINUX, 16);
  uint64_t last = first + count - 1;
  memcpy(e + 32, &first, 8);
  memcpy(e + 40, &last, 8);
  put16name(e, name);
}

static Layout *open_for_edit(const char *disk) {
  static Layout l;
  load_layout(disk, &l);
  if (l.d.installer) fail("This is the installer disk itself.");
  if (l.d.root) fail("The system is running from this disk, so its partitions can’t be changed.");
  if (!l.gpt) fail("The disk has no partition table. Erase it first.");
  lock_disks();
  unmount_disk(disk, 0);
  return &l;
}

static int slot_arg(Layout *l, const char *a) {
  int s = atoi(a) - 1;
  if (s < 0 || s >= 128 || !slot_used(l, s)) fail("No partition %s", a);
  return s;
}

static bool known_fs(const char *fs) { return !strcmp(fs, "fat32") || !strcmp(fs, "ext4") || !strcmp(fs, "none"); }

static void cmd_add(const char *disk, const char *first_s, const char *bytes_s, const char *fs, const char *name) {
  Layout *l = open_for_edit(disk);
  if (!known_fs(fs)) fail("Unknown format.");
  check_name(name);
  uint32_t bs = l->d.bsize;
  uint64_t first = strtoull(first_s, NULL, 10), count = strtoull(bytes_s, NULL, 10) / bs;
  Run r[130];
  int nr = free_runs(l, r, 130);
  bool inside = false;
  for (int i = 0; i < nr; i++)
    if (first >= r[i].first && first + count <= r[i].first + r[i].count) inside = true;
  if (!inside || count < MIB / bs) fail("That space is not free.");
  int s = 0;
  while (s < 128 && slot_used(l, s)) s++;
  if (s == 128) fail("The partition table is full.");
  if (strcmp(fs, "none")) format_fs(disk, first * bs, count * bs, bs, fs, name);
  else {
    int fd = open_raw(disk, O_RDWR);
    if (fd < 0) fail("Could not write to %s: %s", disk, strerror(errno));
    zero_range(fd, first * bs, count * bs < 4 * MIB ? count * bs : 4 * MIB);
    close(fd);
  }
  set_entry(l, s, fs, first, count, name, true);
  out("STEP %s", L("Updating the partition table"));
  commit_layout(disk, l);
  out("DONE %d", s + 1);
}

static void cmd_delete(const char *disk, const char *slot_s) {
  Layout *l = open_for_edit(disk);
  int s = slot_arg(l, slot_s);
  unmount_slot(disk, s);
  memset(l->tab + 128 * s, 0, 128);
  commit_layout(disk, l);
  out("DONE %d", s + 1);
}

static void cmd_format(const char *disk, const char *slot_s, const char *fs, const char *name) {
  Layout *l = open_for_edit(disk);
  int s = slot_arg(l, slot_s);
  if (!strcmp(fs, "none") || !known_fs(fs)) fail("Unknown format.");
  check_name(name);
  unmount_slot(disk, s);
  uint64_t first = slot_first(l, s), count = slot_last(l, s) - first + 1;
  format_fs(disk, first * l->d.bsize, count * l->d.bsize, l->d.bsize, fs, name);
  set_entry(l, s, fs, first, count, name, false);
  commit_layout(disk, l);
  out("DONE %d", s + 1);
}

static void cmd_resize(const char *disk, const char *slot_s, const char *bytes_s) {
  Layout *l = open_for_edit(disk);
  int s = slot_arg(l, slot_s);
  uint32_t bs = l->d.bsize;
  uint64_t unit = MIB / bs, first = slot_first(l, s), old = slot_last(l, s) - first + 1;
  uint64_t cnt = strtoull(bytes_s, NULL, 10) / bs / unit * unit;
  if (cnt < unit) fail("The partition would be too small.");
  if (cnt == old) fail("The size is unchanged.");
  const char *fs = detect_fs(disk, first * bs);
  bool ext = !strcmp(fs, "ext4");
  if (!ext && strcmp(fs, "-"))
    fail("Only ext4 partitions and partitions without a file system can be resized. Erase this one or delete it and add a new one.");
  if (cnt > old) { /* nach hinten bis zur nächsten Partition oder zum Ende der Platte */
    uint64_t limit = l->last_use + 1;
    for (int t = 0; t < 128; t++)
      if (t != s && slot_used(l, t) && slot_first(l, t) > first && slot_first(l, t) < limit) limit = slot_first(l, t);
    if (first + cnt > limit) fail("There is not enough free space behind the partition.");
  }
  if (ext && (access("/usr/sbin/resize2fs", X_OK) || access("/usr/sbin/e2fsck", X_OK)))
    fail("resize2fs is missing, ext4 can’t be resized here.");
  unmount_slot(disk, s);
  char cmd[512], last[256];
  uint64_t sectors = cnt * bs / 512;
  if (ext) {
    out("STEP %s", L("Checking the file system"));
    snprintf(cmd, sizeof cmd, "/usr/sbin/e2fsck -fp '/dev/r%s?offset=%llu'", disk, (unsigned long long)(first * bs));
    int rc = run_cmd(cmd, last, sizeof last);
    if (rc < 0 || (rc >> 8) >= 4) fail("The file system has errors: %s", last);
  }
  snprintf(cmd, sizeof cmd, "/usr/sbin/resize2fs -f '/dev/r%s?offset=%llu' %llus", disk, (unsigned long long)(first * bs),
           (unsigned long long)sectors);
  if (ext && cnt < old) { /* erst das Dateisystem verkleinern, dann die Partition */
    out("STEP %s", L("Resizing the file system"));
    if (run_cmd(cmd, last, sizeof last)) fail("The file system can’t be made that small: %s", last);
  }
  uint64_t lastlba = first + cnt - 1;
  memcpy(l->tab + 128 * s + 40, &lastlba, 8);
  out("STEP %s", L("Updating the partition table"));
  commit_layout(disk, l);
  if (ext && cnt > old) {
    out("STEP %s", L("Resizing the file system"));
    if (run_cmd(cmd, last, sizeof last)) fail("The file system could not be enlarged: %s", last);
  }
  out("DONE %d", s + 1);
}

int main(int argc, char **argv) {
  lang_init();
  if (argc >= 2 && !strcmp(argv[1], "disks")) cmd_disks();
  else if (argc == 4 && !strcmp(argv[1], "install")) cmd_install(argv[2], argv[3]);
  else if (argc == 3 && !strcmp(argv[1], "esp-cat")) return cmd_esp_cat(argv[2]);
  else if (argc == 4 && !strcmp(argv[1], "update-esp")) cmd_update_esp(argv[2], argv[3]);
  else if (argc == 3 && !strcmp(argv[1], "erase")) cmd_erase(argv[2]);
  else if (argc == 3 && !strcmp(argv[1], "layout")) cmd_layout(argv[2]);
  else if (argc == 7 && !strcmp(argv[1], "add")) cmd_add(argv[2], argv[3], argv[4], argv[5], argv[6]);
  else if (argc == 4 && !strcmp(argv[1], "delete")) cmd_delete(argv[2], argv[3]);
  else if (argc == 6 && !strcmp(argv[1], "format")) cmd_format(argv[2], argv[3], argv[4], argv[5]);
  else if (argc == 5 && !strcmp(argv[1], "resize")) cmd_resize(argv[2], argv[3], argv[4]);
  else if (argc == 3 && !strcmp(argv[1], "bootnext") && (!strcmp(argv[2], "recovery") || !strcmp(argv[2], "system")))
    cmd_bootnext(argv[2]);
  else if (argc == 2 && !strcmp(argv[1], "stick")) return cmd_stick();
  else if (argc == 4 && !strcmp(argv[1], "stick-write")) return cmd_stick_write(argv[2], argv[3]);
  else if (argc == 4 && !strcmp(argv[1], "stick-write-boot")) return cmd_stick_write_boot(argv[2], argv[3]);
  else {
    fprintf(stderr, "Aufruf: clokeos-installer disks | install <diskN> <url>|stick: | erase <diskN> | "
                    "bootnext recovery|system | stick\n");
    return 2;
  }
  return 0;
}
