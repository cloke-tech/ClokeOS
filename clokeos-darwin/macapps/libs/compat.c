/* Funktionen, die PureDarwins libSystem nicht hat, die aber die Bibliotheken in libs.sh brauchen.
 * Versteckt (nicht exportiert), damit sie nicht mit einer späteren libSystem kollidieren. */
#include <stddef.h>

__attribute__((visibility("hidden")))
int rand_r(unsigned *seed) {              /* POSIX-Beispielimplementierung */
  unsigned next = *seed;
  int result;
  next *= 1103515245; next += 12345;
  result = (unsigned)(next / 65536) % 2048;
  next *= 1103515245; next += 12345;
  result <<= 10; result ^= (unsigned)(next / 65536) % 1024;
  next *= 1103515245; next += 12345;
  result <<= 10; result ^= (unsigned)(next / 65536) % 1024;
  *seed = next;
  return result;
}
