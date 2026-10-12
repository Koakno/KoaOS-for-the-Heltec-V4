/* meshcore -- a MeshCore client for KoaOS (Heltec V4).
 *
 * Talks the MeshCore over-the-air protocol directly through the KoaOS LoRa calls:
 *   - receives and lists adverts (nodes/repeaters), sends its own signed advert
 *   - public channel, #hashtag channels and private (PSK) channels: read + send
 *   - direct messages (Ed25519 identity, X25519 ECDH, AES-128-ECB + 2-byte HMAC), with delivery acks
 * It is a client/companion: it does not repeat other people's packets.
 *
 * Type text to send it to the current channel; commands start with '/'  (try /help).
 * manifest: requires=lora
 */
#include "koaos.h"
#include "mc_crypto.h"

/* ---------------------------- MeshCore constants ---------------------------- */
#define ROUTE_TRANSPORT_FLOOD  0
#define ROUTE_FLOOD            1
#define ROUTE_DIRECT           2
#define ROUTE_TRANSPORT_DIRECT 3

#define PT_REQ      0x00
#define PT_RESPONSE 0x01
#define PT_TXT_MSG  0x02
#define PT_ACK      0x03
#define PT_ADVERT   0x04
#define PT_GRP_TXT  0x05
#define PT_GRP_DATA 0x06
#define PT_ANON_REQ 0x07
#define PT_PATH     0x08
#define PT_TRACE    0x09

#define MAX_PAYLOAD 184
#define MAX_PATH    64
#define MAX_PACKET  255

#define ADV_LATLON 0x10
#define ADV_FEAT1  0x20
#define ADV_FEAT2  0x40
#define ADV_NAME   0x80

/* "Public" channel key (the well-known default PSK; its channel hash is 0x11) */
static const uint8_t PUBLIC_KEY[16] = {0x8b,0x33,0x87,0xe9,0xc5,0xcd,0xea,0x6a,0xc9,0xe5,0xed,0xba,0xa1,0x15,0xcd,0x72};

/* ------------------------------- state ------------------------------- */
#define MAX_CONTACTS 40
#define MAX_CHANNELS 8
#define NAME_MAX_LEN 31

typedef struct {
  uint8_t  pub[32];
  char     name[32];
  uint8_t  type;          /* 1 chat, 2 repeater, 3 room, 4 sensor */
  uint8_t  pad[3];
  uint32_t last_adv;      /* timestamp from their last advert */
} contact_t;               /* saved to disk as-is */

typedef struct {
  char     name[24];
  uint8_t  key[16];
  uint8_t  hash;
  uint8_t  used;
  uint8_t  pad[2];
} channel_t;

typedef struct {
  uint32_t magic;
  char     name[32];
  float    freq, bw;
  uint8_t  sf, cr, sync, power;
  uint16_t preamble;
  uint8_t  verify_adverts, cur_ch;
} config_t;
#define CFG_MAGIC 0x4D43F001u

static config_t   cfg;
static contact_t  contacts[MAX_CONTACTS];
static int        n_contacts;
static channel_t  chans[MAX_CHANNELS];
static uint8_t    my_prv[64], my_pub[32];

/* per-run caches (not saved) */
static uint8_t c_secret[MAX_CONTACTS][32];
static uint8_t c_have[MAX_CONTACTS];

typedef struct { uint8_t crc[4]; char who[24]; uint32_t t_ms; uint8_t used; } pending_ack_t;
static pending_ack_t pend[4];

static uint8_t  seen[64][8];
static int      seen_pos;
static uint32_t st_rx, st_tx, st_drop, st_dup;
static uint32_t clock_base, clock_ms0;
static int      clock_manual;

static uint8_t  pkt[MAX_PACKET + 1];
static uint8_t  plain[MAX_PAYLOAD + 16];

