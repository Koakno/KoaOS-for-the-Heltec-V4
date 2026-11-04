# Changelog

> **Experimental branch.** 0.11 to 0.14 add VGA and RCA composite output, a graphical desktop, a Bluetooth keyboard, drawing on the OLED for apps
> and the `i2c` shell command. The video outputs and Bluetooth are opt-in at run time (`vga on`, `rca on`, `gui`, `ble on`).
> The stable release is 0.10 on `main`.

## 0.14 (experimental): RCA composite video on GPIO6, with an experimental color mode
- **`rca on|off|status|boot on|off|test|pos <n>|drive <0-3>|black <2-6>|color on|off|sat <n>`.** Composite video on one pin (GPIO6, GPIO3 is a
  dummy clock), switched at run time like `vga`. Same LCD-peripheral trick as VGA: no framebuffer, the bounce-buffer callback builds every
  scanline. The pin carries a pulse-density bit stream that an RC filter at the jack turns into sync, black and white. New files
  `koa_rca.h` (driver) and `koa_rcagen.h` (portable scanline generator, `make rcatest`).
- **Monochrome (default):** 25 Mbit/s stream (sync 0000, black 0101, white 1111), NTSC-style, 15.625 kHz lines, 259 lines per frame, 60.3 Hz.
  Text is 40x24; the 8 desktop colors map to 3 gray levels.
- **Color mode (experimental, `rca color on`).** The bit clock is 28.636 MHz, 8 times the NTSC color subcarrier, so one carrier cycle is exactly
  8 bits. Lines are 1820 bits (15.734 kHz), about 60.75 Hz, with a color burst after sync. Each palette color is one fixed 8-bit carrier pattern,
  picked at start-up by searching all 256 patterns for the one closest to the wanted brightness and hue. The setting is saved (`/config/rcacolor`)
  and takes effect at the next `rca on`; if RCA is running outside the desktop it restarts by itself. `rca sat <0-150>` sets the saturation
  (default 60). The filter at the jack must be much lighter than for monochrome (try 100-220 pF or none).
- **`rca drive <0-3>`** sets the output strength of GPIO6 (default 3, the strongest) and **`rca black <2-6>`** the monochrome black level
  (default 5 = 50%). Neither is saved. `rca pos <n>` (3..16, default 8) is saved in `/config/rcapos`; `rca boot on` creates `/config/rca`.
- **`gui` detects RCA.** If `rca on` is running, the desktop is drawn on the composite output (same 320x240 framebuffer) and VGA is left alone.
- GPIO6 is also VGA's blue pin, so VGA and RCA exclude each other through the pin table (`pins` shows `rca` or `vga`).
- `rca test` shows gray bars + a frame; `rca pos <n>` moves the picture up/down by scanlines and is remembered.
- Hardware: the first test on a real V4 found that the pin delivered too little amplitude at the monitor, so `rca drive` defaults to the strongest
  setting. Color mode is experimental.

## 0.13 (experimental): apps can draw on the OLED and use its I2C bus (ABI 3)
- **Drawing on the OLED (`requires=display`).** Eight new calls: `koa_oled_clear / flush / pixel / line / rect / text / bitmap / contrast`.
  An app that holds the display owns the screen: the console, editor and other built-in drawing step aside (the shell keeps working over
  USB and VGA) and come back when the app exits or releases it. Three fonts, three draw colors (black, white, XOR), everything clipped,
  all coordinates validated. The contrast goes back to the default when the app lets go.
- **I2C on the OLED bus.** `koa_i2c_open(KOA_I2C_OLED_SDA, KOA_I2C_OLED_SCL, hz)` (GPIO17/18) shares the display's own bus with the
  app instead of claiming pins. One lock covers the frame buffer and the bus, so a screen update and an app's transfer never
  interleave. At most 400 kHz; the bus returns to the display's 400 kHz after every app transfer; the display chip (0x3C/0x3D) can be
  probed but not written or read; a write with `stop=0` is held back and sent together with the read that follows it (repeated start).
  GPIO17/18 stay off-limits as plain pins. `pins` shows who holds the shared bus.
- **`i2c` shell command:** `i2c scan`, `i2c get <addr> [reg] [count]`, `i2c set <addr> <byte>...` on the OLED bus, to check wiring without
  writing an app.
- SDK: `koa_i2c_probe / scan / write_reg / read_reg`, `koa_oled_printf`; new apps `i2cscan`, `oleddemo`, `pca9555` (16-bit I2C GPIO expander);
  `make apptest` runs them natively against a fake OS. ABI bumped to 3 (apps built for 1 or 2 run unchanged).
- Since tested on a real V4: OLED drawing, I2C on the OLED bus, the `i2c` command and the `i2cscan`, `oleddemo` and `pca9555` apps work (PCA9555 at address 0x20). It was first checked only with stubs on a PC.

## Bluetooth LE keyboard (experimental; no version number of its own, listed in the order it was added)
- **`ble` command: use a BLE keyboard on the console.** `ble on`, `ble off`, `ble scan [all]`, `ble pair <n>`, `ble status`, `ble forget`, `ble boot on|off`.
  The V4 pairs with a BLE HID keyboard (bonded, passkey shown on the OLED/VGA when the keyboard asks for one), subscribes to its
  input reports and feeds the same bytes a serial terminal would send into the console, so the shell, editor, `gui` and apps work
  unchanged on OLED, VGA and USB at once. Auto-reconnects to the paired keyboard; shows its battery level in `ble status`.
  Off until you type `ble on`. New files `KoaOS/koa_ble.h` (NimBLE glue + manager task) and `KoaOS/koa_hid.h` (portable report ->
  bytes translator: US layout, Shift/Ctrl/Alt, Caps Lock, arrows, Home/End/Del/PgUp/PgDn, keypad, auto-repeat) with `make hidtest`.
