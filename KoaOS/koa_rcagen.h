/*
 * koa_rcagen.h -- composite video (RCA) scanline generator. Portable C++, no hardware calls, tested on a PC (make rcatest).
 *
 * The video signal is ONE pin carrying a 25 Mbit/s pulse-density stream (same idea as the WeatherClock sketch, which used I2S on
 * an ESP8266). A small RC low-pass at the jack turns the bit stream into voltage:
 *     sync tip  0000 -> 0 %       black  0101 -> 50 %       white  1111 -> 100 %      (a 4 bit group = one 160 ns pixel)
 * The LCD peripheral sends one BYTE per bit (bit 0 of every byte goes to the pin), so a scanline is 1600 bytes.
 *
 * One scanline = 1600 bits = 64.0 us (15.625 kHz). The LCD hardware inserts 4 idle clocks per line (hsync 2 + back porch 1 +
 * front porch 1), so the driver asks it for rows of RCA_W = 1596 bits and the 4 idle clocks fall at the end of the front porch,
 * right before the next sync tip, where they cannot be seen whether the pin idles low or holds its last level.
 *
 *   bit   0 .. 119   sync tip (4.8 us)           bit 120 .. 279  back porch, black (6.4 us)
 *   bit 280 .. 1559  picture: 320 pixels x 4 bits (51.2 us)       bit 1560 .. 1595 front porch, black
 *
 * A frame is RCA_H = 256 rows + 3 idle lines the LCD adds (vsync 1 + back porch 1 + front porch 1) = 259 lines = 60.3 Hz.
 * Rows 0-2 are the vertical sync (3 broad lines with a 4.6 us serration). The last row ends in zeros, so the 3 idle lines are
 * sync level if the pin holds its last value. Picture: the 320x240 graphics framebuffer when one exists (rows rcaTop ..
 * rcaTop+239), otherwise the console as 40x24 characters of 8x8 pixels (192 rows, centered in the same window).
 *
 * Needs from the includer (koa_vga.h in the firmware, stand-ins in the PC test): vgaText, vgaCol, vgaFontRam, vgaCx, vgaCy, vgaCurOn.
 */
#ifndef KOA_RCAGEN_H
#define KOA_RCAGEN_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "koa_font.h"
#include "koa_gfx.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#define RCA_BIT_HZ      25000000                    // 25 Mbit/s: 4 bits = 160 ns = one pixel
#define RCA_LINE_BITS   1600                        // 64.0 us
#define RCA_GAP_BITS    4                           // idle clocks the LCD hardware adds to every line (hsync 2 + bp 1 + fp 1)
#define RCA_W           (RCA_LINE_BITS - RCA_GAP_BITS)   // 1596 bits (bytes) per row handed to the LCD
#define RCA_WORDS       (RCA_W / 4)                 // 399 words of 4 bits-as-bytes
#define RCA_H           256                         // rows in the LCD frame (+3 idle lines = 259 lines, 60.3 Hz)
#define RCA_BB_PX       8512                        // bounce buffer: 133 x 64 bytes, divides W*H (48 times), 5.33 rows, 340 us
#define RCA_SYNC_W      30                          // sync tip: 30 words = 120 bits = 4.8 us
#define RCA_BP_W        40                          // back porch: 160 bits = 6.4 us
#define RCA_ACT_W       (RCA_SYNC_W + RCA_BP_W)     // first picture word (11.2 us after the start of sync)
#define RCA_ACT_END     (RCA_ACT_W + GFX_W)         // picture: one word per 320 pixels, 51.2 us
#define RCA_VSYNC_ROWS  3
#define RCA_VSYNC_LOW_W 370                         // vertical sync row: low for 1480 bits, then a 4.6 us serration
#define RCA_TCOLS       40                          // text mode: 40 x 24 characters of 8x8
#define RCA_TROWS       24
#define RCA_TOP_MIN     RCA_VSYNC_ROWS              // first row of the 240-row picture window: 3 .. 16
#define RCA_TOP_MAX     (RCA_H - GFX_H)
#ifndef RCA_TOP_DEFAULT
#define RCA_TOP_DEFAULT 8                           // `rca pos <n>` changes it at run time and remembers it
#endif
#define RCA_TEXT_OFF    ((GFX_H - RCA_TROWS * 8) / 2)   // text window sits 24 rows below the top of the picture window