/* ------------------------------- helpers ------------------------------- */
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void hexprint(const uint8_t *p, int n) { for (int i = 0; i < n; i++) koa_printf("%02x", p[i]); }
static int hexval(int c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; }
static int parse_hex(const char *s, uint8_t *out, int n) {       /* exactly n bytes of hex */
  for (int i = 0; i < n; i++) { int a = hexval(s[2*i]), b = a < 0 ? -1 : hexval(s[2*i+1]); if (a < 0 || b < 0) return -1; out[i] = (uint8_t)(a * 16 + b); }
  return s[2*n] == 0 ? 0 : -1;
}
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int prefix_ci(const char *s, const char *pre) { while (*pre) { if (lc(*s) != lc(*pre)) return 0; s++; pre++; } return 1; }
static void copy_str(char *dst, const char *src, int cap) { int i = 0; while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; } dst[i] = 0; }
static int is_digits(const char *s) { if (!*s) return 0; while (*s) { if (*s < '0' || *s > '9') return 0; s++; } return 1; }
static int to_int(const char *s) { int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
static float to_float(const char *s) {
  float v = 0, f = 0.1f; int neg = 0;
  if (*s == '-') { neg = 1; s++; }
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * f; f /= 10; } }
  return neg ? -v : v;
}
static const char *skip_ws(const char *s) { while (*s == ' ') s++; return s; }
static const char *next_tok(const char *s, char *out, int cap) {     /* copies one word, returns rest */
  s = skip_ws(s); int i = 0;
  while (*s && *s != ' ') { if (i < cap - 1) out[i++] = *s; s++; }
  out[i] = 0; return skip_ws(s);
}
static const char *type_name(int t) { return t == 1 ? "chat" : t == 2 ? "repeater" : t == 3 ? "room" : t == 4 ? "sensor" : "?"; }

static uint32_t now_s(void) {
  int64_t t = koa_unixtime();
  if (t > 0) return (uint32_t)t;
  if (clock_manual) return clock_base + (koa_millis() - clock_ms0) / 1000;
  return 0;
}

/* ------------------------------ persistence ------------------------------ */
static int save_file(const char *name, const void *buf, int n) {
  int fd = koa_open(name, KO_WR); if (fd < 0) return fd;
  int w = koa_write(fd, buf, n); koa_close(fd);
  return w == n ? KE_OK : KE_IO;
}
static int load_file(const char *name, void *buf, int n) {
  int fd = koa_open(name, KO_RD); if (fd < 0) return fd;
  int got = 0;
  while (got < n) { int r = koa_read(fd, (uint8_t *)buf + got, n - got); if (r <= 0) break; got += r; }
  koa_close(fd);
  return got;
}
static void save_cfg(void)      { save_file("config", &cfg, sizeof cfg); }
static void save_contacts(void) { save_file("contacts", contacts, (int)(n_contacts * sizeof(contact_t))); }
static void save_chans(void)    { save_file("channels", chans, sizeof chans); }

static void set_channel_key(channel_t *c, const char *name, const uint8_t key[16]) {
  uint8_t h[32];
  memset(c, 0, sizeof *c);
  copy_str(c->name, name, sizeof c->name);
  memcpy(c->key, key, 16);
  koa_sha256(key, 16, h);
  c->hash = h[0];
  c->used = 1;
}

static int add_hashtag_channel(const char *name) {      /* key = SHA256("#name")[0:16] */
  char full[24]; uint8_t h[32], key[16];
  if (name[0] == '#') copy_str(full, name, sizeof full); else { full[0] = '#'; copy_str(full + 1, name, sizeof full - 1); }
  for (int i = 0; i < MAX_CHANNELS; i++) if (chans[i].used && !strcmp(chans[i].name, full)) return i;
  for (int i = 0; i < MAX_CHANNELS; i++) if (!chans[i].used) {
    koa_sha256(full, (int32_t)strlen(full), h);
    memcpy(key, h, 16);
    set_channel_key(&chans[i], full, key);
    save_chans();
    return i;
  }
  return KE_NOMEM;
}

static void load_all(void) {
  int r = load_file("config", &cfg, sizeof cfg);
  if (r != (int)sizeof cfg || cfg.magic != CFG_MAGIC) {
    memset(&cfg, 0, sizeof cfg);
    cfg.magic = CFG_MAGIC;
    copy_str(cfg.name, "KoaOS", sizeof cfg.name);
    cfg.freq = 910.525f; cfg.bw = 62.5f; cfg.sf = 7; cfg.cr = 5;     /* MeshCore "USA/Canada (Recommended)" preset */
    cfg.sync = 0x12; cfg.power = 10; cfg.preamble = 16;
    cfg.verify_adverts = 0; cfg.cur_ch = 0;
  }
  r = load_file("channels", chans, sizeof chans);
  if (r != (int)sizeof chans || !chans[0].used) { memset(chans, 0, sizeof chans); set_channel_key(&chans[0], "Public", PUBLIC_KEY); save_chans(); }
  r = load_file("contacts", contacts, (int)sizeof contacts);
  n_contacts = r > 0 ? r / (int)sizeof(contact_t) : 0;
}

