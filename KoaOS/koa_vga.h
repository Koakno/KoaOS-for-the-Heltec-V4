/*
 * koa_vga.h -- VGA text output + 320x240 graphics mode (for the GUI, koa_gui.h) (640x480@60, 80x30 characters, 8 colors available), switched on and off at run time
 *
 * Mirrors the console (ttyWrite) onto a VGA monitor using the ESP32-S3 LCD peripheral.
 * Text mode has no framebuffer: every scanline is built on the fly in the LCD bounce-buffer callback from the 80x30 text
 * buffer in internal RAM. Graphics mode (vgaGfxBegin, used by the "gui" program) adds ONE small 320x240 / 4 bit framebuffer
 * (38 KB) that is also read in that callback and doubled to 640x480; it lives in INTERNAL RAM only and exists only while
 * the GUI runs. Never put video memory in PSRAM: it is too slow for this (it caused stray-line glitches).
 * Shell:  vga on | vga off   (see p_vga in KoaOS.ino).  vgaBegin() claims the LCD peripheral and the six pins,
 * vgaEnd() gives both back, so the pins are plain GPIO again while VGA is off.
 * Define KOA_NO_VGA (or -DKOA_NO_VGA) to compile it out entirely (needed on arduino-esp32 2.x, which has no esp_lcd RGB driver).
 *
 * Wiring (R/G/B each through ~220-330 ohm if you have them; weakest drive strength is set in software):
 *   GPIO4 -> VGA pin 1 (R)   GPIO5 -> pin 2 (G)   GPIO6 -> pin 3 (B)
 *   GPIO47 -> pin 13 (HSYNC) GPIO48 -> pin 14 (VSYNC)   GND -> pins 5-8, 10
 *   GPIO3 = dummy pixel-clock output, leave unconnected.
 * While VGA is on the OS owns these six pins in the pin table (owner "vga" in `pins`); apps get KE_BUSY for them.
 *
 * Needs arduino-esp32 core 3.x (esp_lcd RGB driver, IDF 5.1+). No PSRAM needed.
 * Every function here must be called with ttyMx held (ttyWrite, loop() and the vga command do).
 */
#ifndef KOA_VGA_H
#define KOA_VGA_H

// pins (always defined, so the vga command can name them even when KOA_NO_VGA stubs the driver)
#define VGA_PIN_R      4
#define VGA_PIN_G      5
#define VGA_PIN_B      6
#define VGA_PIN_HSYNC  47
#define VGA_PIN_VSYNC  48
#define VGA_PIN_PCLK   3      // dummy

#ifndef KOA_NO_VGA

#include <Arduino.h>
#include <string.h>
#include <esp_attr.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_heap_caps.h>
#include "koa_font.h"
#include "koa_gfx.h"


#define VGA_W      640
#define VGA_H      480
#define VGA_COLS   80
#define VGA_ROWS   30
#define VGA_BB_LINES 10       // bounce buffer = 10 scanlines (6.4 KB, x2, internal RAM)

// Vertical position: move lines between BP and FP, keeping BP + FP = 43 (more BP = picture lower).
#ifndef VGA_V_BP
#define VGA_V_BP 33
#endif
#ifndef VGA_V_FP
#define VGA_V_FP 10
#endif

static char    vgaText[VGA_ROWS][VGA_COLS];
static uint8_t vgaCol[VGA_ROWS][VGA_COLS];    // per-cell color: bit0=R bit1=G bit2=B
static volatile int  vgaCx = 0, vgaCy = 0;
static volatile bool vgaCurOn = true;
static uint8_t vgaCurColor = 7;
static int vgaEsc = 0, vgaNum = 0;
static volatile bool vgaReady = false;
static uint8_t  vgaFontRam[95][8];
static uint32_t vgaExpLo[256], vgaExpHi[256];
static esp_lcd_panel_handle_t vgaPanel = nullptr;

// ---- render core (identical to the tested standalone v2 sketch) ----
static inline __attribute__((always_inline)) void vgaFillLine(uint8_t* dst, int y) {
  int row = y >> 4;
  int gy = (y & 15) >> 1;
  uint32_t* d = (uint32_t*)dst;
  int ccy = vgaCy, ccx = vgaCx; bool con = vgaCurOn;
  for (int c = 0; c < VGA_COLS; c++) {
    int ch = (int)vgaText[row][c] - 32;
    if ((unsigned)ch > 94) ch = 0;
    uint32_t bits = vgaFontRam[ch][gy];
    uint32_t fg = vgaCol[row][c];
    if (con && row == ccy && c == ccx) { bits ^= 0xFF; fg = 7; }
    d[0] = vgaExpLo[bits] * fg;
    d[1] = vgaExpHi[bits] * fg;
    d += 2;
  }
}

// ---- graphics mode: 320x240 @ 4 bit/pixel framebuffer (koa_gfx.h), every pixel doubled to 640x480 ----
static uint32_t vgaGfxExp[256];     // one framebuffer byte (2 pixels) -> 4 output pixels

