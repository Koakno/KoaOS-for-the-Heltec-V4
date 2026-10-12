# KoaOS SDK (ABI v2)

Write apps in C, compile them to WebAssembly, pack them as `.esp`, install them on the V4 over Wi-Fi.

## What you need (on a PC, Mac or Linux box)
- `clang` with the wasm32 target and `wasm-ld`  (Debian/Ubuntu: `sudo apt install clang lld`; macOS: `brew install llvm lld`)
- `python3`, `make`

## Quick start
```
make                   # builds every app in apps/ -> dist/<name>.esp
make serve             # copies dist/*.esp to repo/ and starts the install server (prints the command for the V4)
```
On the V4:
```
install -s <pc-ip>:8000      # point it at your computer (once)
install -l                   # list what the server has
install hellowasm            # download + verify + install
hellowasm                    # run it (type the app's name)
```

## Your first app
```
apps/myapp/main.c
apps/myapp/manifest
```
`main.c`:
```c
#include "koaos.h"
int koa_main(void) {
  koa_puts("hi");
  return 0;
}
```
`manifest` (entry must be `<name>.wasm`; `name` must match the folder):
```
name=myapp
version=1.0
abi=2
type=wasm
entry=myapp.wasm
requires=gps,wifi        # only what you use: wifi, gps, lora, display, gpio, uart, i2c, spi
```
`abi=2` is right for anything new. Use `abi=1` only if you want the package to also load on firmware older than 0.10
(then you cannot use GPIO/UART/I2C/SPI).
Then `make myapp`.

## API
Everything in `include/koaos_abi.h` (the OS side implements exactly that list: 48 functions, checked against the firmware's import table) plus helpers from `include/koaos.h`:
`koa_printf / koa_snprintf / koa_puts / koa_print / koa_putc`, `koa_strerror(err)`, `koa_alive()`.
There is no libc: use these instead of printf/malloc; only a few string/memory functions exist (`memcpy memmove memset memcmp strlen strcmp strncmp strchr`). Use static buffers.

Rules the OS enforces:
- **Capabilities:** a resource only works if it is in the manifest `requires=` *and* the app calls `koa_acquire(KC_...)` first (GPS, Wi-Fi, LoRa, display, GPIO/UART, I2C, SPI). Otherwise you get `KE_PERM`. `koa_release` when done. LoRa, display, I2C and SPI are exclusive (`KE_BUSY` if another app has them). GPS is shared.
- **Errors** are negative numbers (`KE_*`). `koa_http_get` returns bytes, or `KE_*`, or `-<http status>` (e.g. -404).
- **Stopping:** call `koa_alive()` in loops (or any `koa_*` call). If it returns 0 / `KE_KILLED`, clean up and return. Pure compute loops get killed by the OS after a while.
- **Files:** `koa_open("name", KO_RD/KO_WR/KO_APP)`. Flat names, stored in the app's private `/data/<app>/`.

## Limits (firmware defaults)
- `.wasm` file: 256 KB max. Whole app (memory + code + file): about 1 MB of PSRAM. Build uses a 16 KB C stack and 64 KB starting memory (`LDFLAGS` in the Makefile).
- The interpreter's own value stack is only 12 KB: keep recursion shallow.
- wasm3 has no tail calls on the ESP32-S3, so native stack use grows with the longest straight-line run of code (no loops, calls or branches). The firmware gives the interpreter a 48 KB stack (firmware 0.9+). Keep hot functions small, and don't unroll: the Makefile builds with `-Os -fno-unroll-loops`.
- Apps run at idle priority; they are interpreted, so heavy maths is slow.
- No command-line arguments yet (the ABI has no call for them), no display drawing, GPIO interrupts, PWM or ADC yet.

## GPIO, UART, I2C, SPI (ABI 2)
The manifest names the *kind* (`requires=gpio,i2c,spi`; `gpio` also covers UART). The app picks the **pins at run time**,
so moving a wire never needs a rebuild. Set `abi=2` in the manifest.
```c
koa_acquire(KC_GPIO | KC_UART);                       /* what the manifest allowed */
koa_gpio_request(4, KG_OUT_LOW);   koa_gpio_write(4, 1);
int u = koa_uart_open(/*rx*/ 47, /*tx*/ 6, 38400, KU_8N1);   /* returns a handle */
koa_uart_write(u, buf, n);         int got = koa_uart_read(u, buf, sizeof buf, 100);

koa_acquire(KC_I2C);   koa_i2c_open(/*sda*/ 5, /*scl*/ 6, 100000);
koa_i2c_write(0x40, reg, 1, /*stop*/ 0);   koa_i2c_read(0x40, data, 2);

koa_acquire(KC_SPI);   koa_spi_open(/*sck*/ 40, /*mosi*/ 41, /*miso*/ 42, /*cs*/ 48, 8000000, KSPI_MODE0);
koa_spi_transfer(tx, rx, n, 0);
```
Rules: every pin is checked by the OS. `KE_PERM` = not a pin apps may use (or capability missing), `KE_BUSY` = someone
owns it, `KE_INVAL` = bad number. The usable pins are the firmware's allow-list (`KoaOS/koa_io.h`; type `pins` on the
board). The GPS, radio, OLED and USB pins are never available; use `KC_GPS` / `KC_LORA` for those. Two UART ports exist
(`KE_BUSY` for a third), I2C and SPI each have one controller (exclusive). Released or killed apps give everything
back; pins return to high-impedance input. I2C needs external pull-ups. See `apps/iodemo`.

## Testing
`make selftest` checks the SDK's printf against the system libc, `make iotest` checks the pin policy and manifest parser used for GPIO/UART/I2C/SPI, and `make mctest` runs the MeshCore app natively against scripted radio traffic. The apps are compiled to wasm with clang and run on a real V4 (hellowasm, gpswatch, webget, meshcore). If `make` fails on your machine, open an issue with the first error line.

## Files
```
include/koaos_abi.h   the ABI (identical to the firmware's)
include/koaos.h       SDK helpers        src/koaos.c   entry point + mini libc + printf
apps/                 hellowasm, gpswatch, webget, meshcore, iodemo
tools/esppack.py      pack/inspect .esp   tools/repo_server.py   install server
tests/host_test.c     `make selftest`
```
Keep `koaos_abi.h` in sync with the firmware: bump `KOA_ABI_VERSION` only when adding functions.
