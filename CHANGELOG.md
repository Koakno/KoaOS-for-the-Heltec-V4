# Changelog

> **Experimental branch.** 0.11 and 0.12 add VGA output and a graphical desktop. Both are opt-in at run time (`vga on`, `gui`).
> The stable release is 0.10 on `main`.

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