#ifndef RCA_NIB_BLACK
#define RCA_NIB_BLACK 0x5          // 0101 = 50 %: ripple at 12.5 MHz, easy to filter. 0x4 = 25 % (more contrast, ripple at 6.25 MHz)
#endif
// pixel color (C_BLK, C_RED, C_GRN, C_YEL, C_BLU, C_MAG, C_CYN, C_WHT) -> 4 bit pulse pattern. Three gray levels: dark, mid, bright.
#ifndef RCA_LEVELS
#define RCA_LEVELS { RCA_NIB_BLACK, 0x7, 0x7, 0xF, RCA_NIB_BLACK, 0x7, 0x7, 0xF }
#endif

static_assert(RCA_W % 4 == 0, "row must be whole words");
static_assert(RCA_ACT_END + 9 == RCA_WORDS, "sync + back porch + picture + front porch must fill the row");
static_assert((RCA_W * RCA_H) % RCA_BB_PX == 0, "bounce buffer must divide the frame");
static_assert(RCA_BB_PX % 64 == 0, "bounce buffer must be a multiple of the DMA burst size");
static_assert(RCA_TOP_MAX >= RCA_TOP_MIN, "no room for the picture window");

static volatile int  rcaTop  = RCA_TOP_DEFAULT;      // first row of the 320x240 window
static volatile bool rcaTest = false;                // show the test pattern instead of the console / desktop
static volatile uint8_t rcaBlackNib = RCA_NIB_BLACK; // black level as a 4 bit pattern; `rca black <n>` changes it at run time (calls rcaGenInit)
static volatile bool rcaColor = false;               // experimental NTSC color mode (`rca color on`), see the COLOR MODE section below
static volatile int  rcaSat   = 60;                  // color saturation in percent (`rca sat <n>`)
static void rccGenInit();
static uint32_t rcaPixW[8];                          // color -> 4 bit pattern as 4 bytes (first bit out = lowest byte)
static uint32_t rcaBlkW;
static uint32_t rcaBlank[RCA_WORDS];                 // sync + black row
static uint32_t rcaVsync[RCA_WORDS];                 // vertical sync row
static uint32_t rcaScratch[RCA_WORDS];               // one row, for chunks that start or end in the middle of it

// 4 bit group, first bit out = top bit  ->  4 bytes, first bit out in the lowest address (little endian word)
static inline uint32_t rcaNibWord(uint8_t n) {
  return ((n >> 3) & 1u) | (((n >> 2) & 1u) << 8) | (((n >> 1) & 1u) << 16) | ((n & 1u) << 24);
}

static void rcaGenInit() {
  uint8_t lv[8] = RCA_LEVELS;
  uint8_t bn = rcaBlackNib;
  lv[0] = lv[4] = bn;                               // dark colors follow the run-time black level
  for (int i = 0; i < 8; i++) rcaPixW[i] = rcaNibWord(lv[i]);
  rcaBlkW = rcaNibWord(bn);
  for (int w = 0; w < RCA_WORDS; w++) {
    rcaBlank[w] = (w < RCA_SYNC_W) ? 0 : rcaBlkW;
    rcaVsync[w] = (w < RCA_VSYNC_LOW_W) ? 0 : rcaBlkW;
  }
  rccGenInit();
}

static inline int rcaClampTop(int t) { return t < RCA_TOP_MIN ? RCA_TOP_MIN : t > RCA_TOP_MAX ? RCA_TOP_MAX : t; }