static void load_or_make_identity(void) {
  uint8_t buf[96];
  if (load_file("identity", buf, 96) == 96) { memcpy(my_prv, buf, 64); memcpy(my_pub, buf + 64, 32); return; }
  koa_puts("first run: generating your MeshCore identity (slow on the interpreter, be patient)...");
  uint8_t seed[32];
  do {
    koa_random(seed, 32);
    mc_ed_keypair(seed, my_prv, my_pub);
  } while (my_pub[0] == 0x00 || my_pub[0] == 0xFF);                   /* those hash bytes are reserved in MeshCore */
  memcpy(buf, my_prv, 64); memcpy(buf + 64, my_pub, 32);
  if (save_file("identity", buf, 96) != KE_OK) koa_puts("WARNING: could not save identity (flash full?)");
}

/* ------------------------------ radio ------------------------------ */
static int radio_apply(void) {
  koa_lora_cfg_t c;
  memset(&c, 0, sizeof c);
  c.freq_mhz = cfg.freq; c.bw_khz = cfg.bw; c.sf = cfg.sf; c.cr = cfg.cr;
  c.sync_word = cfg.sync; c.power_dbm = (int8_t)cfg.power; c.preamble = cfg.preamble;
  return koa_lora_config(&c);
}

static int already_seen(const uint8_t *p, int plen, int ptype) {
  uint8_t h[32]; static uint8_t tmp[MAX_PAYLOAD + 1];
  tmp[0] = (uint8_t)ptype; memcpy(tmp + 1, p, (size_t)plen);
  koa_sha256(tmp, plen + 1, h);
  for (int i = 0; i < 64; i++) if (!memcmp(seen[i], h, 8)) return 1;
  memcpy(seen[seen_pos], h, 8); seen_pos = (seen_pos + 1) & 63;
  return 0;
}

static int send_packet(uint8_t route, uint8_t ptype, const uint8_t *payload, int plen) {
  if (plen > MAX_PAYLOAD) return KE_INVAL;
  int n = 0;
  pkt[n++] = (uint8_t)((ptype << 2) | route);
  pkt[n++] = 0;                                   /* path length 0: we are the origin */
  memcpy(pkt + n, payload, (size_t)plen); n += plen;
  already_seen(payload, plen, ptype);             /* so we ignore repeaters echoing it back */
  for (int t = 0; t < 6 && koa_lora_cad() == 1; t++) {   /* listen before talk */
    uint8_t r; koa_random(&r, 1); koa_sleep(60 + r);
  }
  int rc = koa_lora_tx(pkt, n);
  if (rc > 0) st_tx++;
  return rc > 0 ? KE_OK : rc;
}

/* ----------------------- encrypt-then-MAC (AES-128-ECB + 2 byte HMAC) ----------------------- */
/* out = mac(2) || ciphertext. Returns total length, or <0. */
static int enc_mac(const uint8_t secret[32], const uint8_t *in, int n, uint8_t *out) {
  static uint8_t padded[MAX_PAYLOAD + 16];
  int blocks = (n + 15) / 16, len = blocks * 16;
  uint8_t mac[32];
  if (len > 176) return KE_INVAL;
  memset(padded, 0, (size_t)len); memcpy(padded, in, (size_t)n);
  if (koa_aes128_ecb(secret, padded, out + 2, len, 1) != KE_OK) return KE_IO;
  koa_hmac_sha256(secret, 32, out + 2, len, mac);
  out[0] = mac[0]; out[1] = mac[1];
  return len + 2;
}
/* in = mac(2) || ciphertext. Returns plaintext length (zero padded), or <0 if the MAC does not match. */
static int dec_mac(const uint8_t secret[32], const uint8_t *in, int n, uint8_t *out) {
  uint8_t mac[32];
  int len = n - 2;
  if (len <= 0 || (len & 15)) return KE_INVAL;
  koa_hmac_sha256(secret, 32, in + 2, len, mac);
  if (mac[0] != in[0] || mac[1] != in[1]) return KE_PERM;
  if (koa_aes128_ecb(secret, in + 2, out, len, 0) != KE_OK) return KE_IO;
  return len;
}
static void chan_secret(const channel_t *c, uint8_t s[32]) { memset(s, 0, 32); memcpy(s, c->key, 16); }

