/*
 * esp_pkg.h  --  KoaOS .esp package format, version 1 (header-only, portable C)
 *
 * No Arduino/ESP32 dependencies, so the firmware, the SDK's host tools and the
 * unit tests all use the same parser.
 *
 * Layout (all integers little-endian):
 *
 *   "ESP1"                     4 bytes  magic
 *   manifest_len               u16
 *   file_count                 u16
 *   manifest                   manifest_len bytes, text, "key=value" per line
 *   file table                 file_count x { name_len u8, name bytes, size u32 }
 *   file data                  concatenated in table order
 *   sha256                     32 bytes, over everything before it
 *
 * The hash catches corrupt or truncated downloads. It does NOT prove who made
 * the package: that needs signatures, which a later format version will add.
 *
 * Manifest keys: name, version, abi, type (script|wasm), entry, requires
 *   requires = comma list of: wifi, gps, lora, display, gpio, uart, i2c, spi
 *              (gpio also grants uart: any pin can carry a UART. Which pins is decided at run time.)
 */
#ifndef ESP_PKG_H
#define ESP_PKG_H

/* Forward declaration for KoaOS.ino: the IDE auto-generates prototypes for functions taking
   WasmCtx* before the struct is defined; this makes those prototypes valid. */
#ifdef __cplusplus
struct WasmCtx;
#endif

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "koaos_abi.h"

#define ESP_MAX_MANIFEST 512
#define ESP_MAX_FILES    16
#define ESP_FNAME_MAX    31
#define ESP_NAME_MAX     24

typedef struct {
  void *ctx;
  int (*read)(void *ctx, uint8_t *buf, int n);   /* bytes read, 0 = EOF, <0 = error */
  int (*seek)(void *ctx, uint32_t pos);          /* 0 = ok */
  uint32_t size;                                  /* total package size in bytes */
} esp_io_t;

typedef struct {
  char name[ESP_NAME_MAX + 1];
  char version[16];
  char type[8];
  char entry[ESP_FNAME_MAX + 1];
  char reqs[64];
  int abi;
  uint32_t caps;                                  /* KC_* bits from reqs */
} esp_manifest_t;

typedef struct { char name[ESP_FNAME_MAX + 1]; uint32_t size; } esp_file_t;

typedef struct {
  esp_manifest_t m;
  char manifest[ESP_MAX_MANIFEST + 1];            /* raw text, stored next to the app */
  int manifest_len;
  int nfiles;
  esp_file_t files[ESP_MAX_FILES];
  uint32_t data_off;                              /* offset of the first file's data */
} esp_pkg_t;

static inline void esp_err(char *err, int n, const char *msg) {
  if (err && n > 0) { strncpy(err, msg, n - 1); err[n - 1] = 0; }
}

/* ---------------- SHA-256 (self-contained, no mbedTLS version dependence) ---------------- */
typedef struct { uint32_t h[8]; uint8_t buf[64]; uint32_t len; uint64_t total; } esp_sha_t;

