/* libclokeos_compat, Teil 6: Compiler- und libm-Hilfsfunktionen, die Apples SDK 26 in Programme
 * einbaut (clang ruft sie für komplexe Multiplikation, Ganzzahlpotenzen, simd) und die
 * PureDarwins libSystem 20.5 nicht exportiert. Eigene Implementierungen nach C99 Anhang G und
 * den Angaben in <simd/>; Vergleich gegen die Mac-libSystem in docs/logic-libsystem.md. */
#include <math.h>
#include <simd/simd.h>
#include <stdint.h>
#include <string.h>

int __isinfd(double x) { uint64_t u; memcpy(&u, &x, 8); return (u & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL; }
int __isnand(double x) { uint64_t u; memcpy(&u, &x, 8); return (u & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL; }
int __isinff(float x) { uint32_t u; memcpy(&u, &x, 4); return (u & 0x7FFFFFFFu) == 0x7F800000u; }

/* ---- komplexe Multiplikation mit Wiederherstellung von Unendlichkeiten (C99 G.5.1) ---- */
static double copysign_inf_or_zero(double isinf_val, double sign) { return __builtin_copysign(isinf_val, sign); }

double _Complex __muldc3(double a, double b, double c, double d) {
  double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
  double x = ac - bd, y = ad + bc;
  if (__builtin_isnan(x) && __builtin_isnan(y)) {
    int recalc = 0;
    if (__builtin_isinf(a) || __builtin_isinf(b)) {
      a = copysign_inf_or_zero(__builtin_isinf(a) ? 1 : 0, a);
      b = copysign_inf_or_zero(__builtin_isinf(b) ? 1 : 0, b);
      if (__builtin_isnan(c)) c = __builtin_copysign(0, c);
      if (__builtin_isnan(d)) d = __builtin_copysign(0, d);
      recalc = 1;
    }
    if (__builtin_isinf(c) || __builtin_isinf(d)) {
      c = copysign_inf_or_zero(__builtin_isinf(c) ? 1 : 0, c);
      d = copysign_inf_or_zero(__builtin_isinf(d) ? 1 : 0, d);
      if (__builtin_isnan(a)) a = __builtin_copysign(0, a);
      if (__builtin_isnan(b)) b = __builtin_copysign(0, b);
      recalc = 1;
    }
    if (!recalc && (__builtin_isinf(ac) || __builtin_isinf(bd) || __builtin_isinf(ad) || __builtin_isinf(bc))) {
      if (__builtin_isnan(a)) a = __builtin_copysign(0, a);
      if (__builtin_isnan(b)) b = __builtin_copysign(0, b);
      if (__builtin_isnan(c)) c = __builtin_copysign(0, c);
      if (__builtin_isnan(d)) d = __builtin_copysign(0, d);
      recalc = 1;
    }
    if (recalc) { x = __builtin_inf() * (a * c - b * d); y = __builtin_inf() * (a * d + b * c); }
  }
  double _Complex r;
  __real__ r = x; __imag__ r = y;
  return r;
}

float _Complex __mulsc3(float a, float b, float c, float d) {
  float ac = a * c, bd = b * d, ad = a * d, bc = b * c;
  float x = ac - bd, y = ad + bc;
  if (__builtin_isnan(x) && __builtin_isnan(y)) {
    int recalc = 0;
    if (__builtin_isinf(a) || __builtin_isinf(b)) {
      a = __builtin_copysignf(__builtin_isinf(a) ? 1 : 0, a);
      b = __builtin_copysignf(__builtin_isinf(b) ? 1 : 0, b);
      if (__builtin_isnan(c)) c = __builtin_copysignf(0, c);
      if (__builtin_isnan(d)) d = __builtin_copysignf(0, d);
      recalc = 1;
    }
    if (__builtin_isinf(c) || __builtin_isinf(d)) {
      c = __builtin_copysignf(__builtin_isinf(c) ? 1 : 0, c);
      d = __builtin_copysignf(__builtin_isinf(d) ? 1 : 0, d);
      if (__builtin_isnan(a)) a = __builtin_copysignf(0, a);
      if (__builtin_isnan(b)) b = __builtin_copysignf(0, b);
      recalc = 1;
    }
    if (!recalc && (__builtin_isinf(ac) || __builtin_isinf(bd) || __builtin_isinf(ad) || __builtin_isinf(bc))) {
      if (__builtin_isnan(a)) a = __builtin_copysignf(0, a);
      if (__builtin_isnan(b)) b = __builtin_copysignf(0, b);
      if (__builtin_isnan(c)) c = __builtin_copysignf(0, c);
      if (__builtin_isnan(d)) d = __builtin_copysignf(0, d);
      recalc = 1;
    }
    if (recalc) { x = __builtin_inff() * (a * c - b * d); y = __builtin_inff() * (a * d + b * c); }
  }
  float _Complex r;
  __real__ r = x; __imag__ r = y;
  return r;
}

double __powidf2(double a, int b) {
  int recip = b < 0;
  double r = 1;
  for (;;) {
    if (b & 1) r *= a;
    b /= 2;
    if (b == 0) break;
    a *= a;
  }
  return recip ? 1 / r : r;
}

float __powisf2(float a, int b) {
  int recip = b < 0;
  float r = 1;
  for (;;) {
    if (b & 1) r *= a;
    b /= 2;
    if (b == 0) break;
    a *= a;
  }
  return recip ? 1 / r : r;
}

/* cos(pi x), exakt an den Stellen, wo das Ergebnis 0, +-1 oder +-0,5-Vielfache ist: erst x auf
 * [0, 1/2] zurückführen, dann erst mit pi malnehmen */
double __cospi(double x) {
  if (__builtin_isnan(x) || __builtin_isinf(x)) return __builtin_nan("");
  double r = fmod(__builtin_fabs(x), 2.0);   /* [0, 2) */
  if (r > 1) r = 2 - r;                       /* cos(pi r) ist symmetrisch um 1 */
  double sign = 1;
  if (r > 0.5) { r = 1 - r; sign = -1; }       /* cos(pi (1-r)) = -cos(pi r) */
  if (r == 0.5) return 0;
  const double pi = 3.14159265358979323846264338327950288;
  if (r > 0.25) return sign * sin(pi * (0.5 - r));
  return sign * cos(pi * r);
}

/* ---- simd ---- */
const simd_double3x3 matrix_identity_double3x3 = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

simd_double3x3 __invert_d3(simd_double3x3 m) {
  simd_double3 c0 = m.columns[0], c1 = m.columns[1], c2 = m.columns[2];
  simd_double3 r0 = {c1.y * c2.z - c1.z * c2.y, c1.z * c2.x - c1.x * c2.z, c1.x * c2.y - c1.y * c2.x};   /* c1 x c2 */
  simd_double3 r1 = {c2.y * c0.z - c2.z * c0.y, c2.z * c0.x - c2.x * c0.z, c2.x * c0.y - c2.y * c0.x};   /* c2 x c0 */
  simd_double3 r2 = {c0.y * c1.z - c0.z * c1.y, c0.z * c1.x - c0.x * c1.z, c0.x * c1.y - c0.y * c1.x};   /* c0 x c1 */
  double det = c0.x * r0.x + c0.y * r0.y + c0.z * r0.z;
  double inv = 1.0 / det;
  /* r0, r1, r2 sind die Zeilen der Inversen (mal det); die Spalten sind deren Transponierte */
  simd_double3x3 out;
  out.columns[0] = (simd_double3){r0.x, r1.x, r2.x} * inv;
  out.columns[1] = (simd_double3){r0.y, r1.y, r2.y} * inv;
  out.columns[2] = (simd_double3){r0.z, r1.z, r2.z} * inv;
  return out;
}

simd_float4 _simd_fma_f4(simd_float4 x, simd_float4 y, simd_float4 z) {
  return (simd_float4){fmaf(x.x, y.x, z.x), fmaf(x.y, y.y, z.y), fmaf(x.z, y.z, z.z), fmaf(x.w, y.w, z.w)};
}
simd_float4 _simd_pow_f4(simd_float4 x, simd_float4 y) {
  return (simd_float4){powf(x.x, y.x), powf(x.y, y.y), powf(x.z, y.z), powf(x.w, y.w)};
}
simd_float4 _simd_round_f4(simd_float4 x) {
  return (simd_float4){roundf(x.x), roundf(x.y), roundf(x.z), roundf(x.w)};
}
