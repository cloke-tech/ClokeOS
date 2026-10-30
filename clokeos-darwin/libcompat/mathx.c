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

#if defined(__x86_64__)
/* ---- nur x86_64: PureDarwins libm hat diese Funktionen nicht (auf arm64 sind sie da) ---- */

float fmaxf(float a, float b) {
  if (__builtin_isnan(a)) return b;
  if (__builtin_isnan(b)) return a;
  return a < b ? b : a;
}

float fminf(float a, float b) {
  if (__builtin_isnan(a)) return b;
  if (__builtin_isnan(b)) return a;
  return b < a ? b : a;
}

/* Rundung "halb weg von null" in double: für float exakt */
long long llroundf(float x) {
  double d = x;
  return (long long)(d >= 0 ? floor(d + 0.5) : ceil(d - 0.5));
}

long lrintf(float x) {
  long r;
  __asm__("cvtss2si %1, %0" : "=r"(r) : "x"(x));   /* aktueller Rundungsmodus */
  return r;
}

/* long double: x87-Befehle (64 Bit Mantisse). fsin/fcos/fpatan sind nur im Bereich |x| < 2^63
 * definiert; darüber wird mit fprem1 durch 2 pi reduziert (die Genauigkeit des x87-pi begrenzt
 * das Ergebnis bei sehr großen Argumenten wie bei jedem x87-libm). */
static long double reduce_2pi(long double x) {
  long double two_pi;
  __asm__("fldpi; fadd %%st(0), %%st" : "=t"(two_pi));
  long double r;
  __asm__("1: fprem1; fnstsw %%ax; testb $4, %%ah; jnz 1b; fstp %%st(1)" : "=t"(r) : "0"(x), "u"(two_pi) : "ax", "cc");
  return r;
}

long double sinl(long double x) {
  long double r;
  if (isinf(x)) return __builtin_nanl("");
  if (__builtin_fabsl(x) > 0x1p62L) x = reduce_2pi(x);
  __asm__("fsin" : "=t"(r) : "0"(x));
  return r;
}

long double cosl(long double x) {
  long double r;
  if (isinf(x)) return __builtin_nanl("");
  if (__builtin_fabsl(x) > 0x1p62L) x = reduce_2pi(x);
  __asm__("fcos" : "=t"(r) : "0"(x));
  return r;
}

long double atanl(long double x) {
  long double r;
  __asm__("fld1; fpatan" : "=t"(r) : "0"(x));
  return r;
}

long double fmodl(long double x, long double y) {
  long double r;
  if (y == 0 || isinf(x) || isnan(x) || isnan(y)) return __builtin_nanl("");
  if (isinf(y)) return x;
  __asm__("1: fprem; fnstsw %%ax; testb $4, %%ah; jnz 1b; fstp %%st(1)" : "=t"(r) : "0"(x), "u"(y) : "ax", "cc");
  return r;
}

long double exp2l(long double x) {
  long double r;
  if (isnan(x)) return x;
  if (x > 16384.0L) return __builtin_infl();
  if (x < -16446.0L) return 0.0L;
  /* 2^x = 2^int(x) * 2^frac(x); f2xm1 liefert 2^f - 1 für |f| <= 1 */
  long double i, f;
  __asm__("frndint" : "=t"(i) : "0"(x));
  f = x - i;
  __asm__("f2xm1; fld1; faddp" : "=t"(f) : "0"(f));
  __asm__("fscale; fstp %%st(1)" : "=t"(r) : "0"(f), "u"(i));
  return r;
}

/* komplexe Division nach C99 Anhang G.5.1 mit Skalierung gegen Über- und Unterlauf */
double _Complex __divdc3(double a, double b, double c, double d) {
  int ilogbw = 0;
  double logbw = logb(fmax(fabs(c), fabs(d)));
  if (isfinite(logbw)) { ilogbw = (int)logbw; c = scalbn(c, -ilogbw); d = scalbn(d, -ilogbw); }
  double denom = c * c + d * d;
  double x = scalbn((a * c + b * d) / denom, -ilogbw);
  double y = scalbn((b * c - a * d) / denom, -ilogbw);
  if (isnan(x) && isnan(y)) {
    if (denom == 0.0 && (!isnan(a) || !isnan(b))) {
      x = copysign(INFINITY, c) * a; y = copysign(INFINITY, c) * b;
    } else if ((isinf(a) || isinf(b)) && isfinite(c) && isfinite(d)) {
      a = copysign(isinf(a) ? 1.0 : 0.0, a); b = copysign(isinf(b) ? 1.0 : 0.0, b);
      x = INFINITY * (a * c + b * d); y = INFINITY * (b * c - a * d);
    } else if (isinf(logbw) && logbw > 0.0 && isfinite(a) && isfinite(b)) {
      c = copysign(isinf(c) ? 1.0 : 0.0, c); d = copysign(isinf(d) ? 1.0 : 0.0, d);
      x = 0.0 * (a * c + b * d); y = 0.0 * (b * c - a * d);
    }
  }
  double _Complex r;
  __real__ r = x; __imag__ r = y;
  return r;
}

float _Complex __divsc3(float a, float b, float c, float d) {
  int ilogbw = 0;
  float logbw = logbf(fmaxf(fabsf(c), fabsf(d)));
  if (isfinite(logbw)) { ilogbw = (int)logbw; c = scalbnf(c, -ilogbw); d = scalbnf(d, -ilogbw); }
  float denom = c * c + d * d;
  float x = scalbnf((a * c + b * d) / denom, -ilogbw);
  float y = scalbnf((b * c - a * d) / denom, -ilogbw);
  if (isnan(x) && isnan(y)) {
    if (denom == 0.0f && (!isnan(a) || !isnan(b))) {
      x = copysignf(INFINITY, c) * a; y = copysignf(INFINITY, c) * b;
    } else if ((isinf(a) || isinf(b)) && isfinite(c) && isfinite(d)) {
      a = copysignf(isinf(a) ? 1.0f : 0.0f, a); b = copysignf(isinf(b) ? 1.0f : 0.0f, b);
      x = INFINITY * (a * c + b * d); y = INFINITY * (b * c - a * d);
    } else if (isinf(logbw) && logbw > 0.0f && isfinite(a) && isfinite(b)) {
      c = copysignf(isinf(c) ? 1.0f : 0.0f, c); d = copysignf(isinf(d) ? 1.0f : 0.0f, d);
      x = 0.0f * (a * c + b * d); y = 0.0f * (b * c - a * d);
    }
  }
  float _Complex r;
  __real__ r = x; __imag__ r = y;
  return r;
}
#endif
