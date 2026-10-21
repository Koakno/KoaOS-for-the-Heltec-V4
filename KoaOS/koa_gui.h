/*
 * koa_gui.h -- the KoaOS graphical environment ("gui" command), drawn on the VGA output.
 *
 *   gui            start it (turns VGA on by itself if needed)           gui boot on|off   start it at every boot
 *
 * 320x240 pixels, 8 colors (koa_gfx.h), doubled to 640x480. Controlled with the keys you type in the USB serial terminal:
 *   arrow keys move, Enter opens, Esc goes back / opens the power menu, Ctrl-C leaves the GUI at once.
 * It is an ordinary KoaOS program: it uses the same system calls as any other, so it cannot do more than the shell can.
 * Built in: Files, Editor (opened from Files), Wi-Fi, GPS, System, Apps (runs installed apps), Terminal, Power.
 *
 * The GUI file is portable on purpose: it only needs the sys_* calls, the koa_gfx.h drawing functions and the few hooks
 * (guiIsDir, guiAlloc, ...) that KoaOS.ino defines just before including it. A PC test harness stubs those to render it.
 */
#ifndef KOA_GUI_H
#define KOA_GUI_H

#include "koa_gfx.h"

#define GUI_BOOT_FILE "/config/gui"

// window geometry (pixels)
#define GUI_WX 8
#define GUI_WY 18
#define GUI_WW 304
#define GUI_WH 204
#define GUI_CX 14                      // content area: 36 columns x 17 rows of 8x10 pixels
#define GUI_CY 34
#define GUI_COLS 36
#define GUI_ROWS 17

enum { GK_NONE = -1, GK_BS = 8, GK_TAB = 9, GK_ENTER = 13, GK_ESC = 0x100, GK_UP, GK_DOWN, GK_LEFT, GK_RIGHT,
       GK_HOME, GK_END, GK_DEL, GK_PGUP, GK_PGDN };

static int guiLastCR = 0;
static bool guiDead = false;                  // could not get the framebuffer back after running a console program

// ---------------------------------------------------------------- keys
// returns a printable ASCII code, a GK_* code, or GK_NONE on timeout. Arrow keys arrive as ESC [ A..D.
static int guiKey(int ms) {
  int c = sys_getc_t(ms);
  if (c < 0) return GK_NONE;
  if (c == '\n' && guiLastCR) { guiLastCR = 0; return GK_NONE; }      // the \n of a \r\n pair
  guiLastCR = (c == '\r');
  if (c == '\r' || c == '\n') return GK_ENTER;
  if (c == 127) return GK_BS;
  if (c == 27) {
    int d = sys_getc_t(40);
    if (d < 0) return GK_ESC;                                         // a lone Esc
    if (d != '[' && d != 'O') return GK_ESC;
    int e = sys_getc_t(40);
    if (e == 'A') return GK_UP;
    if (e == 'B') return GK_DOWN;
    if (e == 'C') return GK_RIGHT;
    if (e == 'D') return GK_LEFT;
    if (e == 'H') return GK_HOME;
    if (e == 'F') return GK_END;
    if (e >= '1' && e <= '8') {                                       // ESC [ n ~
      sys_getc_t(40);
      if (e == '3') return GK_DEL;
      if (e == '5') return GK_PGUP;
      if (e == '6') return GK_PGDN;
      if (e == '1' || e == '7') return GK_HOME;
      return GK_END;
    }
    return GK_NONE;
  }
  return c;
}

// ---------------------------------------------------------------- chrome
static void guiPad(int x, int y, const char* t, int fg, int bg, int cols) {   // exactly `cols` characters, opaque
  for (int i = 0; i < cols; i++) gfxChar(x + i * 8, y, *t ? *t++ : ' ', fg, bg);
}

