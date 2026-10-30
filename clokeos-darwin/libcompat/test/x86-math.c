#include "../mathx.c"
#include <dlfcn.h>
#include <stdio.h>
static void *H;
#define REF(name, type) type name##_ref = (type)dlsym(H, #name)
int main(void) {
  H = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW);
  long double (*rsin)(long double) = dlsym(H, "sinl"), (*rcos)(long double) = dlsym(H, "cosl"), (*ratan)(long double) = dlsym(H, "atanl"), (*rexp2)(long double) = dlsym(H, "exp2l");
  long double (*rfmod)(long double, long double) = dlsym(H, "fmodl");
  double _Complex (*rdiv)(double, double, double, double) = dlsym(H, "__divdc3");
  float _Complex (*rdivf)(float, float, float, float) = dlsym(H, "__divsc3");
  float (*rmax)(float, float) = dlsym(H, "fmaxf"), (*rmin)(float, float) = dlsym(H, "fminf");
  long (*rlrint)(float) = dlsym(H, "lrintf"); long long (*rllround)(float) = dlsym(H, "llroundf");
  long double xs[] = {0, 0.5L, 1, -1, 3.14159265358979L, 100, -1e10L, 1e19L, 1e30L, 0x1p-40L, 2.5L, -7.75L};
  int bad = 0;
  for (unsigned i = 0; i < sizeof xs / sizeof *xs; i++) {
    long double x = xs[i];
    #define CK(f, rf, ...) { long double a = f(__VA_ARGS__), b = rf(__VA_ARGS__); long double d = a > b ? a - b : b - a; long double m = b < 0 ? -b : b; if (!(d <= 1e-15L * (m > 1 ? m : 1) || (a != a && b != b))) { bad++; printf("ABWEICHUNG %s(%Lg): %.20Lg vs %.20Lg\n", #f, x, a, b); } }
    CK(sinl, rsin, x) CK(cosl, rcos, x) CK(atanl, ratan, x)
    if (x < 1000 && x > -1000) CK(exp2l, rexp2, x)
    CK(fmodl, rfmod, x, 1.7L) CK(fmodl, rfmod, x, -0.3L)
  }
  double v[] = {1, 0, -2, INFINITY, NAN, 3e300, 1e-300};
  for (int i = 0; i < 7; i++) for (int j = 0; j < 7; j++) for (int k = 0; k < 7; k += 2) for (int l = 0; l < 7; l += 3) {
    double _Complex a = __divdc3(v[i], v[j], v[k], v[l]), b = rdiv(v[i], v[j], v[k], v[l]);
    int same = (creal(a) == creal(b) || (creal(a) != creal(a) && creal(b) != creal(b))) && (cimag(a) == cimag(b) || (cimag(a) != cimag(a) && cimag(b) != cimag(b)));
    if (!same) { bad++; printf("divdc3 (%g,%g)/(%g,%g): %g %g vs %g %g\n", v[i], v[j], v[k], v[l], creal(a), cimag(a), creal(b), cimag(b)); }
    float _Complex af = __divsc3(v[i], v[j], v[k], v[l]), bf = rdivf(v[i], v[j], v[k], v[l]);
    same = (crealf(af) == crealf(bf) || (crealf(af) != crealf(af) && crealf(bf) != crealf(bf))) && (cimagf(af) == cimagf(bf) || (cimagf(af) != cimagf(af) && cimagf(bf) != cimagf(bf)));
    if (!same) { bad++; printf("divsc3 (%g,%g)/(%g,%g): %g %g vs %g %g\n", v[i], v[j], v[k], v[l], crealf(af), cimagf(af), crealf(bf), cimagf(bf)); }
  }
  float f[] = {0, 1.5f, 2.5f, -2.5f, -0.5f, 0.49999997f, 1e10f, NAN, INFINITY, -INFINITY};
  for (int i = 0; i < 10; i++) {
    for (int j = 0; j < 10; j++) {
      float a = fmaxf(f[i], f[j]), b = rmax(f[i], f[j]); if (!(a == b || (a != a && b != b))) { bad++; printf("fmaxf %g %g\n", f[i], f[j]); }
      a = fminf(f[i], f[j]); b = rmin(f[i], f[j]); if (!(a == b || (a != a && b != b))) { bad++; printf("fminf %g %g\n", f[i], f[j]); }
    }
    if (f[i] == f[i] && f[i] < 1e9f && f[i] > -1e9f) {
      if (lrintf(f[i]) != rlrint(f[i])) { bad++; printf("lrintf %g: %ld vs %ld\n", f[i], lrintf(f[i]), rlrint(f[i])); }
      if (llroundf(f[i]) != rllround(f[i])) { bad++; printf("llroundf %g\n", f[i]); }
    }
  }
  printf("x86_64-Mathe gegen die Mac-libm: %d Abweichungen\n", bad);
  return bad != 0;
}