/* --------------------------- contacts / secrets --------------------------- */
static int find_contact_by_pub(const uint8_t *pub) {
  for (int i = 0; i < n_contacts; i++) if (!memcmp(contacts[i].pub, pub, 32)) return i;
  return -1;
}
static int find_contact(const char *s) {
  if (is_digits(s)) { int i = to_int(s); return (i >= 1 && i <= n_contacts) ? i - 1 : -1; }
  for (int i = 0; i < n_contacts; i++) if (prefix_ci(contacts[i].name, s)) return i;
  return -1;
}
static const uint8_t *contact_secret(int i) {
  if (!c_have[i]) {
    koa_printf("(computing shared key with %s, slow the first time...)\n", contacts[i].name);
    mc_ed_ecdh(c_secret[i], contacts[i].pub, my_prv);
    c_have[i] = 1;
  }
  return c_secret[i];
}

/* ------------------------------ sending ------------------------------ */
static void send_advert(void) {
  static uint8_t body[MAX_PAYLOAD];
  uint8_t app[40]; int al = 0, n = 0; uint32_t ts = now_s();
  if (!ts) { koa_puts("clock not set: use  /time <unix seconds>  (or get a GPS/Wi-Fi time first)"); return; }
  app[al++] = (uint8_t)(1 | ADV_NAME);
  int nl = (int)strlen(cfg.name); if (nl > NAME_MAX_LEN) nl = NAME_MAX_LEN;
  memcpy(app + al, cfg.name, (size_t)nl); al += nl;
  static uint8_t msg[32 + 4 + 40];
  memcpy(msg, my_pub, 32); put32(msg + 32, ts); memcpy(msg + 36, app, (size_t)al);
  uint8_t sig[64];
  koa_puts("signing advert (slow)...");
  mc_ed_sign(sig, msg, (size_t)(36 + al), my_pub, my_prv);
  memcpy(body + n, my_pub, 32); n += 32;
  put32(body + n, ts); n += 4;
  memcpy(body + n, sig, 64); n += 64;
  memcpy(body + n, app, (size_t)al); n += al;
  int rc = send_packet(ROUTE_FLOOD, PT_ADVERT, body, n);
  koa_printf("advert %s\n", rc == KE_OK ? "sent" : koa_strerror(rc));
}

static void send_channel(int ci, const char *text) {
  uint8_t secret[32];
  static uint8_t pl[MAX_PAYLOAD];
  channel_t *c = &chans[ci];
  uint32_t ts = now_s();
  int n = 0;
  put32(plain, ts); plain[4] = 0; n = 5;
  int room = 176 - 5 - 1;
  n += koa_snprintf((char *)plain + n, (size_t)(room + 1), "%s: %s", cfg.name, text);
  if (n > 176) n = 176;
  chan_secret(c, secret);
  pl[0] = c->hash;
  int m = enc_mac(secret, plain, n, pl + 1);
  if (m < 0) { koa_printf("encrypt failed: %s\n", koa_strerror(m)); return; }
  int rc = send_packet(ROUTE_FLOOD, PT_GRP_TXT, pl, 1 + m);
  if (rc != KE_OK) koa_printf("send failed: %s\n", koa_strerror(rc));
}

