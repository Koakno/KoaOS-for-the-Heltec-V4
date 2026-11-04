/* oleddemo -- tries every koa_oled_* call: text in three sizes, a frame, a line, a bitmap and a bouncing ball.
 * While it runs the console is off the OLED (the shell still works over USB / VGA). Press a key to leave. */
#include "koaos.h"

static const uint8_t heart[8] = { 0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00 };   /* 8x8, top bit = leftmost pixel */

int koa_main(void) {
  int rc = koa_acquire(KC_DISPLAY);
  if (rc != KE_OK) { koa_printf("display: %s\n", koa_strerror(rc)); return 1; }

  int x = 20, y = 40, dx = 2, dy = 1;
  for (int frame = 0; koa_alive(); frame++) {
    koa_oled_clear();
    koa_oled_text(0, 14, KOLED_FONT_LARGE, "KoaOS", KOLED_WHITE);
    koa_oled_text(70, 10, KOLED_FONT_MEDIUM, "OLED demo", KOLED_WHITE);
    koa_oled_printf(70, 22, KOLED_FONT_SMALL, "frame %d", frame);
    koa_oled_rect(0, 26, KOLED_W, KOLED_H - 26, 0, KOLED_WHITE);                 /* the play field */
    koa_oled_line(0, 26, KOLED_W - 1, KOLED_H - 1, KOLED_WHITE);                  /* a diagonal through it */
    koa_oled_bitmap(112, 28, 8, 8, heart);

    x += dx; y += dy;                                                             /* the ball */
    if (x < 3 || x > KOLED_W - 5) dx = -dx;
    if (y < 29 || y > KOLED_H - 5) dy = -dy;
    koa_oled_rect(x - 2, y - 2, 5, 5, 1, KOLED_XOR);                               /* XOR: it stays visible over the line */

    koa_oled_flush();
    if (koa_getc(30) >= 0) break;                                                 /* ~30 ms per frame; any key leaves */
  }
  return 0;                                                                       /* the console gets the screen back */
}
