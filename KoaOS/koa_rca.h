/*
 * koa_rca.h -- RCA composite video output on ONE pin (GPIO6), switched on and off at run time like VGA
 *
 * Shell:  rca on | rca off | rca status | rca boot on|off | rca test | rca pos <n> | rca drive <0-3> | rca black <2-6>
 *          | rca color on|off | rca sat <n>      (color = EXPERIMENTAL NTSC color, see COLOR MODE in koa_rcagen.h; see p_rca in KoaOS.ino)
 * Then "gui" notices that RCA is running and draws the desktop on the composite output instead of VGA.
 *
 * The picture is generated exactly like VGA text/graphics: the ESP32-S3 LCD peripheral runs without a framebuffer and the
 * bounce-buffer callback builds every scanline on the fly (koa_rcagen.h). The difference is the data: ONE pin sends a 25 Mbit/s
 * pulse-density stream that an RC filter at the jack turns into sync / black / white (NTSC-style, 15.625 kHz lines, 60.3 Hz,
 * monochrome). Text mode is 40x24, graphics mode is the same 320x240 framebuffer the VGA desktop uses (8 colors -> 3 gray levels).
 *
 * Wiring (same as the WeatherClock project; the pin is 3.3 V, 100% density = white):
 *     GPIO6 --[ 180 ohm ]--+-- RCA center pin        or:  GPIO6 --[ ~100 ohm ]-- pot end 1, pot wiper -- RCA center,
 *                          |                                                                pot end 2 -- GND
 *                   470 pF .. 1 nF                      Contrast = series resistance / pot position. If black looks hatched, add
 *                          |                            the capacitor (wiper to ground in the pot version).
 *     GND -----------------+-- RCA shield
 *     GPIO3 = dummy pixel-clock output, leave unconnected (same as VGA).
 *
 * GPIO6 is also VGA's blue pin, so VGA and RCA can never run together: the first one to start owns the pin in the pin table (owner
 * "rca" / "vga") and the other gets "pins busy". Unplug the VGA cable (or at least its blue wire) while you use RCA: a monitor's
 * 75 ohm load on the same pin would halve the composite signal.
 *
 * Needs arduino-esp32 core 3.x like VGA; KOA_NO_VGA compiles RCA out too (same LCD driver). Every function here must be called
 * with ttyMx held, like the VGA ones.
 */
#ifndef KOA_RCA_H
#define KOA_RCA_H
#include <stdint.h>

#define RCA_PIN_VIDEO  6
#ifndef RCA_PIN_PCLK
#define RCA_PIN_PCLK   3      // dummy pixel clock, same pin VGA uses; leave unconnected
#endif

// Output drive strength of GPIO6 (0 = weakest .. 3 = strongest). Default 3: on the first hardware test the picture only locked with the
// pot turned fully to the signal end, i.e. the S3 pin delivered too little amplitude at the monitor. `rca drive <0..3>` changes it live.
#ifndef RCA_DRIVE_DEFAULT
#define RCA_DRIVE_DEFAULT 3
#endif
static volatile int rcaDrive = RCA_DRIVE_DEFAULT;

#ifndef KOA_NO_VGA

#include "koa_rcagen.h"

static esp_lcd_panel_handle_t rcaPanel = nullptr;

static bool IRAM_ATTR rcaOnBounceEmpty(esp_lcd_panel_handle_t, void* bounce_buf, int pos_px, int len_px, void*) {
  rcaFillChunk((uint8_t*)bounce_buf, pos_px, len_px);
  return false;
}

static inline bool rcaActive() { return rcaReady; }

static void rcaApplyDrive() { gpio_set_drive_capability((gpio_num_t)RCA_PIN_VIDEO, (gpio_drive_cap_t)rcaDrive); }

// stops the video output and hands the pins back as plain high-impedance inputs. Safe to call when RCA is off.
static void rcaEnd() {
  vgaGfxEnd();
  rcaReady = false;                                 // vgaPut/vgaBackspace stop touching the text buffer first
  rcaTest = false;
  if (rcaPanel) { esp_lcd_panel_del(rcaPanel); rcaPanel = nullptr; }
  const int pins[2] = { RCA_PIN_VIDEO, RCA_PIN_PCLK };
  for (int i = 0; i < 2; i++) {
    gpio_reset_pin((gpio_num_t)pins[i]);
    gpio_set_drive_capability((gpio_num_t)pins[i], GPIO_DRIVE_CAP_2);
    pinMode(pins[i], INPUT);
  }
  vgaCols = VGA_COLS; vgaRows = VGA_ROWS;           // the console is back to its VGA size, ready for "vga on"
}

