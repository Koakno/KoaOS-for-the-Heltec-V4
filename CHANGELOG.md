# Changelog

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
