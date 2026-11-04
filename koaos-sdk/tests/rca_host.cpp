/*
 * rca_host.cpp -- tests the composite video generator (KoaOS/koa_rcagen.h) on a PC, with address/undefined-behavior sanitizers on.
 *   g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I../../KoaOS rca_host.cpp -o rca_host && ./rca_host
 * It plays the part of the LCD peripheral: it asks for the frame in 8512-byte bounce-buffer chunks exactly as the ISR is asked,
 * stitches the bytes together (+ the 4 idle clocks the hardware adds per line, + 3 idle lines per frame) and checks the resulting
 * bit stream: line/frame timing, sync/black/white densities, where a framebuffer pixel and a text glyph land, the test pattern.
 * It also runs the stream through an ideal 2 stage RC filter to see the levels the monitor would get (a plausibility check of
 * the encoding, not a model of the real jack network).
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>

#define VGA_COLS 80
#define VGA_ROWS 30
static char     vgaText[VGA_ROWS][VGA_COLS];
static uint8_t  vgaCol[VGA_ROWS][VGA_COLS];
static uint8_t  vgaFontRam[95][8];
static volatile int  vgaCx = 0, vgaCy = 0;
static volatile bool vgaCurOn = false;
#include "koa_rcagen.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t fbMem[GFX_BYTES];

// the LCD: ask for the frame chunk by chunk, like the bounce-buffer interrupt
static std::vector<uint8_t> frame() {
  std::vector<uint8_t> f((size_t)RCA_W * RCA_H);
  std::vector<uint8_t> bb(RCA_BB_PX + 16);
  for (int pos = 0; pos < RCA_W * RCA_H; pos += RCA_BB_PX) {
    memset(bb.data(), 0xEE, bb.size());
    rcaFillChunk(bb.data(), pos, RCA_BB_PX);
    CHECK(bb[RCA_BB_PX] == 0xEE, "chunk wrote past its end");
    memcpy(&f[pos], bb.data(), RCA_BB_PX);
  }
  return f;
}
static const uint8_t* row(const std::vector<uint8_t>& f, int r) { return &f[(size_t)r * RCA_W]; }
static int ones(const uint8_t* p, int a, int b) { int n = 0; for (int i = a; i < b; i++) n += p[i]; return n; }

// density of a word-aligned stretch: fraction of ones
static double dens(const uint8_t* p, int a, int b) { return (double)ones(p, a, b) / (b - a); }

int main() {
  rcaGenInit();
  memcpy(vgaFontRam, vgaFont, sizeof(vgaFontRam));
  memset(vgaText, ' ', sizeof(vgaText)); memset(vgaCol, 7, sizeof(vgaCol));

  // ---- geometry and rates
  CHECK(RCA_W * RCA_H % RCA_BB_PX == 0, "bounce buffer must divide the frame");
  CHECK(RCA_BB_PX % 64 == 0, "burst alignment");
  double lineHz = (double)RCA_BIT_HZ / RCA_LINE_BITS, frameHz = lineHz / (RCA_H + 3);
  printf("line %.3f kHz, frame %.2f Hz, %d lines, bounce %d bytes = %.1f rows = %.0f us\n", lineHz / 1000, frameHz, RCA_H + 3,
         RCA_BB_PX, (double)RCA_BB_PX / RCA_W, RCA_BB_PX / 25.0);
  CHECK(fabs(lineHz - 15625) < 1, "line rate");
  CHECK(frameHz > 59.0 && frameHz < 61.0, "frame rate %.2f", frameHz);
  CHECK(fabs(RCA_SYNC_W * 4 / 25.0 - 4.8) < 0.01, "sync width");
  CHECK(fabs(RCA_ACT_W * 4 / 25.0 - 11.2) < 0.01, "picture starts 11.2 us after sync start");
  CHECK(fabs(GFX_W * 4 / 25.0 - 51.2) < 0.01, "picture is 51.2 us wide");

  // ---- blank frame (no framebuffer, empty console): structure of every kind of row
  gfxFb = nullptr;
  auto f = frame();
  for (int r = 0; r < RCA_VSYNC_ROWS; r++) {
    const uint8_t* p = row(f, r);
    CHECK(ones(p, 0, RCA_VSYNC_LOW_W * 4) == 0, "vsync row %d must be low for 1480 bits", r);
    CHECK(dens(p, RCA_VSYNC_LOW_W * 4, RCA_W) > 0.4, "vsync row %d needs its serration", r);
  }
  int blankRows = 0;
  for (int r = RCA_VSYNC_ROWS; r < RCA_H; r++) {
    const uint8_t* p = row(f, r);
    bool ok = ones(p, 0, 120) == 0;                                   // sync tip: 120 zero bits
    CHECK(ok, "row %d sync tip", r);
    double d = dens(p, 120, RCA_W - 8);
    CHECK(fabs(d - 0.5) < 0.01, "row %d black density %.3f", r, d);
    blankRows++;
  }
  CHECK(blankRows == RCA_H - RCA_VSYNC_ROWS, "rows");
  CHECK(ones(row(f, RCA_H - 1), RCA_W - 8, RCA_W) == 0, "last row must end in zeros (the idle lines after it are then sync level)");

  // ---- chunked fill == row-by-row fill (the slicing path)
  {
    std::vector<uint32_t> w(RCA_WORDS);
    int bad = 0;
    for (int r = 0; r < RCA_H; r++) { rcaBuildRow(w.data(), r); if (memcmp(w.data(), row(f, r), RCA_W)) bad++; }
    CHECK(bad == 0, "%d rows differ between chunked and direct build", bad);
    // odd chunk sizes (multiples of 4) must give the same bytes too
    std::vector<uint8_t> g((size_t)RCA_W * RCA_H);
    for (int pos = 0; pos < RCA_W * RCA_H; pos += 1000) rcaFillChunk(&g[pos], pos, 1000 < RCA_W * RCA_H - pos ? 1000 : RCA_W * RCA_H - pos);
    CHECK(g == f, "1000-byte chunks differ");
  }

  // ---- framebuffer pixels land where they should (low nibble = left pixel), colors map to levels
  memset(fbMem, 0, sizeof(fbMem)); gfxFb = fbMem;
  gfxPix(0, 0, C_WHT); gfxPix(1, 0, C_CYN); gfxPix(319, 239, C_WHT); gfxPix(100, 120, C_YEL); gfxPix(101, 120, C_BLU);
  f = frame();
  auto px = [&](int x, int y) { const uint8_t* p = row(f, rcaTop + y); uint32_t w; memcpy(&w, p + (RCA_ACT_W + x) * 4, 4); return w; };
  CHECK(px(0, 0) == rcaPixW[C_WHT], "pixel (0,0) white");
  CHECK(px(1, 0) == rcaPixW[C_CYN], "pixel (1,0) cyan -> mid");
  CHECK(px(2, 0) == rcaPixW[C_BLK], "pixel (2,0) black");
  CHECK(px(319, 239) == rcaPixW[C_WHT], "last pixel");
  CHECK(px(100, 120) == rcaPixW[C_YEL] && px(101, 120) == rcaPixW[C_BLU], "pair order");
  CHECK(px(0, 1) == rcaPixW[C_BLK], "row below untouched");
  // rows outside the window stay blank
  CHECK(dens(row(f, rcaTop - 1), 120, RCA_W - 8) > 0.49 && dens(row(f, rcaTop - 1), 120, RCA_W - 8) < 0.51, "row above window is plain black");
  // picture window position tracks rcaTop and stays inside the frame at both limits
  for (int t = RCA_TOP_MIN; t <= RCA_TOP_MAX; t += RCA_TOP_MAX - RCA_TOP_MIN) {
    rcaTop = t; auto g = frame();
    uint32_t w; memcpy(&w, row(g, t) + RCA_ACT_W * 4, 4);
    CHECK(w == rcaPixW[C_WHT], "window at top=%d", t);
    memcpy(&w, row(g, t + 239) + (RCA_ACT_W + 319) * 4, 4);
    CHECK(w == rcaPixW[C_WHT], "window bottom at top=%d", t);
  }
  rcaTop = RCA_TOP_DEFAULT;

  // ---- text mode: glyph orientation (bit 0 = leftmost pixel) and cursor
  gfxFb = nullptr;
  vgaText[0][0] = 'A'; vgaText[23][39] = 'Z'; vgaText[5][10] = '#';
  f = frame();
  auto glyphOk = [&](int col, int rowc, char ch) {
    int bad = 0;
    for (int gy = 0; gy < 8; gy++) {
      const uint8_t* p = row(f, rcaTop + RCA_TEXT_OFF + rowc * 8 + gy);
      for (int k = 0; k < 8; k++) {
        uint32_t w; memcpy(&w, p + (RCA_ACT_W + col * 8 + k) * 4, 4);
        bool on = (vgaFont[ch - 32][gy] >> k) & 1;
        if (w != (on ? rcaPixW[7] : rcaBlkW)) bad++;
      }
    }
    return bad == 0;
  };
  CHECK(glyphOk(0, 0, 'A'), "glyph A at the top left");
  CHECK(glyphOk(39, 23, 'Z'), "glyph Z at the bottom right");
  CHECK(glyphOk(10, 5, '#'), "glyph # in the middle");
  vgaCx = 3; vgaCy = 2; vgaCurOn = true; f = frame();
  { uint32_t w; memcpy(&w, row(f, rcaTop + RCA_TEXT_OFF + 2 * 8) + (RCA_ACT_W + 3 * 8) * 4, 4); CHECK(w == rcaPixW[7], "cursor block is white"); }
  vgaCurOn = false;

  // ---- test pattern
  rcaTest = true; gfxFb = nullptr; f = frame();
  CHECK(px(0, 50) == rcaPixW[C_WHT] && px(319, 50) == rcaPixW[C_WHT], "test frame");
  for (int b = 1; b < 8; b++) CHECK(px(b * 40 + 5, 50) == rcaPixW[b], "bar %d", b);
  CHECK(px(11, 200) == rcaPixW[C_WHT] && px(12, 200) == rcaPixW[C_BLK], "one-pixel columns");
  rcaTest = false;

  // ---- what the monitor sees: ideal 2 stage RC (180 ohm x 470 pF each), one frame of text + a white box
  memset(fbMem, 0, sizeof(fbMem)); gfxFb = fbMem; gfxRect(100, 60, 80, 40, C_WHT); gfxRect(100, 120, 80, 40, C_CYN);
  f = frame();
  std::vector<double> sig;                                         // bits at 40 ns, with the 4 idle clocks (assume the pin holds 0) per line
  for (int r = 0; r < RCA_H; r++) { for (int i = 0; i < RCA_W; i++) sig.push_back(row(f, r)[i]); for (int i = 0; i < RCA_GAP_BITS; i++) sig.push_back(0); }
  double a = 1.0 - exp(-40e-9 / (180.0 * 470e-12)), y1 = 0, y2 = 0;
  std::vector<double> out(sig.size());
  for (size_t i = 0; i < sig.size(); i++) { y1 += a * (sig[i] - y1); y2 += a * (y1 - y2); out[i] = y2; }
  auto at = [&](int line, int bit) { return out[(size_t)line * RCA_LINE_BITS + bit]; };
  int L = rcaTop + 80;                                              // a line through the white box
  double sync = at(L, 100), black = at(L, 200), white = at(L, RCA_ACT_W * 4 + 140 * 4), mid = at(rcaTop + 140, RCA_ACT_W * 4 + 140 * 4);
  double rip = 0; for (int i = 0; i < 20; i++) { double v = at(L, 230 + i); if (fabs(v - black) > rip) rip = fabs(v - black); }
  printf("filtered levels: sync %.2f  black %.2f  mid %.2f  white %.2f   (ripple on black +-%.2f)\n", sync, black, mid, white, rip);
  CHECK(sync < 0.05, "sync tip level %.2f", sync);
  CHECK(black > 0.40 && black < 0.60, "black level %.2f", black);
  CHECK(mid > 0.65 && mid < 0.85, "mid level %.2f", mid);
  CHECK(white > 0.95, "white level %.2f", white);
  CHECK(sync < black && black < mid && mid < white, "levels must be ordered");
  // vertical sync: the filtered signal stays at sync level for the whole broad pulse
  { double worst = 0; for (int i = 20; i < RCA_VSYNC_LOW_W * 4; i++) worst = fmax(worst, out[(size_t)1 * RCA_LINE_BITS + i]); CHECK(worst < 0.05, "vsync row level %.2f", worst); }

  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