- Needs the **NimBLE-Arduino 2.5.1** library. `#define KOA_NO_BLE` (top of `koa_ble.h`) leaves it out.

## 0.12 (experimental)
- **Graphical desktop (`gui`).** New graphics mode in the VGA driver: a 320x240, 4 bit/pixel framebuffer (38 KB, internal RAM, allocated
  only while the desktop runs) doubled to 640x480. `gui` starts a launcher with Files, Wi-Fi, GPS, System, Apps, Terminal, Editor and Power,
  driven by arrow keys / Enter / Esc over the USB serial terminal. `gui boot on|off` starts it at boot (before login). New files:
  `koa_gui.h`, `koa_gfx.h` (portable drawing library), `koa_font.h` (font moved out of `koa_vga.h`). `vga off` refuses while the desktop runs;
  killing the desktop frees the framebuffer and text mode returns.
- **`weather`, `aprs` and `aprsd` stay built in** (the experimental merge keeps them, so this build matches the stable 0.10 command set
  plus `vga` and `gui`). If flash or RAM gets tight, they are the first ~300 lines to move out into apps.
- **Removed the ESP8266 link:** `link`, `x`, the UART bridge, `LINK_*` options, `hardware/esp8266-coprocessor/` and the `esp8266flash` app.
  GPIO15/16 are simply two more free pins in the app allow-list.
- `make guitest` renders the desktop on a PC (`koaos-sdk/tests/gui_host.cpp`).

## 0.11
- **VGA output, switched at run time.** New shell command `vga on|off|status` (`vga enabled` / `vga disabled` also work) and
  `vga boot on|off` to start it at boot. Mirrors the console to a 640x480 monitor (80x30 text) with no framebuffer and no PSRAM:
  scanlines are built on the fly in the LCD bounce-buffer callback. `vga off` deletes the LCD panel and returns GPIO 3 4 5 6 47 48
  to plain inputs. The old compile-time `#define KOA_VGA` is gone; `#define KOA_NO_VGA` compiles the driver out (arduino-esp32 2.x).
- **Pin ownership for the OS itself.** VGA claims its six pins in the pin table (`kio_claim_set_sys` in `koa_io.h`), shown as
  `vga` by `pins`. Apps get "busy" for them while VGA is on, and `vga on` refuses to start (naming the holder) if any of them is taken.
- **Link bridge moved to GPIO15 (RX) / GPIO16 (TX)** so it can run together with VGA (it used GPIO4/5, which VGA needs). 15 and 16 are
  now in the app allow-list. `esp8266flash` uses the same two pins for TX/RX. `koa_io.h` also documents that GPIO40/41/42 are wired to
  the GNSS connector.
- **Link bridge** (second console + GPIO expander over a UART to an ESP8266): `link on|off|status` and `x <gpio command>`;
  `LINK_AUTOSTART` build option. New `hardware/esp8266-coprocessor/` (8266 sketch, flashing guide).
- **`esp8266flash` app** (writes the 8266 firmware through its ROM bootloader) added to the SDK apps (build it with `make esp8266flash`).
  The SDK Makefile now packs `*.bin` files that sit next to an app's `main.c` (needed for its `fw.bin`).
- `make iotest` also covers the OS pin claims.

## 0.10
- **GPIO, UART, I2C and SPI for apps (ABI 2).** The manifest declares the kind of I/O (`requires=gpio,i2c,spi`;
  `gpio` includes UART); the app picks the actual pins at run time. New calls: `koa_gpio_*`, `koa_uart_*`,
  `koa_i2c_*`, `koa_spi_*`. Apps built for ABI 1 run unchanged.
- OS-side pin policy in the new `KoaOS/koa_io.h`: an allow-list of pins apps may use, plus a hard-reserved set (GPS, LoRa,
  OLED, USB, Vext, UART0 pads) that can never be granted. One owner per pin; everything is released when the app exits or is killed.
- I2C runs on `Wire1` and SPI on `HSPI`, separate from the OLED and the radio. UART uses hardware UART2 and UART0
  (UART1 stays with the GPS driver). GPS remains shared between any number of apps.
- New shell command `pins`. New SDK example `iodemo`. New host test `make iotest` (pin policy + manifest parser).
- `esppack.py` accepts the new manifest capabilities.
- Docs: new in-depth README, tidied ROADMAP, SDK README example manifest now uses `abi=2`.

## 0.9
- Wasm interpreter task stack raised from 24 KB to 48 KB. Fixes a stack-canary panic when running apps with long
  straight-line code (the MeshCore app crashed during identity generation).
- SDK apps now build with `-Os -fno-unroll-loops` to keep straight-line runs short.
- Added the `meshcore` app (MeshCore chat client) with native tests (`make mctest`).
- Repo reorganised: one firmware sketch, one SDK, patched Wasm3 trimmed to the library sources.

## 0.8
- wasm3 app runtime with PSRAM-only app memory, app capabilities, IPC and crypto host calls.
- `.esp` package format (manifest + files + SHA-256), `install` / `uninstall` / `apps`, Wi-Fi install server.
- Allocator patch for wasm3 (`tools/patch_wasm3.py`). Without it, apps refuse to start instead of using internal RAM.
- Verified: full compile for esp32:esp32:heltec_wifi_lora_32_V4 (PSRAM on), arduino-esp32 3.3.12, Wasm3 0.5.0 (patched),
  RadioLib 7.8.1, U8g2 2.37.1, TinyGPSPlus 1.1.0; all 33 ABI functions have matching host imports;
  `esp_pkg.h` parser fuzzed with 4000 corrupted packages under ASAN/UBSAN.