static const uint32_t esp_k[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ESP_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static inline void esp_sha_block(esp_sha_t *s, const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, h;
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ESP_ROR(w[i-15], 7) ^ ESP_ROR(w[i-15], 18) ^ (w[i-15] >> 3);
    uint32_t s1 = ESP_ROR(w[i-2], 17) ^ ESP_ROR(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ESP_ROR(e, 6) ^ ESP_ROR(e, 11) ^ ESP_ROR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + esp_k[i] + w[i];
    uint32_t S0 = ESP_ROR(a, 2) ^ ESP_ROR(a, 13) ^ ESP_ROR(a, 22);
    uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
  s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static inline void esp_sha_init(esp_sha_t *s) {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  memcpy(s->h, iv, sizeof(iv));
  s->len = 0; s->total = 0;
}

static inline void esp_sha_update(esp_sha_t *s, const uint8_t *p, uint32_t n) {
  s->total += n;
  while (n) {
    uint32_t take = 64 - s->len;
    if (take > n) take = n;
    memcpy(s->buf + s->len, p, take);
    s->len += take; p += take; n -= take;
    if (s->len == 64) { esp_sha_block(s, s->buf); s->len = 0; }
  }
}

static inline void esp_sha_final(esp_sha_t *s, uint8_t out[32]) {
  uint64_t bits = s->total * 8;
  uint8_t one = 0x80, zero = 0;
  esp_sha_update(s, &one, 1);
  while (s->len != 56) esp_sha_update(s, &zero, 1);
  uint8_t lb[8];
  for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
  esp_sha_update(s, lb, 8);
  for (int i = 0; i < 8; i++) {
    out[i*4] = (uint8_t)(s->h[i] >> 24); out[i*4+1] = (uint8_t)(s->h[i] >> 16);
    out[i*4+2] = (uint8_t)(s->h[i] >> 8); out[i*4+3] = (uint8_t)s->h[i];
  }
}

/* ---------------- names ---------------- */
static inline int esp_name_chars_ok(const char *n) {
  for (; *n; n++) {
    char c = *n;
    int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return 0;
  }
  return 1;
}
static inline int esp_valid_app_name(const char *n) {
  return n && *n && n[0] != '.' && strlen(n) <= ESP_NAME_MAX && esp_name_chars_ok(n);
}
static inline int esp_valid_file_name(const char *n) {
  return n && *n && n[0] != '.' && strlen(n) <= ESP_FNAME_MAX && esp_name_chars_ok(n) && strcmp(n, "manifest") != 0;
}

/* ---------------- manifest ---------------- */
static inline int esp_caps_parse(const char *reqs, uint32_t *caps, char *err, int errn) {
  *caps = 0;
  const char *p = reqs;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    char tok[16]; int n = 0;
    while (*p && *p != ',' && *p != ' ') { if (n < 15) tok[n++] = *p; p++; }
    tok[n] = 0;
    if (!strcmp(tok, "wifi")) *caps |= KC_WIFI;
    else if (!strcmp(tok, "gps")) *caps |= KC_GPS;
    else if (!strcmp(tok, "lora")) *caps |= KC_LORA;
    else if (!strcmp(tok, "display")) *caps |= KC_DISPLAY;
    else if (!strcmp(tok, "gpio")) *caps |= KC_GPIO | KC_UART;   /* UART is just GPIO with a peripheral behind it */
    else if (!strcmp(tok, "uart")) *caps |= KC_UART;
    else if (!strcmp(tok, "i2c")) *caps |= KC_I2C;
    else if (!strcmp(tok, "spi")) *caps |= KC_SPI;
    else {
      char m[48];
      snprintf(m, sizeof(m), "unsupported capability '%.14s'", tok);
      esp_err(err, errn, m);
      return -1;
    }
  }
  return 0;
}

static inline void esp_set(char *dst, int dn, const char *v) { strncpy(dst, v, dn - 1); dst[dn - 1] = 0; }

/* text must be NUL-terminated. 0 = ok. */
static inline int esp_manifest_parse(const char *text, esp_manifest_t *m, char *err, int errn) {
  memset(m, 0, sizeof(*m));
  int have_abi = 0;
  const char *p = text;
  while (*p) {
    const char *e = strchr(p, '\n');
    int len = e ? (int)(e - p) : (int)strlen(p);
    char line[160];
    int ll = len < (int)sizeof(line) - 1 ? len : (int)sizeof(line) - 1;
    memcpy(line, p, ll); line[ll] = 0;
    p += len; if (*p == '\n') p++;
    while (ll > 0 && (line[ll-1] == '\r' || line[ll-1] == ' ')) line[--ll] = 0;
    if (!ll || line[0] == '#') continue;
    char *eq = strchr(line, '=');
    if (!eq) { esp_err(err, errn, "bad manifest line"); return -1; }
    *eq = 0;
    const char *k = line, *v = eq + 1;
    while (*v == ' ') v++;
    if (!strcmp(k, "name")) { if (strlen(v) > ESP_NAME_MAX) { esp_err(err, errn, "name too long"); return -1; } esp_set(m->name, sizeof(m->name), v); }
    else if (!strcmp(k, "version")) esp_set(m->version, sizeof(m->version), v);
    else if (!strcmp(k, "type")) esp_set(m->type, sizeof(m->type), v);
    else if (!strcmp(k, "entry")) esp_set(m->entry, sizeof(m->entry), v);
    else if (!strcmp(k, "requires")) esp_set(m->reqs, sizeof(m->reqs), v);
    else if (!strcmp(k, "abi")) { m->abi = atoi(v); have_abi = 1; }
    /* unknown keys are ignored so newer packagers stay compatible */
  }
  if (!m->name[0]) { esp_err(err, errn, "manifest has no name"); return -1; }
  if (!esp_valid_app_name(m->name)) { esp_err(err, errn, "bad app name"); return -1; }
  if (!have_abi || m->abi < 1) { esp_err(err, errn, "manifest has no abi"); return -1; }
  if (m->abi > KOA_ABI_VERSION) {
    char s[48];
    snprintf(s, sizeof(s), "needs ABI %d, this OS has %d", m->abi, KOA_ABI_VERSION);
    esp_err(err, errn, s);
    return -1;
  }
  if (strcmp(m->type, "script") && strcmp(m->type, "wasm")) { esp_err(err, errn, "type must be script or wasm"); return -1; }
  if (!esp_valid_file_name(m->entry)) { esp_err(err, errn, "bad or missing entry"); return -1; }
  if (!m->version[0]) esp_set(m->version, sizeof(m->version), "0");
  return esp_caps_parse(m->reqs, &m->caps, err, errn);
}

/* ---------------- package ---------------- */
static inline int esp_read_full(esp_io_t *io, uint8_t *b, int n) {
  int got = 0;
  while (got < n) {
    int r = io->read(io->ctx, b + got, n - got);
    if (r <= 0) return -1;
    got += r;
  }
  return 0;
}
static inline uint32_t esp_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t esp_u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/*
 * Verifies the hash, parses and validates header, manifest and file table.
 * 0 = ok, and io is left positioned at the first byte of file data
 * (so the caller can copy each file out in table order). On error, err says why.
 */
static inline int esp_pkg_parse(esp_io_t *io, esp_pkg_t *pk, char *err, int errn) {
  uint8_t b[256];
  if (io->size < 8 + 32) { esp_err(err, errn, "not an .esp package"); return -1; }

  /* 1. integrity: hash everything but the last 32 bytes */
  esp_sha_t sh; esp_sha_init(&sh);
  if (io->seek(io->ctx, 0) != 0) { esp_err(err, errn, "can't read package"); return -1; }
  uint32_t left = io->size - 32;
  while (left) {
    int want = left > sizeof(b) ? (int)sizeof(b) : (int)left;
    if (esp_read_full(io, b, want) != 0) { esp_err(err, errn, "package truncated"); return -1; }
    esp_sha_update(&sh, b, want);
    left -= want;
  }
  uint8_t want_hash[32], got_hash[32];
  if (esp_read_full(io, want_hash, 32) != 0) { esp_err(err, errn, "package truncated"); return -1; }
  esp_sha_final(&sh, got_hash);
  if (memcmp(want_hash, got_hash, 32) != 0) { esp_err(err, errn, "corrupt package (hash mismatch)"); return -1; }

  /* 2. header */
  if (io->seek(io->ctx, 0) != 0) { esp_err(err, errn, "can't read package"); return -1; }
  uint8_t h[8];
  if (esp_read_full(io, h, 8) != 0 || memcmp(h, "ESP1", 4) != 0) { esp_err(err, errn, "not an .esp package"); return -1; }
  int mlen = (int)esp_u16(h + 4), nfiles = (int)esp_u16(h + 6);
  if (mlen <= 0 || mlen > ESP_MAX_MANIFEST) { esp_err(err, errn, "bad manifest size"); return -1; }
  if (nfiles < 1 || nfiles > ESP_MAX_FILES) { esp_err(err, errn, "bad file count (1..16)"); return -1; }

  /* 3. manifest */
  memset(pk, 0, sizeof(*pk));
  if (esp_read_full(io, (uint8_t *)pk->manifest, mlen) != 0) { esp_err(err, errn, "package truncated"); return -1; }
  pk->manifest[mlen] = 0;
  pk->manifest_len = mlen;
  if (memchr(pk->manifest, 0, mlen)) { esp_err(err, errn, "bad manifest"); return -1; }
  if (esp_manifest_parse(pk->manifest, &pk->m, err, errn) != 0) return -1;

  /* 4. file table */
  uint32_t pos = 8 + (uint32_t)mlen, total = 0;
  pk->nfiles = nfiles;
  for (int i = 0; i < nfiles; i++) {
    uint8_t nl;
    if (esp_read_full(io, &nl, 1) != 0) { esp_err(err, errn, "package truncated"); return -1; }
    if (nl < 1 || nl > ESP_FNAME_MAX) { esp_err(err, errn, "bad file name length"); return -1; }
    uint8_t rec[ESP_FNAME_MAX + 4];
    if (esp_read_full(io, rec, nl + 4) != 0) { esp_err(err, errn, "package truncated"); return -1; }
    memcpy(pk->files[i].name, rec, nl);
    pk->files[i].name[nl] = 0;
    pk->files[i].size = esp_u32(rec + nl);
    if (!esp_valid_file_name(pk->files[i].name)) { esp_err(err, errn, "bad file name in package"); return -1; }
    for (int j = 0; j < i; j++)
      if (!strcmp(pk->files[j].name, pk->files[i].name)) { esp_err(err, errn, "duplicate file name"); return -1; }
    if (total + pk->files[i].size < total) { esp_err(err, errn, "bad file size"); return -1; }
    total += pk->files[i].size;
    pos += 1 + nl + 4;
  }
  pk->data_off = pos;
  if ((uint64_t)pos + total != (uint64_t)io->size - 32) { esp_err(err, errn, "size mismatch"); return -1; }

  int has_entry = 0;
  for (int i = 0; i < nfiles; i++) if (!strcmp(pk->files[i].name, pk->m.entry)) has_entry = 1;
  if (!has_entry) { esp_err(err, errn, "entry file not in package"); return -1; }
  return 0;
}

#endif /* ESP_PKG_H */