static inline __attribute__((always_inline)) void vgaFillGfxLine(uint8_t* dst, const uint8_t* fb, int y) {
  const uint8_t* s = fb + (y >> 1) * GFX_STRIDE;
  uint32_t* d = (uint32_t*)dst;
  for (int i = 0; i < GFX_STRIDE; i++) d[i] = vgaGfxExp[s[i]];
}

static void IRAM_ATTR vgaFillChunk(uint8_t* buf, int pos_px, int len_px) {
  int y0 = pos_px / VGA_W, n = len_px / VGA_W;
  const uint8_t* fb = gfxFb;                       // graphics mode when a framebuffer exists
  for (int i = 0; i < n; i++) {
    int y = y0 + i;
    uint8_t* line = buf + i * VGA_W;
    if (y >= VGA_H) { memset(line, 0, VGA_W); continue; }
    if ((y & 1) && i > 0) memcpy(line, line - VGA_W, VGA_W);
    else if (fb) vgaFillGfxLine(line, fb, y);
    else vgaFillLine(line, y);
  }
}

static bool IRAM_ATTR vgaOnBounceEmpty(esp_lcd_panel_handle_t, void* bounce_buf, int pos_px, int len_px, void*) {
  vgaFillChunk((uint8_t*)bounce_buf, pos_px, len_px);
  return false;
}

// ---- text console (call with ttyMx held) ----
static void vgaClear() {
  memset(vgaText, ' ', sizeof(vgaText));
  memset(vgaCol, 7, sizeof(vgaCol));
  vgaCx = 0; vgaCy = 0;
}

static void vgaNewLine() {
  vgaCx = 0;
  if (vgaCy < VGA_ROWS - 1) { vgaCy = vgaCy + 1; return; }
  memmove(vgaText[0], vgaText[1], (VGA_ROWS - 1) * VGA_COLS);
  memmove(vgaCol[0], vgaCol[1], (VGA_ROWS - 1) * VGA_COLS);
  memset(vgaText[VGA_ROWS - 1], ' ', VGA_COLS);
  memset(vgaCol[VGA_ROWS - 1], 7, VGA_COLS);
}

// same input rules as scrPut(): \n, \r, printable ASCII, ESC[J (clear) and ESC[H (home)
static void vgaPut(char c) {
  if (!vgaReady) return;
  if (vgaEsc == 1) { if (c == '[') { vgaEsc = 2; vgaNum = 0; } else vgaEsc = 0; return; }
  if (vgaEsc == 2) {
    if (c >= '0' && c <= '9') { vgaNum = vgaNum * 10 + (c - '0'); return; }
    vgaEsc = 0;
    if (c == 'J') vgaClear();
    else if (c == 'H') { vgaCx = 0; vgaCy = 0; }
    return;
  }
  if (c == 27) { vgaEsc = 1; return; }
  if (c == '\r') return;
  if (c == '\n') { vgaNewLine(); return; }
  if (c < 32 || c > 126) return;
  if (vgaCx >= VGA_COLS) vgaNewLine();
  vgaText[vgaCy][vgaCx] = c;
  vgaCol[vgaCy][vgaCx] = vgaCurColor;
  vgaCx = vgaCx + 1;
}

static void vgaBackspace() {
  if (!vgaReady || vgaCx <= 0) return;
  vgaCx = vgaCx - 1;
  vgaText[vgaCy][vgaCx] = ' ';
}

// the picture is generated continuously by the driver, so "flush" only sets the cursor blink state
static void vgaFlush(bool blink) { vgaCurOn = blink; }

static inline bool vgaActive() { return vgaReady; }

// Switch the picture to graphics mode: allocates the 38 KB framebuffer in internal RAM (never PSRAM: too slow for video).
// Returns false if there is no contiguous internal RAM left. Text mode comes back, untouched, after vgaGfxEnd().
static bool vgaGfxBegin() {
  if (!vgaReady) return false;
  if (gfxFb) return true;
  for (int b = 0; b < 256; b++) {
    uint32_t lo = b & 7, hi = (b >> 4) & 7;                   // left pixel = low nibble, right pixel = high nibble
    vgaGfxExp[b] = lo | (lo << 8) | (hi << 16) | (hi << 24);
  }
  uint8_t* f = (uint8_t*)heap_caps_malloc(GFX_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!f) return false;
  memset(f, 0, GFX_BYTES);
  gfxFb = f;                                                  // the next scanline already comes from it
  return true;
}

static void vgaGfxEnd() {
  uint8_t* f = gfxFb;
  if (!f) return;
  gfxFb = nullptr;                                            // renderer goes back to text mode on its next line
  delay(60);                                                  // > 2 frames: make sure no scanline still reads the buffer
  heap_caps_free(f);
}