// bring-up pattern: 8 gray bars in the top half, one-pixel columns (horizontal resolution) in the bottom half, white frame
static inline void rcaTestRow(uint32_t* a, int y) {
  for (int x = 0; x < GFX_W; x++) {
    int c;
    if (y == 0 || y == GFX_H - 1 || x == 0 || x == GFX_W - 1) c = C_WHT;
    else if (y < GFX_H / 2) c = x / (GFX_W / 8);
    else c = (x & 1) ? C_WHT : C_BLK;
    a[x] = rcaPixW[c & 7];
  }
}

// 40x24 console, one pixel row of it (ty = 0..191)
static inline void rcaTextRow(uint32_t* a, int ty) {
  int row = ty >> 3, gy = ty & 7;
  int ccy = vgaCy, ccx = vgaCx; bool con = vgaCurOn;
  for (int c = 0; c < RCA_TCOLS; c++) {
    int ch = (int)vgaText[row][c] - 32;
    if ((unsigned)ch > 94) ch = 0;
    uint32_t bits = vgaFontRam[ch][gy];
    uint32_t fg = vgaCol[row][c];
    if (con && row == ccy && c == ccx) { bits ^= 0xFF; fg = 7; }
    uint32_t fw = rcaPixW[fg & 7];
    for (int k = 0; k < 8; k++) a[k] = ((bits >> k) & 1) ? fw : rcaBlkW;     // bit 0 = leftmost pixel
    a += 8;
  }
}

// one complete row (RCA_WORDS words) of the frame
static void IRAM_ATTR rcaBuildRow(uint32_t* d, int r) {
  if (r < RCA_VSYNC_ROWS) { memcpy(d, rcaVsync, RCA_W); return; }
  memcpy(d, rcaBlank, RCA_W);
  int y = r - rcaTop;
  if ((unsigned)y < (unsigned)GFX_H) {
    uint32_t* a = d + RCA_ACT_W;
    const uint8_t* fb = gfxFb;                                  // graphics mode while a framebuffer exists
    if (rcaTest) rcaTestRow(a, y);
    else if (fb) {
      const uint8_t* s = fb + y * GFX_STRIDE;
      for (int i = 0; i < GFX_STRIDE; i++) {                    // one framebuffer byte = 2 pixels, low nibble = left
        uint32_t b = s[i];
        a[0] = rcaPixW[b & 7];
        a[1] = rcaPixW[(b >> 4) & 7];
        a += 2;
      }
    } else {
      int ty = y - RCA_TEXT_OFF;
      if ((unsigned)ty < (unsigned)(RCA_TROWS * 8)) rcaTextRow(a, ty);
    }
  }
  if (r == RCA_H - 1) { d[RCA_WORDS - 2] = 0; d[RCA_WORDS - 1] = 0; }   // the idle lines that follow are sync level
}