static void guiMenubar() {
  char right[24], mid[40] = "", line[64];
  GpsInfo g;
  sys_gpsinfo(&g);
  if (g.timeValid) snprintf(right, sizeof(right), "%02d:%02d UTC", g.h, g.mi);
  else { unsigned long s = sys_uptime() / 1000; snprintf(right, sizeof(right), "up %lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60); }
  char ssid[33], ip[16]; int rssi;
  if (sys_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi)) snprintf(mid, sizeof(mid), "WiFi %s", ssid);
  snprintf(line, sizeof(line), "%-10.10s %-17.17s %11.11s", OSNAME " " OSVER, mid, right);
  guiPad(0, 3, line, C_WHT, C_BLU, 40);
}

static void guiTaskbar(const char* hint) {
  gfxRect(0, 226, GFX_W, 14, C_WHT);
  gfxRect(0, 226, GFX_W, 1, C_BLK);
  guiPad(4, 230, hint, C_BLK, C_WHT, 38);
}

static void guiBox(int x, int y, int w, int h, const char* title) {
  gfxRect(x + 3, y + 3, w, h, C_BLK);
  gfxPanel(x, y, w, h, C_WHT);
  gfxRect(x + 1, y + 1, w - 2, 12, C_BLU);
  gfxText(x + 6, y + 3, title, C_WHT, -1, 1, (w - 12) / 8);
}

static void guiWindow(const char* title, const char* hint) {
  guiBox(GUI_WX, GUI_WY, GUI_WW, GUI_WH, title);
  gfxRect(GUI_WX + 1, GUI_WY + 14, GUI_WW - 2, GUI_WH - 15, C_WHT);
  gfxRect(GUI_WX + 1, GUI_WY + GUI_WH - 14, GUI_WW - 2, 13, C_CYN);
  gfxText(GUI_WX + 6, GUI_WY + GUI_WH - 11, hint, C_BLK, -1, 1, 36);
}

// one 36-column content row; sel = highlighted
static void guiRow(int row, const char* t, bool sel = false, int fgOverride = -1) {
  int y = GUI_CY + row * 10;
  int bg = sel ? C_BLU : C_WHT, fg = sel ? C_WHT : (fgOverride >= 0 ? fgOverride : C_BLK);
  gfxRect(GUI_CX - 2, y - 1, GUI_COLS * 8 + 4, 1, bg);
  gfxRect(GUI_CX - 2, y + 8, GUI_COLS * 8 + 4, 1, bg);
  gfxRect(GUI_CX - 2, y, 2, 8, bg);
  gfxRect(GUI_CX + GUI_COLS * 8, y, 2, 8, bg);
  guiPad(GUI_CX, y, t, fg, bg, GUI_COLS);
}

static void guiBlank(int fromRow, int toRow) { for (int r = fromRow; r <= toRow; r++) guiRow(r, ""); }

// ---------------------------------------------------------------- dialogs
static void guiToast(const char* msg) {                   // shown until something else is drawn
  guiBox(40, 96, 240, 36, "KoaOS");
  guiPad(48, 116, msg, C_BLK, C_WHT, 28);
}

static void guiMsg(const char* msg) {
  guiBox(40, 88, 240, 52, "Message");
  guiPad(48, 108, msg, C_BLK, C_WHT, 28);
  guiPad(48, 122, "Press any key", C_BLU, C_WHT, 28);
  for (;;) if (guiKey(1000) != GK_NONE) break;
}

static int guiAsk(const char* msg) {                      // 'y', 'n', or 0 for Esc / anything else
  guiBox(40, 88, 240, 52, "Question");
  guiPad(48, 108, msg, C_BLK, C_WHT, 28);
  guiPad(48, 122, "y = yes   n = no   Esc = back", C_BLU, C_WHT, 28);
  for (;;) {
    int k = guiKey(1000);
    if (k == 'y' || k == 'Y') return 'y';
    if (k == 'n' || k == 'N') return 'n';
    if (k == GK_ESC) return 0;
  }
}

static bool guiPrompt(const char* title, const char* label, char* buf, int max, bool mask) {
  int len = (int)strlen(buf);
  bool blink = true;
  guiBox(30, 84, 260, 64, title);
  guiPad(38, 102, label, C_BLK, C_WHT, 31);
  guiPad(38, 134, "Enter = OK        Esc = cancel", C_BLU, C_WHT, 31);
  for (;;) {
    char shown[40];
    int start = len > 29 ? len - 29 : 0, n = 0;
    for (int i = start; i < len && n < 29; i++) shown[n++] = mask ? '*' : buf[i];
    if (blink && n < 30) shown[n++] = '_';
    shown[n] = 0;
    gfxRect(36, 113, 248, 14, C_WHT);
    gfxFrame(36, 113, 248, 14, C_BLK);
    guiPad(40, 116, shown, C_BLK, C_WHT, 30);
    int k = guiKey(500);
    if (k == GK_NONE) { blink = !blink; continue; }
    blink = true;
    if (k == GK_ENTER) return true;
    if (k == GK_ESC) return false;
    if (k == GK_BS) { if (len > 0) buf[--len] = 0; continue; }
    if (k >= 32 && k < 127 && len < max - 1) { buf[len++] = (char)k; buf[len] = 0; }
  }
}

// vertical menu; returns the chosen index or -1 for Esc
static int guiMenu(const char* title, const char* const* items, int n) {
  int sel = 0, h = 20 + n * 12;
  int y = 120 - h / 2;
  guiBox(80, y, 160, h, title);
  for (;;) {
    for (int i = 0; i < n; i++) {
      bool s = i == sel;
      gfxRect(84, y + 17 + i * 12, 152, 11, s ? C_BLU : C_WHT);
      guiPad(90, y + 19 + i * 12, items[i], s ? C_WHT : C_BLK, s ? C_BLU : C_WHT, 18);
    }
    int k = guiKey(1000);
    if (k == GK_UP && sel > 0) sel--;
    else if (k == GK_DOWN && sel < n - 1) sel++;
    else if (k == GK_ENTER) return sel;
    else if (k == GK_ESC) return -1;
  }
}

// ---------------------------------------------------------------- file helpers
struct GEnt { char name[30]; int size; bool dir; };
#define GMAXENT 48

static void guiJoin(char* out, int max, const char* dir, const char* leaf) {
  if (!strcmp(dir, "/")) snprintf(out, max, "/%s", leaf);
  else snprintf(out, max, "%s/%s", dir, leaf);
}

static int guiList(const char* dir, GEnt* e, int max) {
  char nm[48]; int sz, n = 0;
  for (int i = 0; n < max && sys_readdir(dir, i, nm, sizeof(nm), &sz) == 1; i++) {
    const char* leaf = strrchr(nm, '/');
    leaf = leaf ? leaf + 1 : nm;
    if (!*leaf) continue;
    strlcpy(e[n].name, leaf, sizeof(e[n].name));
    e[n].size = sz;
    char full[96];
    guiJoin(full, sizeof(full), dir, leaf);
    e[n].dir = guiIsDir(full);
    n++;
  }
  for (int i = 1; i < n; i++) {                           // directories first, then by name
    GEnt t = e[i]; int j = i - 1;
    while (j >= 0 && ((e[j].dir < t.dir) || (e[j].dir == t.dir && strcmp(e[j].name, t.name) > 0))) { e[j + 1] = e[j]; j--; }
    e[j + 1] = t;
  }
  return n;
}

// ---------------------------------------------------------------- Editor
#define ED_MAX 3000                                       // bytes; keeps the whole file in memory

static int edLayout(const char* b, int len, uint16_t* rs, int cols) {     // visual rows: start index of each
  int n = 0, col = 0;
  rs[n++] = 0;
  for (int i = 0; i < len; i++) {
    if (b[i] == '\n') { rs[n++] = (uint16_t)(i + 1); col = 0; continue; }
    if (col >= cols) { rs[n++] = (uint16_t)i; col = 0; }
    col++;
  }
  return n;
}

static int edRowLen(const char* b, int len, const uint16_t* rs, int nrows, int r) {
  int end = (r + 1 < nrows) ? rs[r + 1] : len;
  int l = end - rs[r];
  if (r + 1 < nrows && l > 0 && b[end - 1] == '\n') l--;
  return l;
}

static int edRowOf(const uint16_t* rs, int nrows, int cur) {
  int lo = 0, hi = nrows - 1;
  while (lo < hi) { int m = (lo + hi + 1) / 2; if (rs[m] <= cur) lo = m; else hi = m - 1; }
  return lo;
}

static bool edSave(const char* path, const char* b, int len) {
  int fd = sys_open(path, K_WR);
  if (fd < 0) return false;
  int w = len > 0 ? sys_write(fd, b, len) : 0;
  sys_close(fd);
  return w == len || len == 0;
}

static void guiEdit(const char* path) {
  char* b = (char*)guiAlloc(ED_MAX + 2);
  uint16_t* rs = (uint16_t*)guiAlloc((ED_MAX + 2) * sizeof(uint16_t));
  if (!b || !rs) { if (b) guiFree(b); if (rs) guiFree(rs); guiMsg("Out of memory"); return; }
  int len = 0;
  bool ro = false;
  int fd = sys_open(path, K_RD);
  if (fd >= 0) {
    int n;
    while (len < ED_MAX && (n = sys_read(fd, b + len, ED_MAX - len)) > 0) len += n;
    char extra;
    bool big = len >= ED_MAX && sys_read(fd, &extra, 1) > 0;
    sys_close(fd);
    if (big) { guiFree(b); guiFree(rs); guiMsg("File too big for the editor"); return; }
    int o = 0;                                            // drop \r, mark binary files read-only
    for (int i = 0; i < len; i++) {
      char c = b[i];
      if (c == '\r') continue;
      if (c == '\t') c = ' ';
      else if (c != '\n' && (c < 32 || c > 126)) { c = '?'; ro = true; }
      b[o++] = c;
    }
    len = o;
  }
  if (ro) guiMsg("Binary file: read-only");
  int cur = 0, top = 0, nrows = 1, wantCol = -1;
  bool dirty = false, blink = true, full = true;
  char name[24];
  const char* leaf = strrchr(path, '/');
  strlcpy(name, leaf ? leaf + 1 : path, sizeof(name));
  char title[40];
  snprintf(title, sizeof(title), "Editor - %s", name);
  for (;;) {
    if (full) { guiWindow(title, "^S save  Esc close  arrows move"); full = false; }
    nrows = edLayout(b, len, rs, GUI_COLS);
    int cr = edRowOf(rs, nrows, cur), col = cur - rs[cr];
    if (cr < top) top = cr;
    if (cr >= top + 16) top = cr - 15;
    for (int v = 0; v < 16; v++) {
      int r = top + v;
      char line[GUI_COLS + 1];
      if (r < nrows) {
        int l = edRowLen(b, len, rs, nrows, r);
        if (l > GUI_COLS) l = GUI_COLS;
        memcpy(line, b + rs[r], l);
        line[l] = 0;
      } else line[0] = 0;
      guiRow(v, line);
    }
    if (blink) {
      int cx = GUI_CX + (col > GUI_COLS ? GUI_COLS : col) * 8, cy = GUI_CY + (cr - top) * 10;
      char under = (cur < len && b[cur] != '\n') ? b[cur] : ' ';
      gfxChar(cx, cy, under, C_WHT, C_BLK);
    }
    char st[40];
    snprintf(st, sizeof(st), "Ln %d Col %d  %s%s", cr + 1, col + 1, ro ? "READ-ONLY " : "", dirty ? "modified" : "");
    guiRow(16, st, false, C_BLU);

    int k = guiKey(500);
    if (k == GK_NONE) { blink = !blink; continue; }
    blink = true;
    int keepCol = (k == GK_UP || k == GK_DOWN || k == GK_PGUP || k == GK_PGDN);
    if (!keepCol) wantCol = -1;
    if (k == GK_ESC) {
      if (dirty) {
        int a = guiAsk("Save changes?");
        if (a == 'y') { if (!edSave(path, b, len)) { guiMsg("Save failed"); full = true; continue; } }
        else if (a != 'n') { full = true; continue; }
      }
      break;
    } else if (k == 19) {                                 // Ctrl-S
      if (ro) guiMsg("Read-only file");
      else if (edSave(path, b, len)) dirty = false;
      else guiMsg("Save failed");
      full = true;
    } else if (k == GK_LEFT) { if (cur > 0) cur--; }
    else if (k == GK_RIGHT) { if (cur < len) cur++; }
    else if (k == GK_HOME) cur = rs[cr];
    else if (k == GK_END) cur = rs[cr] + edRowLen(b, len, rs, nrows, cr);
    else if (keepCol) {
      int step = (k == GK_UP) ? -1 : (k == GK_DOWN) ? 1 : (k == GK_PGUP) ? -15 : 15;
      int tr = cr + step;
      if (tr < 0) tr = 0;
      if (tr > nrows - 1) tr = nrows - 1;
      if (wantCol < 0) wantCol = col;
      int rl = edRowLen(b, len, rs, nrows, tr);
      cur = rs[tr] + (wantCol < rl ? wantCol : rl);
    } else if (!ro) {
      if (k == GK_BS && cur > 0) { memmove(b + cur - 1, b + cur, len - cur); cur--; len--; dirty = true; }
      else if (k == GK_DEL && cur < len) { memmove(b + cur, b + cur + 1, len - cur - 1); len--; dirty = true; }
      else if ((k == GK_ENTER || k == GK_TAB || (k >= 32 && k < 127)) && len < ED_MAX - 2) {
        const char* ins = k == GK_ENTER ? "\n" : k == GK_TAB ? "  " : NULL;
        char one[2] = { (char)k, 0 };
        if (!ins) ins = one;
        int il = (int)strlen(ins);
        memmove(b + cur + il, b + cur, len - cur);
        memcpy(b + cur, ins, il);
        cur += il; len += il; dirty = true;
      }
    }
  }
  guiFree(b);
  guiFree(rs);
}

// ---------------------------------------------------------------- Files
static void guiFiles() {
  GEnt ents[GMAXENT];
  char path[96] = "/";
  int n = 0, sel = 0, top = 0;
  bool reload = true, full = true;
  for (;;) {
    if (full) { guiWindow("Files", "Enter open  e edit  n new  d del"); full = false; }
    if (reload) { n = guiList(path, ents, GMAXENT); reload = false; if (sel >= n) sel = n ? n - 1 : 0; }
    if (sel < top) top = sel;
    if (sel >= top + 15) top = sel - 14;
    char l[48];
    snprintf(l, sizeof(l), "%.44s", path);
    guiRow(0, l, false, C_BLU);
    for (int v = 0; v < 15; v++) {
      int i = top + v;
      if (i < n) {
        char sz[12];
        if (ents[i].dir) strcpy(sz, "<dir>"); else snprintf(sz, sizeof(sz), "%d", ents[i].size);
        snprintf(l, sizeof(l), "%-26.26s %8s", ents[i].name, sz);
        guiRow(1 + v, l, i == sel);
      } else guiRow(1 + v, n == 0 && v == 0 ? "(empty)" : "");
    }
    snprintf(l, sizeof(l), "%d item%s     Left/Backspace = up", n, n == 1 ? "" : "s");
    guiRow(16, l, false, C_BLU);

    int k = guiKey(1000);
    if (k == GK_NONE) continue;
    if (k == GK_ESC) return;
    if (k == GK_UP && sel > 0) sel--;
    else if (k == GK_DOWN && sel < n - 1) sel++;
    else if (k == GK_PGUP) sel = sel > 14 ? sel - 14 : 0;
    else if (k == GK_PGDN) sel = sel + 14 < n ? sel + 14 : (n ? n - 1 : 0);
    else if ((k == GK_LEFT || k == GK_BS) && strcmp(path, "/")) {
      char* s = strrchr(path, '/');
      if (s == path) path[1] = 0; else *s = 0;
      sel = 0; top = 0; reload = true;
    } else if ((k == GK_ENTER || k == GK_RIGHT || k == 'e') && n > 0) {
      char full_[96];
      guiJoin(full_, sizeof(full_), path, ents[sel].name);
      if (ents[sel].dir) { if (k != 'e') { strlcpy(path, full_, sizeof(path)); sel = 0; top = 0; reload = true; } }
      else { guiEdit(full_); full = true; reload = true; }
    } else if (k == 'n') {
      char nm[24] = "";
      if (guiPrompt("New file", "File name:", nm, sizeof(nm), false) && nm[0] && !strchr(nm, '/')) {
        char full_[96];
        guiJoin(full_, sizeof(full_), path, nm);
        guiEdit(full_);
      }
      full = true; reload = true;
    } else if (k == 'd' && n > 0 && !ents[sel].dir) {
      char q[48], full_[96];
      snprintf(q, sizeof(q), "Delete %.20s?", ents[sel].name);
      if (guiAsk(q) == 'y') {
        guiJoin(full_, sizeof(full_), path, ents[sel].name);
        if (sys_unlink(full_) != 0) guiMsg("Delete failed");
      }
      full = true; reload = true;
    }
  }
}

// ---------------------------------------------------------------- Wi-Fi
struct GNet { char ssid[33]; int rssi; int open; };
#define GMAXNET 14

static void guiWifi() {
  GNet nets[GMAXNET];
  int n = 0, sel = 0;
  bool scan = true, full = true;
  for (;;) {
    if (full) { guiWindow("Wi-Fi", "Enter connect r scan d disconnect"); full = false; }
    if (scan) {
      guiBlank(0, 16);
      guiRow(0, "Scanning...", false, C_BLU);
      int c = sys_wifi_scan();
      n = 0;
      for (int i = 0; c > 0 && n < GMAXNET && sys_wifi_entry(i, nets[n].ssid, sizeof(nets[n].ssid), &nets[n].rssi, &nets[n].open) == 1; i++) n++;
      sel = 0; scan = false;
      if (c < 0) guiMsg("Scan failed");
      full = true;
      continue;
    }
    char l[48], ssid[33], ip[16]; int rssi;
    if (sys_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi)) {
      snprintf(l, sizeof(l), "Connected: %.22s", ssid); guiRow(0, l, false, C_BLK);
      snprintf(l, sizeof(l), "%s   %d dBm", ip, rssi); guiRow(1, l, false, C_BLU);
    } else { guiRow(0, "Not connected", false, C_RED); guiRow(1, ""); }
    guiRow(2, "");
    for (int v = 0; v < 13; v++) {
      if (v < n) {
        snprintf(l, sizeof(l), "%-24.24s %4d dBm %c", nets[v].ssid[0] ? nets[v].ssid : "(hidden)", nets[v].rssi, nets[v].open ? 'o' : ' ');
        guiRow(3 + v, l, v == sel);
      } else guiRow(3 + v, n == 0 && v == 0 ? "No networks found - press r" : "");
    }
    guiRow(16, "o = open network (no password)", false, C_BLU);

    int k = guiKey(1500);
    if (k == GK_ESC) return;
    if (k == GK_UP && sel > 0) sel--;
    else if (k == GK_DOWN && sel < n - 1) sel++;
    else if (k == 'r') scan = true;
    else if (k == 'd') { sys_wifi_off(); }
    else if (k == GK_ENTER && n > 0) {
      char pass[64] = "";
      bool go = true;
      if (!nets[sel].open) { go = guiPrompt("Wi-Fi password", nets[sel].ssid, pass, sizeof(pass), true); full = true; }
      if (go) {
        guiWindow("Wi-Fi", "Enter connect r scan d disconnect");
        guiToast("Connecting...");
        int rc = sys_wifi_connect(nets[sel].ssid, pass, 15000);
        guiMsg(rc == 0 ? "Connected" : rc == -2 ? "Wrong password?" : "Timed out");
        full = true;
      }
    }
  }
}

