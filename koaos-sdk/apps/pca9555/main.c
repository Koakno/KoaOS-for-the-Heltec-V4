/* pca9555 -- reads and drives a PCA9555 16-bit I2C GPIO expander on the OLED's bus (GPIO17/18).
 *   Port 0 (P00..P07) = inputs.   Port 1 (P10..P17) = outputs that follow port 0: press a button on P0n, P1n changes.
 * Wiring: SDA/SCL to GPIO17/18, V to 3V3, GND to GND. A button connects its pin to GND (it reads 0 while pressed).
 * If a pin floats, add a 10k pull-up to 3V3. The address is found by itself (0x20..0x27, set by the A0/A1/A2 jumpers).
 * The OLED shows both ports: a filled square = the pin reads high. Press a key on the console to leave. */
#include "koaos.h"

#define REG_IN0   0x00      /* input port 0, then port 1 on the next byte */
#define REG_OUT0  0x02      /* output port 0, then port 1 */
#define REG_CFG0  0x06      /* configuration: 1 = input, 0 = output; port 0, then port 1 */

#define LED_ACTIVE_LOW 0    /* set to 1 if the board's LEDs light when the pin is LOW */

static void show(int addr, const uint8_t in[2], const uint8_t out[2]) {
  koa_oled_clear();
  koa_oled_printf(0, 9, KOLED_FONT_SMALL, "PCA9555 @0x%02X", addr);
  for (int port = 0; port < 2; port++) {
    uint8_t v = port ? out[1] : in[0];
    koa_oled_printf(0, 31 + port * 22, KOLED_FONT_SMALL, "P%d", port);
    for (int bit = 0; bit < 8; bit++) {
      int x = 14 + (7 - bit) * 14;                                  /* bit 7 on the left, like the pins on the board */
      koa_oled_rect(x, 22 + port * 22, 11, 11, (v >> bit) & 1, KOLED_WHITE);
    }
  }
  koa_oled_flush();
}

int koa_main(void) {
  int rc = koa_acquire(KC_I2C | KC_DISPLAY);
  if (rc != KE_OK) { koa_printf("acquire: %s\n", koa_strerror(rc)); return 1; }
  rc = koa_i2c_open(KOA_I2C_OLED_SDA, KOA_I2C_OLED_SCL, 400000);
  if (rc != KE_OK) { koa_printf("i2c open: %s\n", koa_strerror(rc)); return 1; }

  int addr = 0;
  for (int a = 0x20; a <= 0x27; a++) if (koa_i2c_probe(a)) { addr = a; break; }
  if (!addr) { koa_puts("no PCA9555 at 0x20..0x27: check SDA/SCL, power and the A0/A1/A2 jumpers"); return 1; }
  koa_printf("PCA9555 at 0x%02x\n", addr);

  uint8_t cfg[2] = { 0xFF, 0x00 };                                   /* port 0 in, port 1 out */
  uint8_t out[2] = { 0xFF, 0x00 };
  if ((rc = koa_i2c_write_reg(addr, REG_CFG0, cfg, 2)) < 0) { koa_printf("config: %s\n", koa_strerror(rc)); return 1; }

  uint8_t in[2] = { 0, 0 }, last = 0xAA;
  int first = 1;
  while (koa_alive()) {
    if ((rc = koa_i2c_read_reg(addr, REG_IN0, in, 2)) < 0) { koa_printf("read: %s\n", koa_strerror(rc)); return 1; }
    uint8_t want = LED_ACTIVE_LOW ? (uint8_t)~in[0] : in[0];
    if (first || in[0] != last || out[1] != want) {
      out[1] = want;
      if ((rc = koa_i2c_write_reg(addr, REG_OUT0, out, 2)) < 0) { koa_printf("write: %s\n", koa_strerror(rc)); return 1; }
      if (!first) koa_printf("P0=%02x P1=%02x\n", in[0], out[1]);
      show(addr, in, out);
      last = in[0]; first = 0;
    }
    if (koa_getc(20) >= 0) break;                                    /* poll about every 20 ms; any key leaves */
  }
  return 0;
}