static void send_dm(int ci, const char *text) {
  static uint8_t pl[MAX_PAYLOAD];
  contact_t *ct = &contacts[ci];
  uint32_t ts = now_s();
  int tl = (int)strlen(text); if (tl > 150) tl = 150;
  const uint8_t *sec = contact_secret(ci);
  put32(plain, ts); plain[4] = 0;                          /* txt_type 0 (plain), attempt 0 */
  memcpy(plain + 5, text, (size_t)tl);
  int n = 5 + tl;
  pl[0] = ct->pub[0]; pl[1] = my_pub[0];
  int m = enc_mac(sec, plain, n, pl + 2);
  if (m < 0) { koa_printf("encrypt failed: %s\n", koa_strerror(m)); return; }
  /* the ack we expect back: first 4 bytes of SHA256(timestamp | flags | text | our pubkey) */
  static uint8_t ackbuf[5 + 150 + 32]; uint8_t h[32];
  memcpy(ackbuf, plain, (size_t)n); memcpy(ackbuf + n, my_pub, 32);
  koa_sha256(ackbuf, n + 32, h);
  for (int i = 0; i < 4; i++) if (!pend[i].used) { memcpy(pend[i].crc, h, 4); copy_str(pend[i].who, ct->name, sizeof pend[i].who); pend[i].t_ms = koa_millis(); pend[i].used = 1; break; }
  int rc = send_packet(ROUTE_FLOOD, PT_TXT_MSG, pl, 2 + m);
  if (rc != KE_OK) koa_printf("send failed: %s\n", koa_strerror(rc));
  else koa_printf("-> %s: sent (waiting for ack)\n", ct->name);
}

static void send_ack(const uint8_t crc[4]) { send_packet(ROUTE_FLOOD, PT_ACK, crc, 4); }

static void ack_arrived(const uint8_t *crc) {
  for (int i = 0; i < 4; i++) if (pend[i].used && !memcmp(pend[i].crc, crc, 4)) {
    koa_printf("   delivered to %s (%u ms)\n", pend[i].who, (unsigned)(koa_millis() - pend[i].t_ms));
    pend[i].used = 0;
  }
}

/* ------------------------------ receiving ------------------------------ */
static float last_rssi, last_snr;

static void on_advert(const uint8_t *p, int n, int hops) {
  if (n < 32 + 4 + 64) return;
  const uint8_t *pub = p, *sig = p + 36, *app = p + 100; int al = n - 100;
  uint32_t ts = get32(p + 32);
  if (!memcmp(pub, my_pub, 32)) return;
  if (cfg.verify_adverts) {
    static uint8_t msg[32 + 4 + MAX_PAYLOAD];
    memcpy(msg, pub, 32); memcpy(msg + 32, p + 32, 4); memcpy(msg + 36, app, (size_t)al);
    if (mc_ed_verify(sig, msg, (size_t)(36 + al), pub) != 0) { koa_puts("advert with BAD signature ignored"); return; }
  }
  int type = al > 0 ? (app[0] & 0x0F) : 0;
  char name[32]; name[0] = 0;
  if (al > 0) {
    int i = 1;
    if (app[0] & ADV_LATLON) i += 8;
    if (app[0] & ADV_FEAT1)  i += 2;
    if (app[0] & ADV_FEAT2)  i += 2;
    if ((app[0] & ADV_NAME) && i < al) { int k = 0; while (i < al && app[i] && k < 31) name[k++] = (char)app[i++]; name[k] = 0; }
  }
  int idx = find_contact_by_pub(pub), isnew = 0;
  if (idx < 0) {
    if (n_contacts >= MAX_CONTACTS) return;
    idx = n_contacts++; isnew = 1;
    memset(&contacts[idx], 0, sizeof contacts[idx]);
    memcpy(contacts[idx].pub, pub, 32);
    c_have[idx] = 0;
  } else if (ts <= contacts[idx].last_adv) return;       /* old/replayed advert */
  contacts[idx].last_adv = ts; contacts[idx].type = (uint8_t)type;
  if (name[0]) copy_str(contacts[idx].name, name, sizeof contacts[idx].name);
  else if (isnew) { koa_snprintf(contacts[idx].name, sizeof contacts[idx].name, "node-%02x%02x", pub[0], pub[1]); }
  save_contacts();
  koa_printf("[advert] %s%s (%s) id %02x hops %d rssi %.0f snr %.1f%s\n", isnew ? "NEW " : "", contacts[idx].name, type_name(type),
             pub[0], hops, last_rssi, last_snr, cfg.verify_adverts ? " verified" : "");
}

