/*
 * clokeos-xtest: Maus und Tastatur für automatische Tests der Oberfläche, über die serielle
 * Konsole aufgerufen (vmctl.py click/key/type). Die VM hat nur eine relative USB-Maus, die sich von
 * außen nicht genau setzen lässt; XTest erzeugt dagegen echte Ereignisse an genauen Stellen.
 * PureDarwin hat keine libXtst, deshalb schickt das Programm die Anfrage XTestFakeInput selbst.
 *
 *   DISPLAY=:0 clokeos-xtest click <x> <y> [taste]   bewegen und klicken (1 links, 3 rechts)
 *   DISPLAY=:0 clokeos-xtest move <x> <y>
 *   DISPLAY=:0 clokeos-xtest key <name> [...]        Tasten (X-Namen: Return, Escape, a, F4, ...)
 *   DISPLAY=:0 clokeos-xtest type <text>             Text tippen (ASCII)
 */
#include <X11/Xlibint.h>
#include <X11/XKBlib.h>
#include <X11/extensions/xtestproto.h>
#include <X11/keysym.h>
#include <strings.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int major;

static void fake(Display *dpy, int type, int detail, int x, int y) {
  xXTestFakeInputReq *req;
  LockDisplay(dpy);
  GetReq(XTestFakeInput, req);
  req->reqType = major;
  req->xtReqType = X_XTestFakeInput;
  req->type = type;
  req->detail = detail;
  req->time = CurrentTime;
  req->root = type == MotionNotify ? DefaultRootWindow(dpy) : None;
  req->rootX = x;
  req->rootY = y;
  req->deviceid = 0;
  UnlockDisplay(dpy);
  SyncHandle();
  XSync(dpy, False);
}

static void key(Display *dpy, KeySym ks);
// Taste mit Umschalttasten, z. B. ctrl+s, ctrl+shift+s, alt+F4: die Umschalttasten bleiben während der Taste gedrückt
static void chord(Display *dpy, char *spec) {
  KeyCode held[4];
  int count = 0;
  char *last = spec;
  for (char *plus; (plus = strchr(last, '+')) && plus[1]; last = plus + 1) {
    *plus = 0;
    KeySym m = !strcasecmp(last, "ctrl") ? XK_Control_L : !strcasecmp(last, "shift") ? XK_Shift_L :
               !strcasecmp(last, "alt") ? XK_Alt_L : !strcasecmp(last, "super") ? XK_Super_L : NoSymbol;
    if (m == NoSymbol || count == 4) {
      fprintf(stderr, "clokeos-xtest: unbekannte Umschalttaste %s\n", last);
      return;
    }
    held[count] = XKeysymToKeycode(dpy, m);
    fake(dpy, KeyPress, held[count++], 0, 0);
  }
  KeySym ks = XStringToKeysym(last);
  if (ks == NoSymbol) fprintf(stderr, "clokeos-xtest: unbekannte Taste %s\n", last);
  else key(dpy, ks);
  while (count > 0) fake(dpy, KeyRelease, held[--count], 0, 0);
}

static void key(Display *dpy, KeySym ks) {
  KeyCode kc = XKeysymToKeycode(dpy, ks);
  if (!kc) {
    fprintf(stderr, "clokeos-xtest: keine Taste für %s\n", XKeysymToString(ks));
    return;
  }
  bool shift = ks != XkbKeycodeToKeysym(dpy, kc, 0, 0) && ks == XkbKeycodeToKeysym(dpy, kc, 0, 1);
  KeyCode sk = XKeysymToKeycode(dpy, XK_Shift_L);
  if (shift) fake(dpy, KeyPress, sk, 0, 0);
  fake(dpy, KeyPress, kc, 0, 0);
  fake(dpy, KeyRelease, kc, 0, 0);
  if (shift) fake(dpy, KeyRelease, sk, 0, 0);
  usleep(30000);
}

int main(int argc, char **argv) {
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "clokeos-xtest: keine Verbindung zum Bildschirm (DISPLAY=%s)\n", getenv("DISPLAY") ?: "");
    return 1;
  }
  int ev, err;
  if (!XQueryExtension(dpy, XTestExtensionName, &major, &ev, &err)) {
    fprintf(stderr, "clokeos-xtest: Xorg hat kein XTEST\n");
    return 1;
  }
  if (argc >= 4 && (!strcmp(argv[1], "click") || !strcmp(argv[1], "move"))) {
    int x = atoi(argv[2]), y = atoi(argv[3]), b = argc > 4 ? atoi(argv[4]) : 1;
    fake(dpy, MotionNotify, False, x, y);
    usleep(80000);
    if (!strcmp(argv[1], "click")) {
      fake(dpy, ButtonPress, b, 0, 0);
      usleep(60000);
      fake(dpy, ButtonRelease, b, 0, 0);
    }
  } else if (argc >= 3 && !strcmp(argv[1], "key")) {
    for (int i = 2; i < argc; i++) {
      if (strchr(argv[i], '+') && argv[i][strlen(argv[i]) - 1] != '+') {
        chord(dpy, argv[i]);
        continue;
      }
      KeySym ks = XStringToKeysym(argv[i]);
      if (ks == NoSymbol) fprintf(stderr, "clokeos-xtest: unbekannte Taste %s\n", argv[i]);
      else key(dpy, ks);
    }
  } else if (argc >= 3 && !strcmp(argv[1], "type")) {
    for (const char *p = argv[2]; *p; p++) key(dpy, *p == ' ' ? XK_space : (KeySym)(unsigned char)*p);
  } else {
    fprintf(stderr, "Aufruf: clokeos-xtest click|move <x> <y> [taste] | key <name>... | type <text>\n");
    return 2;
  }
  XCloseDisplay(dpy);
  return 0;
}
