/* Prüfprogramm für macapps/libs.sh: ruft je eine Funktion aus libsqlite3, libxml2, libbz2, libpcre
 * und libxar auf. Ausgabe "OK <lib>" oder "FEHLER <lib>: …", Rückgabewert = Zahl der Fehler.
 *   libstest [paket.pkg]      mit Paket: liest dessen TOC mit libxar */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <bzlib.h>
#include <pcre.h>
#include <xar/xar.h>

static int fails;
static void ok(const char *lib, int good, const char *what) {
  if (good) printf("OK %s %s\n", lib, what); else { printf("FEHLER %s: %s\n", lib, what); fails++; }
}

static void t_sqlite(void) {
  sqlite3 *db; char *err = NULL; int n = 0;
  if (sqlite3_open(":memory:", &db)) { ok("sqlite3", 0, "open"); return; }
  int rc = sqlite3_exec(db, "create table t(a integer, b text); insert into t values (1,'eins'),(2,'zwei'),(3,'drei');"
    "create virtual table f using fts5(x); insert into f values ('hallo ClokeOS welt');", NULL, NULL, &err);
  ok("sqlite3", rc == 0, err ? err : "create/insert/fts5");
  sqlite3_stmt *st;
  rc = sqlite3_prepare_v2(db, "select sum(a), json_extract('{\"k\":[10,20]}','$.k[1]'), (select count(*) from f where f match 'clokeos') from t", -1, &st, NULL);
  if (rc == 0 && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0) * 10000 + sqlite3_column_int(st, 1) * 10 + sqlite3_column_int(st, 2);
  ok("sqlite3", n == 60000 + 200 + 1, "select/json/fts5 match");
  sqlite3_finalize(st); sqlite3_close(db);
  printf("   sqlite %s\n", sqlite3_libversion());
}

static void t_xml(void) {
  const char *doc = "<a><b n='1'/><b n='2'/><c>text &amp; mehr</c></a>";
  xmlDocPtr d = xmlReadMemory(doc, (int)strlen(doc), "x.xml", NULL, 0);
  ok("xml2", d != NULL, "parse");
  if (!d) return;
  xmlXPathContextPtr cx = xmlXPathNewContext(d);
  xmlXPathObjectPtr o = xmlXPathEvalExpression((const xmlChar *)"count(//b)", cx);
  ok("xml2", o && o->type == XPATH_NUMBER && o->floatval == 2, "xpath count");
  xmlXPathFreeObject(o);
  o = xmlXPathEvalExpression((const xmlChar *)"string(//c)", cx);
  ok("xml2", o && o->stringval && !strcmp((char *)o->stringval, "text & mehr"), "entity");
  xmlXPathFreeObject(o); xmlXPathFreeContext(cx); xmlFreeDoc(d);
  printf("   libxml2 %s\n", xmlParserVersion);
}

static void t_bz2(void) {
  enum { N = 200000 };
  char *in = malloc(N), *out = malloc(N * 2), *back = malloc(N);
  for (int i = 0; i < N; i++) in[i] = (char)("ClokeOS "[i & 7] ^ (i >> 9));
  unsigned olen = N * 2, blen = N;
  int r1 = BZ2_bzBuffToBuffCompress(out, &olen, in, N, 9, 0, 0);
  int r2 = BZ2_bzBuffToBuffDecompress(back, &blen, out, olen, 0, 0);
  ok("bz2", r1 == BZ_OK && r2 == BZ_OK && blen == N && !memcmp(in, back, N), "Hin- und Rückweg");
  printf("   bzip2 %s, %d -> %u Bytes\n", BZ2_bzlibVersion(), N, olen);
  free(in); free(out); free(back);
}

static void t_pcre(void) {
  const char *e; int eo, ov[30];
  pcre *re = pcre_compile("(\\d+)-(\\w+)", PCRE_UTF8, &e, &eo, NULL);
  int rc = re ? pcre_exec(re, NULL, "xx 42-Logic", 11, 0, 0, ov, 30) : -99;
  ok("pcre", rc == 3 && ov[2] == 3 && ov[5] == 11, "compile/exec");
  printf("   pcre %s\n", pcre_version());
  if (re) pcre_free(re);
}

static void t_xar(const char *pkg) {
  char tmp[] = "/tmp/libstest.XXXXXX";
  int fd = mkstemp(tmp); if (fd >= 0) close(fd);
  xar_t x = xar_open(tmp, WRITE);
  ok("xar", x != NULL, "open write");
  if (x) {
    xar_opt_set(x, XAR_OPT_COMPRESSION, "gzip");
    xar_opt_set(x, XAR_OPT_TOCCKSUM, "sha1");
    xar_file_t f = xar_add_frombuffer(x, NULL, "hallo.txt", "Hallo ClokeOS Hallo ClokeOS\n", 28);
    ok("xar", f != NULL, "add");
    ok("xar", xar_close(x) == 0, "close (TOC schreiben, SHA-1)");
  }
  x = xar_open(tmp, READ);
  int found = 0;
  if (x) {
    xar_iter_t it = xar_iter_new();
    for (xar_file_t f = xar_file_first(x, it); f; f = xar_file_next(it)) {
      char *buf = NULL; size_t sz = 0;
      if (xar_extract_tobuffersz(x, f, &buf, &sz) == 0 && sz == 28 && !memcmp(buf, "Hallo", 5)) found = 1;
      free(buf);
    }
    xar_iter_free(it); xar_close(x);
  }
  ok("xar", found, "zurücklesen mit Prüfsummen");
  unlink(tmp);
  if (pkg) {
    x = xar_open(pkg, READ); int n = 0;
    if (x) {
      xar_iter_t it = xar_iter_new();
      for (xar_file_t f = xar_file_first(x, it); f; f = xar_file_next(it)) n++;
      xar_iter_free(it); xar_close(x);
    }
    printf("   %s: %d Einträge im TOC\n", pkg, n);
    ok("xar", n > 0, "Paket lesen");
  }
}

int main(int argc, char **argv) {
  t_sqlite(); t_xml(); t_bz2(); t_pcre(); t_xar(argc > 1 ? argv[1] : NULL);
  printf("%s (%d Fehler)\n", fails ? "FEHLGESCHLAGEN" : "ALLES OK", fails);
  return fails;
}