// returns true on success (also if it is already running). On failure everything is undone.
static bool rcaBegin() {
  if (rcaReady) return true;
  if (vgaReady) return false;                       // GPIO6 is VGA's blue pin
  memcpy(vgaFontRam, vgaFont, sizeof(vgaFontRam));
  rcaGenInit();
  vgaEsc = 0; vgaNum = 0;
  vgaCols = RCA_TCOLS; vgaRows = RCA_TROWS;
  vgaClear();

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  const bool col = rcaColor;                        // experimental color mode: 28.636 MHz bit clock, 1812-bit rows (see COLOR MODE in koa_rcagen.h)
  rccFrame = 0;
  cfg.timings.pclk_hz = col ? RCC_BIT_HZ : RCA_BIT_HZ;   // one clock = one bit of the pulse-density stream
  cfg.timings.h_res = col ? RCC_W : RCA_W;          // mono: 1596 bits + 4 idle clocks = 1600 = 64.0 us per line; color: 1812 + 8 = 1820 = 63.56 us
  cfg.timings.v_res = RCA_H;                        // 256 + 3 idle lines = 259 lines
  cfg.timings.hsync_pulse_width = col ? 3 : 2;
  cfg.timings.hsync_back_porch = col ? 3 : 1;
  cfg.timings.hsync_front_porch = col ? 2 : 1;
  cfg.timings.vsync_pulse_width = 1;
  cfg.timings.vsync_back_porch = 1;
  cfg.timings.vsync_front_porch = 1;
  cfg.timings.flags.hsync_idle_low = 0;
  cfg.timings.flags.vsync_idle_low = 0;
  cfg.timings.flags.de_idle_high = 0;
  cfg.timings.flags.pclk_active_neg = 0;
  cfg.timings.flags.pclk_idle_high = 0;
  cfg.data_width = 8;
  cfg.bits_per_pixel = 8;
  cfg.num_fbs = 0;
  cfg.bounce_buffer_size_px = col ? RCC_BB_PX : RCA_BB_PX;
  cfg.hsync_gpio_num = -1;                          // no sync pins: sync is part of the video signal
  cfg.vsync_gpio_num = -1;
  cfg.de_gpio_num = -1;
  cfg.pclk_gpio_num = RCA_PIN_PCLK;
  cfg.disp_gpio_num = -1;
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = -1;
  cfg.data_gpio_nums[0] = RCA_PIN_VIDEO;            // bit 0 of every byte = the video signal
  cfg.flags.no_fb = 1;
  cfg.flags.fb_in_psram = 0;

  if (esp_lcd_new_rgb_panel(&cfg, &rcaPanel) != ESP_OK) { rcaPanel = nullptr; vgaCols = VGA_COLS; vgaRows = VGA_ROWS; return false; }
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_bounce_empty = rcaOnBounceEmpty;
  if (esp_lcd_rgb_panel_register_event_callbacks(rcaPanel, &cbs, nullptr) != ESP_OK ||
      esp_lcd_panel_reset(rcaPanel) != ESP_OK ||
      esp_lcd_panel_init(rcaPanel) != ESP_OK) { rcaEnd(); return false; }
  rcaApplyDrive();                                  // strongest drive by default: it has to push 3.3 V through ~250 ohm into 75 ohm
  rcaReady = true;
  return true;
}

#else  /* KOA_NO_VGA: empty stubs so the rest of the OS needs no #ifdefs */

static int rcaTop = 8;
static bool rcaTest = false;
static volatile uint8_t rcaBlackNib = 5;
static volatile bool rcaColor = false;
static volatile int rcaSat = 60;
static inline void rccGenInit() {}
static inline void rcaGenInit() {}
static inline void rcaApplyDrive() {}
#define RCA_TOP_MIN 3
#define RCA_TOP_MAX 16
static inline bool rcaActive() { return false; }
static inline bool rcaBegin() { return false; }
static inline void rcaEnd() {}

#endif /* KOA_NO_VGA */
#endif /* KOA_RCA_H */
