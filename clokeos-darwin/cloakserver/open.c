/*
 * open: Dateien, Ordner und Programme öffnen wie unter macOS (/usr/bin/open). Den Auftrag führt CloakServer aus
 * (Programme starten, Ordner in Fundus, Dokumente mit ihrem Standardprogramm); open hängt ihn als Zeile an die
 * Eigenschaft _CLOKE_OPEN des Root-Fensters ("with<TAB>flags<TAB>pfad").
 *
 *   open pfad …               öffnen (Ordner in Fundus, .app starten, Dokumente mit dem Standardprogramm)
 *   open -a Programm [pfad …] mit einem bestimmten Programm (Name, z. B. Notes, oder Pfad zum .app)
 *   open -b kennung [pfad …]  mit dem Programm dieser Kennung (com.clokeos.fundus …)
 *   open -R pfad …            in Fundus zeigen
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void) {
  fprintf(stderr, "usage: open [-a application | -b bundle-id] [-R] [file ...]\n");
  exit(1);
}

int main(int argc, char **argv) {
  const char *with = NULL;
  int reveal = 0, opt;
  while ((opt = getopt(argc, argv, "a:b:Rh")) != -1) {
    switch (opt) {
    case 'a': case 'b': with = optarg; break;
    case 'R': reveal = 1; break;
    default: usage();
    }
  }
  if (optind >= argc && !with) usage();
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) { fprintf(stderr, "open: no display (DISPLAY not set?)\n"); return 1; }
  Atom prop = XInternAtom(dpy, "_CLOKE_OPEN", False), utf8 = XInternAtom(dpy, "UTF8_STRING", False);
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  int any = 0;
  for (int i = optind; i < argc || (!any && with && i == optind); i++) {
    char abs[PATH_MAX] = "";
    if (i < argc) {
      if (!realpath(argv[i], abs)) { fprintf(stderr, "open: %s does not exist\n", argv[i]); continue; }
    }
    char line[PATH_MAX + 512];
    int n = snprintf(line, sizeof line, "%s\t%s\t%s\n", with ? with : "-", reveal ? "R" : "", abs);
    if (len + n + 1 > cap) { cap = (len + n + 1) * 2; buf = realloc(buf, cap); }
    memcpy(buf + len, line, n);
    len += n;
    any = 1;
    if (i >= argc) break;
  }
  if (len) {
    XChangeProperty(dpy, DefaultRootWindow(dpy), prop, utf8, 8, PropModeAppend, (unsigned char *)buf, (int)len);
    XSync(dpy, False);
  }
  XCloseDisplay(dpy);
  return any ? 0 : 1;
}