// ---------------------------------------------------------------- GPS
static void guiGps() {
  resAcquire(KC_GPS);                                     // powers it on; released again when we leave
  guiWindow("GPS", "Esc: close (GPS powers off)");
  for (;;) {
    GpsInfo g;
    sys_gpsinfo(&g);
    char l[48];
    snprintf(l, sizeof(l), "%3.0f", g.mph);
    gfxText(GUI_CX + 4, GUI_CY + 6, l, C_BLK, C_WHT, 4);                    // rows 0-3: big speed
    gfxText(GUI_CX + 4 + 13 * 8, GUI_CY + 26, "mph", C_BLU, C_WHT);
    guiBlank(4, 4);
    snprintf(l, sizeof(l), "%s   satellites: %d", !g.on ? "GPS off" : g.locValid ? "FIX" : "searching...", g.sats);
    guiRow(5, l, false, g.locValid ? C_BLK : C_RED);
    for (int i = 0; i < 12; i++) {                                           // row 6: one bar per satellite
      gfxRect(GUI_CX + i * 12, GUI_CY + 60, 10, 10, C_WHT);
      gfxRect(GUI_CX + i * 12, GUI_CY + (i < g.sats ? 62 : 67), 10, i < g.sats ? 8 : 3, i < g.sats ? C_GRN : C_BLK);
    }
    guiBlank(7, 7);
    if (g.locValid) {
      snprintf(l, sizeof(l), "Lat  %.6f", g.lat); guiRow(8, l);
      snprintf(l, sizeof(l), "Lon  %.6f", g.lng); guiRow(9, l);
      snprintf(l, sizeof(l), "Alt  %.0f m", g.alt); guiRow(10, l);
      snprintf(l, sizeof(l), "Crs  %.0f deg", g.crs); guiRow(11, l);
    } else { guiRow(8, "Lat  --"); guiRow(9, "Lon  --"); guiRow(10, "Alt  --"); guiRow(11, "Crs  --"); }
    if (g.timeValid) snprintf(l, sizeof(l), "UTC  %04d-%02d-%02d %02d:%02d", g.y, g.mo, g.d, g.h, g.mi);
    else snprintf(l, sizeof(l), "UTC  --");
    guiRow(12, l);
    guiBlank(13, 16);
    if (guiKey(500) == GK_ESC) break;
  }
  resRelease(KC_GPS);
}

