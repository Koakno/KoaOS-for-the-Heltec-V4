/* Host-side self test: runs the SDK's printf against the PC's libc. No wasm toolchain needed.
 *   make selftest
 */
#include <stdio.h>
#include <string.h>
#include "koaos.h"

static char out[1024]; static int outn;
int32_t koa_write(int32_t fd, const void *b, int32_t n) { (void)fd; int m = n > 3 ? 3 : n; memcpy(out + outn, b, (size_t)m); outn += m; return m; } /* partial writes on purpose */
int32_t koa_yield(void) { return KE_OK; }
void koa_exit(int32_t rc) { (void)rc; }
int koa_main(void) { return 0; }

static int fails;
#define CHECK(...) do { char a[256], b[256]; \
  int na = koa_snprintf(a, sizeof a, __VA_ARGS__); int nb = snprintf(b, sizeof b, __VA_ARGS__); \
  if (na != nb || strcmp(a, b)) { printf("FAIL %-22s -> '%s' (%d)  libc '%s' (%d)\n", #__VA_ARGS__, a, na, b, nb); fails++; } } while (0)

int main(void) {
  CHECK("%d|%i|%5d|%-5d|%05d|%+d|% d", 42, -7, 42, 42, 42, 42, 42);
  CHECK("%d %d %d", 0, -2147483647 - 1, 2147483647);
  CHECK("%u|%x|%X|%08x|%#x|%#X", 4000000000u, 255u, 255u, 48879u, 255u, 255u);
  CHECK("%#x", 0u);
  CHECK("%ld|%lld|%llu|%lx", -123456789L, -9000000000000000000LL, 18000000000000000000ULL, 0xDEADBEEFUL);
  CHECK("%zu|%hd|%hhu", (size_t)77, (short)-5, (unsigned char)200);
  CHECK("%s|%10s|%-10s|%.3s|%s", "abc", "abc", "abc", "abcdef", "");
  CHECK("%c%c%c|%3c", 'a', 'b', 'c', 'z');
  CHECK("%%|100%%");
  CHECK("%.3d|%8.3d|%-8.3d|", 7, 7, 7);
  CHECK("%*d|%-*d|%.*s", 6, 12, 6, 12, 2, "hello");
  CHECK("%f|%.0f|%.1f|%.2f|%.3f", 3.14159265, 2.6, 2.26, 1.234, -0.5);
  CHECK("%8.3f|%-8.2f|%08.2f|%+.1f", 3.14159, 3.14159, -3.14159, 2.0);
  CHECK("%.6f|%.9f", 37.7749295, -122.4194155);
  CHECK("%f|%.2f", 0.0, 99.999);
  CHECK("%.2f|%.2f", 123456789.126, 0.994);
  /* truncation: result is cut and NUL-terminated, return value is the full length */
  { char t[8]; int n = koa_snprintf(t, sizeof t, "%s", "0123456789"); if (n != 10 || strcmp(t, "0123456")) { printf("FAIL truncation '%s' %d\n", t, n); fails++; } }
  { char t[1]; int n = koa_snprintf(t, 0, "abc"); if (n != 3) { printf("FAIL cap0\n"); fails++; } }
  { char t[16]; const char *np = 0; koa_snprintf(t, sizeof t, "%s", np); if (strcmp(t, "(null)")) { printf("FAIL null str\n"); fails++; } }
  { char t[16]; koa_snprintf(t, sizeof t, "%p", (void *)0x1234); if (strcmp(t, "0x1234")) { printf("FAIL %%p '%s'\n", t); fails++; } }
  /* console output goes through koa_write even when it only accepts 3 bytes at a time */
  outn = 0; koa_printf("hello %d\n", 12345); out[outn] = 0;
  if (strcmp(out, "hello 12345\n")) { printf("FAIL koa_printf '%s'\n", out); fails++; }
  outn = 0; koa_puts("line"); out[outn] = 0;
  if (strcmp(out, "line\n")) { printf("FAIL koa_puts '%s'\n", out); fails++; }
  if (strcmp(koa_strerror(KE_PERM), "not permitted (capability missing in manifest, or not acquired)")) { printf("FAIL strerror\n"); fails++; }
  printf(fails ? "%d FAILED\n" : "all printf/output tests passed\n", fails);
  return fails != 0;
}