static void on_group(const uint8_t *p, int n) {
  if (n < 1 + 2 + 16) return;
  uint8_t secret[32];
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!chans[i].used || chans[i].hash != p[0]) continue;
    chan_secret(&chans[i], secret);
    int m = dec_mac(secret, p + 1, n - 1, plain);
    if (m < 5) continue;
    plain[m < (int)sizeof plain ? m : (int)sizeof plain - 1] = 0;
    if (plain[4] != 0) { koa_printf("[%s] (non-text group data, type 0x%02x)\n", chans[i].name, plain[4]); return; }
    koa_printf("[%s] ", chans[i].name); koa_print((const char *)plain + 5); koa_putc('\n');
    return;
  }
}

static void on_text(const uint8_t *p, int n, int is_path) {
  if (n < 2 + 2 + 16 || p[0] != my_pub[0]) return;       /* not addressed to us */
  for (int i = 0; i < n_contacts; i++) {
    if (contacts[i].pub[0] != p[1]) continue;
    int m = dec_mac(contact_secret(i), p + 2, n - 2, plain);
    if (m < 5) continue;                                  /* wrong contact (hash collision) or corrupted */
    if (is_path) {                                        /* returned path; an ack may ride along as "extra" */
      int pl = plain[0];
      if (1 + pl + 1 <= m) {
        int et = plain[1 + pl], eo = 2 + pl;
        if ((et & 0x0F) == PT_ACK && eo + 4 <= m) ack_arrived(plain + eo);
      }
      return;
    }
    uint32_t ts = get32(plain);
    int flags = plain[4], ttype = flags >> 2;
    plain[m < (int)sizeof plain ? m : (int)sizeof plain - 1] = 0;
    int tl = (int)strlen((const char *)plain + 5);
    if (ttype == 0) { koa_printf("[DM] %s: ", contacts[i].name); koa_print((const char *)plain + 5); koa_putc('\n'); }
    else koa_printf("[DM] %s sent a type-%d message (not shown)\n", contacts[i].name, ttype);
    /* ack: first 4 bytes of SHA256(timestamp | flags | text | sender pubkey) */
    static uint8_t ab[5 + 180 + 32]; uint8_t h[32];
    (void)ts;
    memcpy(ab, plain, (size_t)(5 + tl)); memcpy(ab + 5 + tl, contacts[i].pub, 32);
    koa_sha256(ab, 5 + tl + 32, h);
    send_ack(h);
    return;
  }
}

static void handle_packet(const uint8_t *b, int len) {
  if (len < 2) { st_drop++; return; }
  int hdr = b[0], route = hdr & 3, ptype = (hdr >> 2) & 15, i = 1;
  if (route == ROUTE_TRANSPORT_FLOOD || route == ROUTE_TRANSPORT_DIRECT) { if (len < 6) { st_drop++; return; } i += 4; }
  int pl = b[i++], hops = pl & 63, hsz = (pl >> 6) + 1;
  int pathbytes = hops * hsz;
  if (hsz == 4 || pathbytes > MAX_PATH || i + pathbytes > len) { st_drop++; return; }
  i += pathbytes;
  int plen = len - i;
  if (plen < 0 || plen > MAX_PAYLOAD) { st_drop++; return; }
  const uint8_t *p = b + i;
  st_rx++;
  if (already_seen(p, plen, ptype)) { st_dup++; return; }
  switch (ptype) {
    case PT_ADVERT:  on_advert(p, plen, hops); break;
    case PT_GRP_TXT: on_group(p, plen); break;
    case PT_TXT_MSG: on_text(p, plen, 0); break;
    case PT_PATH:    on_text(p, plen, 1); break;
    case PT_ACK:     if (plen >= 4) ack_arrived(p); break;
    default: break;                                       /* REQ/RESPONSE/TRACE/etc: not handled by this client */
  }
}

