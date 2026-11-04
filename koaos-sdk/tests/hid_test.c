// make hidtest - checks koa_hid.h (HID report -> terminal bytes) on a PC
#include <stdio.h>
#include <string.h>
#include "../../KoaOS/koa_hid.h"
static int fails = 0;
#define EXPECT(name, cond) do { if (!(cond)) { printf("FAIL: %s\n", name); fails++; } } while (0)
static int press(koa_hid_t* h, uint8_t mods, uint8_t k, uint32_t t, char* out) {
  uint8_t r[8] = { mods, 0, k, 0, 0, 0, 0, 0 };
  return koa_hid_report(h, r, t, out, 16);
}
static void release(koa_hid_t* h, uint32_t t) { uint8_t r[8] = {0}; char o[16]; koa_hid_report(h, r, t, o, 16); }
int main() {
  koa_hid_t h; char o[32]; int n;
  koa_hid_init(&h);
  n = press(&h, 0, 0x04, 0, o);               EXPECT("a", n == 1 && o[0] == 'a');           release(&h, 1);
  n = press(&h, KM_LSHIFT, 0x04, 2, o);       EXPECT("shift a", n == 1 && o[0] == 'A');     release(&h, 3);
  n = press(&h, KM_LCTRL, 0x06, 4, o);        EXPECT("ctrl-c", n == 1 && o[0] == 3);        release(&h, 5);
  n = press(&h, KM_RSHIFT, 0x1E, 6, o);       EXPECT("shift 1", n == 1 && o[0] == '!');     release(&h, 7);
  n = press(&h, 0, 0x27, 8, o);               EXPECT("0", n == 1 && o[0] == '0');           release(&h, 9);
  n = press(&h, 0, 0x28, 10, o);              EXPECT("enter", n == 1 && o[0] == '\r');      release(&h, 11);
  n = press(&h, 0, 0x2A, 12, o);              EXPECT("bksp", n == 1 && o[0] == 127);        release(&h, 13);
  n = press(&h, 0, 0x2C, 14, o);              EXPECT("space", n == 1 && o[0] == ' ');       release(&h, 15);
  n = press(&h, 0, 0x2D, 16, o);              EXPECT("-", n == 1 && o[0] == '-');           release(&h, 17);
  n = press(&h, KM_LSHIFT, 0x2D, 18, o);      EXPECT("_", n == 1 && o[0] == '_');           release(&h, 19);
  n = press(&h, 0, 0x38, 20, o);              EXPECT("/", n == 1 && o[0] == '/');           release(&h, 21);
  n = press(&h, 0, 0x52, 22, o);              EXPECT("up", n == 3 && !memcmp(o, "\x1b[A", 3)); release(&h, 23);
  n = press(&h, 0, 0x51, 24, o);              EXPECT("down", n == 3 && !memcmp(o, "\x1b[B", 3)); release(&h, 25);
  n = press(&h, 0, 0x4C, 26, o);              EXPECT("del", n == 4 && !memcmp(o, "\x1b[3~", 4)); release(&h, 27);
  n = press(&h, KM_LALT, 0x05, 28, o);        EXPECT("alt-b", n == 2 && o[0] == 27 && o[1] == 'b'); release(&h, 29);
  n = press(&h, KM_LCTRL, 0x2C, 30, o);       EXPECT("ctrl-space NUL", n == 1 && o[0] == 0); release(&h, 31);
  n = press(&h, 0, 0x01, 32, o);              EXPECT("rollover error ignored", n == 0);     release(&h, 33);
  // caps lock toggles letter case, and the Caps key itself sends nothing
  n = press(&h, 0, 0x39, 40, o);              EXPECT("caps sends nothing", n == 0);         release(&h, 41);
  n = press(&h, 0, 0x04, 42, o);              EXPECT("caps a", n == 1 && o[0] == 'A');      release(&h, 43);
  n = press(&h, KM_LSHIFT, 0x04, 44, o);      EXPECT("caps+shift a", n == 1 && o[0] == 'a'); release(&h, 45);
  n = press(&h, 0, 0x39, 46, o);                                                                release(&h, 47);
  // two keys at once, then only the new one is reported when a third arrives
  { uint8_t r1[8] = {0,0,0x04,0x05,0,0,0,0}; n = koa_hid_report(&h, r1, 100, o, 16); EXPECT("two keys", n == 2 && o[0] == 'a' && o[1] == 'b');
    uint8_t r2[8] = {0,0,0x04,0x05,0x06,0,0,0}; n = koa_hid_report(&h, r2, 110, o, 16); EXPECT("third key only", n == 1 && o[0] == 'c'); release(&h, 120); }
  // auto-repeat: nothing before 500 ms, then every 35 ms, stops on release
  koa_hid_init(&h);
  n = press(&h, 0, 0x04, 1000, o);            EXPECT("repeat first", n == 1);
  EXPECT("no repeat at 400ms", koa_hid_tick(&h, 1400, o) == 0);
  n = koa_hid_tick(&h, 1500, o);              EXPECT("repeat at 500ms", n == 1 && o[0] == 'a');
  EXPECT("not again at 510ms", koa_hid_tick(&h, 1510, o) == 0);
  n = koa_hid_tick(&h, 1536, o);              EXPECT("repeat at +35ms", n == 1);
  release(&h, 1540);
  EXPECT("no repeat after release", koa_hid_tick(&h, 2000, o) == 0);
  // arrow keys repeat as whole sequences
  n = press(&h, 0, 0x51, 3000, o);            EXPECT("down seq", n == 3);
  n = koa_hid_tick(&h, 3500, o);              EXPECT("down repeats whole seq", n == 3 && !memcmp(o, "\x1b[B", 3));
  koa_hid_release(&h);
  EXPECT("release clears repeat", koa_hid_tick(&h, 9000, o) == 0);
  // millis() wrap-around must not stall or fire early
  koa_hid_init(&h);
  n = press(&h, 0, 0x04, 0xFFFFFF00u, o);
  EXPECT("wrap: not early", koa_hid_tick(&h, 0xFFFFFF80u, o) == 0);
  EXPECT("wrap: fires after delay", koa_hid_tick(&h, 0xFFFFFF00u + 500, o) == 1);
  printf(fails ? "%d FAILED\n" : "hid_test: all passed\n", fails);
  return fails != 0;
}
