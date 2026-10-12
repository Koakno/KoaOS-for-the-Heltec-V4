/* hellowasm -- the smallest useful KoaOS app. Needs no capabilities. */
#include "koaos.h"

int koa_main(void) {
  koa_puts("Hello from a KoaOS wasm app!");
  koa_printf("ABI version %d, uptime %u ms\n", (int)koa_abi_version(), (unsigned)koa_millis());

  uint8_t r[4];
  if (koa_random(r, sizeof r) >= 0)
    koa_printf("random bytes: %02x%02x%02x%02x\n", r[0], r[1], r[2], r[3]);

  int64_t t = koa_unixtime();
  if (t < 0) koa_puts("clock not set yet (no GPS or Wi-Fi time)");
  else       koa_printf("unix time: %lld\n", (long long)t);
  return 0;
}