// ---------------------------------------------------------------- System
static void guiSystem() {
  guiWindow("System", "Esc: close");
  for (;;) {
    char l[64], ssid[33], ip[16]; int rssi;
    unsigned long fr, tot, fu, ft, s = sys_uptime() / 1000, pf, pt;
    sys_meminfo(&fr, &tot);
    sys_fsinfo(&fu, &ft);
    guiPsram(&pf, &pt);
    GpsInfo g; sys_gpsinfo(&g);
    snprintf(l, sizeof(l), "%s %s  ESP32-S3", OSNAME, OSVER); guiRow(0, l, false, C_BLU);
    snprintf(l, sizeof(l), "Uptime  %lud %02lu:%02lu:%02lu", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60); guiRow(1, l);
    snprintf(l, sizeof(l), "RAM     %lu / %lu KB free", fr / 1024, tot / 1024); guiRow(2, l);
    snprintf(l, sizeof(l), "PSRAM   %lu / %lu KB free", pf / 1024, pt / 1024); guiRow(3, l);
    snprintf(l, sizeof(l), "Storage %lu / %lu KB used", fu / 1024, ft / 1024); guiRow(4, l);
    if (sys_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi)) snprintf(l, sizeof(l), "Wi-Fi   %.12s %s", ssid, ip);
    else snprintf(l, sizeof(l), "Wi-Fi   not connected");
    guiRow(5, l);
    snprintf(l, sizeof(l), "GPS     %s", !g.on ? "off" : g.locValid ? "fix" : "no fix"); guiRow(6, l);
    guiRow(7, "Video   VGA 640x480, GUI 320x240");
    guiRow(8, "");
    guiRow(9, "PID  NAME          FLAGS", false, C_BLU);
    int row = 10;
    for (int i = 0; i < MAXPROC && row < 17; i++) {
      if (!procs[i].used) continue;
      snprintf(l, sizeof(l), "%-4d %-13.13s %s%s", procs[i].pid, procs[i].name, procs[i].pid == fgpid ? "fg " : "", (procs[i].flags & PF_WASM) ? "wasm" : "");
      guiRow(row++, l);
    }
    guiBlank(row, 16);
    if (guiKey(1000) == GK_ESC) break;
  }
}

