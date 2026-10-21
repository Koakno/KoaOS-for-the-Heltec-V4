/*
 * koa_gfx.h -- tiny 2D drawing library for the VGA graphics mode (portable C++, no hardware calls, tested on a PC)
 *
 * Framebuffer: 320x240, 4 bits per pixel (two pixels per byte, low nibble = left pixel), 38,400 bytes in INTERNAL RAM.
 * The VGA driver doubles every pixel to 640x480. Only the low 3 bits of a pixel reach the pins: bit0=R bit1=G bit2=B.
 * All calls are safe when no framebuffer exists (they do nothing), so a stray draw can never crash.
 */
#ifndef KOA_GFX_H
#define KOA_GFX_H
#include <stdint.h>
#include <string.h>
#include "koa_font.h"

#define GFX_W      320
#define GFX_H      240
#define GFX_STRIDE (GFX_W / 2)
#define GFX_BYTES  (GFX_STRIDE * GFX_H)

enum { C_BLK = 0, C_RED, C_GRN, C_YEL, C_BLU, C_MAG, C_CYN, C_WHT };

static uint8_t* volatile gfxFb = nullptr;       // set by vgaGfxBegin(), cleared by vgaGfxEnd()

static inline void gfxPix(int x, int y, int c) {
  uint8_t* f = gfxFb;
  if (!f || (unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return;
  uint8_t* p = f + y * GFX_STRIDE + (x >> 1);
  if (x & 1) *p = (uint8_t)((*p & 0x0F) | ((c & 7) << 4));
  else       *p = (uint8_t)((*p & 0xF0) | (c & 7));
}

static void gfxFill(int c) {
  uint8_t* f = gfxFb;
  if (f) memset(f, (c & 7) * 0x11, GFX_BYTES);
}

static void gfxRect(int x, int y, int w, int h, int c) {
  if (!gfxFb) return;
  int x1 = x + w, y1 = y + h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > GFX_W) x1 = GFX_W;
  if (y1 > GFX_H) y1 = GFX_H;
  for (int yy = y; yy < y1; yy++) for (int xx = x; xx < x1; xx++) gfxPix(xx, yy, c);
}

static void gfxFrame(int x, int y, int w, int h, int c) {
  gfxRect(x, y, w, 1, c); gfxRect(x, y + h - 1, w, 1, c);
  gfxRect(x, y, 1, h, c); gfxRect(x + w - 1, y, 1, h, c);
}

static void gfxLine(int x0, int y0, int x1, int y1, int c) {      // Bresenham
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    gfxPix(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void gfxCircle(int cx, int cy, int r, int c, bool fill) {
  for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) {
    int d = x * x + y * y;
    if (fill ? d <= r * r : (d <= r * r && d >= (r - 1) * (r - 1))) gfxPix(cx + x, cy + y, c);
  }
}

// one 8x8 character, scaled s times. bg < 0 = transparent
static void gfxChar(int x, int y, char ch, int fg, int bg, int s = 1) {
  int i = (int)(unsigned char)ch - 32;
  if ((unsigned)i > 94) i = 0;
  for (int row = 0; row < 8; row++) {
    uint8_t bits = vgaFont[i][row];
    for (int col = 0; col < 8; col++) {
      bool on = (bits >> col) & 1;
      if (on) gfxRect(x + col * s, y + row * s, s, s, fg);
      else if (bg >= 0) gfxRect(x + col * s, y + row * s, s, s, bg);
    }
  }
}

// text; returns the x after the last character. max < 0 = whole string
static int gfxText(int x, int y, const char* t, int fg, int bg = -1, int s = 1, int max = -1) {
  for (int n = 0; *t && (max < 0 || n < max); t++, n++, x += 8 * s) gfxChar(x, y, *t, fg, bg, s);
  return x;
}

// 3D-ish button/panel with a 1px black frame and a white/black bevel
static void gfxPanel(int x, int y, int w, int h, int fill) {
  gfxRect(x, y, w, h, fill);
  gfxFrame(x, y, w, h, C_BLK);
}

#endif
