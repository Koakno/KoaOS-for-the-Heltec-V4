/* i2cscan -- lists every device that answers on the OLED's I2C bus (GPIO17/18) and shows the list on the OLED too.
 * Wire a sensor or expander to the board's SDA/SCL (GPIO17/18), 3V3 and GND, run this, and its address shows up.
 * The OLED itself answers at 0x3C. Press a key to leave. */
#include "koaos.h"

int koa_main(void) {
  int rc = koa_acquire(KC_I2C);
  if (rc != KE_OK) { koa_printf("acquire i2c: %s\n", koa_strerror(rc)); return 1; }
  rc = koa_i2c_open(KOA_I2C_OLED_SDA, KOA_I2C_OLED_SCL, 100000);      /* the display's own bus, shared with it */
  if (rc != KE_OK) { koa_printf("i2c open: %s\n", koa_strerror(rc)); return 1; }

  uint8_t found[40];
  int n = koa_i2c_scan(found, (int)sizeof found);
  for (int i = 0; i < n; i++) koa_printf("0x%02x%s\n", found[i], (found[i] == 0x3C || found[i] == 0x3D) ? "  (the OLED)" : "");
  if (!n) koa_puts("nothing answered: check SDA/SCL (GPIO17/18), 3V3 and GND");

  if (koa_acquire(KC_DISPLAY) == KE_OK) {                            /* optional: show it on the screen as well */
    koa_oled_clear();
    koa_oled_printf(0, 12, KOLED_FONT_MEDIUM, "I2C scan: %d found", n);
    int shown = 0;
    for (int i = 0; i < n && shown < 12; i++) {
      if (found[i] == 0x3C || found[i] == 0x3D) continue;             /* that is the screen you are reading */
      koa_oled_printf((shown % 4) * 32, 30 + (shown / 4) * 12, KOLED_FONT_MEDIUM, "%02X", found[i]);
      shown++;
    }
    if (!shown) koa_oled_text(0, 34, KOLED_FONT_SMALL, "no other devices", KOLED_WHITE);
    koa_oled_text(0, 62, KOLED_FONT_SMALL, "press a key", KOLED_WHITE);
    koa_oled_flush();
    koa_getc(60000);                                                  /* leave after a key or a minute */
  }
  return 0;                                                           /* the OS gives the bus and the screen back */
}