// ---------------------------------------------------------------- run a console program, then come back
static void guiLeaveGfx() { vgaGfxEnd(); sys_ttyctl(0); }
static bool guiEnterGfx();

static void guiExternal(const char* prog, const char* args) {
  guiLeaveGfx();
  pr("\x1b[2J\x1b[H");
  int pid = sys_spawn(prog, args, -1, false);
  if (pid < 0) prf("cannot run %s\n", prog);
  else sys_wait(pid);
  if (strcmp(prog, "sh")) {
    pr("\n[press Enter to return to the desktop]");
    char line[8];
    readline(line, sizeof(line));
  }
  if (!guiEnterGfx()) { pr("gui: out of memory, staying in the console\n"); guiDead = true; }
}

// ---------------------------------------------------------------- Apps
struct GApp { char name[26]; char info[24]; };
#define GMAXAPP 24

static int guiAppList(GApp* a, int max) {
  char nm[48]; int sz, n = 0;
  for (int i = 0; n < max && sys_readdir("/apps", i, nm, sizeof(nm), &sz) == 1; i++) {
    const char* leaf = strrchr(nm, '/');
    leaf = leaf ? leaf + 1 : nm;
    if (!*leaf || leaf[0] == '.') continue;
    strlcpy(a[n].name, leaf, sizeof(a[n].name));
    char full[64];
    guiJoin(full, sizeof(full), "/apps", leaf);
    if (!guiIsDir(full)) strcpy(a[n].info, "script");
    else {
      esp_manifest_t m;
      if (pkgLoadManifest(leaf, &m) == 0) snprintf(a[n].info, sizeof(a[n].info), "%s %s", m.version, m.type);
      else strcpy(a[n].info, "damaged");
    }
    n++;
  }
  return n;
}

