/*
 * koa_io.h -- pin policy and pin ownership for apps (GPIO / UART / I2C / SPI).
 *
 * Portable C, no Arduino calls: the firmware uses it, and tests/io_test.c checks it on a PC.
 * The firmware does the hardware part (pinMode, Wire1, ...); this file only answers
 * "may this process have this pin?" and remembers who has it.
 */
#ifndef KOA_IO_H
#define KOA_IO_H

#include <stdint.h>
#include <string.h>
#include "koaos_abi.h"

#define KIO_NPINS 49          /* ESP32-S3: GPIO0..GPIO48 */

/*
 * The ONLY pins an app may ever ask for (bit n = GPIOn). Everything else is KE_PERM, even if free.
 * This is the one place to edit when you wire something up. Override with -DKOA_IO_ALLOW_MASK=... if you like.
 *
 * Allowed:  3 4 5 6   40 41 42   47 48
 *
 * Deliberately NOT allowed (taken by the OS or risky on the Heltec V4):
 *   0            boot button / strapping            1        battery sense
 *   2 7 46       LoRa front-end enable/power/PA     8-14     LoRa radio (SPI, reset, busy, DIO1)
 *   17 18 21     OLED (I2C + reset)                 19 20    native USB
 *   34 38 39     GNSS power / UART                  36       Vext power switch
 *   35 37        LED / battery-sense control        43 44    UART0 (USB serial monitor)
 *   45           strapping (flash voltage)          26,33 and the 27-32 block:
 *                tied to the PSRAM/flash on some V4 variants. NOT allowed until you have tested them on your
 *                board (for example, add (1ULL << 26) only after checking with `free` that PSRAM still
 *                works while the pin is in use).
 */
#ifndef KOA_IO_ALLOW_MASK
#define KOA_IO_ALLOW_MASK ((1ULL << 3)  | (1ULL << 4)  | (1ULL << 5)  | (1ULL << 6)  | \
                           (1ULL << 40) | (1ULL << 41) | (1ULL << 42) | \
                           (1ULL << 47) | (1ULL << 48))
#endif

/*
 * Pins that belong to the OS, always. kio_pin_allowed() refuses these even if someone adds them to
 * KOA_IO_ALLOW_MASK by mistake. Apps reach the GPS and the radio only through the gps / lora capabilities.
 *   GPS:   34 (power) 38 (TX) 39 (RX)          LoRa: 2 7 46 (front end) 8 9 10 11 12 13 14 (SX1262)
 *   OLED:  17 18 21                            Vext: 36          USB: 19 20          UART0 default pads: 43 44
 */
#define KOA_IO_RESERVED_MASK ((1ULL << 2)  | (1ULL << 7)  | (1ULL << 8)  | (1ULL << 9)  | (1ULL << 10) | \
                              (1ULL << 11) | (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | (1ULL << 46) | \
                              (1ULL << 17) | (1ULL << 18) | (1ULL << 21) | (1ULL << 36) | \
                              (1ULL << 34) | (1ULL << 38) | (1ULL << 39) | \
                              (1ULL << 19) | (1ULL << 20) | (1ULL << 43) | (1ULL << 44))

enum { KP_NONE = 0, KP_GPIO, KP_UART, KP_I2C, KP_SPI };   /* what an owned pin is being used for */

typedef struct {
  int32_t pid[KIO_NPINS];        /* 0 = free */
  uint8_t kind[KIO_NPINS];       /* KP_* */
  uint8_t mode[KIO_NPINS];       /* KG_* for GPIO pins */
} koa_pins_t;

static inline void kio_init(koa_pins_t *t) { memset(t, 0, sizeof(*t)); }

static inline int kio_pin_allowed(int pin) {
  return pin >= 0 && pin < KIO_NPINS && ((KOA_IO_ALLOW_MASK >> pin) & 1ULL) && !((KOA_IO_RESERVED_MASK >> pin) & 1ULL);
}

/* 1 if this process owns the pin for this purpose */
static inline int kio_owned(const koa_pins_t *t, int pin, int32_t pid, int kind) {
  return pin >= 0 && pin < KIO_NPINS && t->pid[pin] == pid && t->kind[pin] == kind;
}

/* claim one pin. Claiming a pin you already own for the same purpose just updates its mode. */
static inline int kio_claim(koa_pins_t *t, int pin, int32_t pid, int kind, uint8_t mode) {
  if (pin < 0 || pin >= KIO_NPINS) return KE_INVAL;
  if (!kio_pin_allowed(pin)) return KE_PERM;
  if (t->pid[pin]) {
    if (t->pid[pin] == pid && t->kind[pin] == kind) { t->mode[pin] = mode; return KE_OK; }
    return KE_BUSY;
  }
  t->pid[pin] = pid; t->kind[pin] = (uint8_t)kind; t->mode[pin] = mode;
  return KE_OK;
}

/* claim several pins for one bus, all or nothing. Entries < 0 mean "not used". Same pin twice = KE_INVAL. */
static inline int kio_claim_set(koa_pins_t *t, const int *pins, int n, int32_t pid, int kind) {
  int used = 0;
  for (int i = 0; i < n; i++) {
    int p = pins[i];
    if (p < 0) continue;
    if (p >= KIO_NPINS) return KE_INVAL;
    for (int j = 0; j < i; j++) if (pins[j] == p) return KE_INVAL;
    if (!kio_pin_allowed(p)) return KE_PERM;
    if (t->pid[p]) return KE_BUSY;
    used++;
  }
  if (!used) return KE_INVAL;
  for (int i = 0; i < n; i++) if (pins[i] >= 0) kio_claim(t, pins[i], pid, kind, 0);
  return KE_OK;
}

static inline int kio_release(koa_pins_t *t, int pin, int32_t pid, int kind) {
  if (!kio_owned(t, pin, pid, kind)) return KE_INVAL;
  t->pid[pin] = 0; t->kind[pin] = KP_NONE; t->mode[pin] = 0;
  return KE_OK;
}

/* next pin >= from that this process owns for this purpose, or -1 (walk it to reset hardware, then release) */
static inline int kio_next(const koa_pins_t *t, int32_t pid, int kind, int from) {
  for (int p = from < 0 ? 0 : from; p < KIO_NPINS; p++)
    if (t->pid[p] == pid && t->kind[p] == kind) return p;
  return -1;
}

#endif /* KOA_IO_H */
