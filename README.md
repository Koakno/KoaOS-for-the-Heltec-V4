# KoaOS for Heltec V4

> **Experimental branch (0.12).** Adds VGA output and a graphical desktop on top of the stable 0.10 release. The stable code is on `main`.

A small Unix-style operating system for the **Heltec WiFi LoRa 32 V4** (ESP32-S3). Flash it once, log in over USB,
and you have a pocket computer with a shell, a text editor, GPS, Wi-Fi, weather and LoRa APRS, plus an *experimental* **graphical desktop on a VGA monitor**.
Then **install apps over Wi-Fi without ever reflashing**, or write your own with the included SDK.

*KoaOS is an independent project and is not made or endorsed by Heltec. "Heltec" is used only to say which board it runs on.*

---

## Getting started

You need a Heltec WiFi LoRa 32 **V4** (not V3: KoaOS needs the V4's PSRAM), a USB cable, and the Arduino IDE
(or ArduinoDroid on Android).

1. **Install libraries** from the Library Manager: `U8g2`, `TinyGPSPlus`, `RadioLib`.
2. **Install the patched Wasm3:** copy `lib/Wasm3` into your Arduino `libraries` folder (so you have `libraries/Wasm3`).
   Keep only one Wasm3 folder there.
3. **Open `KoaOS/KoaOS.ino`**, choose the board and settings from [Flash settings](#flash-settings), and upload.
4. **Open a serial terminal** on the board's USB port (Arduino Serial Monitor, PuTTY, or a serial app on your phone).
   Anything you type also shows on the board's OLED.
5. **Log in:** `root` / `1234`, then try:

```
root@koa:~# help
root@koa:~# free              # must show a psram line; 0/0 means PSRAM is off
root@koa:~# wifi scan
root@koa:~# wifi connect <ssid> <password>
root@koa:~# gps
root@koa:~# weather
root@koa:~# gui               # experimental: desktop on a VGA monitor (see below)
```

That is already a working system: the shell, editor, GPS, weather, APRS tools, Wi-Fi and the experimental desktop are built into the firmware, so there is
something to play with right after flashing. When you want more, see [Installing apps](#installing-apps).

---

## What it is

KoaOS turns the V4 into a small, self-contained device you operate from a terminal: no phone app, no PC needed once it is
flashed. It is **not Linux** and does not try to be. It is a compact embedded OS built around three ideas:

- **A real shell and filesystem.** Login, processes, files, an editor, simple scripts.
- **Apps you can install and remove at runtime.** The firmware stays put; apps are separate packages.
- **The OS owns the hardware.** Apps ask for Wi-Fi, GPS, LoRa, display or pins, and the OS grants, shares or refuses.
  Two apps cannot fight over the radio or the same pin.

## How it works

```
 your computer / phone                         Heltec V4
 ┌────────────────────┐   Wi-Fi (HTTP)   ┌──────────────────────────────┐
 │ repo_server.py     │ ───────────────▶ │ install <app>                │
 │ folder of .esp     │                  │   verify SHA-256, store      │
 └────────────────────┘                  │   in LittleFS                │
        ▲                                │                              │
        │ make (SDK)                     │ KoaOS firmware (Arduino C++) │
   C source ──▶ wasm32 ──▶ .esp          │   shell · processes · files  │
                                         │   resource manager (caps+pins)│
                                         │   wasm3 interpreter ──▶ apps │
                                         └──────────────────────────────┘
```

- **Firmware:** one Arduino sketch (`KoaOS/`). It provides the shell, LittleFS storage, Wi-Fi, GPS, LoRa and the
  built-in tools, plus the **ABI**: the list of functions apps can call.
- **Apps:** C programs compiled to WebAssembly and run by [wasm3](https://github.com/wasm3/wasm3), an interpreter.
  App memory lives in PSRAM, inside a sandbox, so a crashing app does not take the OS down.
- **Packages (`.esp`):** a manifest, the app files and a SHA-256, in one file.
- **Capabilities:** a manifest line such as `requires=wifi,gps` declares what an app may use. The app must also acquire
  each resource at run time. Anything else returns a permission error.
- **Repository:** `install` downloads packages from a small Python server you run on a computer or phone on the same Wi-Fi.

## Built-in commands

```
help clear uname uptime  free df ls cat rm echo   date whoami reboot gps
edit <file> (or nano)    neofetch   ps kill sleep exit   pins
wifi   aprs   aprsd   weather       run install uninstall apps
vga on|off|status|boot on|off       gui   gui boot on|off      (vga and gui are experimental)
```
The shell supports `>`, `>>` and `&`, and runs simple shell scripts. `edit` is a Nano-style editor.

## VGA output and the desktop (`gui`) - experimental

> **Experimental.** `vga` and `gui` are new, tested by one person on one monitor. They are off until you type the command, and
> nothing else in the system depends on them. Expect rough edges, and please report what you find.

**VGA text (optional).** Type `vga on` and the console is mirrored to a VGA monitor (640x480, 80x30 text); `vga off` stops it and
gives the pins back. `vga boot on` starts it at every boot. Each scanline is generated on the fly from the text buffer in internal
RAM, so no PSRAM is used (PSRAM is too slow for video). Wiring: GPIO4 R, GPIO5 G, GPIO6 B, GPIO47 HSYNC, GPIO48 VSYNC (through
about 220-330 ohm), GND to the VGA ground pins; GPIO3 is a dummy clock output, leave it unconnected. While VGA is on, those six
pins belong to the OS (`pins` shows `vga`), so apps get a "busy" error for them. Needs arduino-esp32 3.x; add `#define KOA_NO_VGA`
at the top of `koa_vga.h` to compile it (and the desktop) out on older cores.

**The desktop.** `gui` switches the VGA output to a 320x240 graphics mode (every pixel doubled to 640x480, 8 colors, one 38 KB
framebuffer in internal RAM that exists only while the desktop runs) and starts a launcher. It turns VGA on by itself if needed.
You control it with the keys you type in the USB serial terminal: **arrow keys** move, **Enter** opens, **Esc** goes back (on the
desktop: power menu), **Ctrl-C** leaves at once. The OLED shows a hint while it runs.

| Tile | What it does |
|---|---|
| Files | browse LittleFS; Enter opens a file in the editor, `e` edit, `n` new file, `d` delete, Left/Backspace goes up |
| Wi-Fi | scan, pick a network, type the password, connect, disconnect |
| GPS | speed, satellites, position, altitude, time (powers the GPS only while the window is open) |
| System | uptime, RAM, PSRAM, storage, Wi-Fi, running processes |
| Apps | lists installed apps; Enter runs one on the console and returns to the desktop afterwards |
| Terminal | a normal shell on the VGA console; `exit` returns to the desktop |
| Editor | small text editor (up to 3000 bytes per file): arrows, Ctrl-S saves, Esc closes |
| Power | reboot, or exit to the console |

`gui boot on` starts the desktop at every boot, **before the login prompt**, so anyone with the board can reach the Terminal tile
without the password. It is off by default. Windows are shown one at a time (this is a launcher with app windows, not a multi-window
desktop manager). Apps installed with `install` still run on the text console; they cannot draw in the desktop yet.

## Capabilities

- Login, processes (`ps`, `kill`), background jobs, LittleFS files that survive reboots and reflashes (same partition scheme)
- GPS with a shared on/off manager: several programs can use it, and it powers down when nobody needs it
- Wi-Fi client, HTTP/HTTPS (TLS is in the firmware, so apps do not need their own)
- Weather from Open-Meteo, LoRa APRS (`aprs`, `aprsd` daemon)
- Experimental: a graphical desktop on VGA (`gui`): files, editor, Wi-Fi, GPS, system info, app launcher
- Installable apps: `install`, `uninstall`, `apps`, with a private `/data/<app>/` folder for each
- App access to: Wi-Fi, GPS, LoRa, GPIO, UART (2 ports), I2C, SPI, files, a hardware RNG, SHA-256, HMAC-SHA256, AES-128,
  and IPC so a command like `app start` can talk to a running `app` daemon
- OS-enforced pin allow-list. GPS, radio, OLED, USB and power-switch pins can never be handed to an app, and `pins` shows
  who owns what. Everything an app holds is released when it exits or is killed.

## Limitations (please read)

- **V4 only.** Other boards (including the V3) have no PSRAM and are not supported.
- **Terminal input only.** You type over USB serial. There is no on-board keyboard yet (I2C keyboard support is planned).
- **GPIO is an app API, not a driver.** Apps can drive pins, UARTs, I2C and SPI today, but there are no OS-level drivers
  built on top of it yet (for example, no keyboard driver).
- **Not available to apps yet:** GPIO interrupts, PWM, ADC, drawing on the display, command-line arguments.
  The display capability exists but is reserved; only built-in tools draw on the OLED for now.
- **Apps are interpreted:** heavy maths is slow. Limits: `.wasm` up to 256 KB, about 1 MB of PSRAM per app.
- **Security is basic.** The default login is a convenience, not protection: anyone holding the board can reflash it.
  `install` uses plain HTTP, and the SHA-256 in a package catches corruption but does not prove who made it.
  Only install from servers you control. The firmware runs no network server, so nothing listens on Wi-Fi.
- **No SSH client, public app repository, or USB file transfer yet.** See [ROADMAP.md](ROADMAP.md).
- **MeshCore is a client.** It is fully functional but does not repeat other nodes' traffic. See [Apps included](#apps-included).

## Flash settings

Use the Arduino IDE board **Heltec WiFi LoRa 32 (V4)**, or **ESP32S3 Dev Module** (ArduinoDroid has no V4 entry) with:

| Setting | Value |
|---|---|
| JTAG Adapter | Integrated USB JTAG |
| PSRAM | **QSPI PSRAM** (required) |
| Flash Mode / Size | QIO 80MHz / 16MB (128Mb) |
| Arduino Runs On / Events Run On | Core 1 / Core 1 |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Enabled |
| USB Firmware MSC / DFU On Boot | Disabled / Disabled |
| Upload Mode | UART0 / Hardware CDC |
| Partition Scheme | **No OTA (2MB APP/2MB SPIFFS)** |
| CPU Frequency | 240MHz (WiFi) |
| Upload Speed | 512000 |
| Core Debug Level | None |
| Erase All Flash Before Sketch Upload | Disabled |
| Zigbee Mode | Disabled |

ESP32 Arduino core 3.x. Changing the partition scheme erases the filesystem on the next flash; reflashing with the same
scheme keeps your files and apps. If you would rather patch the stock Wasm3 library yourself, run
`python3 tools/patch_wasm3.py` (see `lib/Wasm3/KOAOS-PATCH.md`). Without the patch, apps refuse to start.

## Installing apps

Put `.esp` files (for example the ones in `apps/prebuilt/`) in a folder on a computer or phone and serve it:
```
python3 koaos-sdk/tools/repo_server.py apps/prebuilt 8000
```
Then on the board (same Wi-Fi):
```
install -s <host-ip>:8000     # set the repository once
install -l                    # list what is available
install meshcore
meshcore                      # type /quit to leave
```
Remove an app with `uninstall <app>`; `apps` lists what is installed.

## Apps included

| App | Needs | What it does |
|---|---|---|
| built in: `gps`, `wifi`, `weather`, `aprs`, `aprsd`, `edit`, `gui` (experimental) | n/a | Part of the firmware, ready right after flashing |
| `hellowasm` | nothing | Smallest example app |
| `gpswatch` | gps | Shows GPS fix information |
| `webget` | wifi | Fetches a URL over HTTP/HTTPS |
| `meshcore` | lora | MeshCore chat/companion client |

**MeshCore status:** the app is fully functional. It implements the MeshCore over-the-air protocol (adverts,
public/hashtag/private channels, encrypted direct messages, acks) and runs on the V4. It also passes the native tests in
`koaos-sdk/tests` against scripted traffic. It is a client only and does not repeat other nodes' traffic. Feedback is welcome.

## The SDK in short

`koaos-sdk/` lets you write apps in C and build them into `.esp` packages. You need `clang` with `wasm-ld`, `python3` and
`make` on a PC, or use GitHub Actions or Termux if you only have a phone (`koaos-sdk/BUILD-ON-PHONE.md`).

```c
#include "koaos.h"
int koa_main(void) {
  koa_puts("hi");
  return 0;
}
```
```
cd koaos-sdk
make myapp            # -> dist/myapp.esp
make serve            # serve it to the board
```

What the SDK gives an app (ABI v2, 48 functions): console I/O, files in a private folder, time and RNG, IPC, Wi-Fi and HTTP(S),
GPS, LoRa (config, CAD, TX, RX), crypto helpers, and GPIO, UART, I2C and SPI on OS-approved pins. Apps declare what they
need in `manifest` and pick pins at run time, so changing a wire never needs a rebuild. There is no libc; the SDK provides
`koa_printf` and a few string functions. Full details: `koaos-sdk/README.md`.

## What is in this repository

```
KoaOS/            firmware sketch: KoaOS.ino, esp_pkg.h (.esp parser), koaos_abi.h (the ABI), koa_io.h (pin policy),
                  koa_vga.h (VGA text + graphics driver), koa_gfx.h (drawing), koa_gui.h (the desktop), koa_font.h
lib/Wasm3/        patched wasm3 Arduino library (MIT, see lib/Wasm3/LICENSE)
koaos-sdk/        app SDK: headers, mini libc, Makefile, esppack and repo_server tools,
                  example apps (hellowasm, gpswatch, webget, meshcore, iodemo) and native tests
apps/prebuilt/    ready-to-install .esp apps
examples/         tiny shell-script examples and a script package
tools/            patch_wasm3.py
.github/          workflow that builds every app to .esp on GitHub
ROADMAP.md        what is done and what is planned
CHANGELOG.md
```

## Tests

`make selftest`, `make iotest` and `make mctest` in `koaos-sdk/` run on a normal computer: the SDK printf, the pin policy and
manifest parser, and the MeshCore crypto and app logic. `make guitest` compiles the desktop on a PC, drives it with scripted keys
and writes screenshots (PPM) to `koaos-sdk/gui_shots/`, so you can see layout changes without flashing. Apps are built to WebAssembly with clang and run on a real V4.

## Memory and stack notes

App memory lives in PSRAM (1 MB per app); the OS and each app's interpreter stack live in internal RAM. wasm3 has no tail
calls on this chip, so stack use grows with the longest straight-line run of code. The interpreter task gets 48 KB
(`WASM_TASK_STACK`), and the SDK builds apps with `-Os -fno-unroll-loops` to keep those runs short.

## Credits

- [wasm3](https://github.com/wasm3/wasm3) by Volodymyr Shymanskyy and Steven Massey (MIT)
- [MeshCore](https://github.com/meshcore-dev/MeshCore), whose protocol the `meshcore` app speaks
- U8g2, TinyGPSPlus, RadioLib, arduino-esp32

## License

MIT, see `LICENSE`. The bundled wasm3 library keeps its own MIT license in `lib/Wasm3/LICENSE`.