static void guiApps() {
  GApp apps[GMAXAPP];
  int n = guiAppList(apps, GMAXAPP), sel = 0;
  bool full = true;
  for (;;) {
    if (full) { guiWindow("Apps", "Enter run  Esc close"); full = false; }
    char l[48];
    guiRow(0, "Installed apps", false, C_BLU);
    for (int v = 0; v < 15; v++) {
      if (v < n) { snprintf(l, sizeof(l), "%-16.16s %-18.18s", apps[v].name, apps[v].info); guiRow(1 + v, l, v == sel); }
      else guiRow(1 + v, n == 0 && v == 0 ? "No apps installed yet" : "");
    }
    guiRow(16, "Runs on the console, returns here", false, C_BLU);
    int k = guiKey(1000);
    if (k == GK_ESC) return;
    if (k == GK_UP && sel > 0) sel--;
    else if (k == GK_DOWN && sel < n - 1) sel++;
    else if (k == GK_ENTER && n > 0) { guiExternal(apps[sel].name, ""); return; }
  }
}

// ---------------------------------------------------------------- desktop
struct GTile { const char* label; int id; };
static const GTile guiTiles[8] = {
  { "Files", 0 }, { "Wi-Fi", 1 }, { "GPS", 2 }, { "System", 3 },
  { "Apps", 4 }, { "Terminal", 5 }, { "Editor", 6 }, { "Power", 7 },
};

