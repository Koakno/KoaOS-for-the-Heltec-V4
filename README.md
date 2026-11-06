# KoaOS for Heltec V4

> **Experimental branch (0.14).** Builds on the stable 0.10 release (the code on `main`) and adds VGA output, RCA composite video, a graphical
> desktop, Bluetooth keyboard support, apps that draw on the OLED, and an `i2c` shell command. The video outputs and Bluetooth are off until
> you switch them on.

A small Unix-style operating system for the **Heltec WiFi LoRa 32 V4** (ESP32-S3). Flash it once, log in over USB,
and you have a pocket computer with a shell, a text editor, GPS, Wi-Fi, weather and LoRa APRS, plus *experimental* video output (**VGA or RCA composite**) with a **graphical desktop**, and a **Bluetooth keyboard**.
Then **install apps over Wi-Fi without ever reflashing**, or write your own with the included SDK.

*KoaOS is an independent project and is not made or endorsed by Heltec. "Heltec" is used only to say which board it runs on.*

---

## Security and threat model

KoaOS is a hobby OS for the Heltec WiFi LoRa 32 V4. It is meant for networks
you control, not hostile environments. Here is what it does and doesn't
protect against.

### What it protects against
- **Apps are sandboxed.** Installed apps run as WebAssembly in an interpreter
  and can't touch kernel memory.
- **Apps declare what they use.** Each app's manifest lists the hardware it
  needs (Wi-Fi, LoRa, GPS, GPIO, and so on). The installer prints this as
  `it uses: ...`, and the kernel enforces it: a capability that isn't declared
  returns a permission error.
- **Apps can only reach their own files.** An app's file access is limited to
  its own folder (`/data/<app>/`): no subdirectories, no dot-files, no
  escaping the folder.
- **Pins are claimed one at a time.** The display bus pins can never be given
  to an app.
- **No listening services.** The OS doesn't run a server. Anything that
  accepts incoming connections is an app you chose to install.
- **Nothing starts on its own** except the login prompt and, if you turn them
  on, the VGA, RCA and GUI outputs.

**Read the `uses:` line before you keep an app.** A calculator that wants LoRa
and GPS is not a calculator.

### What it does NOT protect against
- **Physical access.** Secure boot and flash encryption are not enabled. Anyone
  holding the device can dump and read the flash. The login prompt is a
  convenience, not a lock.
- **Untrusted networks.** HTTPS connections don't validate certificates. The
  hash inside an `.esp` package catches corrupted downloads, but it travels
  with the file, so it can't detect a deliberately swapped one. Install apps
  only over a network you trust, and run the app server from a computer you
  trust.
- **Shell-script apps.** A script app runs shell commands with the shell's own
  authority and isn't sandboxed like a wasm app. The installer tells you when
  something is a script. Only install scripts you've read or trust.
- **An app misusing what it declared.** An app that declares Wi-Fi can still
  send out any data it can reach.
- **Radio and GPS input.** Treat anything received over LoRa or from the GNSS
  module as untrusted. The built-in GPS and APRS demo run as part of the OS,
  not in the sandbox, and haven't been audited against malformed input.

### Reporting problems
Open an issue on GitHub.


---

## Getting started

