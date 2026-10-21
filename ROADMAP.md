# Roadmap

Where KoaOS is and where it is going. This is a plan, not a promise: priorities change as people use it.
Status as of release **0.10**.

**Legend:** ✅ done · 🟡 partly done · ⬜ planned

## Done

| | Feature | Notes |
|---|---|---|
| ✅ | Installable apps | `install`, `uninstall`, `apps`; private `/data/<app>/` per app; wasm and script packages |
| ✅ | Local Wi-Fi repository | `koaos-sdk/tools/repo_server.py` serves a folder; the board uses `install -s`, `-l`, `<app>` |
| ✅ | `.esp` package format | magic + manifest + file table + payload + SHA-256 (`esp_pkg.h`, `esppack.py`) |
| ✅ | SDK | C to wasm32 with clang and wasm-ld, mini libc, Makefile, example apps, native tests, Termux and GitHub Actions paths |
| ✅ | OS-managed hardware | `koa_acquire` / `koa_release` for Wi-Fi, GPS, LoRa, display, GPIO, UART, I2C, SPI (ABI 2) |
| ✅ | Pin policy | firmware allow-list in `koa_io.h`; GPS, radio, OLED, USB and power pins are permanently reserved; one owner per pin; everything released on exit or kill; `pins` shows ownership |
| ✅ | Capability checks | manifest `requires=` is enforced (`wifi`, `gps`, `lora`, `display`, `gpio`, `uart`, `i2c`, `spi`) |
| ✅ | MeshCore client | in-app protocol implementation, fully functional |

## In progress / partly done

| | Feature | What exists | What is missing |
|---|---|---|---|
| 🟡 | Package security | SHA-256 integrity check on every `.esp`; capability enforcement | signatures, and "verified / unknown / corrupt" trust levels |
| 🟡 | USB and Wi-Fi together | Wi-Fi install works; USB is a serial terminal | installing packages and moving files over USB |
| 🟡 | Persistence and recovery | firmware and apps are separate; `uninstall` removes a broken app without reflashing | a dedicated recovery mode beyond reset + shell |

## Next up (hardware and input)

- ⬜ **GPIO-based drivers.** Apps can already drive pins, UART, I2C and SPI. What is missing is OS-level drivers built on top.
- ⬜ **I2C keyboard support**, so the board can be used without a USB terminal.
- ⬜ **GPIO interrupts / edge waits**, **PWM** and **ADC** for apps.
- ⬜ **Display API for apps.** The `display` capability is reserved in the ABI; apps cannot draw yet.
- ⬜ Command-line arguments for apps (the ABI has no call for them yet).
- ⬜ Optional per-app pin lists in the manifest as an extra lock, SPI with several chip-selects, sharing the I2C bus with the OLED.

## Later

- ⬜ **SSH client**, so the V4 can log in to PCs and servers over Wi-Fi and act as a tiny portable terminal.
  Example: `ssh user@192.168.1.50`.
- ⬜ **Public / community app repository.** `install <name>` would search a configured online repository, download the
  package, verify it and install it. This depends on package signatures above.
- ⬜ **USB storage / file transfer** (serial, storage or MTP modes). The OS and the host computer must never write the same
  filesystem at once, so KoaOS would suspend its own access while a computer has it.
- ⬜ **MeshCore:** consider repeating, learned direct routes and retries.
- ⬜ **Other boards** with PSRAM, if there is demand.

## Direction

KoaOS is meant to stay a small embedded OS designed for the ESP32-S3, not a port of Linux. The milestone that makes it a
platform is already met: flash the OS once, install apps separately, let anyone build apps with the SDK, and let other V4
users install them over Wi-Fi from a repository they control. What remains is deepening it: stronger trust in packages,
public distribution, richer hardware support and network tools.
