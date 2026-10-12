/* gpswatch -- prints a GPS fix every 2 seconds. Press any key to quit.
 * manifest: requires=gps   (the GPS is powered while the app holds KC_GPS) */
#include "koaos.h"

int koa_main(void) {
  int rc = koa_acquire(KC_GPS);
  if (rc != KE_OK) { koa_printf("can't use the GPS: %s\n", koa_strerror(rc)); return 1; }

  koa_puts("GPS watch - press any key to quit");
  while (koa_alive()) {
    koa_gps_t g;
    if (koa_gps_info(&g) == KE_OK) {
      if (g.loc_valid)
        koa_printf("%d sats  %.5f, %.5f  alt %.0f m  %.1f mph\n", (int)g.sats, g.lat, g.lng, g.alt, g.mph);
      else
        koa_printf("searching... %d sats\n", (int)g.sats);
    }
    if (koa_getc(2000) >= 0) break;            /* waits up to 2 s for a key */
  }
  koa_release(KC_GPS);
  return 0;
}