// ================================================== COLOR MODE (experimental) ==================================================
// Same idea as above (ONE pin, pulse-density stream, one byte per bit), but the bit clock is 8 x the NTSC color subcarrier:
//     28.636364 MHz = 8 x 3.579545 MHz, so ONE subcarrier cycle = exactly 8 bits and a pixel (4 bits) = half a cycle.
// A line is 1820 bits = 227.5 subcarrier cycles = 63.556 us = 15.734 kHz (exactly the horizontal frequency the PD-53 board's
// oscillator wants), a frame is 259 lines. Because a line is 227.5 cycles the carrier is shifted by 180 degrees from one line to the
// next (real NTSC does the same): row r of frame f uses carrier offset 4 * ((r + f) & 1) bits. The frame counter f toggles every time the
// LCD driver asks for position 0 (259 lines is an odd number, so the carrier is only continuous across frames if f is counted).
//
// Colors: every palette color is one fixed 8-bit pattern per carrier cycle (rccPat[]), chosen at init by brute force over all 256
// patterns: the pattern whose density (= luma) and first harmonic (= chroma amplitude and phase) are closest to what NTSC wants for
// that color. The pattern is indexed by ABSOLUTE carrier phase, so a pixel is just a 4-bit half of it (rccPix[color][half]).
// The color burst (8 cycles after the sync pulse, phase 180 degrees) is chosen the same way. Hue is relative to the burst, so it
// does not matter that the absolute carrier phase is arbitrary.
//
// Levels: sync 0 %, black 25 % (2/8 density, standard-like), white 100 %. The chroma ripple sits at 3.58 MHz and (for the black
// pattern 1000 1000) at 7.16 MHz, so the RC filter at the jack must be MUCH lighter than in monochrome mode (corner >= 6 MHz,
// i.e. no big capacitor; try 100-220 pF or none).
#define RCC_BIT_HZ      28636364                    // 8 x 3.579545 MHz
#define RCC_LINE_BITS   1820                        // 227.5 subcarrier cycles
#define RCC_GAP_BITS    8                           // idle clocks the LCD adds per line (hsync 3 + back porch 3 + front porch 2)
#define RCC_W           (RCC_LINE_BITS - RCC_GAP_BITS)   // 1812 bits per row handed to the LCD
#define RCC_WORDS       (RCC_W / 4)                 // 453 words of 4 bits-as-bytes (one word = one pixel = half a carrier cycle)
#define RCC_BB_PX       9664                        // bounce buffer: 151 x 64 bytes, divides W*H (48 times), 5.33 rows
#define RCC_SYNC_W      34                          // 136 bits = 4.75 us
#define RCC_BURST_W0    40                          // burst starts 160 bits = 5.6 us after the start of sync
#define RCC_BURST_N     16                          // 8 cycles = 64 bits = 2.2 us
#define RCC_PIC_W0      101                         // first picture word: 404 bits = 14.1 us (320 pixels = 44.7 us, centered in the active window)
#define RCC_VSYNC_LOW_W 420                         // vertical sync row: low for 1680 bits, then a 4.6 us serration

static_assert(RCC_W % 4 == 0, "color row must be whole words");
static_assert((RCC_W * RCA_H) % RCC_BB_PX == 0, "color bounce buffer must divide the frame");
static_assert(RCC_BB_PX % 64 == 0, "color bounce buffer must be a multiple of the DMA burst size");
static_assert(RCC_PIC_W0 + GFX_W + 2 <= RCC_WORDS, "color picture must fit in the row");
static_assert(RCC_BURST_W0 + RCC_BURST_N < RCC_PIC_W0, "burst must end before the picture");
static_assert((RCC_LINE_BITS % 8) == 4, "line must be an odd number of half cycles (carrier flips 180 degrees per line)");

static uint8_t  rccPat[8];                          // per palette color: one 8-bit carrier cycle (bit k = carrier phase 45 deg * k)
static uint8_t  rccBurstPat;
static uint32_t rccPix[8][2];                       // [color][half]: half 0 = bits 0..3 of the cycle, half 1 = bits 4..7 (as 4 bytes)
static uint32_t rccBurstW[2], rccBlkW[2];
static uint32_t rccBlank[2][RCC_WORDS];             // sync + burst + black, for carrier offset 0 / 4 bits
static uint32_t rccVsync[RCC_WORDS];
static uint32_t rccScratch[RCC_WORDS];
static volatile uint32_t rccFrame = 0;

// pattern p (bit k set = pulse in slot k of the cycle): density d and chroma phasor T = A * e^(j*phase) of its first harmonic,
// defined so that the signal d + A*sin(2*pi*k/8 + phase) has phasor T
static void rccEval(int p, float& d, float& tr, float& ti) {
  static const float cs[8] = { 1.0f, 0.70710678f, 0.0f, -0.70710678f, -1.0f, -0.70710678f, 0.0f, 0.70710678f };
  static const float sn[8] = { 0.0f, 0.70710678f, 1.0f, 0.70710678f, 0.0f, -0.70710678f, -1.0f, -0.70710678f };
  float xr = 0, xi = 0; int n = 0;
  for (int k = 0; k < 8; k++) if ((p >> k) & 1) { n++; xr += cs[k]; xi -= sn[k]; }   // X = sum e^(-j*2*pi*k/8)
  d = n / 8.0f;
  tr = -xi / 4.0f; ti = xr / 4.0f;                                                   // T = j*X/4
}