You need a Heltec WiFi LoRa 32 **V4** (not V3: KoaOS needs the V4's PSRAM), a USB cable, and the Arduino IDE
(or ArduinoDroid on Android).

1. **Install libraries** from the Library Manager: `U8g2`, `TinyGPSPlus`, `RadioLib` and `NimBLE-Arduino` (version **2.5.1**, for the
   Bluetooth keyboard; if you do not want Bluetooth, put `#define KOA_NO_BLE` at the top of `KoaOS/koa_ble.h` and skip this library).
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
root@koa:~# gui               # experimental: desktop on a VGA monitor, or on RCA if `rca on` is running (see below)
```

That is already a working system: the shell, editor, GPS, weather, APRS tools, Wi-Fi, the experimental desktop and the video and Bluetooth
commands are built into the firmware, so there is something to play with right after flashing. When you want more, see [Installing apps](#installing-apps).

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
edit <file> (or nano, vi)   neofetch (or fetch)   ps kill sleep exit (or logout)   pins
i2c scan | get <addr> [reg] [count] | set <addr> <byte>...          (hex; the OLED's I2C bus, GPIO17/18)
wifi   aprs   aprsd   weather       run install uninstall apps
vga on|off|status|boot on|off                                       (VGA output, experimental)
rca on|off|status|boot on|off|test|pos <n>|drive <0-3>|black <2-6>|color on|off|sat <n>   (RCA composite output, experimental)
gui   gui boot on|off                                               (desktop on VGA or RCA, experimental)
ble on|off|scan [all]|pair <n>|status|forget|boot on|off           (Bluetooth keyboard, experimental)
```
The shell supports `>`, `>>` and `&`, and runs simple shell scripts. `edit` is a Nano-style editor. `i2c` is for checking wiring without
writing an app: it talks to whatever you connect to the OLED's SDA/SCL pins (see [The SDK in short](#the-sdk-in-short)).

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
framebuffer in internal RAM that exists only while the desktop runs) and starts a launcher. If `rca on` is already running it draws on the
composite output instead (see below); otherwise it turns VGA on by itself.
You control it with the keys you type in the USB serial terminal (or on a paired [Bluetooth keyboard](#bluetooth-keyboard-ble---experimental)): **arrow keys** move, **Enter** opens, **Esc** goes back (on the
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
desktop manager). Apps installed with `install` still run on the text console; they cannot draw in the desktop yet (they can draw on the OLED, see
[The SDK in short](#the-sdk-in-short)).

## RCA composite video (`rca`) - experimental

`rca on` sends the console to a composite (RCA) display on **GPIO6**; `rca off` releases the pins, `rca boot on|off` starts it at boot.
Type `gui` while it is on and the desktop is drawn on the composite output instead of VGA. It uses the same trick as VGA: no
framebuffer for the output, every scanline is built on the fly, and one pin sends a pulse-density bit stream that an RC filter at the
jack turns into sync, black and white. Text is 40x24 and the desktop is the same 320x240 picture as on VGA.

| Mode | Bit clock | Lines | What you get |
|---|---|---|---|
| Monochrome (default) | 25 MHz | 15.625 kHz, 60.3 Hz, 259 lines per frame | NTSC-style black and white; the desktop's 8 colors become three gray levels. Gives a usable picture on real hardware, but it looks "off"; color mode looks better |
| Color (`rca color on`, **experimental**) | 28.636 MHz (8x the NTSC color subcarrier) | 15.734 kHz, about 60.75 Hz | NTSC color: the 8 desktop colors come out as colors |

```
GPIO6 --[ 180 ohm ]--+-- RCA center pin        (or: GPIO6 -[100 ohm]- pot end 1, wiper -> center pin, end 2 -> GND)
                     |
              470 pF .. 1 nF  (add it if black looks hatched; monochrome only, see below)
                     |
GND -----------------+-- RCA shield            (GPIO3 is a dummy clock pin: leave it unconnected)
```

GPIO6 is also VGA's blue pin, so use one output at a time (the second one says "pins busy"), and unplug the VGA cable while using RCA:
a monitor's 75 ohm load would halve the signal.

| Command | What it does |
|---|---|
| `rca on` / `rca off` | start or stop the output (`rca` alone means `rca on`); `rca off` refuses while the desktop is running |
| `rca status` | on/off, start at boot, picture position, drive, black level, color mode, saturation |
| `rca boot on\|off` | start RCA at every boot (saved) |
| `rca test` | toggle a test picture: gray bars, one-pixel columns and a white frame |
| `rca pos <n>` | picture position, 3..16, smaller = higher (default 8, saved). Use it if the picture sits too high or low or the bottom is cut off |
| `rca drive <0-3>` | output strength of GPIO6, 3 = strongest (default, not saved) |
| `rca black <2-6>` | monochrome black level, 5 = 50% (default), 4 = darker, more contrast (not saved) |
| `rca color on\|off` | experimental NTSC color (saved). If RCA is running outside the desktop it restarts by itself; with the desktop open, leave it, then `rca off`, `rca on` |
| `rca sat <0-150>` | color saturation in percent, default 60 (not saved) |

**Color mode needs a much lighter filter.** The color signal has ripple at 3.58 MHz and 7.16 MHz, so the RC filter at the jack must pass
at least about 6 MHz: try 100-220 pF or no capacitor at all. The 470 pF to 1 nF capacitor above is for monochrome.

Status: `rca` is experimental. On a real V4 the monochrome mode gives a functional picture that looks wrong, so **we recommend `rca color on`** (with the lighter filter above). The first test on a real V4 found that the pin
delivered too little amplitude at the monitor, which is why `rca drive` defaults to the strongest setting. The scanline generators are also checked on a PC with `make rcatest`.

## Bluetooth keyboard (`ble`) - experimental

> **Experimental.** Tested on one keyboard. Off until you type `ble on`.

Type on the OLED, a VGA monitor or an RCA display without a computer or phone: the V4 acts as a Bluetooth LE host for a **BLE HID keyboard** and feeds
its keys into the console exactly like the USB serial terminal, so the shell, `edit`, `gui` and apps need no changes.

```
ble on                  start the Bluetooth stack
ble off                 stop it
ble scan                scan for 6 seconds and list keyboards in range, up to 8 (put the keyboard in pairing mode first; "ble scan all" lists everything)
ble pair 0              pair with entry 0; if the keyboard asks for a passkey it is shown on screen: type it on the keyboard, Enter
ble status              connected?, keyboard battery, start at boot?
ble forget              drop the bond and the remembered keyboard
ble boot on|off         start BLE at every boot (the paired keyboard then reconnects by itself)
```

How to tell if a keyboard will work: it must be BLE (not classic Bluetooth) and expose the HID service (UUID 0x1812), which the free
nRF Connect phone app shows. Dongle (2.4 GHz) keyboards are not Bluetooth and do not work. A BLE keyboard talks to one device per
pairing slot, so disconnect it from your phone or use another slot. A sleeping keyboard does not advertise: press a key and it
reconnects within a few seconds. Layout is US. Needs the **NimBLE-Arduino 2.5.1** library; `#define KOA_NO_BLE` at the top of
`koa_ble.h` leaves it all out. While BLE is on it uses roughly 40-60 KB of internal RAM, and BLE and Wi-Fi share one radio: while Wi-Fi is actively
being used (a large download, or the moment `weather` runs) the keyboard drops out, and it reconnects shortly after Wi-Fi goes quiet.

## Capabilities

- Login, processes (`ps`, `kill`), background jobs, LittleFS files that survive reboots and reflashes (same partition scheme)
- GPS with a shared on/off manager: several programs can use it, and it powers down when nobody needs it
- Wi-Fi client, HTTP/HTTPS (TLS is in the firmware, so apps do not need their own)
- Weather from Open-Meteo, LoRa APRS (`aprs`, `aprsd` daemon)
- Experimental: video output on VGA or RCA composite (`vga`, `rca`), with a graphical desktop (`gui`): files, editor, Wi-Fi, GPS, system info, app launcher
- Experimental: a Bluetooth LE keyboard for the console (`ble`), working alongside USB serial
- `i2c scan|get|set` from the shell, on the OLED's own I2C bus
- Installable apps: `install`, `uninstall`, `apps`, with a private `/data/<app>/` folder for each
- App access to: Wi-Fi, GPS, LoRa, GPIO, UART (2 ports), I2C (also on the OLED's own bus), SPI, drawing on the OLED, files, a hardware RNG,
  SHA-256, HMAC-SHA256, AES-128, and IPC so a command like `app start` can talk to a running `app` daemon
- OS-enforced pin allow-list. GPS, radio, OLED, USB and power-switch pins can never be handed to an app, and `pins` shows
  who owns what. Everything an app holds is released when it exits or is killed.

## Limitations (please read)

- **V4 only.** Other boards (including the V3) have no PSRAM and are not supported.
- **Keyboard input only.** You type over the USB serial terminal or on a paired BLE keyboard (`ble`). There is no mouse and no on-board
  keyboard yet (I2C keyboard support is planned).
- **GPIO is an app API, not a driver.** Apps can drive pins, UARTs, I2C and SPI today, but there are no OS-level drivers
  built on top of it yet (for example, no I2C keyboard driver).
- **Not available to apps yet:** GPIO interrupts, PWM, ADC, command-line arguments, drawing on the VGA/RCA desktop
  (apps can draw on the OLED: `koa_oled_*`, capability `display`).
- **Video and Bluetooth are experimental.** VGA and the desktop were tested on one monitor and BLE on one keyboard; RCA, and RCA color most
  of all, are the newest. VGA and RCA share GPIO6, so only one runs at a time. BLE and Wi-Fi share one radio, so the keyboard drops out while Wi-Fi is actively in use and comes back shortly afterwards.
- **Apps are interpreted:** heavy maths is slow. Limits: `.wasm` up to 256 KB, about 1 MB of PSRAM per app.
- **Security is basic.** The default login is a convenience, not protection: anyone holding the board can reflash it.
  `install` uses plain HTTP, and the SHA-256 in a package catches corruption but does not prove who made it.
  Only install from servers you control. The firmware runs no network server, so nothing listens on Wi-Fi.
- **No SSH client, public app repository, or USB file transfer yet.** See [ROADMAP.md](ROADMAP.md).
- **MeshCore is a client, and direct messages are not working yet.** Adverts and channel messages (the public channel and other channels) work over the air on a real V4. Sending or receiving a **direct message crashes the system** (known bug, being fixed), so use channels only for now. It does not repeat other nodes' traffic. See [Apps included](#apps-included).

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
scheme keeps your files and apps, including when you switch between the stable and experimental builds. If you would rather patch the stock Wasm3 library yourself, run
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
| `iodemo` | gpio, i2c | Blinks one pin and scans an I2C bus (pins chosen in the source) |
| `i2cscan` | i2c, display | Lists every device on the OLED's I2C bus (GPIO17/18) and shows the list on the OLED |
| `oleddemo` | display | Tries every `koa_oled_*` call: three font sizes, a frame, a line, a bitmap and a bouncing ball |
| `pca9555` | i2c, display | Reads and drives a PCA9555 16-bit I2C GPIO expander on the OLED's bus, and shows both ports on the OLED. Finds the chip by itself at 0x20..0x27; tested on hardware with the chip at 0x20 |

`hellowasm`, `gpswatch`, `webget` and `meshcore` are in `apps/prebuilt/`. `iodemo`, `i2cscan`, `oleddemo` and `pca9555` are SDK example apps that need ABI 2 or 3:
build them with `make <name>` in `koaos-sdk/` (`make iodemo`, ...). `repo/` holds built copies of some `.esp` files.

**MeshCore status:** channels work over the air on a real V4 (adverts and the public and other channels); **direct messages currently crash the system** (known bug, being fixed), so use channels only for now. It implements the MeshCore over-the-air protocol (adverts,
public/hashtag/private channels, encrypted direct messages, acks) in code; direct messages are the part that does not work on hardware yet. It also passes the native tests in
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

What the SDK gives an app (ABI v3, 56 functions): console I/O, files in a private folder, time and RNG, IPC, Wi-Fi and HTTP(S),
GPS, LoRa (config, CAD, TX, RX), crypto helpers, GPIO, UART, I2C and SPI on OS-approved pins, I2C on the OLED's own bus
(GPIO17/18, shared with the display) and drawing on the OLED. Apps declare what they
need in `manifest` and pick pins at run time, so changing a wire never needs a rebuild. There is no libc; the SDK provides
`koa_printf` and a few string functions. Full details: `koaos-sdk/README.md`.

## What is in this repository

```
KoaOS/            firmware sketch: KoaOS.ino, esp_pkg.h (.esp parser), koaos_abi.h (the ABI), koa_io.h (pin policy),
                  koa_vga.h (VGA text + graphics driver), koa_rca.h (RCA driver), koa_rcagen.h (RCA scanline generator),
                  koa_gfx.h (drawing), koa_gui.h (the desktop), koa_font.h, koa_ble.h (Bluetooth LE glue), koa_hid.h (keyboard reports)
lib/Wasm3/        patched wasm3 Arduino library (MIT, see lib/Wasm3/LICENSE)
koaos-sdk/        app SDK: headers, mini libc, Makefile, esppack and repo_server tools, native tests, and example apps
                  (hellowasm, gpswatch, webget, meshcore, iodemo, i2cscan, oleddemo, pca9555)
apps/prebuilt/    ready-to-install .esp apps (hellowasm, gpswatch, webget, meshcore)
repo/             built .esp files (current compiler flags, includes iodemo); same format as apps/prebuilt/
repo_server.py    copy of koaos-sdk/tools/repo_server.py
examples/         tiny shell-script examples and a script package
tools/            patch_wasm3.py
.github/          workflow that builds every app to .esp on GitHub
ROADMAP.md        what is done and what is planned
CHANGELOG.md
```

## Tests

These run on a normal computer, from `koaos-sdk/`:

| Command | What it checks |
|---|---|
| `make selftest` | the SDK's printf against the system libc |
| `make iotest` | the pin policy and the manifest parser (also with every pin allowed, to prove the OS pins stay refused) |
| `make apptest` | the `i2cscan`, `oleddemo` and `pca9555` apps against a fake OS, a fake PCA9555 and a fake screen |
| `make hidtest` | the keyboard report translator used by the Bluetooth keyboard |
| `make rcatest` | the RCA composite scanline generator |
| `make mctest` | the MeshCore crypto and app logic against scripted radio traffic |
| `make guitest` | compiles the desktop on a PC, drives it with scripted keys and writes screenshots (PPM) to `koaos-sdk/gui_shots/`, so you can see layout changes without flashing |

Apps are built to WebAssembly with clang and run on a real V4.

## Memory and stack notes

App memory lives in PSRAM (1 MB per app); the OS and each app's interpreter stack live in internal RAM. wasm3 has no tail
calls on this chip, so stack use grows with the longest straight-line run of code. The interpreter task gets 48 KB
(`WASM_TASK_STACK`), and the SDK builds apps with `-Os -fno-unroll-loops` to keep those runs short.

## Credits

- [wasm3](https://github.com/wasm3/wasm3) by Volodymyr Shymanskyy and Steven Massey (MIT)
- [MeshCore](https://github.com/meshcore-dev/MeshCore), whose protocol the `meshcore` app speaks
- U8g2, TinyGPSPlus, RadioLib, NimBLE-Arduino, arduino-esp32

## License

MIT, see `LICENSE`. The bundled wasm3 library keeps its own MIT license in `lib/Wasm3/LICENSE`.