/* ------------------------------ commands ------------------------------ */
static void show_help(void) {
  koa_puts("text            send to the current channel");
  koa_puts("/help           this list");
  koa_puts("/me             your name, id and public key");
  koa_puts("/name <n>       set your name");
  koa_puts("/adv            announce yourself (signed flood advert)");
  koa_puts("/contacts       nodes heard");
  koa_puts("/msg <#|name> <text>   direct message");
  koa_puts("/ch             list channels; /use <n> pick one");
  koa_puts("/join #name     join a #hashtag channel");
  koa_puts("/addch <name> <32 hex>  add a private channel by key");
  koa_puts("/radio [mhz bw sf cr]   show/set radio;  /power <dBm>  /sync <hex>");
  koa_puts("/verify on|off  check advert signatures (slow)");
  koa_puts("/time <unix>    set clock if no GPS/Wi-Fi time");
  koa_puts("/stats  /quit");
}

static void cmd(const char *line) {
  char w[24]; const char *rest = next_tok(line + 1, w, sizeof w);
  if (!strcmp(w, "help") || !strcmp(w, "?")) show_help();
  else if (!strcmp(w, "me")) {
    koa_printf("name %s  id %02x\npublic key ", cfg.name, my_pub[0]); hexprint(my_pub, 32); koa_putc('\n');
  }
  else if (!strcmp(w, "name")) {
    if (!*rest) { koa_puts("usage: /name <name>"); return; }
    copy_str(cfg.name, rest, NAME_MAX_LEN + 1); save_cfg(); koa_printf("name is now %s\n", cfg.name);
  }
  else if (!strcmp(w, "adv")) send_advert();
  else if (!strcmp(w, "contacts")) {
    if (!n_contacts) koa_puts("none heard yet (adverts show up as repeaters/nodes announce themselves; /adv to announce yourself)");
    for (int i = 0; i < n_contacts; i++) koa_printf("%2d  %-20s %-8s id %02x\n", i + 1, contacts[i].name, type_name(contacts[i].type), contacts[i].pub[0]);
  }
  else if (!strcmp(w, "msg")) {
    char who[32]; const char *text = next_tok(rest, who, sizeof who);
    int ci = who[0] ? find_contact(who) : -1;
    if (ci < 0 || !*text) { koa_puts("usage: /msg <number|name> <text>   (see /contacts)"); return; }
    if (contacts[ci].type == 2) { koa_puts("that is a repeater; direct text messages go to chat nodes"); return; }
    if (!now_s()) { koa_puts("clock not set: /time <unix>"); return; }
    send_dm(ci, text);
  }
  else if (!strcmp(w, "ch")) {
    for (int i = 0; i < MAX_CHANNELS; i++) if (chans[i].used) koa_printf("%s %d  %-22s hash %02x\n", i == cfg.cur_ch ? "*" : " ", i + 1, chans[i].name, chans[i].hash);
  }
  else if (!strcmp(w, "use")) {
    int n = to_int(rest);
    if (n >= 1 && n <= MAX_CHANNELS && chans[n - 1].used) { cfg.cur_ch = (uint8_t)(n - 1); save_cfg(); koa_printf("now talking on %s\n", chans[n - 1].name); }
    else koa_puts("usage: /use <channel number>  (see /ch)");
  }
  else if (!strcmp(w, "join")) {
    if (!*rest) { koa_puts("usage: /join #name"); return; }
    int i = add_hashtag_channel(rest);
    if (i < 0) koa_puts("no room for more channels");
    else { cfg.cur_ch = (uint8_t)i; save_cfg(); koa_printf("joined %s (hash %02x), now talking on it\n", chans[i].name, chans[i].hash); }
  }
  else if (!strcmp(w, "addch")) {
    char nm[24], hx[40]; const char *r2 = next_tok(rest, nm, sizeof nm); next_tok(r2, hx, sizeof hx);
    uint8_t key[16];
    if (!nm[0] || parse_hex(hx, key, 16) != 0) { koa_puts("usage: /addch <name> <32 hex chars>"); return; }
    for (int i = 0; i < MAX_CHANNELS; i++) if (!chans[i].used) { set_channel_key(&chans[i], nm, key); save_chans(); koa_printf("added %s (hash %02x) as channel %d\n", nm, chans[i].hash, i + 1); return; }
    koa_puts("no room for more channels");
  }
  else if (!strcmp(w, "radio")) {
    if (*rest) {
      char a[16], b[16], c[8], d[8]; const char *r = next_tok(rest, a, sizeof a); r = next_tok(r, b, sizeof b); r = next_tok(r, c, sizeof c); next_tok(r, d, sizeof d);
      if (!a[0] || !b[0] || !c[0] || !d[0]) { koa_puts("usage: /radio <mhz> <bw khz> <sf> <cr 5-8>   e.g. /radio 910.525 62.5 7 5"); return; }
      cfg.freq = to_float(a); cfg.bw = to_float(b); cfg.sf = (uint8_t)to_int(c); cfg.cr = (uint8_t)to_int(d);
      int rc = radio_apply(); if (rc == KE_OK) save_cfg(); else koa_printf("radio rejected those settings: %s\n", koa_strerror(rc));
    }
    koa_printf("radio %.3f MHz  bw %.1f  sf %d  cr 4/%d  sync 0x%02x  power %d dBm  preamble %d\n", cfg.freq, cfg.bw, cfg.sf, cfg.cr, cfg.sync, cfg.power, cfg.preamble);
  }
  else if (!strcmp(w, "power")) { cfg.power = (uint8_t)to_int(rest); if (radio_apply() == KE_OK) save_cfg(); koa_printf("power %d dBm\n", cfg.power); }
  else if (!strcmp(w, "sync")) {
    uint8_t v; char h2[4]; copy_str(h2, rest, 4);
    if (strlen(h2) == 2 && parse_hex(h2, &v, 1) == 0) { cfg.sync = v; radio_apply(); save_cfg(); koa_printf("sync word 0x%02x\n", v); } else koa_puts("usage: /sync <2 hex digits>   (MeshCore: 12)");
  }
  else if (!strcmp(w, "verify")) { cfg.verify_adverts = !strcmp(rest, "on"); save_cfg(); koa_printf("advert signature checking %s\n", cfg.verify_adverts ? "on" : "off"); }
  else if (!strcmp(w, "time")) {
    uint32_t t = (uint32_t)to_int(rest);
    if (!t) { koa_printf("clock: %u\n", (unsigned)now_s()); return; }
    clock_base = t; clock_ms0 = koa_millis(); clock_manual = 1; koa_printf("clock set to %u\n", (unsigned)t);
  }
  else if (!strcmp(w, "stats")) koa_printf("rx %u  tx %u  dup %u  dropped %u  contacts %d\n", (unsigned)st_rx, (unsigned)st_tx, (unsigned)st_dup, (unsigned)st_drop, n_contacts);
  else koa_puts("unknown command, /help");
}

