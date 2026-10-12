/* iodemo -- tries the GPIO and I2C calls: blinks one pin and scans an I2C bus.
 * The manifest only says "gpio,i2c" (the kinds of I/O). The PINS are chosen here, at run time:
 * change them and rebuild, or read them from a file with koa_open() if you want no rebuild at all.
 * Try `pins` in the shell while it runs to see who owns what. */
#include "koaos.h"

#define PIN_LED  4      /* output: wire an LED + resistor to it */
#define PIN_SDA  5      /* I2C bus pins (external pull-ups needed) */
#define PIN_SCL  6

int koa_main(void) {
  int rc = koa_acquire(KC_GPIO | KC_I2C);
  if (rc != KE_OK) { koa_printf("acquire: %s\n", koa_strerror(rc)); return 1; }

  rc = koa_gpio_request(PIN_LED, KG_OUT_LOW);
  if (rc != KE_OK) { koa_printf("gpio %d: %s\n", PIN_LED, koa_strerror(rc)); return 1; }

  rc = koa_i2c_open(PIN_SDA, PIN_SCL, 100000);
  if (rc != KE_OK) { koa_printf("i2c %d/%d: %s\n", PIN_SDA, PIN_SCL, koa_strerror(rc)); return 1; }

  int found = 0;
  for (int a = 0x08; a <= 0x77 && koa_alive(); a++) {
    if (koa_i2c_write(a, 0, 0, 1) == 0) { koa_printf("i2c device at 0x%02x\n", a); found++; }   /* n=0 is an address probe */
  }
  if (!found) koa_puts("i2c: nothing answered");

  for (int i = 0; i < 10 && koa_alive(); i++) {
    koa_gpio_write(PIN_LED, i & 1);
    koa_sleep(250);
  }
  koa_puts("done");
  return 0;     /* the OS releases the pins and the bus when we exit */
}
