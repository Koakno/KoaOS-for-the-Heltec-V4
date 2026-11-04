/* app_mock.c -- runs an SDK app natively against a fake OS: a PCA9555 and the OLED on a fake I2C bus, a fake screen.
 *   make apptest
 * Not a firmware test (the firmware's I2C/OLED logic is covered separately); this checks that the apps, the SDK helpers
 * and the ABI header fit together and do the right thing when the OS behaves as documented. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "koaos.h"

/* ---- fake world ---- */
static int  acquired, i2c_open_, scenario_presses;
static int  calls_before_key = 12;                     /* koa_getc returns a key after this many polls */
static uint8_t pca_reg, pca_cfg[2] = { 0xFF, 0xFF }, pca_out[2], pca_in[2] = { 0xFF, 0xFF };
static uint8_t held_ptr; static int held_valid, held_addr;
static int  pca_addr = 0x21;
static uint8_t scr[64][128]; static int flushes, drawn_calls;
static int  last_flush_filled;

static void ascii_dump(void) {
  for (int y = 0; y < 64; y += 2) { char row[129]; for (int x = 0; x < 128; x++) row[x] = (scr[y][x] || scr[y + 1][x]) ? '#' : '.'; row[128] = 0; puts(row); }
}

int32_t koa_write(int32_t fd, const void *b, int32_t n) { (void)fd; fwrite(b, 1, (size_t)n, stdout); return n; }
int32_t koa_yield(void) { return KE_OK; }
void koa_exit(int32_t rc) { exit(rc); }
void koa_sleep(int32_t ms) { (void)ms; }
int32_t koa_getc(int32_t t) { (void)t; if (--calls_before_key == 5) { pca_in[0] = 0xF7; scenario_presses++; }   /* "press" P03 */ return calls_before_key <= 0 ? 'q' : -1; }
int32_t koa_acquire(uint32_t caps) { acquired |= (int)caps; return KE_OK; }
int32_t koa_release(uint32_t caps) { acquired &= ~(int)caps; return KE_OK; }

int32_t koa_i2c_open(int32_t sda, int32_t scl, int32_t hz) { if (!(acquired & KC_I2C)) return KE_PERM; if (hz > 400000 && sda == 17) return KE_INVAL; i2c_open_ = 1; (void)scl; return KE_OK; }
int32_t koa_i2c_close(void) { i2c_open_ = 0; return KE_OK; }
int32_t koa_i2c_write(int32_t addr, const void *buf, int32_t n, int32_t stop) {
  const uint8_t *b = buf;
  if (!i2c_open_) return KE_INVAL;
  if (addr != pca_addr && addr != 0x3C) return KE_NOENT;
  if (addr == 0x3C) return n ? KE_PERM : 0;
  if (n == 0) return 0;
  if (!stop) { held_ptr = b[0]; held_valid = 1; held_addr = addr; return n; }
  pca_reg = b[0];
  for (int i = 1; i < n; i++) {
    uint8_t r = pca_reg;
    if (r == 6 || r == 7) pca_cfg[r - 6] = b[i]; else if (r == 2 || r == 3) pca_out[r - 2] = b[i];
    pca_reg = (r == 1 || r == 3 || r == 7) ? (uint8_t)(r - 1) : (uint8_t)(r + 1);      /* the two registers of a pair alternate */
  }
  return n;
}
int32_t koa_i2c_read(int32_t addr, void *buf, int32_t n) {
  uint8_t *b = buf;
  if (addr != pca_addr) return KE_IO;
  uint8_t r = held_valid && held_addr == addr ? held_ptr : pca_reg; held_valid = 0;
  for (int i = 0; i < n; i++) { b[i] = pca_in[r & 1]; r ^= 1; }
  return n;
}

static int color_ok(int c) { return c >= 0 && c <= 2; }
static void px(int x, int y, int c) { if (x >= 0 && x < 128 && y >= 0 && y < 64) { if (c == 2) scr[y][x] ^= 1; else scr[y][x] = (uint8_t)c; drawn_calls++; } }
int32_t koa_oled_clear(void) { if (!(acquired & KC_DISPLAY)) return KE_PERM; memset(scr, 0, sizeof scr); return KE_OK; }
int32_t koa_oled_flush(void) { if (!(acquired & KC_DISPLAY)) return KE_PERM; flushes++; int f = 0; for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) f += scr[y][x]; last_flush_filled = f; return KE_OK; }
int32_t koa_oled_pixel(int32_t x, int32_t y, int32_t c) { if (!color_ok(c)) return KE_INVAL; px(x, y, c); return KE_OK; }
int32_t koa_oled_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) { px(x0, y0, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
  return KE_OK;
}
int32_t koa_oled_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t fill, int32_t c) {
  if (!color_ok(c) || w < 0 || h < 0) return KE_INVAL;
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) if (fill || j == 0 || j == h - 1 || i == 0 || i == w - 1) px(x + i, y + j, c);
  return KE_OK;
}
int32_t koa_oled_text(int32_t x, int32_t y, int32_t font, const char *s, int32_t c) { int w = (int)strlen(s) * (font == 0 ? 5 : font == 1 ? 6 : 9); for (int i = 0; i < w; i++) px(x + i, y - 2, c); return w; }
int32_t koa_oled_bitmap(int32_t x, int32_t y, int32_t w, int32_t h, const void *bits) {
  const uint8_t *b = bits; int rb = (w + 7) / 8;
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) if (b[j * rb + i / 8] & (0x80 >> (i & 7))) px(x + i, y + j, 1);
  return KE_OK;
}
int32_t koa_oled_contrast(int32_t l) { (void)l; return KE_OK; }

/* ---- checks ---- */
static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
#if defined(APP_I2CSCAN)
  int rc = koa_main();
  CHECK(rc == 0 && flushes == 1 && last_flush_filled > 0);
#elif defined(APP_OLEDDEMO)
  calls_before_key = 40;
  int rc = koa_main();
  CHECK(rc == 0 && flushes >= 30 && last_flush_filled > 100);
#elif defined(APP_PCA9555)
  int rc = koa_main();
  CHECK(rc == 0);
  CHECK(pca_cfg[0] == 0xFF && pca_cfg[1] == 0x00);                     /* port 0 input, port 1 output */
  CHECK(scenario_presses == 1 && pca_out[1] == 0xF7);                  /* the "press" on P03 reached the LED port */
  CHECK(flushes >= 2);
#else
#error pick an app
#endif
  if (getenv("SHOW")) ascii_dump();
  printf(fails ? "apptest: %d FAILED\n" : "apptest: passed\n", fails);
  return fails != 0;
}