static inline bool vgaGfxActive() { return gfxFb != nullptr; }

// stops the video output and hands the six pins back as plain high-impedance inputs. Safe to call when VGA is off.
static void vgaEnd() {
  vgaGfxEnd();
  vgaReady = false;                              // vgaPut/vgaBackspace stop touching the text buffer first
  if (vgaPanel) { esp_lcd_panel_del(vgaPanel); vgaPanel = nullptr; }     // stops the LCD DMA and frees both bounce buffers
  const int pins[6] = { VGA_PIN_R, VGA_PIN_G, VGA_PIN_B, VGA_PIN_HSYNC, VGA_PIN_VSYNC, VGA_PIN_PCLK };
  for (int i = 0; i < 6; i++) {
    gpio_reset_pin((gpio_num_t)pins[i]);         // detach the LCD signal from the pad
    gpio_set_drive_capability((gpio_num_t)pins[i], GPIO_DRIVE_CAP_2);   // back to the default drive strength
    pinMode(pins[i], INPUT);                     // and no pull-up left behind
  }
}

// returns true on success (also if it is already running). On failure everything is undone.
static bool vgaBegin() {
  if (vgaReady) return true;
  memcpy(vgaFontRam, vgaFont, sizeof(vgaFontRam));
  for (int b = 0; b < 256; b++) {
    uint32_t lo = 0, hi = 0;
    for (int i = 0; i < 4; i++) {
      if ((b >> i) & 1) lo |= (1u << (8 * i));
      if ((b >> (i + 4)) & 1) hi |= (1u << (8 * i));
    }
    vgaExpLo[b] = lo; vgaExpHi[b] = hi;
  }
  vgaEsc = 0; vgaNum = 0;
  vgaClear();

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = 25000000;                 // 640x480@60, negative-polarity syncs
  cfg.timings.h_res = VGA_W;
  cfg.timings.v_res = VGA_H;
  cfg.timings.hsync_pulse_width = 96;
  cfg.timings.hsync_back_porch = 48;
  cfg.timings.hsync_front_porch = 16;
  cfg.timings.vsync_pulse_width = 2;
  cfg.timings.vsync_back_porch = VGA_V_BP;
  cfg.timings.vsync_front_porch = VGA_V_FP;
  cfg.timings.flags.hsync_idle_low = 0;
  cfg.timings.flags.vsync_idle_low = 0;
  cfg.timings.flags.de_idle_high = 0;
  cfg.timings.flags.pclk_active_neg = 0;
  cfg.timings.flags.pclk_idle_high = 0;
  cfg.data_width = 8;
  cfg.bits_per_pixel = 8;
  cfg.num_fbs = 0;
  cfg.bounce_buffer_size_px = VGA_BB_LINES * VGA_W;
  cfg.hsync_gpio_num = VGA_PIN_HSYNC;
  cfg.vsync_gpio_num = VGA_PIN_VSYNC;
  cfg.de_gpio_num = -1;
  cfg.pclk_gpio_num = VGA_PIN_PCLK;
  cfg.disp_gpio_num = -1;
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = -1;
  cfg.data_gpio_nums[0] = VGA_PIN_R;
  cfg.data_gpio_nums[1] = VGA_PIN_G;
  cfg.data_gpio_nums[2] = VGA_PIN_B;
  cfg.flags.no_fb = 1;
  cfg.flags.fb_in_psram = 0;

  if (esp_lcd_new_rgb_panel(&cfg, &vgaPanel) != ESP_OK) { vgaPanel = nullptr; return false; }
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_bounce_empty = vgaOnBounceEmpty;
  if (esp_lcd_rgb_panel_register_event_callbacks(vgaPanel, &cbs, nullptr) != ESP_OK ||
      esp_lcd_panel_reset(vgaPanel) != ESP_OK ||
      esp_lcd_panel_init(vgaPanel) != ESP_OK) { vgaEnd(); return false; }
  gpio_set_drive_capability((gpio_num_t)VGA_PIN_R, GPIO_DRIVE_CAP_0);   // weakest drive: no resistors needed to try it
  gpio_set_drive_capability((gpio_num_t)VGA_PIN_G, GPIO_DRIVE_CAP_0);
  gpio_set_drive_capability((gpio_num_t)VGA_PIN_B, GPIO_DRIVE_CAP_0);
  vgaReady = true;
  return true;
}

#else  /* KOA_NO_VGA: empty stubs so the rest of the OS needs no #ifdefs */

static inline bool vgaActive() { return false; }
static inline bool vgaBegin() { return false; }
static inline bool vgaGfxBegin() { return false; }
static inline void vgaGfxEnd() {}
static inline bool vgaGfxActive() { return false; }
static inline void vgaEnd() {}
static inline void vgaPut(char) {}
static inline void vgaBackspace() {}
static inline void vgaFlush(bool) {}

#endif /* KOA_NO_VGA */
#endif /* KOA_VGA_H */