static void guiIcon(int id, int x, int y) {               // drawn in a 40x32 box at x,y
  switch (id) {
    case 0:  // folder
      gfxRect(x + 4, y + 4, 14, 6, C_YEL); gfxFrame(x + 4, y + 4, 14, 6, C_BLK);
      gfxRect(x + 4, y + 9, 32, 21, C_YEL); gfxFrame(x + 4, y + 9, 32, 21, C_BLK); break;
    case 1:  // wifi: rising bars
      for (int i = 0; i < 4; i++) { gfxRect(x + 6 + i * 8, y + 26 - i * 7, 6, 4 + i * 7, C_GRN); gfxFrame(x + 6 + i * 8, y + 26 - i * 7, 6, 4 + i * 7, C_BLK); } break;
    case 2:  // gps: target
      gfxCircle(x + 20, y + 16, 13, C_RED, false); gfxCircle(x + 20, y + 16, 4, C_RED, true);
      gfxRect(x + 19, y + 0, 2, 32, C_RED); gfxRect(x + 4, y + 15, 32, 2, C_RED); break;
    case 3:  // system: chip
      gfxRect(x + 9, y + 6, 22, 20, C_BLK); gfxRect(x + 13, y + 10, 14, 12, C_GRN);
      for (int i = 0; i < 4; i++) { gfxRect(x + 5, y + 8 + i * 5, 4, 2, C_BLK); gfxRect(x + 31, y + 8 + i * 5, 4, 2, C_BLK); } break;
    case 4:  // apps: 2x2 squares
      gfxRect(x + 6, y + 3, 13, 12, C_RED); gfxRect(x + 21, y + 3, 13, 12, C_GRN);
      gfxRect(x + 6, y + 17, 13, 12, C_BLU); gfxRect(x + 21, y + 17, 13, 12, C_YEL); break;
    case 5:  // terminal
      gfxRect(x + 3, y + 3, 34, 26, C_BLK); gfxText(x + 7, y + 12, ">_", C_GRN); break;
    case 6:  // editor: page with lines
      gfxRect(x + 9, y + 1, 22, 30, C_WHT); gfxFrame(x + 9, y + 1, 22, 30, C_BLK);
      for (int i = 0; i < 5; i++) gfxRect(x + 13, y + 7 + i * 5, i == 4 ? 8 : 14, 2, C_BLK);
      break;
    case 7:  // power
      gfxCircle(x + 20, y + 17, 12, C_RED, false); gfxRect(x + 19, y + 1, 3, 14, C_CYN); gfxRect(x + 19, y + 3, 3, 12, C_RED);
      break;
  }
}

