/*
 * koaos_abi.h  --  KoaOS application ABI, version 2 (DRAFT)
 *
 * One header, two readers:
 *   - the SDK compiles apps against it (functions become wasm imports from
 *     module "koa")
 *   - the firmware implements every function listed here as a host function
 *
 * Rules for keeping this stable once published:
 *   - never change or remove an existing function; add new ones
 *   - never reorder or resize fields in a struct; append only
 *   - bump KOA_ABI_VERSION when something is added; apps declare the
 *     minimum version they need (abi= in the manifest)
 */
#ifndef KOAOS_ABI_H
#define KOAOS_ABI_H

#include <stdint.h>

#define KOA_ABI_VERSION 2   /* 2 = adds GPIO / UART / I2C / SPI. Apps built for ABI 1 run unchanged. */

#ifdef __wasm__
#define KFN(ret, name, args) \
  __attribute__((import_module("koa"), import_name(#name))) ret name args;
#else
#define KFN(ret, name, args) ret name args;   /* firmware side: implemented in the OS */
#endif

/* ---- error codes (negative return values) ---- */
#define KE_OK        0
#define KE_PERM     -1   /* capability not declared in manifest */
#define KE_BUSY     -2   /* resource owned by another process */
#define KE_NOENT    -3
#define KE_INVAL    -4
#define KE_NOMEM    -5
#define KE_TIMEOUT  -6
#define KE_KILLED   -7   /* process was killed: clean up and return */
#define KE_NOSYS    -8
#define KE_IO       -9

/* ---- capabilities: manifest "requires=" names map to these bits ---- */
#define KC_WIFI     (1u << 0)
#define KC_GPS      (1u << 1)
#define KC_LORA     (1u << 2)
#define KC_GPIO     (1u << 3)
#define KC_UART     (1u << 4)
#define KC_DISPLAY  (1u << 5)
#define KC_I2C      (1u << 6)
#define KC_SPI      (1u << 7)
/* manifest: "gpio" grants KC_GPIO | KC_UART (any pin can be a UART pin, so UART is part of generic GPIO);
 *           "uart" grants KC_UART alone; "i2c" and "spi" grant their own bits. */

/* ---- open flags ---- */
#define KO_RD   0
#define KO_WR   1
#define KO_APP  2

/* ============ process / time ============ */
KFN(int32_t,  koa_abi_version, (void))
KFN(void,     koa_exit,        (int32_t rc))
KFN(int32_t,  koa_yield,       (void))            /* returns KE_KILLED if kill was requested */
KFN(void,     koa_sleep,       (int32_t ms))
KFN(uint32_t, koa_millis,      (void))
KFN(uint32_t, koa_micros,      (void))
KFN(int64_t,  koa_unixtime,    (void))            /* -1 if clock not set (no GPS/Wi-Fi time yet) */
KFN(int32_t,  koa_random,      (void *buf, int32_t n))   /* hardware RNG */

/* ============ console (fd 0 = tty in, fd 1 = tty out) ============ */
KFN(int32_t,  koa_write,       (int32_t fd, const void *buf, int32_t n))
KFN(int32_t,  koa_read,        (int32_t fd, void *buf, int32_t n))
KFN(int32_t,  koa_getc,        (int32_t timeout_ms))     /* -1 on timeout */

/* ============ files: paths are relative to the app's private /data/<app>/ ============ */
KFN(int32_t,  koa_open,        (const char *path, int32_t flags))
KFN(int32_t,  koa_close,       (int32_t fd))
KFN(int32_t,  koa_unlink,      (const char *path))
KFN(int32_t,  koa_readdir,     (const char *dir, int32_t idx, char *name, int32_t nmax, int32_t *size))

/* ============ IPC: lets "meshcore id Kat" talk to a running "meshcore start" ============ */
KFN(int32_t,  koa_ipc_listen,  (const char *name))                       /* daemon claims a name */
KFN(int32_t,  koa_ipc_recv,    (void *buf, int32_t nmax, int32_t timeout_ms, int32_t *client))
KFN(int32_t,  koa_ipc_reply,   (int32_t client, const void *buf, int32_t n))
KFN(int32_t,  koa_ipc_call,    (const char *name, const void *req, int32_t n,
                                   void *resp, int32_t rmax, int32_t timeout_ms))  /* returns reply length */

/* ============ exclusive resources (checked against manifest requires=) ============ */
KFN(int32_t,  koa_acquire,     (uint32_t caps))    /* KE_PERM / KE_BUSY on failure */
KFN(int32_t,  koa_release,     (uint32_t caps))

/* ============ GPS (needs KC_GPS) ============ */
typedef struct {
  uint8_t  on, loc_valid, time_valid, _pad;
  int32_t  sats;
  int32_t  year, month, day, hour, minute;
  double   lat, lng, mph, alt, course;
} koa_gps_t;
KFN(int32_t,  koa_gps_start,   (void))
KFN(int32_t,  koa_gps_stop,    (void))
KFN(int32_t,  koa_gps_info,    (koa_gps_t *out))

/* ============ Wi-Fi / HTTP (needs KC_WIFI; TLS lives in firmware, not in the app) ============ */
KFN(int32_t,  koa_wifi_status, (char *ssid, int32_t smax, char *ip, int32_t imax, int32_t *rssi))
KFN(int32_t,  koa_http_get,    (const char *url, void *buf, int32_t nmax))   /* returns bytes, or <0 */

/* ============ LoRa (needs KC_LORA; exclusive while held) ============ */
typedef struct {
  float    freq_mhz, bw_khz;
  uint8_t  sf, cr, sync_word, _pad;
  int8_t   power_dbm, _pad2;
  uint16_t preamble;
} koa_lora_cfg_t;
KFN(int32_t,  koa_lora_config, (const koa_lora_cfg_t *cfg))
KFN(int32_t,  koa_lora_cad,    (void))                                       /* 1 = channel busy */
KFN(int32_t,  koa_lora_tx,     (const void *buf, int32_t n))
KFN(int32_t,  koa_lora_rx,     (void *buf, int32_t nmax, int32_t timeout_ms,
                                   float *rssi, float *snr))                    /* returns length */

/* ============ crypto helpers (firmware-accelerated via mbedTLS) ============
 * Ed25519 is NOT provided here; apps that need it compile it in. */
KFN(int32_t,  koa_sha256,      (const void *in, int32_t n, uint8_t out[32]))
KFN(int32_t,  koa_hmac_sha256, (const void *key, int32_t klen, const void *in, int32_t n, uint8_t out[32]))
KFN(int32_t,  koa_aes128_ecb,  (const uint8_t key[16], const void *in, void *out, int32_t n, int32_t encrypt))

/* ============ GPIO / UART / I2C / SPI (ABI 2) ============
 * The manifest says which KINDS of I/O the app may use (requires=gpio,i2c,spi). The app picks the actual
 * PINS at run time, so a different wiring needs no rebuild. The OS decides per pin:
 *   KE_INVAL  pin number out of range / bad argument
 *   KE_PERM   capability missing, not acquired, or pin is not on the OS allow-list (display, radio, GPS, USB ...)
 *   KE_BUSY   pin or peripheral already owned by another process (or by this one, for another purpose)
 * Everything an app owns is released when it exits or is killed. Released pins go back to high-impedance input.
 * Acquire first:  koa_acquire(KC_GPIO | KC_UART)   koa_acquire(KC_I2C)   koa_acquire(KC_SPI)
 */

/* ---- GPIO (KC_GPIO) ---- */
#define KG_IN          0
#define KG_IN_PULLUP   1
#define KG_IN_PULLDOWN 2
#define KG_OUT_LOW     3   /* output, level is set before the pin turns into an output (no glitch) */
#define KG_OUT_HIGH    4
KFN(int32_t,  koa_gpio_request, (int32_t pin, int32_t mode))     /* claim a pin; call again to change its mode */
KFN(int32_t,  koa_gpio_release, (int32_t pin))
KFN(int32_t,  koa_gpio_read,    (int32_t pin))                   /* 0 / 1, or <0 */
KFN(int32_t,  koa_gpio_write,   (int32_t pin, int32_t level))    /* pin must be an output */

/* ---- UART (KC_UART; two ports available; rx or tx may be -1, not both) ---- */
#define KU_8N1  0
#define KU_8E1  1
#define KU_8O1  2
#define KU_8N2  3
KFN(int32_t,  koa_uart_open,    (int32_t rx, int32_t tx, int32_t baud, int32_t format))   /* returns a handle >= 0 */
KFN(int32_t,  koa_uart_close,   (int32_t h))
KFN(int32_t,  koa_uart_write,   (int32_t h, const void *buf, int32_t n))                  /* returns n */
KFN(int32_t,  koa_uart_read,    (int32_t h, void *buf, int32_t nmax, int32_t timeout_ms)) /* bytes available, 0 on timeout */

/* ---- I2C controller (KC_I2C; exclusive; own bus, separate from the OLED; needs external pull-ups) ---- */
KFN(int32_t,  koa_i2c_open,     (int32_t sda, int32_t scl, int32_t freq_hz))
KFN(int32_t,  koa_i2c_close,    (void))
KFN(int32_t,  koa_i2c_write,    (int32_t addr, const void *buf, int32_t n, int32_t stop))  /* addr 0x08..0x77, n<=128; n=0 probes (KE_NOENT = no ACK); stop=0 keeps the bus for a read */
KFN(int32_t,  koa_i2c_read,     (int32_t addr, void *buf, int32_t n))                      /* n<=128, returns bytes */

/* ---- SPI controller (KC_SPI; exclusive; own bus, separate from the radio) ---- */
#define KSPI_MODE0    0
#define KSPI_MODE1    1
#define KSPI_MODE2    2
#define KSPI_MODE3    3
#define KSPI_KEEP_CS  1    /* leave chip-select asserted after this transfer */
KFN(int32_t,  koa_spi_open,     (int32_t sck, int32_t mosi, int32_t miso, int32_t cs, int32_t freq_hz, int32_t mode))   /* mosi/miso/cs may be -1 */
KFN(int32_t,  koa_spi_close,    (void))
KFN(int32_t,  koa_spi_transfer, (const void *tx, void *rx, int32_t n, int32_t flags))  /* n<=256; tx or rx may be NULL; returns n */

/* reserved for later: display/buttons, GPIO interrupts */

#endif /* KOAOS_ABI_H */