static inline uint32_t rccHalfWord(uint8_t pat, int h) {
  uint32_t w = 0;
  for (int k = 0; k < 4; k++) if ((pat >> (4 * h + k)) & 1) w |= 1u << (8 * k);       // first bit out = lowest byte
  return w;
}

// picks the best 8-bit pattern for every palette color for a given burst pattern; returns the total error (fills pat[] when given)
static float rccSolveColors(int burstPat, float sat, uint8_t* pat) {
  float d0, br, bi; rccEval(burstPat, d0, br, bi);
  float ab = sqrtf(br * br + bi * bi), pb = atan2f(bi, br);           // actual burst amplitude and phase (the ideal is 180 degrees)
  float k = sat / 100.0f * 0.75f * (ab / 0.15f);                      // 0.75 = density range of 100 IRE; ab/0.15 keeps chroma/burst right
  float total = 0;
  for (int c = 1; c < 7; c++) {
    float R = (c & 1) ? 1.0f : 0.0f, G = (c & 2) ? 1.0f : 0.0f, B = (c & 4) ? 1.0f : 0.0f;
    float Y = 0.299f * R + 0.587f * G + 0.114f * B;
    float U = 0.492f * (B - Y), V = 0.877f * (R - Y);
    float A = k * sqrtf(U * U + V * V), ph = atan2f(V, U) + (pb - 3.14159265f);   // hue is relative to the burst we really send
    float Tr = A * cosf(ph), Ti = A * sinf(ph), dt = 0.25f + 0.75f * Y;
    float bc = 1e9f; int bp = 0;
    for (int p = 0; p < 256; p++) {
      float d, tr, ti; rccEval(p, d, tr, ti);
      float cc = 1.5f * (d - dt) * (d - dt) + (tr - Tr) * (tr - Tr) + (ti - Ti) * (ti - Ti);
      if (cc < bc) { bc = cc; bp = p; }
    }
    total += bc;
    if (pat) pat[c] = (uint8_t)bp;
  }
  return total;
}

static void rccGenInit() {
  // burst: any pattern with exactly the black density (2 pulses of 8) and enough amplitude; the one that lets the 6 colors come out best wins
  float sat = (float)rcaSat, best = 1e9f; int bb = 0x11;
  for (int p = 1; p < 256; p++) {
    float d, tr, ti; rccEval(p, d, tr, ti);
    if (d != 0.25f || sqrtf(tr * tr + ti * ti) < 0.12f) continue;
    float t = rccSolveColors(p, sat, nullptr);
    if (t < best) { best = t; bb = p; }
  }
  rccBurstPat = (uint8_t)bb;
  rccSolveColors(bb, sat, rccPat);
  rccPat[0] = 0x11;                                                   // black: pulses in slots 0 and 4 = exactly the black level, no chroma
  rccPat[7] = 0xFF;                                                   // white
  for (int c = 0; c < 8; c++) { rccPix[c][0] = rccHalfWord(rccPat[c], 0); rccPix[c][1] = rccHalfWord(rccPat[c], 1); }
  for (int h = 0; h < 2; h++) { rccBurstW[h] = rccHalfWord(rccBurstPat, h); rccBlkW[h] = rccPix[0][h]; }
  for (int p = 0; p < 2; p++) {                                       // p = carrier offset of the row in half cycles
    for (int w = 0; w < RCC_WORDS; w++) {
      int h = (w + p) & 1;
      uint32_t v;
      if (w < RCC_SYNC_W) v = 0;
      else if (w >= RCC_BURST_W0 && w < RCC_BURST_W0 + RCC_BURST_N) v = rccBurstW[h];
      else v = rccBlkW[h];
      rccBlank[p][w] = v;
    }
  }
  for (int w = 0; w < RCC_WORDS; w++) rccVsync[w] = (w < RCC_VSYNC_LOW_W) ? 0 : rccBlkW[w & 1];
}

