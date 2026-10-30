/* Prüft per dlsym(RTLD_DEFAULT), ob jedes Symbol aus der Liste über libSystem auffindbar ist (wie dyld es bindet) */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  void *h = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW); FILE *f = fopen(argv[1], "r"); char line[512]; int ok = 0, bad = 0;
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\n")] = 0;
    if (!line[0]) continue;
    if (dlsym(h, line + 1)) ok++; else { bad++; printf("FEHLT %s\n", line); }
  }
  printf("aufgelöst %d, fehlend %d\n", ok, bad);
  return 0;
}
