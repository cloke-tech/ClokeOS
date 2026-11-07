// bench-raven.c – Rechenkerne für den Vergleich der Raven-Engines (Unicorn / JIT) mit nativem arm64 und Rosetta 2.
// Windows (mingw): x86_64-w64-mingw32-gcc -O2 bench-raven.c -o bench-raven.exe;  Mac: clang -O2 [-target x86_64-apple-macos11] bench-raven.c
// Jeder Kern läuft mit fester Arbeitsmenge; Ausgabe: Zeit in ms und eine Prüfsumme (muss in allen Läufen gleich sein).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return 1000.0 * (double)c.QuadPart / (double)f.QuadPart; }
#else
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec * 1e-6; }
#endif
static uint64_t fib(int n) { return n < 2 ? (uint64_t)n : fib(n - 1) + fib(n - 2); }
static uint32_t crc(const uint8_t *p, size_t n) { uint32_t c = ~0u; for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); } return ~c; }
static double flt(int n) { double s = 0; for (int i = 1; i <= n; i++) s += 1.0 / ((double)i * i) + sqrt((double)i) * 1e-9; return s; }
static float vsum(const float *a, const float *b, int n) { float s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i] + a[i]; return s; }
static uint8_t buf[1 << 20], buf2[1 << 20]; static uint32_t mem[1 << 20]; static float fa[1 << 16], fb[1 << 16];
typedef struct N { struct N *next; int v; } N; static N nodes[1 << 16];
int main(void) {
	double t, r; uint32_t c = 0; uint64_t f; uint32_t x = 1; uint64_t sum = 0;
	t = now(); f = fib(35); r = now() - t; printf("fib(35)          %8.0f ms  (%llu)\n", r, (unsigned long long)f);
	for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 31);
	t = now(); for (int k = 0; k < 4; k++) { buf[0] = (uint8_t)k; c += crc(buf, sizeof buf); } r = now() - t; printf("crc32 bitwise 4MB%8.0f ms  (%08x)\n", r, c);
	t = now(); double s = flt(20000000); r = now() - t; printf("float sqrt/div   %8.0f ms  (%.6f)\n", r, s);
	t = now(); for (int k = 0; k < 20; k++) for (size_t i = 0; i < (1 << 20); i++) { x = x * 1664525u + 1013904223u; mem[x & ((1 << 20) - 1)] += x; } r = now() - t; printf("random RMW 4MB   %8.0f ms  (%u)\n", r, mem[5]);
	for (int i = 0; i < (1 << 16); i++) { fa[i] = (float)(i & 255) * 0.5f; fb[i] = (float)((i * 7) & 255) * 0.25f; }
	t = now(); float vs = 0; for (int k = 0; k < 2000; k++) vs += vsum(fa, fb, 1 << 16); r = now() - t; printf("float vector sum %8.0f ms  (%.1f)\n", r, vs);
	t = now(); for (int k = 0; k < 400; k++) { memcpy(buf2, buf, sizeof buf); buf[k] ^= buf2[k + 1]; } r = now() - t; printf("memcpy 400 MiB   %8.0f ms  (%u)\n", r, buf2[7]);
	for (int i = 0; i < (1 << 16); i++) nodes[i].next = &nodes[(i * 40503u + 1) & 0xffff], nodes[i].v = i;
	t = now(); N *p = nodes; for (int i = 0; i < 20000000; i++) { sum += p->v; p = p->next; } r = now() - t; printf("pointer chasing  %8.0f ms  (%llu)\n", r, (unsigned long long)sum);
	return 0;
}