static void guiTile(int i, bool sel) {
  int col = i % 4, row = i / 4;
  int x = 8 + col * 78, y = 26 + row * 98;
  gfxRect(x, y, 70, 88, sel ? C_BLU : C_BLK);
  gfxRect(x + 2, y + 2, 66, 84, C_WHT);
  gfxRect(x + 3, y + 3, 64, 62, C_CYN);
  guiIcon(guiTiles[i].id, x + 15, y + 18);
  gfxRect(x + 2, y + 67, 66, 19, sel ? C_BLU : C_WHT);
  int len = (int)strlen(guiTiles[i].label);
  gfxText(x + 35 - len * 4, y + 73, guiTiles[i].label, sel ? C_WHT : C_BLK, -1);
}

static void guiDesktop(int sel) {
  gfxFill(C_CYN);
  gfxRect(0, 0, GFX_W, 14, C_BLU);
  guiMenubar();
  for (int i = 0; i < 8; i++) guiTile(i, i == sel);
  guiTaskbar("Arrows move   Enter open   Esc power");
}

static bool guiEnterGfx() {
  if (!vgaGfxBegin()) return false;
  sys_ttyctl(TTY_RAW | TTY_NOSIG | TTY_NOSCREEN | TTY_GFX);
  guiOled();
  return true;
}

static void guiOpen(int idx) {
  switch (idx) {
    case 0: guiFiles(); break;
    case 1: guiWifi(); break;
    case 2: guiGps(); break;
    case 3: guiSystem(); break;
    case 4: guiApps(); break;
    case 5: guiExternal("sh", ""); break;
    case 6: {
      char nm[24] = "notes.txt";
      if (guiPrompt("Editor", "Open or create file in /:", nm, sizeof(nm), false) && nm[0] && !strchr(nm, '/')) {
        char p[40]; snprintf(p, sizeof(p), "/%s", nm); guiEdit(p);
      }
      break;
    }
    case 7: break;                                        // handled by the caller
  }
}

// returns true when the user chose to leave the GUI
static bool guiPower() {
  static const char* const items[] = { "Reboot", "Exit to console", "Cancel" };
  int r = guiMenu("Power", items, 3);
  if (r == 0) { guiToast("Rebooting..."); sys_sleep(400); sys_reboot(); }
  return r == 1;
}

bool guiBootFlag() { int fd = sys_open(GUI_BOOT_FILE, K_RD); if (fd < 0) return false; sys_close(fd); return true; }
static void guiBootSet(bool on) {
  if (on) { int fd = sys_open(GUI_BOOT_FILE, K_WR); if (fd >= 0) { sys_write(fd, "1", 1); sys_close(fd); } }
  else sys_unlink(GUI_BOOT_FILE);
}

static int guiRun() {
  int sel = 0;
  bool full = true;
  unsigned long lastBar = 0;
  for (;;) {
    if (guiDead) return 1;
    if (full) { guiDesktop(sel); full = false; }
    if (sys_uptime() - lastBar > 900) { lastBar = sys_uptime(); guiMenubar(); }
    int k = guiKey(500);
    int old = sel;
    if (k == 3) return 0;                                 // Ctrl-C: out
    else if (k == GK_LEFT && sel % 4 > 0) sel--;
    else if (k == GK_RIGHT && sel % 4 < 3) sel++;
    else if (k == GK_UP && sel >= 4) sel -= 4;
    else if (k == GK_DOWN && sel < 4) sel += 4;
    else if (k == GK_TAB) sel = (sel + 1) % 8;
    else if (k == GK_ESC) { if (guiPower()) return 0; full = true; }
    else if (k == GK_ENTER) {
      if (sel == 7) { if (guiPower()) return 0; }
      else guiOpen(sel);
      full = true;
    }
    if (sel != old) { guiTile(old, false); guiTile(sel, true); }
  }
}

int p_gui(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "boot")) {
    if (argc > 2 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
      guiBootSet(!strcmp(argv[2], "on"));
      prf("gui will %sstart at boot\n", !strcmp(argv[2], "on") ? "" : "not ");
    } else prf("gui at boot: %s   (gui boot on|off)\n", guiBootFlag() ? "on" : "off");
    return 0;
  }
  if (argc > 1) { pr("usage: gui | gui boot on|off\n"); return 1; }
  if (guiPid) { pr("gui: already running\n"); return 1; }
  bool ownVga = false;
  if (!vgaActive()) {
    const char* who = "?";
    int r = vgaEnable(&who);
    if (r == KE_BUSY) { prf("gui: vga pins busy (held by %s, see: pins)\n", who); return 1; }
    if (r != KE_OK) { pr("gui: could not start the video driver\n"); return 1; }
    ownVga = true;
  }
  guiPid = sys_getpid();
  if (!guiEnterGfx()) {
    guiPid = 0;
    if (ownVga) vgaDisable();
    pr("gui: not enough internal RAM for the 38 KB framebuffer (turn Wi-Fi off and retry)\n");
    return 1;
  }
  guiRun();
  vgaGfxEnd();
  sys_ttyctl(0);
  guiPid = 0;
  if (ownVga) vgaDisable();
  pr("\x1b[2J\x1b[H");
  return 0;
}

#endif
