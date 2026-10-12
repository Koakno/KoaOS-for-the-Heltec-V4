/* io_test.c -- checks the pin policy (koa_io.h) and the manifest capability parser (esp_pkg.h) on a PC.
 *   make iotest       (also builds with -DKOA_IO_ALLOW_MASK=~0ULL to prove OS pins stay locked out) */
#include <stdio.h>
#include <string.h>
#include "../../KoaOS/koa_io.h"
#include "../../KoaOS/esp_pkg.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
  koa_pins_t t; kio_init(&t);

  /* pins the OS owns can never be handed to an app, whatever the allow-list says */
  static const int os[] = { 2, 7, 8, 9, 10, 11, 12, 13, 14, 46, 17, 18, 21, 36, 34, 38, 39, 19, 20, 43, 44 };
  for (unsigned i = 0; i < sizeof(os) / sizeof(os[0]); i++) {
    CHECK(!kio_pin_allowed(os[i]));
    CHECK(kio_claim(&t, os[i], 1, KP_GPIO, KG_IN) == KE_PERM);
  }
  CHECK(kio_claim(&t, -1, 1, KP_GPIO, 0) == KE_INVAL && kio_claim(&t, KIO_NPINS, 1, KP_GPIO, 0) == KE_INVAL);

#ifndef KOA_IO_ALLOW_MASK_TEST
  /* default allow-list */
  CHECK(kio_pin_allowed(4) && kio_pin_allowed(47) && !kio_pin_allowed(0) && !kio_pin_allowed(26) && !kio_pin_allowed(1));
#endif

  /* ownership */
  int a = 4, b = 5;
  CHECK(kio_claim(&t, a, 1, KP_GPIO, KG_IN) == KE_OK);
  CHECK(kio_claim(&t, a, 1, KP_GPIO, KG_OUT_HIGH) == KE_OK && t.mode[a] == KG_OUT_HIGH);   /* same owner, same purpose: mode change */
  CHECK(kio_claim(&t, a, 1, KP_UART, 0) == KE_BUSY);                                        /* same owner, other purpose */
  CHECK(kio_claim(&t, a, 2, KP_GPIO, KG_IN) == KE_BUSY);                                    /* other process */
  CHECK(kio_owned(&t, a, 1, KP_GPIO) && !kio_owned(&t, a, 2, KP_GPIO) && !kio_owned(&t, b, 1, KP_GPIO));
  CHECK(kio_release(&t, a, 2, KP_GPIO) == KE_INVAL);
  CHECK(kio_release(&t, a, 1, KP_GPIO) == KE_OK && !t.pid[a]);

  /* a bus claims its pins all or nothing */
  kio_init(&t);
  CHECK(kio_claim(&t, 6, 9, KP_GPIO, 0) == KE_OK);
  int bus[3] = { 3, 6, 5 };
  CHECK(kio_claim_set(&t, bus, 3, 1, KP_SPI) == KE_BUSY && !t.pid[3] && !t.pid[5]);          /* nothing half-claimed */
  int dup[2] = { 3, 3 };   CHECK(kio_claim_set(&t, dup, 2, 1, KP_I2C) == KE_INVAL);
  int none[2] = { -1, -1 }; CHECK(kio_claim_set(&t, none, 2, 1, KP_I2C) == KE_INVAL);
  int os2[2] = { 4, 8 };   CHECK(kio_claim_set(&t, os2, 2, 1, KP_I2C) == KE_PERM && !t.pid[4]);
  int ok[3] = { 3, 4, -1 };
  CHECK(kio_claim_set(&t, ok, 3, 1, KP_UART) == KE_OK && kio_owned(&t, 3, 1, KP_UART) && kio_owned(&t, 4, 1, KP_UART));
  CHECK(kio_next(&t, 1, KP_UART, 0) == 3 && kio_next(&t, 1, KP_UART, 4) == 4 && kio_next(&t, 1, KP_UART, 5) == -1);

  /* manifest: the category is declared, pins are not */
  uint32_t caps; char err[64];
  CHECK(esp_caps_parse("gpio", &caps, err, sizeof(err)) == 0 && caps == (KC_GPIO | KC_UART));   /* gpio includes uart */
  CHECK(esp_caps_parse("gpio,i2c", &caps, err, sizeof(err)) == 0 && caps == (KC_GPIO | KC_UART | KC_I2C));
  CHECK(esp_caps_parse("i2c", &caps, err, sizeof(err)) == 0 && caps == KC_I2C);
  CHECK(esp_caps_parse("spi,gps,lora", &caps, err, sizeof(err)) == 0 && caps == (KC_SPI | KC_GPS | KC_LORA));
  CHECK(esp_caps_parse("uart", &caps, err, sizeof(err)) == 0 && caps == KC_UART);
  CHECK(esp_caps_parse("gpio:4", &caps, err, sizeof(err)) != 0);                                /* pins are NOT in the manifest */
  CHECK(esp_caps_parse("wifi,gps,lora,display", &caps, err, sizeof(err)) == 0 && caps == (KC_WIFI | KC_GPS | KC_LORA | KC_DISPLAY));
  CHECK(esp_caps_parse("", &caps, err, sizeof(err)) == 0 && caps == 0);
  CHECK(KOA_ABI_VERSION == 2);

  printf(fails ? "io_test: %d FAILED\n" : "io_test: all passed\n", fails);
  return fails != 0;
}
