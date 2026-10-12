/* webget -- fetches a page over Wi-Fi (http or https; TLS is done by the firmware).
 * Connect first with the OS's `wifi` command. Edit URL below.
 * manifest: requires=wifi */
#include "koaos.h"

#define URL "http://example.com/"

static char body[2048];

int koa_main(void) {
  int rc = koa_acquire(KC_WIFI);
  if (rc != KE_OK) { koa_printf("can't use Wi-Fi: %s\n", koa_strerror(rc)); return 1; }

  char ssid[33], ip[16];
  int32_t rssi = 0;
  if (koa_wifi_status(ssid, sizeof ssid, ip, sizeof ip, &rssi) != 1) {
    koa_puts("not connected to Wi-Fi (use the wifi command first)");
    koa_release(KC_WIFI);
    return 1;
  }
  koa_printf("connected: %s  %s  %d dBm\n", ssid, ip, (int)rssi);

  int n = koa_http_get(URL, body, sizeof body - 1);
  if (n < 0) {
    if (n <= -100) koa_printf("server answered HTTP %d\n", -n);   /* HTTP errors come back as -status */
    else           koa_printf("request failed: %s\n", koa_strerror(n));
    koa_release(KC_WIFI);
    return 1;
  }
  body[n] = 0;
  koa_printf("%d bytes:\n", n);
  koa_puts(body);
  koa_release(KC_WIFI);
  return 0;
}