/* ------------------------------ main ------------------------------ */
int koa_main(void) {
  int rc = koa_acquire(KC_LORA);
  if (rc != KE_OK) { koa_printf("can't use the radio: %s\n", koa_strerror(rc)); return 1; }
  load_all();
  load_or_make_identity();
  rc = radio_apply();
  if (rc != KE_OK) { koa_printf("radio setup failed: %s\n", koa_strerror(rc)); koa_release(KC_LORA); return 1; }

  koa_printf("MeshCore for KoaOS   %s   id %02x\n", cfg.name, my_pub[0]);
  koa_printf("%.3f MHz  bw %.1f  sf %d  cr 4/%d   channel: %s\n", cfg.freq, cfg.bw, cfg.sf, cfg.cr, chans[cfg.cur_ch].name);
  koa_puts("type to chat, /help for commands, /adv to announce yourself");
  if (!now_s()) koa_puts("note: clock not set (no GPS/Wi-Fi time); sending needs /time <unix>");

  char line[200]; int ll = 0;
  while (koa_alive()) {
    int n = koa_lora_rx(pkt, MAX_PACKET, 40, &last_rssi, &last_snr);
    if (n > 0) handle_packet(pkt, n);
    else if (n < 0 && n != KE_TIMEOUT) { if (n == KE_KILLED) break; }
    int c;
    while ((c = koa_getc(0)) >= 0) {
      if (c == '\n' || c == '\r') {
        line[ll] = 0; ll = 0;
        if (line[0] == '/') {
          if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) goto done;
          cmd(line);
        } else if (line[0]) {
          if (!now_s()) koa_puts("clock not set: /time <unix>");
          else send_channel(cfg.cur_ch, line);
        }
      } else if (ll < (int)sizeof line - 1) line[ll++] = (char)c;
    }
  }
done:
  koa_release(KC_LORA);
  koa_puts("bye");
  return 0;
}