static inline void rccTestRow(uint32_t* a, int y, int h0) {          // 8 color bars in the top half, one-pixel columns below
  for (int x = 0; x < GFX_W; x++) {
    int c;
    if (y == 0 || y == GFX_H - 1 || x == 0 || x == GFX_W - 1) c = C_WHT;
    else if (y < GFX_H / 2) c = x / (GFX_W / 8);
    else c = (x & 1) ? C_WHT : C_BLK;
    a[x] = rccPix[c & 7][(h0 + x) & 1];
  }
}

static inline void rccTextRow(uint32_t* a, int ty, int h0) {
  int row = ty >> 3, gy = ty & 7;
  int ccy = vgaCy, ccx = vgaCx; bool con = vgaCurOn;
  for (int c = 0; c < RCA_TCOLS; c++) {
    int ch = (int)vgaText[row][c] - 32;
    if ((unsigned)ch > 94) ch = 0;
    uint32_t bits = vgaFontRam[ch][gy];
    uint32_t fg = vgaCol[row][c] & 7;
    if (con && row == ccy && c == ccx) { bits ^= 0xFF; fg = 7; }
    for (int k = 0; k < 8; k++) a[k] = rccPix[((bits >> k) & 1) ? fg : 0][(h0 + k) & 1];
    a += 8;
  }
}

static void IRAM_ATTR rccBuildRow(uint32_t* d, int r, uint32_t fr) {
  if (r < RCA_VSYNC_ROWS) { memcpy(d, rccVsync, RCC_W); return; }
  int p = (r + (int)fr) & 1;
  memcpy(d, rccBlank[p], RCC_W);
  int y = r - rcaTop;
  if ((unsigned)y < (unsigned)GFX_H) {
    uint32_t* a = d + RCC_PIC_W0;
    int h0 = (RCC_PIC_W0 + p) & 1;                  // half cycle of the first picture word
    const uint8_t* fb = gfxFb;
    if (rcaTest) rccTestRow(a, y, h0);
    else if (fb) {
      const uint8_t* s = fb + y * GFX_STRIDE;
      for (int i = 0; i < GFX_STRIDE; i++) {
        uint32_t b = s[i];
        a[0] = rccPix[b & 7][h0];
        a[1] = rccPix[(b >> 4) & 7][h0 ^ 1];
        a += 2;
      }
    } else {
      int ty = y - RCA_TEXT_OFF;
      if ((unsigned)ty < (unsigned)(RCA_TROWS * 8)) rccTextRow(a, ty, h0);
    }
  }
  if (r == RCA_H - 1) { d[RCC_WORDS - 2] = 0; d[RCC_WORDS - 1] = 0; }   // the idle lines that follow are sync level
}

static void IRAM_ATTR rccFillChunk(uint8_t* buf, int pos, int len) {
  if (pos == 0) rccFrame = rccFrame + 1;
  uint32_t fr = rccFrame;
  int row = pos / RCC_W, off = pos % RCC_W;
  while (len > 0) {
    int n = RCC_W - off;
    if (n > len) n = len;
    if (row >= RCA_H) { memset(buf, 0, n); }
    else if (off == 0 && n == RCC_W) rccBuildRow((uint32_t*)buf, row, fr);
    else { rccBuildRow(rccScratch, row, fr); memcpy(buf, (const uint8_t*)rccScratch + off, n); }
    buf += n; len -= n; row++; off = 0;
  }
}
// ================================================ end of COLOR MODE ================================================

// the LCD driver asks for [pos, pos+len) bytes of the frame: build the rows that overlap it
static void IRAM_ATTR rcaFillChunk(uint8_t* buf, int pos, int len) {
  if (rcaColor) { rccFillChunk(buf, pos, len); return; }
  int row = pos / RCA_W, off = pos % RCA_W;
  while (len > 0) {
    int n = RCA_W - off;
    if (n > len) n = len;
    if (row >= RCA_H) { memset(buf, 0, n); }
    else if (off == 0 && n == RCA_W) rcaBuildRow((uint32_t*)buf, row);
    else { rcaBuildRow(rcaScratch, row); memcpy(buf, (const uint8_t*)rcaScratch + off, n); }
    buf += n; len -= n; row++; off = 0;
  }
}

#endif
