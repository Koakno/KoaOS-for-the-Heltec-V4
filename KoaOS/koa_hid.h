// koa_hid.h - USB/BLE HID boot-keyboard report -> terminal bytes. No Arduino or BLE dependencies, so it is unit-tested on a PC
// (make hidtest). The bytes are exactly what a serial terminal would send, so they go straight into ttyInputChar():
//   printable ASCII, Enter = \r, Backspace = 127, Tab = 9, Esc = 27, Ctrl-<letter> = 1..26, arrows = ESC [ A..D,
//   Home/End = ESC [ H / F, Del/Ins/PgUp/PgDn = ESC [ n ~.  Alt-<key> sends ESC first.  US layout.
//
// A report is 8 bytes: [0] modifier bits, [1] reserved, [2..7] up to six held key usage codes (0 = none, 1 = rollover error).
#pragma once
#include <stdint.h>
#include <string.h>

#define KHID_REPEAT_DELAY_MS 500
#define KHID_REPEAT_RATE_MS   35

enum { KM_LCTRL = 1, KM_LSHIFT = 2, KM_LALT = 4, KM_LGUI = 8, KM_RCTRL = 16, KM_RSHIFT = 32, KM_RALT = 64, KM_RGUI = 128 };

typedef struct {
  uint8_t  prev[8];      // last report
  uint8_t  held;         // usage code that auto-repeats (0 = none)
  uint8_t  mods;         // modifiers of the last report
  uint8_t  caps;         // caps-lock state (tracked here: the keyboard's LED is not driven)
  uint32_t t_next;       // when the next repeat is due
} koa_hid_t;

static inline void koa_hid_init(koa_hid_t* h) { memset(h, 0, sizeof(*h)); }
static inline void koa_hid_release(koa_hid_t* h) { uint8_t c = h->caps; memset(h, 0, sizeof(*h)); h->caps = c; }   // link dropped: forget held keys

// Bytes for one key press. Returns the length written to out (max 5), 0 for keys that send nothing.
static inline int koa_hid_key(uint8_t code, uint8_t mods, uint8_t caps, char* out) {
  static const char un[] = "-=[]\\#;'`,./";          // usage 0x2D..0x38
  static const char sh[] = "_+{}|~:\"~<>?";
  static const char shnum[] = "!@#$%^&*()";
  const int shift = (mods & (KM_LSHIFT | KM_RSHIFT)) != 0;
  const int ctrl  = (mods & (KM_LCTRL | KM_RCTRL)) != 0;
  const int alt   = (mods & (KM_LALT | KM_RALT)) != 0;
  char c = 0;
  const char* seq = 0;

  if (code >= 0x04 && code <= 0x1D) {                         // letters
    c = (char)('a' + (code - 0x04));
    if (ctrl) c = (char)(c - 'a' + 1);
    else if (shift ^ (caps != 0)) c = (char)(c - 'a' + 'A');
  } else if (code >= 0x1E && code <= 0x27) {                  // 1..9, 0
    int i = code - 0x1E;
    c = shift ? shnum[i] : (char)(i == 9 ? '0' : '1' + i);
    if (ctrl && !shift && i == 1) c = 0;                      // Ctrl-2 = NUL, like a terminal
  } else if (code >= 0x2D && code <= 0x38) {
    int i = code - 0x2D;
    c = shift ? sh[i] : un[i];
    if (ctrl && code == 0x2F) c = 27;                         // Ctrl-[  = Esc
    if (ctrl && code == 0x31) c = 28;                         // Ctrl-\.
    if (ctrl && code == 0x30) c = 29;                         // Ctrl-]
  } else if (code >= 0x59 && code <= 0x62) {                  // keypad 1..9, 0 (num-lock assumed on)
    c = (char)(code == 0x62 ? '0' : '1' + (code - 0x59));
  } else switch (code) {
    case 0x28: case 0x58: c = '\r'; break;                    // Enter, keypad Enter
    case 0x29: c = 27; break;                                 // Esc
    case 0x2A: c = 127; break;                                // Backspace
    case 0x2B: c = 9; break;                                  // Tab
    case 0x2C: c = ctrl ? 0 : ' '; break;                     // Space (Ctrl-Space = NUL)
    case 0x54: c = '/'; break;                                // keypad / * - + .
    case 0x55: c = '*'; break;
    case 0x56: c = '-'; break;
    case 0x57: c = '+'; break;
    case 0x63: c = '.'; break;
    case 0x4F: seq = "\x1b[C"; break;                         // Right Left Down Up
    case 0x50: seq = "\x1b[D"; break;
    case 0x51: seq = "\x1b[B"; break;
    case 0x52: seq = "\x1b[A"; break;
    case 0x4A: seq = "\x1b[H"; break;                         // Home
    case 0x4D: seq = "\x1b[F"; break;                         // End
    case 0x49: seq = "\x1b[2~"; break;                        // Insert
    case 0x4C: seq = "\x1b[3~"; break;                        // Delete
    case 0x4B: seq = "\x1b[5~"; break;                        // Page Up
    case 0x4E: seq = "\x1b[6~"; break;                        // Page Down
    default: return 0;
  }
  int n = 0;
  if (alt && !seq) out[n++] = 27;                             // Alt = meta prefix
  if (seq) { for (; *seq; seq++) out[n++] = *seq; return n; }
  out[n++] = c;                                               // NUL (Ctrl-Space) is a real byte, so no c==0 test
  return n;
}

static inline int koa_hid_has(const uint8_t* r, uint8_t code) {
  for (int i = 2; i < 8; i++) if (r[i] == code) return 1;
  return 0;
}

// Feed one report. Writes the bytes for newly pressed keys to out and returns how many (cap is the size of out).
static inline int koa_hid_report(koa_hid_t* h, const uint8_t* r, uint32_t now, char* out, int cap) {
  int n = 0;
  if (r[2] == 1) return 0;                                    // rollover error (too many keys): ignore the whole report
  uint8_t last = 0;
  for (int i = 2; i < 8; i++) {
    uint8_t k = r[i];
    if (!k || koa_hid_has(h->prev, k)) continue;              // empty slot, or already held
    if (k == 0x39) { h->caps ^= 1; continue; }                // Caps Lock
    char tmp[6];
    int m = koa_hid_key(k, r[0], h->caps, tmp);
    if (m && n + m <= cap) { memcpy(out + n, tmp, m); n += m; last = k; }
  }
  if (last) { h->held = last; h->t_next = now + KHID_REPEAT_DELAY_MS; }
  else if (h->held && !koa_hid_has(r, h->held)) h->held = 0; // the repeating key was let go
  memcpy(h->prev, r, 8);
  h->mods = r[0];
  return n;
}

// Call regularly. Returns the bytes for an auto-repeat that is due (0 or a few bytes).
static inline int koa_hid_tick(koa_hid_t* h, uint32_t now, char* out) {
  if (!h->held || (int32_t)(now - h->t_next) < 0) return 0;
  if (!koa_hid_has(h->prev, h->held)) { h->held = 0; return 0; }
  h->t_next = now + KHID_REPEAT_RATE_MS;
  return koa_hid_key(h->held, h->mods, h->caps, out);
}
