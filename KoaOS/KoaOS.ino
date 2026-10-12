#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>
#include <vector>
#include <TinyGPSPlus.h>
#include <U8g2lib.h>
#include <HWCDC.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <RadioLib.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <wasm3.h>          // "Wasm3" library (Library Manager). Needs a one-time allocator patch: see WASM RUNTIME section
#include "koaos_abi.h"   // shared with the SDK: error codes, KC_* capabilities, API structs
#include "esp_pkg.h"        // .esp package format: parser, manifest, sha-256 (portable, unit-tested on a PC)
#include "koa_io.h"         // pin allow-list + pin ownership table for app GPIO/UART/I2C/SPI (portable, unit-tested on a PC)
#include <driver/gpio.h>   // gpio_set_level: preset an output latch before the pin becomes an output

HWCDC USBSerial;
#define Serial USBSerial

#define OSNAME       "KoaOS"
#define HOSTNAME     "koa"   // prompt/login/neofetch hostname
#define OSVER        "0.10"
#define LOGIN_USER   "root"
#define LOGIN_PASS   "1234"
#define VEXT_PIN     36
#define OLED_SDA     17
#define OLED_SCL     18
#define OLED_RST     21
#define GNSS_PWR     34   // LOW = on, HIGH = off
#define GNSS_RX_PIN  39
#define GNSS_TX_PIN  38
#define GNSS_BAUD    9600

#define COLS 25
#define ROWS 8
#define EROWS 7
#define MAXPROC 8
#define MAXFD 8

#define PF_NOKILL 1
#define PF_BG     2
#define PF_BIGSTACK 4   // network programs need a bigger task stack
#define PF_WASM     8   // wasm app: runs at idle priority, memory comes from PSRAM only

#define WASM_APP_MEM        (1024u * 1024u)   // per-app PSRAM quota (linear memory + wasm stack + code + the .wasm file)
#define WASM_PSRAM_RESERVE  (128u * 1024u)    // PSRAM that is never handed to apps
#define WASM_RT_STACK       (12 * 1024)       // wasm3 value stack, allocated in PSRAM
#define WASM_MAX_BIN        (256u * 1024u)    // biggest .wasm accepted
#define WASM_TASK_STACK     49152             // native stack of the interpreter task (internal RAM). wasm3 has no tail calls on Xtensa, so stack use grows with the longest straight-line run of wasm ops (meshcore: ~520)

#define TTY_RAW      1   // no line discipline
#define TTY_NOECHO   2
#define TTY_NOSIG    4   // Ctrl-C is just a byte
#define TTY_NOSCREEN 8   // output goes to USB only, no CRLF conversion
#define TTY_GFX      16  // program draws the OLED itself

#define K_RD  0
#define K_WR  1
#define K_APP 2

// Heltec V4 LoRa pins (from the V4 pinout)
#define LR_NSS    8
#define LR_SCK    9
#define LR_MOSI   10
#define LR_MISO   11
#define LR_RST    12
#define LR_BUSY   13
#define LR_DIO1   14
// V4 front-end / power amplifier control
#define LR_FEM_PWR 7    // VFEM_Ctrl: front-end power
#define LR_FEM_EN  2    // FEM_EN:    front-end enable
#define LR_FEM_PA  46   // FEM_PA:    high = PA path (transmit)

enum { FD_NONE, FD_TTY, FD_FILE, FD_NULL, FD_GPS };
enum { P_RUN = 1, P_ZOMBIE = 2 };

U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);
TinyGPSPlus gps;
HardwareSerial GPSSerial(1);

bool gpsOn = false;
volatile int gpsReaders = 0;
uint8_t gpsRing[512];
volatile uint32_t gWritten = 0;          // total bytes ever put in gpsRing; each /dev/gps reader tracks its own position
SemaphoreHandle_t gpsMx = NULL;          // guards gps (TinyGPS) + UART + gpsOn; created in setup()
SemaphoreHandle_t resMx = NULL;          // guards resource ownership; created in setup()
int  gpsRefs = 0;                        // processes holding KC_GPS
bool gpsSticky = false;                  // "gps start" pins power on regardless of holders

struct GpsInfo {
  bool on, locValid, timeValid;
  int sats, y, mo, d, h, mi;
  double lat, lng, mph, alt, crs;
};

struct FD {
  uint8_t type = FD_NONE;
  File f;
  uint32_t gpos = 0;     // FD_GPS: read position in the ring
};

struct Proc {
  bool used = false;
  int pid = 0, ppid = 0;
  char name[25];          // validAppName() allows up to 24 chars
  volatile int state = 0;
  volatile bool killed = false;
  int exitcode = 0;
  int flags = 0;
  TaskHandle_t task = NULL;
  FD fd[MAXFD];
  char args[192];
  int (*entry)(int, char**) = NULL;
  uint32_t allowed = 0xFFFFFFFFu;   // KC_* capabilities this process may acquire (apps: from manifest)
  uint32_t held = 0;                // KC_* capabilities it currently holds
  void* wctx = NULL;                // WasmCtx for wasm apps
  volatile bool inHost = false;     // wasm app is inside an OS call
  unsigned long killTick = 0;       // when a kill was first noticed (0 = none)
  uint8_t reapStrikes = 0;
};

struct Prog {
  const char* name;
  int (*fn)(int, char**);
  int flags;
};

const Prog* findProg(const char* n);
int p_run(int, char**);   // script runner, used by sys_spawn
int  resAcquire(uint32_t cap);
int  resRelease(uint32_t cap);
void resReleaseAll(struct Proc* p);
void wasmTeardown(struct Proc* p);
void wasmReaper();
bool wasmAvailable();
void ipcDropAll(int pid);
int  p_wasm(int, char**);

Proc procs[MAXPROC];
int nextPid = 1;
volatile int fgpid = 0;

// ================= TTY =================
char scr[ROWS][COLS + 1];
int curRow = 0, curCol = 0;
volatile bool dirty = true, blinkOn = true;
unsigned long lastDraw = 0, lastBlink = 0;

SemaphoreHandle_t ttyMx;
QueueHandle_t ttyIn;
volatile int ttyFlags = 0;
int oesc = 0, onum = 0;
char lbuf[96];
int llen = 0;
bool lcr = false;

void scrNewLine() {
  curCol = 0;
  if (curRow < ROWS - 1) curRow++;
  else {
    for (int r = 1; r < ROWS; r++) memcpy(scr[r - 1], scr[r], COLS + 1);
    memset(scr[ROWS - 1], 0, COLS + 1);
  }
  dirty = true;
}

void scrPut(char c) {
  if (oesc == 1) { if (c == '[') { oesc = 2; onum = 0; } else oesc = 0; return; }
  if (oesc == 2) {
    if (c >= '0' && c <= '9') { onum = onum * 10 + (c - '0'); return; }
    oesc = 0;
    if (c == 'J') { memset(scr, 0, sizeof(scr)); curRow = 0; curCol = 0; }
    else if (c == 'H') { curRow = 0; curCol = 0; }
    dirty = true;
    return;
  }
  if (c == 27) { oesc = 1; return; }
  if (c == '\r') return;
  if (c == '\n') { scrNewLine(); return; }
  if (c < 32) return;
  if (curCol >= COLS) scrNewLine();
  scr[curRow][curCol++] = c;
  dirty = true;
}

void ttyWrite(const char* b, int n) {
  xSemaphoreTake(ttyMx, portMAX_DELAY);
  bool noscr = ttyFlags & TTY_NOSCREEN;
  char tmp[128];
  int t = 0;
  for (int i = 0; i < n; i++) {
    if (b[i] == '\n' && !noscr) tmp[t++] = '\r';
    tmp[t++] = b[i];
    if (t >= 125) { Serial.write((const uint8_t*)tmp, t); t = 0; }
  }
  if (t) Serial.write((const uint8_t*)tmp, t);
  if (!noscr) for (int i = 0; i < n; i++) scrPut(b[i]);
  xSemaphoreGive(ttyMx);
}

void ttyEchoBackspace() {
  xSemaphoreTake(ttyMx, portMAX_DELAY);
  Serial.print("\b \b");
  if (curCol > 0) { curCol--; scr[curRow][curCol] = 0; dirty = true; }
  xSemaphoreGive(ttyMx);
}

void ttyCtl(int flags) {
  xSemaphoreTake(ttyMx, portMAX_DELAY);
  ttyFlags = flags;
  llen = 0;
  dirty = true;
  xSemaphoreGive(ttyMx);
}

void ttyReset() {
  xSemaphoreTake(ttyMx, portMAX_DELAY);
  ttyFlags = 0;
  llen = 0;
  oesc = 0;
  xQueueReset(ttyIn);
  dirty = true;
  xSemaphoreGive(ttyMx);
}

Proc* procByPid(int pid) {
  for (int i = 0; i < MAXPROC; i++) if (procs[i].used && procs[i].pid == pid) return &procs[i];
  return NULL;
}

Proc* procSelf() {
  TaskHandle_t h = xTaskGetCurrentTaskHandle();
  for (int i = 0; i < MAXPROC; i++) if (procs[i].used && procs[i].task == h) return &procs[i];
  return NULL;
}

void ttySignal() {
  Proc* p = procByPid(fgpid);
  if (!p) return;
  ttyWrite("^C\n", 3);
  llen = 0;
  if (p->flags & PF_NOKILL) {
    char n = '\n';
    xQueueSend(ttyIn, &n, 0);   // shell just shows a fresh prompt
  } else {
    p->killed = true;
  }
}

// called from loop() for every byte typed on USB
void ttyInputChar(char c) {
  if (ttyFlags & TTY_RAW) {
    if (c == 3 && !(ttyFlags & TTY_NOSIG)) { ttySignal(); return; }
    xQueueSend(ttyIn, &c, 0);
    return;
  }
  if (c == 3) { ttySignal(); return; }
  bool echo = !(ttyFlags & TTY_NOECHO);
  if (c == '\r' || c == '\n') {
    if (c == '\n' && lcr) { lcr = false; return; }
    lcr = (c == '\r');
    if (echo) ttyWrite("\n", 1);
    for (int i = 0; i < llen; i++) xQueueSend(ttyIn, &lbuf[i], 0);
    char n = '\n';
    xQueueSend(ttyIn, &n, 0);
    llen = 0;
    return;
  }
  lcr = false;
  if (c == 8 || c == 127) {
    if (llen > 0) { llen--; if (echo) ttyEchoBackspace(); }
    return;
  }
  if (c >= 32 && c < 127 && llen < (int)sizeof(lbuf) - 1) {
    lbuf[llen++] = c;
    if (echo) ttyWrite(&c, 1);
  }
}

void drawTerm() {
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tr);
  for (int r = 0; r < ROWS; r++) display.drawStr(0, r * 8 + 7, scr[r]);
  if (blinkOn) display.drawBox(curCol * 5, curRow * 8, 4, 7);
  display.sendBuffer();
}

void kprintf(const char* fmt, ...) {
  char b[128];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  ttyWrite(b, strlen(b));
}

// ================= PROCESSES =================
void fdRelease(FD& d, bool doClose) {
  if (d.type == FD_GPS && doClose && gpsReaders > 0) gpsReaders--;
  if (d.type == FD_FILE && doClose && d.f) d.f.close();
  d.f = File();
  d.type = FD_NONE;
}

void procFinish(Proc* p, int rc) {
  wasmTeardown(p);                  // wasm app memory goes back to PSRAM first
  ipcDropAll(p->pid);
  for (int i = 0; i < MAXFD; i++) fdRelease(p->fd[i], true);
  resReleaseAll(p);                 // radio, gps, display: freed however the process ended
  if (fgpid == p->pid) { fgpid = p->ppid; ttyReset(); }
  if (p->flags & PF_BG) {
    kprintf("[%d] done\n", p->pid);
    p->used = false;
  } else {
    p->exitcode = rc;
    p->state = P_ZOMBIE;
  }
}

void procExit(Proc* p, int rc) {
  procFinish(p, rc);
  vTaskDelete(NULL);
}

int tokenize(char* s, char** argv, int maxn) {
  int n = 0;
  while (*s && n < maxn) {
    while (*s == ' ') s++;
    if (!*s) break;
    if (*s == '"') {
      s++;
      argv[n++] = s;
      while (*s && *s != '"') s++;
      if (*s) *s++ = 0;
    } else {
      argv[n++] = s;
      while (*s && *s != ' ') s++;
      if (*s) *s++ = 0;
    }
  }
  return n;
}

void procMain(void* arg) {
  Proc* p = (Proc*)arg;
  p->task = xTaskGetCurrentTaskHandle();
  char* argv[10];
  int argc = 0;
  argv[argc++] = p->name;
  argc += tokenize(p->args, argv + 1, 8);
  argv[argc] = NULL;
  int rc = p->entry(argc, argv);
  procExit(p, rc);
}

void sysCheck() {
  Proc* p = procSelf();
  if (p && p->killed) procExit(p, 130);
}

// ================= SYSCALLS =================
int sys_getpid() { Proc* p = procSelf(); return p ? p->pid : -1; }

int sys_getc_t(int ms) {   // poll-style read of one tty byte, -1 on timeout
  sysCheck();
  unsigned long end = millis() + ms;
  for (;;) {
    long rem = (long)(end - millis());
    if (rem < 0) rem = 0;
    int w = rem > 50 ? 50 : (int)rem;
    char c;
    if (xQueueReceive(ttyIn, &c, pdMS_TO_TICKS(w)) == pdTRUE) return (unsigned char)c;
    sysCheck();
    if ((long)(millis() - end) >= 0) return -1;
  }
}

int sys_write(int fd, const char* b, int n) {
  sysCheck();
  Proc* p = procSelf();
  if (!p || fd < 0 || fd >= MAXFD) return -1;
  FD& d = p->fd[fd];
  switch (d.type) {
    case FD_TTY:  ttyWrite(b, n); return n;
    case FD_FILE: return d.f.write((const uint8_t*)b, n);
    case FD_NULL: return n;
  }
  return -1;
}

int sys_read(int fd, char* buf, int n) {
  sysCheck();
  Proc* p = procSelf();
  if (!p || fd < 0 || fd >= MAXFD) return -1;
  FD& d = p->fd[fd];
  switch (d.type) {
    case FD_TTY: {
      int c;
      do { c = sys_getc_t(50); } while (c < 0);
      int cnt = 0;
      buf[cnt++] = (char)c;
      if (ttyFlags & TTY_RAW) return cnt;
      while (buf[cnt - 1] != '\n' && cnt < n) {
        c = sys_getc_t(100);
        if (c < 0) continue;
        buf[cnt++] = (char)c;
      }
      return cnt;
    }
    case FD_FILE: return d.f.read((uint8_t*)buf, n);
    case FD_NULL: return 0;
    case FD_GPS: {
      if (!gpsOn) return 0;
      uint32_t w = gWritten;
      if (w - d.gpos > 512) d.gpos = w - 512;     // fell behind: skip to the oldest byte still in the ring
      int cnt = 0;
      while (cnt < n && d.gpos != w) {
        buf[cnt++] = gpsRing[d.gpos % 512];
        d.gpos++;
      }
      return cnt;
    }
  }
  return -1;
}

int sys_open(const char* path, int flags) {
  sysCheck();
  Proc* p = procSelf();
  if (!p) return -1;
  int fd = -1;
  for (int i = 0; i < MAXFD; i++) if (p->fd[i].type == FD_NONE) { fd = i; break; }
  if (fd < 0) return -1;
  FD& d = p->fd[fd];
  if (!strcmp(path, "/dev/null")) { d.type = FD_NULL; return fd; }
  if (!strcmp(path, "/dev/tty"))  { d.type = FD_TTY;  return fd; }
  if (!strcmp(path, "/dev/gps"))  { d.type = FD_GPS; d.gpos = gWritten; gpsReaders++; return fd; }
  String fp = path[0] == '/' ? String(path) : String("/") + path;
  File f = LittleFS.open(fp, flags == K_RD ? "r" : (flags == K_APP ? "a" : "w"));
  if (!f) return -1;
  if (f.isDirectory()) { f.close(); return -1; }
  d.f = f;
  d.type = FD_FILE;
  return fd;
}

int sys_close(int fd) {
  sysCheck();
  Proc* p = procSelf();
  if (!p || fd < 0 || fd >= MAXFD) return -1;
  fdRelease(p->fd[fd], true);
  return 0;
}

int sys_unlink(const char* path) {
  sysCheck();
  String fp = path[0] == '/' ? String(path) : String("/") + path;
  return LittleFS.remove(fp) ? 0 : -1;
}

int sys_readdir(const char* dir, int idx, char* name, int nmax, int* size) {
  sysCheck();
  File root = LittleFS.open(dir);
  if (!root) return -1;
  File f = root.openNextFile();
  int i = 0;
  while (f) {
    if (i == idx) { strlcpy(name, f.name(), nmax); *size = f.size(); return 1; }
    f = root.openNextFile();
    i++;
  }
  return 0;
}

// app names: letters, digits, _ - . only (no slashes, can't start with a dot)
bool validAppName(const char* n) {
  if (!n || !*n || n[0] == '.' || strlen(n) > 24) return false;
  for (const char* c = n; *c; c++)
    if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.') return false;
  return true;
}

// reads /apps/<name>/manifest. 0 = ok
int pkgLoadManifest(const char* name, esp_manifest_t* m) {
  char path[48];
  snprintf(path, sizeof(path), "/apps/%s/manifest", name);
  File f = LittleFS.open(path, "r");
  if (!f) return -1;
  char buf[ESP_MAX_MANIFEST + 1];
  int n = f.read((uint8_t*)buf, ESP_MAX_MANIFEST);
  f.close();
  if (n <= 0) return -1;
  buf[n] = 0;
  char err[40];
  return esp_manifest_parse(buf, m, err, sizeof(err)) == 0 ? 0 : -1;
}

// outfd (if >= 0) becomes the child's stdout; ownership moves to the child
int sys_spawn(const char* prog, const char* args, int outfd, bool bg) {
  sysCheck();
  Proc* par = procSelf();
  if (!par) return -1;
  const Prog* pr = findProg(prog);
  bool script = false, isWasm = false;
  uint32_t pkgCaps = 0xFFFFFFFFu;
  char spath[80];
  if (!pr) {
    // not built in: an installed package (/apps/<name>/) or a plain script (/apps/<name>)
    if (!validAppName(prog)) return -2;
    snprintf(spath, sizeof(spath), "/apps/%s", prog);
    File af = LittleFS.open(spath, "r");
    if (!af) return -2;
    bool isdir = af.isDirectory();
    af.close();
    if (isdir) {
      esp_manifest_t man;
      if (pkgLoadManifest(prog, &man) != 0) return -4;        // damaged package
      isWasm = !strcmp(man.type, "wasm");
      if (isWasm && !wasmAvailable()) return -5;              // wasm apps need PSRAM
      snprintf(spath, sizeof(spath), "/apps/%s/%s", prog, man.entry);
      pkgCaps = man.caps;
    }
    script = true;
  }
  Proc* c = NULL;
  for (int i = 0; i < MAXPROC; i++) if (!procs[i].used) { c = &procs[i]; break; }
  if (!c) return -3;
  c->pid = nextPid++;
  c->ppid = par->pid;
  strlcpy(c->name, prog, sizeof(c->name));
  if (script) snprintf(c->args, sizeof(c->args), "%s %s", spath, args);
  else strlcpy(c->args, args, sizeof(c->args));
  c->killed = false;
  c->exitcode = 0;
  c->allowed = par->allowed & pkgCaps;   // children never gain more than their parent has
  c->held = 0;
  c->wctx = NULL; c->inHost = false; c->killTick = 0; c->reapStrikes = 0;
  c->state = P_RUN;
  c->flags = (script ? 0 : pr->flags) | (bg ? PF_BG : 0) | (isWasm ? PF_WASM : 0);
  c->entry = isWasm ? p_wasm : (script ? p_run : pr->fn);
  c->task = NULL;
  for (int i = 0; i < MAXFD; i++) fdRelease(c->fd[i], false);
  c->fd[0] = par->fd[0];
  c->fd[1] = (outfd >= 0 && outfd < MAXFD) ? par->fd[outfd] : par->fd[1];
  c->fd[2] = par->fd[2];
  if (bg) { c->fd[0] = FD(); c->fd[0].type = FD_NULL; }
  if (outfd >= 0 && outfd < MAXFD) fdRelease(par->fd[outfd], false);
  c->used = true;
  if (!bg && !(par->flags & PF_BG)) fgpid = c->pid;   // background scripts never steal the keyboard
  TaskHandle_t h;
  int stack = (c->flags & PF_WASM) ? WASM_TASK_STACK : ((c->flags & PF_BIGSTACK) ? 16384 : 8192);   // https needs room
  int prio = (c->flags & PF_WASM) ? 0 : 1;   // wasm apps run at idle priority: they can never starve the OS
  if (xTaskCreatePinnedToCore(procMain, c->name, stack, c, prio, &h, 0) != pdPASS) {
    c->used = false;
    if (!bg && !(par->flags & PF_BG)) fgpid = par->pid;
    return -3;
  }
  return c->pid;
}

int sys_wait(int pid) {
  for (;;) {
    sysCheck();
    Proc* c = procByPid(pid);
    if (!c) return -1;
    if (c->state == P_ZOMBIE) { int rc = c->exitcode; c->used = false; return rc; }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

int sys_kill(int pid) {
  sysCheck();
  Proc* t = procByPid(pid);
  if (!t) return -1;
  if (t->flags & PF_NOKILL || t->pid == 1) return -2;
  t->killed = true;
  return 0;
}

int sys_psinfo(int idx, int* pid, int* ppid, char* st, char* name) {
  sysCheck();
  int k = 0;
  for (int j = 0; j < MAXPROC; j++) {
    if (!procs[j].used) continue;
    if (k == idx) {
      *pid = procs[j].pid;
      *ppid = procs[j].ppid;
      *st = procs[j].state == P_ZOMBIE ? 'Z' : 'R';
      strlcpy(name, procs[j].name, 25);
      return 1;
    }
    k++;
  }
  return 0;
}

void sys_sleep(int ms) {
  unsigned long end = millis() + ms;
  while ((long)(millis() - end) < 0) { sysCheck(); vTaskDelay(pdMS_TO_TICKS(20)); }
}

unsigned long sys_uptime() { return millis(); }
void sys_ttyctl(int flags) {
  sysCheck();
  Proc* me = procSelf();
  if (flags & TTY_GFX) { if (resAcquire(KC_DISPLAY) != KE_OK) flags &= ~TTY_GFX; }   // only one process may draw the OLED
  else if (me && (me->held & KC_DISPLAY)) resRelease(KC_DISPLAY);
  ttyCtl(flags);
}
void sys_reboot() { ESP.restart(); }
void sys_meminfo(unsigned long* fr, unsigned long* tot) { *fr = ESP.getFreeHeap(); *tot = ESP.getHeapSize(); }
void sys_fsinfo(unsigned long* used, unsigned long* tot) { *used = LittleFS.usedBytes(); *tot = LittleFS.totalBytes(); }

// ---- GPS power: on while a process holds KC_GPS, or while the user pinned it with "gps start" ----
static void gpsApply() {                 // caller holds gpsMx
  bool want = gpsSticky || gpsRefs > 0;
  if (want && !gpsOn) {
    digitalWrite(GNSS_PWR, LOW);
    vTaskDelay(pdMS_TO_TICKS(100));
    GPSSerial.begin(GNSS_BAUD, SERIAL_8N1, GNSS_RX_PIN, GNSS_TX_PIN);
    gpsOn = true;
  } else if (!want && gpsOn) {
    gpsOn = false;
    GPSSerial.end();
    digitalWrite(GNSS_PWR, HIGH);
  }
}

// "gps start" / "gps stop" pin the power on/off.
// Returns 0 = changed, 1 = already that way, 2 = (stop) unpinned but still held on by a running app.
int sys_gpsctl(int cmd) {
  sysCheck();
  xSemaphoreTake(gpsMx, portMAX_DELAY);
  int r;
  if (cmd == 1) {
    r = gpsSticky ? 1 : 0;
    gpsSticky = true;
    gpsApply();
  } else if (!gpsSticky) {
    r = gpsOn ? 2 : 1;
  } else {
    gpsSticky = false;
    gpsApply();
    r = gpsOn ? 2 : 0;
  }
  xSemaphoreGive(gpsMx);
  return r;
}

void sys_gpsinfo(GpsInfo* g) {
  xSemaphoreTake(gpsMx, portMAX_DELAY);  // loop() feeds TinyGPS under this lock, so we never read a half-updated fix
  g->on = gpsOn;
  g->sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  g->locValid = gpsOn && gps.location.isValid();
  g->lat = gps.location.lat();
  g->lng = gps.location.lng();
  g->mph = gps.speed.isValid() ? gps.speed.mph() : 0.0;
  g->alt = gps.altitude.meters();
  g->crs = gps.course.isValid() ? gps.course.deg() : 0;
  g->timeValid = gpsOn && gps.date.isValid() && gps.time.isValid();
  g->y = gps.date.year(); g->mo = gps.date.month(); g->d = gps.date.day();
  g->h = gps.time.hour(); g->mi = gps.time.minute();
  xSemaphoreGive(gpsMx);
}

// ================= LORA (kernel driver) =================
// One process at a time owns the radio (resAcquire(KC_LORA)); everyone else gets KE_BUSY.
SX1262 radio = new Module(LR_NSS, LR_DIO1, LR_RST, LR_BUSY);
volatile bool loraRxFlag = false;
int loraOwner = 0;                       // pid, 0 = free
int dispOwner = 0;

void IRAM_ATTR loraIsr() { loraRxFlag = true; }

static void loraFemOff() {
  digitalWrite(LR_FEM_PA, LOW);
  digitalWrite(LR_FEM_EN, LOW);
  digitalWrite(LR_FEM_PWR, LOW);
}

static int loraHwOn() {
  pinMode(LR_FEM_PWR, OUTPUT); digitalWrite(LR_FEM_PWR, HIGH);
  pinMode(LR_FEM_EN, OUTPUT);  digitalWrite(LR_FEM_EN, HIGH);
  pinMode(LR_FEM_PA, OUTPUT);  digitalWrite(LR_FEM_PA, LOW);
  SPI.begin(LR_SCK, LR_MISO, LR_MOSI, LR_NSS);
  // neutral defaults; the owner calls sys_lora_config() for its own settings.
  // try a 1.8V TCXO first, then fall back to a plain crystal
  int st = radio.begin(915.0, 125.0, 9, 5, 0x12, 10, 8, 1.8);
  if (st != RADIOLIB_ERR_NONE) st = radio.begin(915.0, 125.0, 9, 5, 0x12, 10, 8, 0.0);
  if (st != RADIOLIB_ERR_NONE) { loraFemOff(); return KE_IO; }
  radio.setDio2AsRfSwitch(true);
  radio.setDio1Action(loraIsr);
  radio.startReceive();
  loraRxFlag = false;
  return KE_OK;
}

static void loraHwOff() {
  radio.clearDio1Action();               // no stray interrupts after the owner is gone
  radio.sleep();
  loraFemOff();
}

static bool loraMine() { Proc* p = procSelf(); return p && loraOwner == p->pid; }

int sys_lora_config(const koa_lora_cfg_t* c) {
  sysCheck();
  if (!loraMine()) return KE_PERM;
  if (!c || c->sf < 5 || c->sf > 12 || c->cr < 5 || c->cr > 8) return KE_INVAL;
  int st = radio.setFrequency(c->freq_mhz);
  if (st == RADIOLIB_ERR_NONE) st = radio.setBandwidth(c->bw_khz);
  if (st == RADIOLIB_ERR_NONE) st = radio.setSpreadingFactor(c->sf);
  if (st == RADIOLIB_ERR_NONE) st = radio.setCodingRate(c->cr);
  if (st == RADIOLIB_ERR_NONE) st = radio.setSyncWord(c->sync_word);
  if (st == RADIOLIB_ERR_NONE) st = radio.setOutputPower(c->power_dbm);
  if (st == RADIOLIB_ERR_NONE) st = radio.setPreambleLength(c->preamble);
  radio.startReceive();
  loraRxFlag = false;
  return st == RADIOLIB_ERR_NONE ? KE_OK : KE_INVAL;
}

int sys_lora_cad() {                     // 1 = channel busy (or scan failed), 0 = free
  sysCheck();
  if (!loraMine()) return KE_PERM;
  int r = radio.scanChannel();
  radio.startReceive();
  loraRxFlag = false;
  return r != RADIOLIB_CHANNEL_FREE ? 1 : 0;
}

int sys_lora_tx(const void* buf, int n) {     // returns bytes sent, or <0
  sysCheck();
  if (!loraMine()) return KE_PERM;
  if (!buf || n <= 0 || n > 255) return KE_INVAL;
  digitalWrite(LR_FEM_PA, HIGH);         // PA path on for transmit
  int st = radio.transmit((uint8_t*)buf, n);
  digitalWrite(LR_FEM_PA, LOW);
  radio.startReceive();
  loraRxFlag = false;
  return st == RADIOLIB_ERR_NONE ? n : KE_IO;
}

// Wait up to timeout_ms for a packet. Returns its length, 0 on timeout, <0 on error.
// (A kill request is noticed here, so a process blocked in lora_rx still dies on Ctrl-C / kill.)
int sys_lora_rx(void* buf, int nmax, int timeout_ms, float* rssi, float* snr) {
  sysCheck();
  if (!loraMine()) return KE_PERM;
  if (!buf || nmax <= 0) return KE_INVAL;
  unsigned long end = millis() + (timeout_ms > 0 ? (unsigned long)timeout_ms : 0UL);
  for (;;) {
    sysCheck();
    if (loraRxFlag) {
      loraRxFlag = false;
      size_t len = radio.getPacketLength();
      if (len > (size_t)nmax) len = nmax;
      int st = radio.readData((uint8_t*)buf, len);
      float r = radio.getRSSI(), sn = radio.getSNR();
      radio.startReceive();
      if (st == RADIOLIB_ERR_NONE && len > 0) {
        if (rssi) *rssi = r;
        if (snr) *snr = sn;
        return (int)len;
      }
    }
    if ((long)(millis() - end) >= 0) return 0;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ================= I/O: GPIO, UART, I2C, SPI (for apps) =================
// Apps pick PINS at run time; the manifest only says which KINDS of I/O they may use (gpio, i2c, spi; gpio includes uart).
// Policy (which pins an app may ever touch) and ownership live in koa_io.h; this section does the hardware part.
//   GPIO  shared capability, pins claimed one by one          UART  2 ports, any pins (GPIO matrix)
//   I2C   exclusive: Wire1 (the OLED keeps Wire)              SPI   exclusive: HSPI (the radio keeps SPI)
// The GPS and the radio are NOT reachable through this: their pins are permanently off-limits to apps (koa_io.h) and
// they are only available through the gps / lora capabilities. GPS stays shared (any number of apps, receive-only).
// The two app UARTs use hardware UART2 and UART0 (UART1 belongs to the GPS driver and is never handed out). UART0's
// default pins (43/44, the USB-serial pads) are off-limits too: an app always names its own pins.
// Everything an app owns is released by resReleaseP() however the app ended.
static koa_pins_t ioPins;                 // guarded by resMx
static HardwareSerial appUart2(2);
static HardwareSerial appUart0(0);
struct UartSlot { bool open; int pid; int rx, tx; };
static UartSlot uartSlot[2];
static int  i2cOwner = 0;  static bool i2cOpen = false;
static int  spiOwner = 0;  static bool spiOpen = false;
static int  spiCs = -1;    static bool spiCsHeld = false;
static uint32_t spiFreq = 1000000; static uint8_t spiModeSel = SPI_MODE0;
static SPIClass appSpi(HSPI);

static HardwareSerial* uartObj(int slot) { return slot == 0 ? &appUart2 : &appUart0; }

static void ioApplyMode(int pin, int mode) {
  switch (mode) {
    case KG_IN:          pinMode(pin, INPUT); break;
    case KG_IN_PULLUP:   pinMode(pin, INPUT_PULLUP); break;
    case KG_IN_PULLDOWN: pinMode(pin, INPUT_PULLDOWN); break;
    case KG_OUT_LOW:     gpio_set_level((gpio_num_t)pin, 0); pinMode(pin, OUTPUT); break;   // level first, then output: no glitch
    case KG_OUT_HIGH:    gpio_set_level((gpio_num_t)pin, 1); pinMode(pin, OUTPUT); break;
  }
}

// hands every pin this process owns for one purpose back as a plain high-impedance input. Caller holds resMx.
static void ioPinsRelease(int pid, int kind) {
  for (int p = kio_next(&ioPins, pid, kind, 0); p >= 0; p = kio_next(&ioPins, pid, kind, p + 1)) {
    pinMode(p, INPUT);
    kio_release(&ioPins, p, pid, kind);
  }
}

// closes one UART slot. Caller holds resMx. (Draining the TX queue is the caller's job: this must never block.)
static void uartShut(int slot) {
  UartSlot& u = uartSlot[slot];
  if (!u.open) return;
  uartObj(slot)->end();
  if (u.rx >= 0) { pinMode(u.rx, INPUT); kio_release(&ioPins, u.rx, u.pid, KP_UART); }
  if (u.tx >= 0) { pinMode(u.tx, INPUT); kio_release(&ioPins, u.tx, u.pid, KP_UART); }
  u.open = false; u.pid = 0; u.rx = u.tx = -1;
}

static void uartCloseAll(int pid) {
  for (int i = 0; i < 2; i++) if (uartSlot[i].open && uartSlot[i].pid == pid) uartShut(i);
}

static void i2cShutdown(int pid) {
  if (i2cOpen) { Wire1.end(); i2cOpen = false; }
  ioPinsRelease(pid, KP_I2C);
}

static void spiShutdown(int pid) {
  if (spiOpen) { appSpi.end(); spiOpen = false; }
  spiCs = -1; spiCsHeld = false;
  ioPinsRelease(pid, KP_SPI);
}

// ================= RESOURCES =================
// KC_GPS is shared (refcounted). KC_LORA, KC_DISPLAY, KC_I2C and KC_SPI are exclusive.
// KC_GPIO and KC_UART are shared: they only open the door; each pin / UART port is claimed (and checked) when used.
// A process may only acquire what its manifest allowed (built-ins: everything).
// Whatever it still holds when it exits is released by procExit().
int resAcquire(uint32_t cap) {
  Proc* p = procSelf();
  if (!p) return KE_PERM;
  if (!(p->allowed & cap)) return KE_PERM;
  xSemaphoreTake(resMx, portMAX_DELAY);
  int rc = KE_OK;
  if (p->held & cap) {
    // already ours
  } else if (cap == KC_GPS) {
    xSemaphoreTake(gpsMx, portMAX_DELAY);
    gpsRefs++;
    gpsApply();
    xSemaphoreGive(gpsMx);
    p->held |= cap;
  } else if (cap == KC_LORA) {
    if (loraOwner) rc = KE_BUSY;
    else { rc = loraHwOn(); if (rc == KE_OK) { loraOwner = p->pid; p->held |= cap; } }
  } else if (cap == KC_DISPLAY) {
    if (dispOwner) rc = KE_BUSY;
    else { dispOwner = p->pid; p->held |= cap; }
  } else if (cap == KC_WIFI) {
    p->held |= cap;                 // shared; only gates koa_http_get / koa_wifi_status
  } else if (cap == KC_GPIO || cap == KC_UART) {
    p->held |= cap;                 // shared; pins and UART ports are claimed individually
  } else if (cap == KC_I2C) {
    if (i2cOwner) rc = KE_BUSY; else { i2cOwner = p->pid; p->held |= cap; }
  } else if (cap == KC_SPI) {
    if (spiOwner) rc = KE_BUSY; else { spiOwner = p->pid; p->held |= cap; }
  } else {
    rc = KE_NOSYS;
  }
  xSemaphoreGive(resMx);
  return rc;
}

static int resReleaseP(Proc* p, uint32_t cap) {
  xSemaphoreTake(resMx, portMAX_DELAY);
  if (!(p->held & cap)) { xSemaphoreGive(resMx); return KE_INVAL; }
  p->held &= ~cap;
  if (cap == KC_GPS) {
    xSemaphoreTake(gpsMx, portMAX_DELAY);
    if (gpsRefs > 0) gpsRefs--;
    gpsApply();
    xSemaphoreGive(gpsMx);
  } else if (cap == KC_LORA) {
    if (loraOwner == p->pid) { loraOwner = 0; loraHwOff(); }
  } else if (cap == KC_DISPLAY) {
    if (dispOwner == p->pid) dispOwner = 0;
  } else if (cap == KC_GPIO) {
    ioPinsRelease(p->pid, KP_GPIO);
  } else if (cap == KC_UART) {
    uartCloseAll(p->pid);
  } else if (cap == KC_I2C) {
    if (i2cOwner == p->pid) { i2cShutdown(p->pid); i2cOwner = 0; }
  } else if (cap == KC_SPI) {
    if (spiOwner == p->pid) { spiShutdown(p->pid); spiOwner = 0; }
  }
  xSemaphoreGive(resMx);
  return KE_OK;
}

int resRelease(uint32_t cap) {
  Proc* p = procSelf();
  return p ? resReleaseP(p, cap) : KE_PERM;
}

void resReleaseAll(Proc* p) {
  uint32_t h = p->held;
  for (int i = 0; i < 32; i++) if (h & (1u << i)) resReleaseP(p, 1u << i);
}

// ================= USERLAND HELPERS =================
void pr(const char* s) { sys_write(1, s, strlen(s)); }

void prf(const char* fmt, ...) {
  char b[160];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  sys_write(1, b, strlen(b));
}

int readline(char* buf, int max) {
  int n = sys_read(0, buf, max - 1);
  if (n < 0) n = 0;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
  buf[n] = 0;
  return n;
}

// ================= PROGRAMS =================
int p_help(int, char**) {
  pr("help clear uname uptime\nfree df ls cat rm echo\ndate whoami reboot gps\nedit <file> neofetch\nps kill sleep exit pins\nwifi aprs aprsd weather\nrun install uninstall apps\nuse > >> and & in sh\n");
  return 0;
}

int p_clear(int, char**) { pr("\x1b[2J\x1b[H"); return 0; }

int p_uname(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "-a")) prf("%s %s hkernel esp32s3 LX7 x2\n", OSNAME, OSVER);
  else prf("%s\n", OSNAME);
  return 0;
}

int p_whoami(int, char**) { pr("root\n"); return 0; }
int p_uptime(int, char**) { prf("up %lu s\n", sys_uptime() / 1000); return 0; }

int p_free(int, char**) {
  unsigned long f, t;
  sys_meminfo(&f, &t);
  prf("free:  %lu\ntotal: %lu\n", f, t);
  prf("psram: %lu/%lu\n", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned long)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  return 0;
}

int p_pins(int, char**) {                       // what apps may use, and who holds what right now
  pr("allowed:");
  for (int i = 0; i < KIO_NPINS; i++) if (kio_pin_allowed(i)) prf(" %d", i);
  pr("\n");
  static const char* kn[] = {"-", "gpio", "uart", "i2c", "spi"};
  int n = 0;
  for (int i = 0; i < KIO_NPINS; i++) {
    if (!ioPins.pid[i]) continue;
    const char* who = "?";
    for (int j = 0; j < MAXPROC; j++) if (procs[j].used && procs[j].pid == ioPins.pid[i]) who = procs[j].name;
    prf("%2d %-4s %s\n", i, kn[ioPins.kind[i] <= KP_SPI ? ioPins.kind[i] : 0], who);
    n++;
  }
  if (!n) pr("none in use\n");
  return 0;
}

int p_df(int, char**) {
  unsigned long u, t;
  sys_fsinfo(&u, &t);
  prf("used:  %lu\ntotal: %lu\n", u, t);
  return 0;
}

int p_ls(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : "/";
  char name[48];
  int size;
  int r = sys_readdir(dir, 0, name, sizeof(name), &size);
  if (r < 0) { prf("ls: %s: no such directory\n", dir); return 1; }
  for (int i = 0; sys_readdir(dir, i, name, sizeof(name), &size) == 1; i++) prf("%6d %s\n", size, name);
  return 0;
}

int p_cat(int argc, char** argv) {
  if (argc < 2) { pr("usage: cat <file>\n"); return 1; }
  for (int i = 1; i < argc; i++) {
    int fd = sys_open(argv[i], K_RD);
    if (fd < 0) { prf("cat: %s: no such file\n", argv[i]); continue; }
    char b[64];
    int n;
    while ((n = sys_read(fd, b, sizeof(b))) > 0) sys_write(1, b, n);
    sys_close(fd);
  }
  return 0;
}

int p_rm(int argc, char** argv) {
  if (argc < 2) { pr("usage: rm <file>\n"); return 1; }
  if (sys_unlink(argv[1]) != 0) { pr("rm: failed\n"); return 1; }
  return 0;
}

int p_echo(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (i > 1) pr(" ");
    pr(argv[i]);
  }
  pr("\n");
  return 0;
}

int p_date(int, char**) {
  GpsInfo g;
  sys_gpsinfo(&g);
  if (g.timeValid) prf("%04d-%02d-%02d %02d:%02d UTC\n", g.y, g.mo, g.d, g.h, g.mi);
  else pr("no time (gps start)\n");
  return 0;
}

int p_reboot(int, char**) { pr("rebooting...\n"); sys_sleep(300); sys_reboot(); return 0; }

int p_ps(int, char**) {
  pr("PID PPID S NAME\n");
  int pid, ppid;
  char st, name[25];
  for (int i = 0; sys_psinfo(i, &pid, &ppid, &st, name); i++) prf("%3d %4d %c %s\n", pid, ppid, st, name);
  return 0;
}

int p_kill(int argc, char** argv) {
  if (argc < 2) { pr("usage: kill <pid>\n"); return 1; }
  int r = sys_kill(atoi(argv[1]));
  if (r == -1) pr("kill: no such process\n");
  else if (r == -2) pr("kill: not permitted\n");
  return r == 0 ? 0 : 1;
}

int p_sleep(int argc, char** argv) {
  if (argc < 2) { pr("usage: sleep <sec>\n"); return 1; }
  sys_sleep(atoi(argv[1]) * 1000);
  return 0;
}

int p_neofetch(int, char**) {
  const char* art[6] = { " /\\_/\\ ", "( o.o )", " > ^ < ", "       ", "       ", "       " };
  unsigned long fr, tot;
  sys_meminfo(&fr, &tot);
  unsigned long up = sys_uptime() / 1000;
  GpsInfo g;
  sys_gpsinfo(&g);
  char l[6][24];
  snprintf(l[0], 24, "root@" HOSTNAME);
  snprintf(l[1], 24, "OS: %s %s", OSNAME, OSVER);
  snprintf(l[2], 24, "CPU: ESP32-S3 x2");
  snprintf(l[3], 24, "Up: %lum %lus", up / 60, up % 60);
  snprintf(l[4], 24, "RAM: %lu/%luK", (tot - fr) / 1024, tot / 1024);
  snprintf(l[5], 24, "GPS: %s", !g.on ? "off" : (g.locValid ? "fix" : "no fix"));
  for (int i = 0; i < 6; i++) prf("%s %s\n", art[i], l[i]);
  return 0;
}

void drawGpsView(const GpsInfo& g) {
  char buf[32];
  display.clearBuffer();
  display.setFont(u8g2_font_logisoso24_tn);
  snprintf(buf, sizeof(buf), "%.0f", g.mph);
  display.drawStr(0, 28, buf);
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(60, 28, "mph");
  snprintf(buf, sizeof(buf), "Sat:%d", g.sats);
  display.drawStr(92, 12, buf);
  if (!g.on) display.drawStr(0, 46, "GPS is off");
  else if (g.locValid) {
    snprintf(buf, sizeof(buf), "Lat: %.6f", g.lat);
    display.drawStr(0, 46, buf);
    snprintf(buf, sizeof(buf), "Lon: %.6f", g.lng);
    display.drawStr(0, 60, buf);
  } else display.drawStr(0, 46, "No fix yet...");
  display.sendBuffer();
}

int p_gps(int argc, char** argv) {
  const char* a = argc > 1 ? argv[1] : "";
  GpsInfo g;
  if (!strcmp(a, "start")) {
    pr(sys_gpsctl(1) ? "gps: already running\n" : "gps: powered on\n");
  } else if (!strcmp(a, "stop")) {
    int r = sys_gpsctl(0);
    pr(r == 0 ? "gps: powered off\n" : r == 2 ? "gps: unpinned, still in use by a running app\n" : "gps: not running\n");
  } else if (!strcmp(a, "status")) {
    sys_gpsinfo(&g);
    if (!g.on) { pr("gps is off\n"); return 0; }
    prf("sats: %d\n", g.sats);
    if (g.locValid) {
      prf("lat: %.6f\nlon: %.6f\n", g.lat, g.lng);
      prf("speed: %.1f mph\n", g.mph);
      prf("alt: %.0f m\n", g.alt);
    } else pr("no fix yet\n");
  } else if (!strcmp(a, "view")) {
    resAcquire(KC_GPS);                    // powers it on if needed; handed back when we exit
    sys_ttyctl(TTY_RAW | TTY_GFX);
    for (;;) {
      sys_gpsinfo(&g);
      drawGpsView(g);
      if (sys_getc_t(500) >= 0) break;
    }
  } else if (!strcmp(a, "nmea")) {
    resAcquire(KC_GPS);
    int fd = sys_open("/dev/gps", K_RD);
    if (fd < 0) { pr("gps: can't open /dev/gps\n"); return 1; }
    pr("streaming NMEA, Ctrl-C to stop\n");
    sys_ttyctl(TTY_RAW | TTY_NOSCREEN);
    char b[64];
    for (;;) {
      int n = sys_read(fd, b, sizeof(b));
      if (n > 0) sys_write(1, b, n);
      else sys_sleep(10);
    }
  } else pr("usage: gps start|stop|status|view|nmea\n");
  return 0;
}

// ---------- editor (userland program) ----------
std::vector<String> el;
int ecy = 0, ecx = 0, etop = 0, eleft = 0;
bool emod = false, eConfirm = false, edDirty = false, edDone = false, edBlink = true;
String efile, emsg;
int escSt = 0, escNum = 0;

void edChanged() { edDirty = true; }

void edClamp() {
  if (ecy < 0) ecy = 0;
  if (ecy >= (int)el.size()) ecy = el.size() - 1;
  int len = el[ecy].length();
  if (ecx > len) ecx = len;
  if (ecx < 0) ecx = 0;
  if (ecy < etop) etop = ecy;
  if (ecy >= etop + EROWS) etop = ecy - EROWS + 1;
  if (ecx < eleft) eleft = ecx;
  if (ecx >= eleft + COLS) eleft = ecx - COLS + 1;
}

void editOpen(const char* fn) {
  efile = fn[0] == '/' ? String(fn) : String("/") + fn;
  el.clear();
  int fd = sys_open(efile.c_str(), K_RD);
  if (fd >= 0) {
    String cur = "";
    char b[64];
    int n;
    while ((n = sys_read(fd, b, sizeof(b))) > 0) {
      for (int i = 0; i < n; i++) {
        if (b[i] == '\n') { el.push_back(cur); cur = ""; }
        else if (b[i] != '\r') cur += b[i];
      }
    }
    if (cur.length() > 0) el.push_back(cur);
    sys_close(fd);
  }
  if (el.empty()) el.push_back("");
  ecy = 0; ecx = 0; etop = 0; eleft = 0;
  emod = false; eConfirm = false; emsg = "";
  escSt = 0; escNum = 0;
}

void editSave() {
  int fd = sys_open(efile.c_str(), K_WR);
  if (fd < 0) { emsg = "Save failed!"; return; }
  for (size_t i = 0; i < el.size(); i++) {
    sys_write(fd, el[i].c_str(), el[i].length());
    sys_write(fd, "\n", 1);
  }
  sys_close(fd);
  emod = false;
  emsg = "Saved";
}

void edLeft()  { if (ecx > 0) ecx--; else if (ecy > 0) { ecy--; ecx = el[ecy].length(); } }
void edRight() { if (ecx < (int)el[ecy].length()) ecx++; else if (ecy < (int)el.size() - 1) { ecy++; ecx = 0; } }
void edUp()    { if (ecy > 0) ecy--; }
void edDown()  { if (ecy < (int)el.size() - 1) ecy++; }
void edHome()  { ecx = 0; }
void edEnd()   { ecx = el[ecy].length(); }

void edBackspace() {
  if (ecx > 0) {
    el[ecy].remove(ecx - 1, 1);
    ecx--;
    emod = true;
  } else if (ecy > 0) {
    int pl = el[ecy - 1].length();
    el[ecy - 1] += el[ecy];
    el.erase(el.begin() + ecy);
    ecy--;
    ecx = pl;
    emod = true;
  }
}

void edDelete() {
  if (ecx < (int)el[ecy].length()) {
    el[ecy].remove(ecx, 1);
    emod = true;
  } else if (ecy < (int)el.size() - 1) {
    el[ecy] += el[ecy + 1];
    el.erase(el.begin() + ecy + 1);
    emod = true;
  }
}

void edEnter() {
  if (el.size() >= 500) { emsg = "File too long"; return; }
  String rest = el[ecy].substring(ecx);
  el[ecy].remove(ecx);
  el.insert(el.begin() + ecy + 1, rest);
  ecy++;
  ecx = 0;
  emod = true;
}

void edKillLine() {
  el.erase(el.begin() + ecy);
  if (el.empty()) el.push_back("");
  if (ecy >= (int)el.size()) ecy = el.size() - 1;
  ecx = 0;
  emod = true;
}

void edInsert(char c) {
  if (el[ecy].length() >= 200) return;
  String s = el[ecy];
  el[ecy] = s.substring(0, ecx) + String(c) + s.substring(ecx);
  ecx++;
  emod = true;
}

void editKey(char c) {
  if (escSt == 1) {
    if (c == '[' || c == 'O') { escSt = 2; escNum = 0; } else escSt = 0;
    return;
  }
  if (escSt == 2) {
    if (c >= '0' && c <= '9') { escNum = escNum * 10 + (c - '0'); return; }
    escSt = 0;
    emsg = ""; eConfirm = false;
    switch (c) {
      case 'A': edUp(); break;
      case 'B': edDown(); break;
      case 'C': edRight(); break;
      case 'D': edLeft(); break;
      case 'H': edHome(); break;
      case 'F': edEnd(); break;
      case '~':
        if (escNum == 3) edDelete();
        else if (escNum == 1) edHome();
        else if (escNum == 4) edEnd();
        else if (escNum == 5) { for (int i = 0; i < EROWS; i++) edUp(); }
        else if (escNum == 6) { for (int i = 0; i < EROWS; i++) edDown(); }
        break;
    }
    edClamp(); edChanged();
    return;
  }
  if (c == 27) { escSt = 1; return; }

  if (c == 24) {
    if (emod && !eConfirm) {
      eConfirm = true;
      emsg = "Unsaved! ^X again";
      edChanged();
    } else edDone = true;
    return;
  }

  emsg = ""; eConfirm = false;

  switch (c) {
    case 19: case 15: editSave(); break;
    case 11: edKillLine(); break;
    case 1:  edHome(); break;
    case 5:  edEnd(); break;
    case 2:  edLeft(); break;
    case 6:  edRight(); break;
    case 16: edUp(); break;
    case 14: edDown(); break;
    case '\r': edEnter(); break;
    case '\n': break;
    case 8: case 127: edBackspace(); break;
    case '\t': edInsert(' '); edInsert(' '); break;
    default:
      if (c >= 32 && c < 127) edInsert(c);
      break;
  }
  edClamp();
  edChanged();
}

void drawEdit() {
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tr);
  display.setDrawColor(1);
  for (int r = 0; r < EROWS; r++) {
    int idx = etop + r;
    if (idx >= (int)el.size()) break;
    if ((int)el[idx].length() > eleft) {
      String v = el[idx].substring(eleft, eleft + COLS);
      display.drawStr(0, r * 8 + 7, v.c_str());
    }
  }
  char sb[40];
  if (emsg.length()) snprintf(sb, sizeof(sb), "%.25s", emsg.c_str());
  else snprintf(sb, sizeof(sb), "%.12s %d:%d%s", efile.c_str() + 1, ecy + 1, ecx + 1, emod ? " *" : "");
  display.drawBox(0, 56, 128, 8);
  display.setDrawColor(0);
  display.drawStr(0, 63, sb);
  display.setDrawColor(1);
  if (edBlink) {
    display.setDrawColor(2);
    display.drawBox((ecx - eleft) * 5, (ecy - etop) * 8, 5, 8);
    display.setDrawColor(1);
  }
  display.sendBuffer();
}

void renderEditSerial() {
  String o = "\x1b[H\x1b[2J";
  for (int r = 0; r < EROWS; r++) {
    int idx = etop + r;
    if (idx < (int)el.size() && (int)el[idx].length() > eleft)
      o += el[idx].substring(eleft, eleft + COLS);
    o += "\r\n";
  }
  o += "-- " + efile;
  if (emod) o += " [modified]";
  o += " " + String(ecy + 1) + ":" + String(ecx + 1) + " | ^S save ^X exit ^K cut line";
  if (emsg.length()) o += " | " + emsg;
  char pos[24];
  snprintf(pos, sizeof(pos), "\x1b[%d;%dH", ecy - etop + 1, ecx - eleft + 1);
  o += pos;
  sys_write(1, o.c_str(), o.length());
}

int p_edit(int argc, char** argv) {
  if (argc < 2) { pr("usage: edit <file>\n"); return 1; }
  editOpen(argv[1]);
  edDone = false; edDirty = true; edBlink = true;
  sys_ttyctl(TTY_RAW | TTY_NOSIG | TTY_NOSCREEN | TTY_GFX);
  unsigned long lb = millis();
  bool redraw = true;
  while (!edDone) {
    if (redraw) {
      drawEdit();
      if (edDirty) { edDirty = false; renderEditSerial(); }
      redraw = false;
    }
    int c = sys_getc_t(100);
    if (c >= 0) { editKey((char)c); redraw = true; }
    if (millis() - lb > 500) { lb = millis(); edBlink = !edBlink; redraw = true; }
  }
  pr("\x1b[2J\x1b[H");
  el.clear();
  return 0;
}

// ---------- login / shell / init ----------
int p_login(int, char**) {
  char user[96], pass[96];
  for (;;) {
    pr(HOSTNAME " login: ");
    readline(user, sizeof(user));
    sys_ttyctl(TTY_NOECHO);
    pr("Password: ");
    readline(pass, sizeof(pass));
    sys_ttyctl(0);
    pr("\n");
    if (!strcmp(user, LOGIN_USER) && !strcmp(pass, LOGIN_PASS)) return 0;
    sys_sleep(800);
    pr("Login incorrect\n\n");
  }
}

// Runs one command line (handles > >> and &). Used by the shell and by scripts.
// Returns the program's exit code (130 = Ctrl-C), 127 if not found.
int runLine(char* s) {
  while (*s == ' ') s++;
  int n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
  if (!*s) return 0;

  bool bg = false;
  if (s[n - 1] == '&') {
    bg = true;
    s[--n] = 0;
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
    if (!*s) return 0;
  }

  // find redirection outside quotes
  char* file = NULL;
  bool app = false, inq = false;
  for (char* q = s; *q; q++) {
    if (*q == '"') inq = !inq;
    else if (*q == '>' && !inq) {
      *q = 0;
      q++;
      if (*q == '>') { app = true; q++; }
      while (*q == ' ') q++;
      file = q;
      break;
    }
  }
  n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
  if (!*s) return 0;

  char* args = strchr(s, ' ');
  if (args) { *args++ = 0; while (*args == ' ') args++; } else args = (char*)"";

  int outfd = -1;
  if (file) {
    if (!*file) { pr("sh: missing filename\n"); return 1; }
    outfd = sys_open(file, app ? K_APP : K_WR);
    if (outfd < 0) { pr("sh: can't open file\n"); return 1; }
  }

  int pid = sys_spawn(s, args, outfd, bg);
  if (pid == -2) {
    prf("%s: not found\n", s);
    if (outfd >= 0) sys_close(outfd);
    return 127;
  }
  if (pid == -4) { prf("%s: damaged package (reinstall it)\n", s); if (outfd >= 0) sys_close(outfd); return 126; }
  if (pid == -5) { prf("%s: needs PSRAM (enable it in the Tools menu)\n", s); if (outfd >= 0) sys_close(outfd); return 126; }
  if (pid < 0) {
    pr("sh: cannot spawn\n");
    if (outfd >= 0) sys_close(outfd);
    return 1;
  }
  if (bg) { prf("[%d]\n", pid); return 0; }
  return sys_wait(pid);
}

// run <script>  : executes a text file, one command per line (# = comment)
// Programs in /apps are started through this automatically.
int p_run(int argc, char** argv) {
  if (argc < 2) { pr("usage: run <script>\n"); return 1; }
  int fd = sys_open(argv[1], K_RD);
  if (fd < 0) { prf("run: %s: not found\n", argv[1]); return 1; }
  String all = "";
  char b[64];
  int n;
  while ((n = sys_read(fd, b, sizeof(b))) > 0) {
    for (int i = 0; i < n; i++) if (all.length() < 4096) all += b[i];
  }
  sys_close(fd);

  int rc = 0;
  int pos = 0;
  int len = all.length();
  while (pos < len) {
    int e = all.indexOf('\n', pos);
    if (e < 0) e = len;
    String ln = all.substring(pos, e);
    pos = e + 1;
    ln.trim();
    if (ln.length() == 0 || ln[0] == '#') continue;
    if (ln == "exit") break;
    char line[128];
    strlcpy(line, ln.c_str(), sizeof(line));
    rc = runLine(line);
    if (rc == 130) break;   // Ctrl-C stops the whole script
  }
  return rc == 130 ? 130 : 0;
}

int p_sh(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "-l")) {
    pr("\x1b[2J\x1b[H");
    int pid = sys_spawn("neofetch", "", -1, false);
    if (pid > 0) sys_wait(pid);
    pr("\n");
  }
  char line[128];
  for (;;) {
    pr("root@" HOSTNAME ":~# ");
    int n = readline(line, sizeof(line));
    if (n <= 0) continue;
    char* s = line;
    while (*s == ' ') s++;
    n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
    if (!*s) continue;
    if (!strcmp(s, "exit") || !strcmp(s, "logout")) { pr("logout\n\n"); return 0; }
    runLine(s);
  }
}

int p_init(int, char**) {
  for (;;) {
    int pid = sys_spawn("login", "", -1, false);
    if (pid < 0) { sys_sleep(1000); continue; }
    sys_wait(pid);
    pid = sys_spawn("sh", "-l", -1, false);
    if (pid > 0) sys_wait(pid);
    pr("\n");
  }
}

// ================= WIFI =================
int sys_wifi_scan() {            // blocks (killable), returns count or <0
  sysCheck();
  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  WiFi.scanNetworks(true);       // async, so we can poll sysCheck()
  int n;
  while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) {
    sysCheck();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return n;
}

int sys_wifi_entry(int i, char* ssid, int nmax, int* rssi, int* isopen) {
  if (i < 0 || i >= WiFi.scanComplete()) return 0;
  strlcpy(ssid, WiFi.SSID(i).c_str(), nmax);
  *rssi = WiFi.RSSI(i);
  *isopen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
  return 1;
}

int sys_wifi_connect(const char* ssid, const char* pass, int ms) {
  sysCheck();
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);        // don't write creds to flash on every connect
  WiFi.begin(ssid, (pass && *pass) ? pass : NULL);
  unsigned long end = millis() + ms;
  while (WiFi.status() != WL_CONNECTED) {
    if (WiFi.status() == WL_CONNECT_FAILED) { WiFi.disconnect(); return -2; }  // usually bad password
    if ((long)(millis() - end) >= 0) { WiFi.disconnect(); return -1; }         // timeout / not found
    sysCheck();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return 0;
}

int sys_wifi_status(char* ssid, int nmax, char* ip, int imax, int* rssi) {
  if (WiFi.status() != WL_CONNECTED) return 0;
  strlcpy(ssid, WiFi.SSID().c_str(), nmax);
  strlcpy(ip, WiFi.localIP().toString().c_str(), imax);
  *rssi = WiFi.RSSI();
  return 1;
}

void sys_wifi_off() { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }

int p_wifi(int argc, char** argv) {
  const char* cmd = argc > 1 ? argv[1] : "status";

  if (!strcmp(cmd, "scan")) {
    pr("scanning...\n");
    int n = sys_wifi_scan();
    if (n < 0) { pr("wifi: scan failed\n"); return 1; }
    char ssid[33]; int rssi, isopen;
    for (int i = 0; sys_wifi_entry(i, ssid, sizeof(ssid), &rssi, &isopen) == 1; i++)
      prf("%2d %-15.15s %3d%c\n", i + 1, ssid[0] ? ssid : "(hidden)", rssi, isopen ? 'o' : ' ');
    return 0;
  }

  if (!strcmp(cmd, "connect")) {
    if (argc < 3) { pr("usage: wifi connect <name|#n>\n"); return 1; }
    char ssid[33];
    if (argv[2][0] == '#') {                       // pick from last scan
      int rssi, isopen;
      if (sys_wifi_entry(atoi(argv[2] + 1) - 1, ssid, sizeof(ssid), &rssi, &isopen) != 1) {
        pr("wifi: no such entry, run wifi scan\n"); return 1;
      }
    } else strlcpy(ssid, argv[2], sizeof(ssid));

    char pass[64];
    sys_ttyctl(TTY_NOECHO);
    pr("Password (Enter = open): ");
    readline(pass, sizeof(pass));
    sys_ttyctl(0);
    pr("\n");

    prf("connecting to %s...\n", ssid);
    int rc = sys_wifi_connect(ssid, pass, 15000);
    if (rc == 0) { return p_wifi(1, argv); }       // prints status
    pr(rc == -2 ? "wifi: auth failed\n" : "wifi: timed out\n");
    return 1;
  }

  if (!strcmp(cmd, "off")) { sys_wifi_off(); pr("wifi off\n"); return 0; }

  char ssid[33], ip[16]; int rssi;
  if (sys_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi)) prf("%s\n%s  %d dBm\n", ssid, ip, rssi);
  else pr("not connected\n");
  return 0;
}

// ================= WASM RUNTIME (wasm3; every byte an app touches lives in PSRAM) =================
//
// How apps are kept away from the OS:
//   * wasm3 allocates ALL of its memory (linear memory, value stack, compiled code) through three
//     hook functions below. They only ever allocate from PSRAM (MALLOC_CAP_SPIRAM): there is no
//     fallback to internal RAM. Each app has a byte quota (WASM_APP_MEM), and PSRAM is never
//     drained below WASM_PSRAM_RESERVE. A runaway memory.grow just fails inside the app.
//   * The hooks only work if wasm3's allocator calls them. Run  tools/patch_wasm3.py  once on the Wasm3
//     library folder (or just copy the already-patched lib/Wasm3 folder from this repo). It makes m3_Malloc / m3_FreeImpl /
//     m3_Realloc in .../libraries/Wasm3/src/m3_core.c call the three functions defined below
//     (koa_wasm_calloc / koa_wasm_realloc / koa_wasm_free). Keep only ONE Wasm3 folder in libraries.
//
//     If that edit is missing, p_wasm() notices (hook counter stays 0) and refuses to run the app
//     instead of silently using internal RAM.
//   * Apps run at task priority 0 (same as the idle task), so a busy loop can't starve the OS or
//     trip the idle watchdog. A kill that the app doesn't notice (pure compute loop) is enforced
//     by wasmReaper() from loop().

struct WasmCtx {
  Proc* p;
  size_t used, peak, limit;      // PSRAM bytes: in use / high-water mark / quota
  uint32_t hookHits;             // how many allocations wasm3 routed through our hooks
  IM3Environment env;
  IM3Runtime rt;
  uint8_t* bin;                  // the .wasm file (wasm3 keeps pointing into it)
  int exitcode;
};

struct WHdr { size_t size; WasmCtx* ctx; uint32_t magic; uint32_t pad; };   // 16 bytes, keeps allocations 16-aligned
static_assert(sizeof(WHdr) == 16, "WHdr must be 16 bytes");
#define WHDR_MAGIC 0xA55A3CC3u
#define WASM_CAPS  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)   // PSRAM only, never internal RAM

bool wasmAvailable() { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0; }

static WasmCtx* wctxSelf() {
  Proc* p = procSelf();
  return p ? (WasmCtx*)p->wctx : NULL;
}

static bool wPsramOk(size_t need) {
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= need + WASM_PSRAM_RESERVE;
}

static void* wAlloc(WasmCtx* c, size_t n) {
  if (!c) return NULL;
  if (n == 0) n = 1;
  if (n > c->limit || c->used + n > c->limit) return NULL;       // over this app's quota
  if (!wPsramOk(n + sizeof(WHdr))) return NULL;                  // would eat the PSRAM reserve
  WHdr* h = (WHdr*)heap_caps_calloc(1, n + sizeof(WHdr), WASM_CAPS);
  if (!h) return NULL;
  h->size = n; h->ctx = c; h->magic = WHDR_MAGIC;
  c->used += n;
  if (c->used > c->peak) c->peak = c->used;
  return h + 1;
}

static void wFree(void* p) {
  if (!p) return;
  WHdr* h = (WHdr*)p - 1;
  if (h->magic != WHDR_MAGIC) return;                            // not ours: never hand it to the heap
  if (h->ctx) h->ctx->used -= h->size;
  h->magic = 0;
  heap_caps_free(h);
}

static void* wRealloc(WasmCtx* c, void* p, size_t n) {
  if (!p) return wAlloc(c, n);
  if (n == 0) { wFree(p); return NULL; }
  WHdr* h = (WHdr*)p - 1;
  if (h->magic != WHDR_MAGIC) return NULL;
  WasmCtx* o = h->ctx;
  size_t old = h->size;
  if (n == old) return p;
  if (n > old) {
    if (n > o->limit || o->used - old + n > o->limit) return NULL;
    if (!wPsramOk(n + sizeof(WHdr))) return NULL;
  }
  WHdr* nh = (WHdr*)heap_caps_realloc(h, n + sizeof(WHdr), WASM_CAPS);
  if (!nh) return NULL;                                          // old block is still valid
  o->used = o->used - old + n;
  if (o->used > o->peak) o->peak = o->used;
  nh->size = n;
  if (n > old) memset((uint8_t*)(nh + 1) + old, 0, n - old);     // wasm memory must grow zeroed
  return nh + 1;
}

// the three functions wasm3's allocator is patched to call (see the note at the top of this section)
extern "C" void* koa_wasm_calloc(size_t n) {
  WasmCtx* c = wctxSelf();
  if (c) c->hookHits++;
  return wAlloc(c, n);
}
extern "C" void* koa_wasm_realloc(void* p, size_t n, size_t) {
  WasmCtx* c = wctxSelf();
  if (c) c->hookHits++;
  return wRealloc(c, p, n);
}
extern "C" void koa_wasm_free(void* p) { wFree(p); }

// frees everything an app owns. Safe to call twice, from the app's own task or (after the task is gone) from loop().
void wasmTeardown(Proc* p) {
  WasmCtx* c = (WasmCtx*)p->wctx;
  if (!c) return;
  p->wctx = NULL;
  if (c->rt)  { m3_FreeRuntime(c->rt); c->rt = NULL; }            // also frees the loaded module
  if (c->env) { m3_FreeEnvironment(c->env); c->env = NULL; }
  if (c->bin) { wFree(c->bin); c->bin = NULL; }
  free(c);
}

// kills a process that ignored a kill request and never calls into the OS (e.g. `while(1){}` in wasm)
static void procReap(Proc* p) {
  if (p->task) vTaskDelete(p->task);
  procFinish(p, 137);
}

// called from loop(): enforces kill on wasm apps stuck in pure computation
void wasmReaper() {
  unsigned long now = millis();
  for (int i = 0; i < MAXPROC; i++) {
    Proc* p = &procs[i];
    if (!p->used || p->state != P_RUN || !(p->flags & PF_WASM) || !p->killed) { if (p->used && !p->killed) { p->killTick = 0; p->reapStrikes = 0; } continue; }
    if (!p->killTick) { p->killTick = now | 1; continue; }
    if ((long)(now - p->killTick) < 1500) continue;               // grace period: let it exit by itself
    if (p->inHost) { p->reapStrikes = 0; continue; }              // inside an OS call: that call will notice the kill
    if (++p->reapStrikes < 2) continue;                           // outside the OS on two checks in a row
    procReap(p);
  }
}

// ---------------- IPC: lets "meshcore id Kat" talk to a running "meshcore start" ----------------
// Fixed-size messages, fixed tables: an app can't make the OS allocate more on its behalf.
#define IPC_MAXSRV  4
#define IPC_MAXCALL 4
#define IPC_MSG     256
#define IPC_QDEPTH  4

struct IpcReq  { uint8_t slot, gen; uint16_t len; uint8_t data[IPC_MSG]; };
struct IpcSrv  { bool used; int pid; char name[ESP_NAME_MAX + 1]; QueueHandle_t q; };
struct IpcCall { bool used; uint8_t gen; int srv; int clientPid; volatile bool done; volatile int rlen; uint8_t resp[IPC_MSG]; };

static IpcSrv  ipcSrv[IPC_MAXSRV];
static IpcCall ipcCalls[IPC_MAXCALL];
SemaphoreHandle_t ipcMx = NULL;                                   // created in setup()

static int ipcSrvOf(int pid) {
  for (int i = 0; i < IPC_MAXSRV; i++) if (ipcSrv[i].used && ipcSrv[i].pid == pid) return i;
  return -1;
}

static int ipcListen(const char* name, int pid) {
  xSemaphoreTake(ipcMx, portMAX_DELAY);
  int rc = KE_OK, freeIdx = -1;
  for (int i = 0; i < IPC_MAXSRV; i++) {
    if (!ipcSrv[i].used) { if (freeIdx < 0) freeIdx = i; continue; }
    if (ipcSrv[i].pid == pid || !strcmp(ipcSrv[i].name, name)) rc = KE_BUSY;   // one name per process, names are unique
  }
  if (rc == KE_OK && freeIdx < 0) rc = KE_NOMEM;
  if (rc == KE_OK) {
    QueueHandle_t q = xQueueCreate(IPC_QDEPTH, sizeof(IpcReq));
    if (!q) rc = KE_NOMEM;
    else {
      ipcSrv[freeIdx].used = true;
      ipcSrv[freeIdx].pid = pid;
      strlcpy(ipcSrv[freeIdx].name, name, sizeof(ipcSrv[freeIdx].name));
      ipcSrv[freeIdx].q = q;
    }
  }
  xSemaphoreGive(ipcMx);
  return rc;
}

// a process is gone: drop its listener (callers waiting on it get KE_IO) and any call slots it held
void ipcDropAll(int pid) {
  if (!ipcMx) return;
  xSemaphoreTake(ipcMx, portMAX_DELAY);
  for (int i = 0; i < IPC_MAXSRV; i++) {
    if (!ipcSrv[i].used || ipcSrv[i].pid != pid) continue;
    for (int j = 0; j < IPC_MAXCALL; j++)
      if (ipcCalls[j].used && ipcCalls[j].srv == i && !ipcCalls[j].done) { ipcCalls[j].rlen = KE_IO; ipcCalls[j].done = true; }
    vQueueDelete(ipcSrv[i].q);
    ipcSrv[i].q = NULL;
    ipcSrv[i].used = false;
  }
  for (int j = 0; j < IPC_MAXCALL; j++)
    if (ipcCalls[j].used && ipcCalls[j].clientPid == pid) { ipcCalls[j].used = false; ipcCalls[j].gen++; }
  xSemaphoreGive(ipcMx);
}

// ---------------- host-function plumbing ----------------
#ifndef m3ApiRawFunction
#define m3ApiRawFunction(NAME) const void * NAME (IM3Runtime runtime, IM3ImportContext _ctx, uint64_t * _sp, void * _mem)
#endif

static const char kExitMsg[]   = "koa: exit";
static const char kKilledMsg[] = "koa: killed";

struct HostScope {                     // marks "this app is inside an OS call" so the reaper never cuts one in half
  Proc* p;
  explicit HostScope(Proc* pp) : p(pp) { p->inHost = true; }
  ~HostScope() { p->inHost = false; }
};

#define W_BEGIN() \
  WasmCtx* wc = (WasmCtx*)m3_GetUserData(runtime); (void)wc; \
  Proc* wp = wc->p; \
  HostScope hscope(wp);
// every call except koa_yield / koa_exit: once a kill is pending the app is unwound
#define W_ENTER() \
  W_BEGIN() \
  if (wp->killed) { if (!wp->killTick) wp->killTick = millis() | 1; return kKilledMsg; }
#define R(v)    do { m3ApiReturn(v); } while (0)
#define G(off, len) gptr(runtime, _mem, (uint32_t)(off), (uint32_t)(len))
#define NEED(ptr)   do { if (!(ptr)) return m3Err_trapOutOfBoundsMemoryAccess; } while (0)

// pointer into the app's linear memory, or NULL if [off, off+len) isn't inside it
static inline uint8_t* gptr(IM3Runtime rt, void* mem, uint32_t off, uint32_t len) {
  return ((uint64_t)off + len <= (uint64_t)m3_GetMemorySize(rt)) ? (uint8_t*)mem + off : NULL;
}

// NUL-terminated string from app memory. Returns length, or -1 if it's out of bounds / too long.
static int gstr(IM3Runtime rt, void* mem, uint32_t off, char* out, int cap) {
  uint32_t size = m3_GetMemorySize(rt);
  for (int i = 0; i < cap; i++) {
    if ((uint64_t)off + i >= size) return -1;
    char c = ((const char*)mem)[off + i];
    out[i] = c;
    if (!c) return i;
  }
  out[cap - 1] = 0;
  return -1;
}

static inline void put32(uint8_t* p, int32_t v) { memcpy(p, &v, 4); }
static inline void putf(uint8_t* p, float v)    { memcpy(p, &v, 4); }
static inline void putd(uint8_t* p, double v)   { memcpy(p, &v, 8); }

static bool fsLow() { return LittleFS.totalBytes() - LittleFS.usedBytes() < 16384; }   // keep room for the OS's own files

// "name" -> "/data/<app>/name" (flat: no subdirectories, no dot-files, no escaping the app's folder)
static bool wAppPath(Proc* wp, const char* rel, char* out, size_t n) {
  if (!rel[0] || rel[0] == '.' || strlen(rel) > ESP_FNAME_MAX || !esp_name_chars_ok(rel)) return false;
  char dir[40];
  snprintf(dir, sizeof(dir), "/data/%s", wp->name);
  if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
  snprintf(out, n, "%s/%s", dir, rel);
  return true;
}

// ---------------- process / time ----------------
static m3ApiRawFunction(h_abi_version) {
  m3ApiReturnType(int32_t);
  W_ENTER();
  R(KOA_ABI_VERSION);
}

static m3ApiRawFunction(h_exit) {
  m3ApiGetArg(int32_t, rc);
  W_BEGIN();
  wc->exitcode = rc;
  return kExitMsg;                                  // unwinds out of wasm3; p_wasm() turns it into the exit code
}

static m3ApiRawFunction(h_yield) {
  m3ApiReturnType(int32_t);
  W_BEGIN();
  if (wp->killed) { if (!wp->killTick) wp->killTick = millis() | 1; R(KE_KILLED); }
  vTaskDelay(1);
  R(KE_OK);
}

static m3ApiRawFunction(h_sleep) {
  m3ApiGetArg(int32_t, ms);
  W_ENTER();
  if (ms < 0) ms = 0;
  unsigned long end = millis() + (unsigned long)ms;
  while ((long)(millis() - end) < 0 && !wp->killed) vTaskDelay(pdMS_TO_TICKS(10));
  return m3Err_none;
}

static m3ApiRawFunction(h_millis) {
  m3ApiReturnType(int32_t);
  W_ENTER();
  R((int32_t)millis());
}

static m3ApiRawFunction(h_micros) {
  m3ApiReturnType(int32_t);
  W_ENTER();
  R((int32_t)micros());
}

static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static m3ApiRawFunction(h_unixtime) {
  m3ApiReturnType(int64_t);
  W_ENTER();
  time_t t = time(NULL);
  if (t > 1700000000) R((int64_t)t);                // system clock was set (Wi-Fi/SNTP)
  int64_t out = -1;
  xSemaphoreTake(gpsMx, portMAX_DELAY);
  if (gpsOn && gps.date.isValid() && gps.time.isValid())
    out = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day()) * 86400LL +
          gps.time.hour() * 3600 + gps.time.minute() * 60 + gps.time.second();
  xSemaphoreGive(gpsMx);
  R(out);
}

static m3ApiRawFunction(h_random) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, n);
  W_ENTER();
  if (n < 0 || n > 4096) R(KE_INVAL);
  if (n == 0) R(KE_OK);
  uint8_t* b = G(bufp, n); NEED(b);
  esp_fill_random(b, (size_t)n);
  R(KE_OK);
}

// ---------------- console + files ----------------
static m3ApiRawFunction(h_write) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, fd);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, n);
  W_ENTER();
  if (n < 0 || fd < 0 || fd >= MAXFD || wp->fd[fd].type == FD_NONE) R(KE_INVAL);
  if (n == 0) R(0);
  if (n > 4096) n = 4096;                           // partial write; the app loops
  uint8_t* b = G(bufp, n); NEED(b);
  if (wp->fd[fd].type == FD_FILE && fsLow()) R(KE_IO);
  int r = sys_write(fd, (const char*)b, n);
  R(r < 0 ? KE_IO : r);
}

static m3ApiRawFunction(h_read) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, fd);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, n);
  W_ENTER();
  if (n < 0 || fd < 0 || fd >= MAXFD || wp->fd[fd].type == FD_NONE) R(KE_INVAL);
  if (n == 0) R(0);
  uint8_t* b = G(bufp, n); NEED(b);
  int r = sys_read(fd, (char*)b, n);
  R(r < 0 ? KE_IO : r);
}

static m3ApiRawFunction(h_getc) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, ms);
  W_ENTER();
  R(sys_getc_t(ms < 0 ? 0 : ms));
}

static m3ApiRawFunction(h_open) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, pathp);
  m3ApiGetArg(int32_t, flags);
  W_ENTER();
  char rel[ESP_FNAME_MAX + 2], full[80];
  if (gstr(runtime, _mem, pathp, rel, sizeof(rel)) < 0 || !wAppPath(wp, rel, full, sizeof(full))) R(KE_INVAL);
  if (flags != KO_RD && flags != KO_WR && flags != KO_APP) R(KE_INVAL);
  if (flags != KO_RD && fsLow()) R(KE_IO);
  int fd = sys_open(full, flags == KO_RD ? K_RD : (flags == KO_APP ? K_APP : K_WR));
  R(fd >= 0 ? fd : (flags == KO_RD ? KE_NOENT : KE_IO));
}

static m3ApiRawFunction(h_close) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, fd);
  W_ENTER();
  R(sys_close(fd) == 0 ? KE_OK : KE_INVAL);
}

static m3ApiRawFunction(h_unlink) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, pathp);
  W_ENTER();
  char rel[ESP_FNAME_MAX + 2], full[80];
  if (gstr(runtime, _mem, pathp, rel, sizeof(rel)) < 0 || !wAppPath(wp, rel, full, sizeof(full))) R(KE_INVAL);
  R(sys_unlink(full) == 0 ? KE_OK : KE_NOENT);
}

static m3ApiRawFunction(h_readdir) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, dirp);
  m3ApiGetArg(int32_t, idx);
  m3ApiGetArg(uint32_t, namep);
  m3ApiGetArg(int32_t, nmax);
  m3ApiGetArg(uint32_t, sizep);
  W_ENTER();
  char d[4];
  if (gstr(runtime, _mem, dirp, d, sizeof(d)) < 0 || (d[0] && strcmp(d, "."))) R(KE_INVAL);   // only the app's own folder
  if (idx < 0 || nmax < 1) R(KE_INVAL);
  uint8_t* np = G(namep, nmax); NEED(np);
  uint8_t* sp = NULL;
  if (sizep) { sp = G(sizep, 4); NEED(sp); }
  char dir[40];
  snprintf(dir, sizeof(dir), "/data/%s", wp->name);
  if (!LittleFS.exists(dir)) R(0);
  char tmp[48];
  int sz = 0;
  int r = sys_readdir(dir, idx, tmp, sizeof(tmp), &sz);
  if (r < 0) R(KE_IO);
  if (r == 0) R(0);
  const char* leaf = strrchr(tmp, '/');
  leaf = leaf ? leaf + 1 : tmp;
  strlcpy((char*)np, leaf, nmax);
  if (sp) put32(sp, sz);
  R(1);
}

// ---------------- IPC ----------------
static m3ApiRawFunction(h_ipc_listen) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, namep);
  W_ENTER();
  char name[ESP_NAME_MAX + 2];
  if (gstr(runtime, _mem, namep, name, sizeof(name)) < 0 || !esp_valid_app_name(name)) R(KE_INVAL);
  R(ipcListen(name, wp->pid));
}

static m3ApiRawFunction(h_ipc_recv) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, nmax);
  m3ApiGetArg(int32_t, timeout);
  m3ApiGetArg(uint32_t, clientp);
  W_ENTER();
  if (nmax < 0) R(KE_INVAL);
  uint8_t* b = G(bufp, nmax); NEED(b);
  uint8_t* cp = NULL;
  if (clientp) { cp = G(clientp, 4); NEED(cp); }
  xSemaphoreTake(ipcMx, portMAX_DELAY);
  int s = ipcSrvOf(wp->pid);
  QueueHandle_t q = s >= 0 ? ipcSrv[s].q : NULL;
  xSemaphoreGive(ipcMx);
  if (!q) R(KE_PERM);                               // call koa_ipc_listen first
  unsigned long end = millis() + (timeout > 0 ? (unsigned long)timeout : 0UL);
  for (;;) {
    if (wp->killed) R(KE_KILLED);
    IpcReq req;
    if (xQueueReceive(q, &req, pdMS_TO_TICKS(50)) == pdTRUE) {
      int n = req.len < nmax ? req.len : nmax;
      if (n > 0) memcpy(b, req.data, n);
      if (cp) put32(cp, (int32_t)(req.slot | ((uint32_t)req.gen << 8)));
      R(n);
    }
    if (timeout >= 0 && (long)(millis() - end) >= 0) R(KE_TIMEOUT);
  }
}

static m3ApiRawFunction(h_ipc_reply) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, client);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, n);
  W_ENTER();
  if (n < 0 || n > IPC_MSG) R(KE_INVAL);
  uint8_t* b = G(bufp, n); NEED(b);
  int slot = client & 0xFF, gen = (client >> 8) & 0xFF;
  int rc = KE_INVAL;
  xSemaphoreTake(ipcMx, portMAX_DELAY);
  int s = ipcSrvOf(wp->pid);
  if (s >= 0 && slot < IPC_MAXCALL) {
    IpcCall& c = ipcCalls[slot];
    if (c.used && c.gen == gen && c.srv == s && !c.done) {
      if (n > 0) memcpy(c.resp, b, n);
      c.rlen = n;
      c.done = true;
      rc = KE_OK;
    }
  }
  xSemaphoreGive(ipcMx);
  R(rc);
}

static m3ApiRawFunction(h_ipc_call) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, namep);
  m3ApiGetArg(uint32_t, reqp);
  m3ApiGetArg(int32_t, n);
  m3ApiGetArg(uint32_t, respp);
  m3ApiGetArg(int32_t, rmax);
  m3ApiGetArg(int32_t, timeout);
  W_ENTER();
  char name[ESP_NAME_MAX + 2];
  if (gstr(runtime, _mem, namep, name, sizeof(name)) < 0) R(KE_INVAL);
  if (n < 0 || n > IPC_MSG || rmax < 0) R(KE_INVAL);
  uint8_t* rq = G(reqp, n); NEED(rq);
  uint8_t* rs = G(respp, rmax); NEED(rs);
  if (timeout < 0) timeout = 0;
  if (timeout > 60000) timeout = 60000;

  IpcReq req;
  req.len = (uint16_t)n;
  if (n > 0) memcpy(req.data, rq, n);
  int slot = -1, rc = KE_OK;
  xSemaphoreTake(ipcMx, portMAX_DELAY);
  int s = -1;
  for (int i = 0; i < IPC_MAXSRV; i++) if (ipcSrv[i].used && !strcmp(ipcSrv[i].name, name)) { s = i; break; }
  if (s < 0) rc = KE_NOENT;
  else {
    for (int j = 0; j < IPC_MAXCALL; j++) if (!ipcCalls[j].used) { slot = j; break; }
    if (slot < 0) rc = KE_BUSY;
    else {
      IpcCall& c = ipcCalls[slot];
      c.used = true; c.gen++; c.srv = s; c.clientPid = wp->pid; c.done = false; c.rlen = 0;
      req.slot = (uint8_t)slot; req.gen = c.gen;
      if (xQueueSend(ipcSrv[s].q, &req, 0) != pdTRUE) { c.used = false; c.gen++; rc = KE_BUSY; }
    }
  }
  xSemaphoreGive(ipcMx);
  if (rc != KE_OK) R(rc);

  IpcCall& c = ipcCalls[slot];
  unsigned long end = millis() + (unsigned long)timeout;
  while (!c.done) {
    if (wp->killed || (long)(millis() - end) >= 0) {
      xSemaphoreTake(ipcMx, portMAX_DELAY);
      if (!c.done) { c.used = false; c.gen++; xSemaphoreGive(ipcMx); R(wp->killed ? KE_KILLED : KE_TIMEOUT); }
      xSemaphoreGive(ipcMx);
      break;                                         // reply landed at the last moment
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  int rl = c.rlen;
  if (rl >= 0) { if (rl > rmax) rl = rmax; if (rl > 0) memcpy(rs, c.resp, rl); }
  xSemaphoreTake(ipcMx, portMAX_DELAY);
  c.used = false; c.gen++;
  xSemaphoreGive(ipcMx);
  R(rl < 0 ? KE_IO : rl);
}

// ---------------- exclusive resources ----------------
static m3ApiRawFunction(h_acquire) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, caps);
  W_ENTER();
  if (!caps) R(KE_INVAL);
  uint32_t got = 0;
  int rc = KE_OK;
  for (int i = 0; i < 32 && rc == KE_OK; i++) {
    uint32_t bit = 1u << i;
    if (!(caps & bit)) continue;
    bool had = (wp->held & bit) != 0;
    rc = resAcquire(bit);
    if (rc == KE_OK && !had) got |= bit;
  }
  if (rc != KE_OK) for (int i = 0; i < 32; i++) if (got & (1u << i)) resRelease(1u << i);   // all or nothing
  R(rc);
}

static m3ApiRawFunction(h_release) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, caps);
  W_ENTER();
  if (!caps) R(KE_INVAL);
  int rc = KE_INVAL;
  for (int i = 0; i < 32; i++) {
    uint32_t bit = 1u << i;
    if ((caps & bit) && resRelease(bit) == KE_OK) rc = KE_OK;
  }
  R(rc);
}

// ---------------- GPS (holding KC_GPS already powers it) ----------------
static m3ApiRawFunction(h_gps_start) {
  m3ApiReturnType(int32_t);
  W_ENTER();
  R((wp->held & KC_GPS) ? KE_OK : KE_PERM);
}

static m3ApiRawFunction(h_gps_stop) {
  m3ApiReturnType(int32_t);
  W_ENTER();
  if (!(wp->held & KC_GPS)) R(KE_PERM);
  R(resRelease(KC_GPS));
}

static m3ApiRawFunction(h_gps_info) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, outp);
  W_ENTER();
  if (!(wp->held & KC_GPS)) R(KE_PERM);
  uint8_t* o = G(outp, 72); NEED(o);
  GpsInfo g;
  sys_gpsinfo(&g);
  // koa_gps_t as the wasm32 compiler lays it out (doubles are 8-aligned): written by offset, not by host struct
  memset(o, 0, 72);
  o[0] = g.on; o[1] = g.locValid; o[2] = g.timeValid;
  put32(o + 4, g.sats);
  put32(o + 8, g.y); put32(o + 12, g.mo); put32(o + 16, g.d); put32(o + 20, g.h); put32(o + 24, g.mi);
  putd(o + 32, g.lat); putd(o + 40, g.lng); putd(o + 48, g.mph); putd(o + 56, g.alt); putd(o + 64, g.crs);
  R(KE_OK);
}

// ---------------- Wi-Fi / HTTP ----------------
static m3ApiRawFunction(h_wifi_status) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, ssidp);
  m3ApiGetArg(int32_t, smax);
  m3ApiGetArg(uint32_t, ipp);
  m3ApiGetArg(int32_t, imax);
  m3ApiGetArg(uint32_t, rssip);
  W_ENTER();
  if (!(wp->held & KC_WIFI)) R(KE_PERM);
  if (smax < 1 || imax < 1) R(KE_INVAL);
  uint8_t* sp = G(ssidp, smax); NEED(sp);
  uint8_t* ip = G(ipp, imax);   NEED(ip);
  uint8_t* rp = NULL;
  if (rssip) { rp = G(rssip, 4); NEED(rp); }
  char ssid[33], ipb[16];
  int rssi = 0;
  int ok = sys_wifi_status(ssid, sizeof(ssid), ipb, sizeof(ipb), &rssi);
  if (!ok) { ssid[0] = 0; ipb[0] = 0; }
  strlcpy((char*)sp, ssid, smax);
  strlcpy((char*)ip, ipb, imax);
  if (rp) put32(rp, rssi);
  R(ok ? 1 : 0);
}

struct BufSink : public Stream {         // collects an HTTP body straight into app memory, dropping what doesn't fit
  uint8_t* b; int cap, n;
  BufSink(uint8_t* bb, int c) : b(bb), cap(c), n(0) {}
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t c) override { if (n < cap) { b[n++] = c; return 1; } return 0; }
  size_t write(const uint8_t* d, size_t l) override {
    size_t t = (size_t)(cap - n) < l ? (size_t)(cap - n) : l;
    if (t) { memcpy(b + n, d, t); n += (int)t; }
    return t;
  }
};

// Not a template on purpose: WiFiClient and WiFiClientSecure both derive from WiFiClient, and some IDEs
// (ArduinoDroid) generate a broken prototype for template functions.
static int httpFetch(WiFiClient& client, const char* url, uint8_t* out, int nmax) {
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) return KE_IO;
  int code = http.GET();
  int result;
  if (code < 0) result = KE_IO;
  else if (code < 200 || code > 299) result = -code;          // HTTP errors come back as -status (e.g. -404); KE_* are -1..-9
  else {
    BufSink sink(out, nmax);
    int r = http.writeToStream(&sink);                         // handles chunked encoding for us
    result = (sink.n > 0 || r >= 0) ? sink.n : KE_IO;          // a body bigger than the buffer is truncated, not an error
  }
  http.end();
  return result;
}

static volatile bool httpBusy = false;                         // one request at a time: TLS needs ~40 KB of internal RAM

static m3ApiRawFunction(h_http_get) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, urlp);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, nmax);
  W_ENTER();
  if (!(wp->held & KC_WIFI)) R(KE_PERM);
  char url[256];
  if (gstr(runtime, _mem, urlp, url, sizeof(url)) < 0) R(KE_INVAL);
  bool tls = !strncmp(url, "https://", 8);
  if (!tls && strncmp(url, "http://", 7)) R(KE_INVAL);
  if (nmax <= 0) R(KE_INVAL);
  if (nmax > 65536) nmax = 65536;
  uint8_t* out = G(bufp, nmax); NEED(out);
  if (WiFi.status() != WL_CONNECTED) R(KE_IO);
  if (httpBusy) R(KE_BUSY);
  httpBusy = true;
  int r;
  if (tls) { WiFiClientSecure cl; cl.setInsecure(); r = httpFetch(cl, url, out, nmax); }   // same trust model as the weather command
  else     { WiFiClient cl; r = httpFetch(cl, url, out, nmax); }
  httpBusy = false;
  R(r);
}

// ---------------- LoRa (owner checks live in the sys_lora_* driver) ----------------
static m3ApiRawFunction(h_lora_config) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, cfgp);
  W_ENTER();
  if (!(wp->held & KC_LORA)) R(KE_PERM);
  uint8_t* c = G(cfgp, 16); NEED(c);
  koa_lora_cfg_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  memcpy(&cfg.freq_mhz, c, 4);
  memcpy(&cfg.bw_khz, c + 4, 4);
  cfg.sf = c[8]; cfg.cr = c[9]; cfg.sync_word = c[10];
  cfg.power_dbm = (int8_t)c[12];
  cfg.preamble = (uint16_t)(c[14] | (c[15] << 8));
  R(sys_lora_config(&cfg));
}

static m3ApiRawFunction(h_lora_cad) {
  m3ApiReturnType(int32_t);
  W_ENTER();
  if (!(wp->held & KC_LORA)) R(KE_PERM);
  R(sys_lora_cad());
}

static m3ApiRawFunction(h_lora_tx) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, n);
  W_ENTER();
  if (!(wp->held & KC_LORA)) R(KE_PERM);
  if (n <= 0 || n > 255) R(KE_INVAL);
  uint8_t* b = G(bufp, n); NEED(b);
  R(sys_lora_tx(b, n));
}

static m3ApiRawFunction(h_lora_rx) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, nmax);
  m3ApiGetArg(int32_t, timeout);
  m3ApiGetArg(uint32_t, rssip);
  m3ApiGetArg(uint32_t, snrp);
  W_ENTER();
  if (!(wp->held & KC_LORA)) R(KE_PERM);
  if (nmax <= 0) R(KE_INVAL);
  if (nmax > 255) nmax = 255;
  uint8_t* b = G(bufp, nmax); NEED(b);
  uint8_t* rp = NULL; uint8_t* sp = NULL;
  if (rssip) { rp = G(rssip, 4); NEED(rp); }
  if (snrp)  { sp = G(snrp, 4);  NEED(sp); }
  float rssi = 0, snr = 0;
  int r = sys_lora_rx(b, nmax, timeout, &rssi, &snr);
  if (r > 0) { if (rp) putf(rp, rssi); if (sp) putf(sp, snr); }
  R(r);
}

// ---------------- crypto ----------------
static m3ApiRawFunction(h_sha256) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, inp);
  m3ApiGetArg(int32_t, n);
  m3ApiGetArg(uint32_t, outp);
  W_ENTER();
  if (n < 0) R(KE_INVAL);
  uint8_t* in = G(inp, n);  NEED(in);
  uint8_t* out = G(outp, 32); NEED(out);
  esp_sha_t s; esp_sha_init(&s);
  if (n) esp_sha_update(&s, in, (uint32_t)n);
  esp_sha_final(&s, out);
  R(KE_OK);
}

static m3ApiRawFunction(h_hmac_sha256) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, keyp);
  m3ApiGetArg(int32_t, klen);
  m3ApiGetArg(uint32_t, inp);
  m3ApiGetArg(int32_t, n);
  m3ApiGetArg(uint32_t, outp);
  W_ENTER();
  if (klen < 0 || n < 0) R(KE_INVAL);
  uint8_t* key = G(keyp, klen); NEED(key);
  uint8_t* in  = G(inp, n);     NEED(in);
  uint8_t* out = G(outp, 32);   NEED(out);
  uint8_t k[64];
  memset(k, 0, sizeof(k));
  if (klen > 64) { esp_sha_t s; esp_sha_init(&s); esp_sha_update(&s, key, (uint32_t)klen); esp_sha_final(&s, k); }
  else if (klen) memcpy(k, key, klen);
  uint8_t pad[64], inner[32];
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  esp_sha_t s; esp_sha_init(&s);
  esp_sha_update(&s, pad, 64);
  if (n) esp_sha_update(&s, in, (uint32_t)n);
  esp_sha_final(&s, inner);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  esp_sha_init(&s);
  esp_sha_update(&s, pad, 64);
  esp_sha_update(&s, inner, 32);
  esp_sha_final(&s, out);
  R(KE_OK);
}

static m3ApiRawFunction(h_aes128_ecb) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, keyp);
  m3ApiGetArg(uint32_t, inp);
  m3ApiGetArg(uint32_t, outp);
  m3ApiGetArg(int32_t, n);
  m3ApiGetArg(int32_t, enc);
  W_ENTER();
  if (n <= 0 || n > 65536 || (n & 15)) R(KE_INVAL);
  uint8_t* key = G(keyp, 16); NEED(key);
  uint8_t* in  = G(inp, n);   NEED(in);
  uint8_t* out = G(outp, n);  NEED(out);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  int rc = enc ? mbedtls_aes_setkey_enc(&ctx, key, 128) : mbedtls_aes_setkey_dec(&ctx, key, 128);
  for (int i = 0; rc == 0 && i < n; i += 16) {
    uint8_t blk[16];                                // in/out may overlap, so go through a temp block
    rc = mbedtls_aes_crypt_ecb(&ctx, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, in + i, blk);
    if (rc == 0) memcpy(out + i, blk, 16);
  }
  mbedtls_aes_free(&ctx);
  R(rc == 0 ? KE_OK : KE_IO);
}

// ---------------- GPIO / UART / I2C / SPI ----------------
// Every call: the app must hold the capability (manifest + koa_acquire), and the OS checks each pin against
// the allow-list and the ownership table in koa_io.h. Blocking calls watch wp->killed like every other host call.
static m3ApiRawFunction(h_gpio_request) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, pin);
  m3ApiGetArg(int32_t, mode);
  W_ENTER();
  if (!(wp->held & KC_GPIO)) R(KE_PERM);
  if (mode < KG_IN || mode > KG_OUT_HIGH) R(KE_INVAL);
  xSemaphoreTake(resMx, portMAX_DELAY);
  int rc = kio_claim(&ioPins, pin, wp->pid, KP_GPIO, (uint8_t)mode);
  if (rc == KE_OK) ioApplyMode(pin, mode);
  xSemaphoreGive(resMx);
  R(rc);
}

static m3ApiRawFunction(h_gpio_release) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, pin);
  W_ENTER();
  if (!(wp->held & KC_GPIO)) R(KE_PERM);
  xSemaphoreTake(resMx, portMAX_DELAY);
  int rc = kio_release(&ioPins, pin, wp->pid, KP_GPIO);
  if (rc == KE_OK) pinMode(pin, INPUT);
  xSemaphoreGive(resMx);
  R(rc);
}

static m3ApiRawFunction(h_gpio_read) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, pin);
  W_ENTER();
  if (!(wp->held & KC_GPIO)) R(KE_PERM);
  if (pin < 0 || pin >= KIO_NPINS) R(KE_INVAL);
  if (!kio_owned(&ioPins, pin, wp->pid, KP_GPIO)) R(KE_PERM);        // not requested (or not yours)
  R(digitalRead(pin) ? 1 : 0);
}

static m3ApiRawFunction(h_gpio_write) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, pin);
  m3ApiGetArg(int32_t, level);
  W_ENTER();
  if (!(wp->held & KC_GPIO)) R(KE_PERM);
  if (pin < 0 || pin >= KIO_NPINS) R(KE_INVAL);
  if (!kio_owned(&ioPins, pin, wp->pid, KP_GPIO)) R(KE_PERM);
  if (ioPins.mode[pin] != KG_OUT_LOW && ioPins.mode[pin] != KG_OUT_HIGH) R(KE_INVAL);   // it's an input
  digitalWrite(pin, level ? HIGH : LOW);
  R(KE_OK);
}

static bool uartMine(Proc* wp, int h) { return h >= 0 && h < 2 && uartSlot[h].open && uartSlot[h].pid == wp->pid; }

static m3ApiRawFunction(h_uart_open) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, rx);
  m3ApiGetArg(int32_t, tx);
  m3ApiGetArg(int32_t, baud);
  m3ApiGetArg(int32_t, fmt);
  W_ENTER();
  if (!(wp->held & KC_UART)) R(KE_PERM);
  // both pins -1 would make arduino-esp32 fall back to the port's DEFAULT pins (which may be the OLED): refuse
  if ((rx < 0 && tx < 0) || baud < 300 || baud > 2000000 || fmt < KU_8N1 || fmt > KU_8N2) R(KE_INVAL);
  static const uint32_t cfg[4] = { SERIAL_8N1, SERIAL_8E1, SERIAL_8O1, SERIAL_8N2 };
  int pins[2] = { rx, tx };
  int rc = KE_BUSY;
  int slot = !uartSlot[0].open ? 0 : (!uartSlot[1].open ? 1 : -1);    // both ports taken: KE_BUSY
  xSemaphoreTake(resMx, portMAX_DELAY);
  if (slot >= 0 && !uartSlot[slot].open) rc = kio_claim_set(&ioPins, pins, 2, wp->pid, KP_UART);
  if (rc == KE_OK) {
    HardwareSerial* u = uartObj(slot);
    u->setRxBufferSize(512);
    u->begin((unsigned long)baud, cfg[fmt], rx, tx);
    if (!(*u)) {                                                      // driver didn't start: undo the claim
      for (int i = 0; i < 2; i++) if (pins[i] >= 0) kio_release(&ioPins, pins[i], wp->pid, KP_UART);
      rc = KE_IO;
    } else {
      uartSlot[slot].open = true; uartSlot[slot].pid = wp->pid; uartSlot[slot].rx = rx; uartSlot[slot].tx = tx;
      rc = slot;
    }
  }
  xSemaphoreGive(resMx);
  R(rc);
}

static m3ApiRawFunction(h_uart_close) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, h);
  W_ENTER();
  if (!(wp->held & KC_UART)) R(KE_PERM);
  if (!uartMine(wp, h)) R(KE_INVAL);
  if (uartSlot[h].tx >= 0) uartObj(h)->flush();                       // let queued bytes leave (outside the lock: slow baud rates)
  xSemaphoreTake(resMx, portMAX_DELAY);
  if (uartMine(wp, h)) uartShut(h);
  xSemaphoreGive(resMx);
  R(KE_OK);
}

static m3ApiRawFunction(h_uart_write) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, h);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, n);
  W_ENTER();
  if (!(wp->held & KC_UART)) R(KE_PERM);
  if (!uartMine(wp, h) || uartSlot[h].tx < 0) R(KE_INVAL);            // not open, not yours, or opened without a TX pin
  if (n <= 0 || n > 1024) R(KE_INVAL);
  uint8_t* b = G(bufp, n); NEED(b);
  HardwareSerial* u = uartObj(h);
  for (int off = 0; off < n; off += 128) {
    if (wp->killed) R(KE_KILLED);
    int k = n - off < 128 ? n - off : 128;
    u->write(b + off, (size_t)k);
  }
  R(n);
}

static m3ApiRawFunction(h_uart_read) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, h);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, nmax);
  m3ApiGetArg(int32_t, timeout);
  W_ENTER();
  if (!(wp->held & KC_UART)) R(KE_PERM);
  if (!uartMine(wp, h) || uartSlot[h].rx < 0 || nmax <= 0) R(KE_INVAL);   // not open, not yours, or opened without an RX pin
  if (nmax > 1024) nmax = 1024;
  if (timeout < 0) timeout = 0;
  uint8_t* b = G(bufp, nmax); NEED(b);
  HardwareSerial* u = uartObj(h);
  unsigned long end = millis() + (unsigned long)timeout;
  while (!u->available()) {
    if (wp->killed) R(KE_KILLED);
    if ((long)(millis() - end) >= 0) R(0);                            // timeout, nothing arrived
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  int got = 0;
  while (got < nmax && u->available()) b[got++] = (uint8_t)u->read();  // whatever is there now; doesn't wait to fill the buffer
  R(got);
}

static bool i2cMine(Proc* wp) { return i2cOpen && i2cOwner == wp->pid; }

static m3ApiRawFunction(h_i2c_open) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, sda);
  m3ApiGetArg(int32_t, scl);
  m3ApiGetArg(int32_t, freq);
  W_ENTER();
  if (!(wp->held & KC_I2C)) R(KE_PERM);
  if (sda < 0 || scl < 0 || freq < 10000 || freq > 1000000) R(KE_INVAL);
  int pins[2] = { sda, scl };
  xSemaphoreTake(resMx, portMAX_DELAY);
  int rc;
  if (i2cOpen) rc = KE_BUSY;                                          // close it first
  else {
    rc = kio_claim_set(&ioPins, pins, 2, wp->pid, KP_I2C);
    if (rc == KE_OK) {
      if (Wire1.begin(sda, scl, (uint32_t)freq)) i2cOpen = true;
      else { ioPinsRelease(wp->pid, KP_I2C); rc = KE_IO; }
    }
  }
  xSemaphoreGive(resMx);
  R(rc);
}

static m3ApiRawFunction(h_i2c_close) {
  m3ApiReturnType(int32_t);
  W_ENTER();
  if (!(wp->held & KC_I2C)) R(KE_PERM);
  xSemaphoreTake(resMx, portMAX_DELAY);
  int rc = KE_INVAL;
  if (i2cMine(wp)) { i2cShutdown(wp->pid); rc = KE_OK; }
  xSemaphoreGive(resMx);
  R(rc);
}

static m3ApiRawFunction(h_i2c_write) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, addr);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, n);
  m3ApiGetArg(int32_t, stop);
  W_ENTER();
  if (!(wp->held & KC_I2C)) R(KE_PERM);
  if (!i2cMine(wp) || addr < 0x08 || addr > 0x77 || n < 0 || n > 128) R(KE_INVAL);
  uint8_t* b = G(bufp, n); NEED(b);
  Wire1.beginTransmission((uint8_t)addr);
  if (n && Wire1.write(b, (size_t)n) != (size_t)n) { Wire1.endTransmission(true); R(KE_INVAL); }
  uint8_t e = Wire1.endTransmission(stop != 0);
  if (e == 0) R(n);
  R(e == 2 ? KE_NOENT : (e == 5 ? KE_TIMEOUT : (e == 1 ? KE_INVAL : KE_IO)));   // 2 = no ACK on the address, 5 = bus timeout
}

static m3ApiRawFunction(h_i2c_read) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, addr);
  m3ApiGetArg(uint32_t, bufp);
  m3ApiGetArg(int32_t, n);
  W_ENTER();
  if (!(wp->held & KC_I2C)) R(KE_PERM);
  if (!i2cMine(wp) || addr < 0x08 || addr > 0x77 || n <= 0 || n > 128) R(KE_INVAL);
  uint8_t* b = G(bufp, n); NEED(b);
  size_t want = Wire1.requestFrom((uint16_t)addr, (size_t)n, true);
  int got = 0;
  while (got < (int)want && got < n && Wire1.available()) b[got++] = (uint8_t)Wire1.read();
  R(got > 0 ? got : KE_IO);
}

static bool spiMine(Proc* wp) { return spiOpen && spiOwner == wp->pid; }

static m3ApiRawFunction(h_spi_open) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(int32_t, sck);
  m3ApiGetArg(int32_t, mosi);
  m3ApiGetArg(int32_t, miso);
  m3ApiGetArg(int32_t, cs);
  m3ApiGetArg(int32_t, freq);
  m3ApiGetArg(int32_t, mode);
  W_ENTER();
  if (!(wp->held & KC_SPI)) R(KE_PERM);
  if (sck < 0 || (mosi < 0 && miso < 0) || freq < 1000 || freq > 40000000 || mode < KSPI_MODE0 || mode > KSPI_MODE3) R(KE_INVAL);
  int pins[4] = { sck, mosi, miso, cs };
  xSemaphoreTake(resMx, portMAX_DELAY);
  int rc;
  if (spiOpen) rc = KE_BUSY;
  else {
    rc = kio_claim_set(&ioPins, pins, 4, wp->pid, KP_SPI);
    if (rc == KE_OK) {
      appSpi.begin(sck, miso, mosi, -1);                              // chip select is driven by hand below, so unused pins stay -1
      if (cs >= 0) { gpio_set_level((gpio_num_t)cs, 1); pinMode(cs, OUTPUT); }   // idle high before it becomes an output
      static const uint8_t modes[4] = { SPI_MODE0, SPI_MODE1, SPI_MODE2, SPI_MODE3 };
      spiFreq = (uint32_t)freq; spiModeSel = modes[mode]; spiCs = cs; spiCsHeld = false;
      spiOpen = true;
    }
  }
  xSemaphoreGive(resMx);
  R(rc);
}

static m3ApiRawFunction(h_spi_close) {
  m3ApiReturnType(int32_t);
  W_ENTER();
  if (!(wp->held & KC_SPI)) R(KE_PERM);
  xSemaphoreTake(resMx, portMAX_DELAY);
  int rc = KE_INVAL;
  if (spiMine(wp)) { if (spiCs >= 0) digitalWrite(spiCs, HIGH); spiShutdown(wp->pid); rc = KE_OK; }   // deselect the device first
  xSemaphoreGive(resMx);
  R(rc);
}

static m3ApiRawFunction(h_spi_transfer) {
  m3ApiReturnType(int32_t);
  m3ApiGetArg(uint32_t, txp);
  m3ApiGetArg(uint32_t, rxp);
  m3ApiGetArg(int32_t, n);
  m3ApiGetArg(int32_t, flags);
  W_ENTER();
  if (!(wp->held & KC_SPI)) R(KE_PERM);
  if (!spiMine(wp) || n <= 0 || n > 256) R(KE_INVAL);
  uint8_t* tx = txp ? G(txp, n) : NULL; if (txp && !tx) return m3Err_trapOutOfBoundsMemoryAccess;
  uint8_t* rx = rxp ? G(rxp, n) : NULL; if (rxp && !rx) return m3Err_trapOutOfBoundsMemoryAccess;
  uint8_t zeros[256];
  if (!tx) { memset(zeros, 0, (size_t)n); tx = zeros; }               // read-only transfer: clock out zeros
  appSpi.beginTransaction(SPISettings(spiFreq, MSBFIRST, spiModeSel));
  if (spiCs >= 0 && !spiCsHeld) digitalWrite(spiCs, LOW);
  appSpi.transferBytes(tx, rx, (uint32_t)n);
  if (spiCs >= 0 && !(flags & KSPI_KEEP_CS)) digitalWrite(spiCs, HIGH);
  spiCsHeld = (flags & KSPI_KEEP_CS) != 0;
  appSpi.endTransaction();
  R(n);
}

// ---------------- the import table: module "koa", names/signatures from koaos_abi.h ----------------
struct HostFn { const char* name; const char* sig; M3RawCall fn; };
static const HostFn kHostFns[] = {
  {"koa_abi_version", "i()",      h_abi_version},
  {"koa_exit",        "v(i)",     h_exit},
  {"koa_yield",       "i()",      h_yield},
  {"koa_sleep",       "v(i)",     h_sleep},
  {"koa_millis",      "i()",      h_millis},
  {"koa_micros",      "i()",      h_micros},
  {"koa_unixtime",    "I()",      h_unixtime},
  {"koa_random",      "i(ii)",    h_random},
  {"koa_write",       "i(iii)",   h_write},
  {"koa_read",        "i(iii)",   h_read},
  {"koa_getc",        "i(i)",     h_getc},
  {"koa_open",        "i(ii)",    h_open},
  {"koa_close",       "i(i)",     h_close},
  {"koa_unlink",      "i(i)",     h_unlink},
  {"koa_readdir",     "i(iiiii)", h_readdir},
  {"koa_ipc_listen",  "i(i)",     h_ipc_listen},
  {"koa_ipc_recv",    "i(iiii)",  h_ipc_recv},
  {"koa_ipc_reply",   "i(iii)",   h_ipc_reply},
  {"koa_ipc_call",    "i(iiiiii)", h_ipc_call},
  {"koa_acquire",     "i(i)",     h_acquire},
  {"koa_release",     "i(i)",     h_release},
  {"koa_gps_start",   "i()",      h_gps_start},
  {"koa_gps_stop",    "i()",      h_gps_stop},
  {"koa_gps_info",    "i(i)",     h_gps_info},
  {"koa_wifi_status", "i(iiiii)", h_wifi_status},
  {"koa_http_get",    "i(iii)",   h_http_get},
  {"koa_lora_config", "i(i)",     h_lora_config},
  {"koa_lora_cad",    "i()",      h_lora_cad},
  {"koa_lora_tx",     "i(ii)",    h_lora_tx},
  {"koa_lora_rx",     "i(iiiii)", h_lora_rx},
  {"koa_sha256",      "i(iii)",   h_sha256},
  {"koa_hmac_sha256", "i(iiiii)", h_hmac_sha256},
  {"koa_aes128_ecb",  "i(iiiii)", h_aes128_ecb},
  // ABI 2: GPIO / UART / I2C / SPI
  {"koa_gpio_request", "i(ii)",     h_gpio_request},
  {"koa_gpio_release", "i(i)",      h_gpio_release},
  {"koa_gpio_read",    "i(i)",      h_gpio_read},
  {"koa_gpio_write",   "i(ii)",     h_gpio_write},
  {"koa_uart_open",    "i(iiii)",   h_uart_open},
  {"koa_uart_close",   "i(i)",      h_uart_close},
  {"koa_uart_write",   "i(iii)",    h_uart_write},
  {"koa_uart_read",    "i(iiii)",   h_uart_read},
  {"koa_i2c_open",     "i(iii)",    h_i2c_open},
  {"koa_i2c_close",    "i()",       h_i2c_close},
  {"koa_i2c_write",    "i(iiii)",   h_i2c_write},
  {"koa_i2c_read",     "i(iii)",    h_i2c_read},
  {"koa_spi_open",     "i(iiiiii)", h_spi_open},
  {"koa_spi_close",    "i()",       h_spi_close},
  {"koa_spi_transfer", "i(iiii)",   h_spi_transfer},
};

// ---------------- the process entry point for type=wasm packages ----------------
static int wasmRun(WasmCtx* c, const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) { pr("wasm: can't open program\n"); return 1; }
  size_t sz = f.size();
  if (sz < 8 || sz > WASM_MAX_BIN) { f.close(); prf("wasm: bad size (max %u KB)\n", (unsigned)(WASM_MAX_BIN / 1024)); return 1; }
  c->bin = (uint8_t*)wAlloc(c, sz);
  if (!c->bin) { f.close(); pr("wasm: no PSRAM for program\n"); return 1; }
  size_t got = 0;
  while (got < sz) {
    int r = f.read(c->bin + got, sz - got);
    if (r <= 0) break;
    got += r;
  }
  f.close();
  if (got != sz) { pr("wasm: read failed\n"); return 1; }

  c->env = m3_NewEnvironment();
  if (!c->env) { pr("wasm: out of memory\n"); return 1; }
  c->rt = m3_NewRuntime(c->env, WASM_RT_STACK, c);
  if (!c->rt) { pr("wasm: out of memory\n"); return 1; }
  if (c->hookHits == 0) {                            // wasm3 allocated without calling us: refuse rather than use internal RAM
    pr("wasm: allocator not patched\nsee WASM RUNTIME note\n");
    return 1;
  }

  IM3Module mod;
  M3Result r = m3_ParseModule(c->env, &mod, c->bin, (uint32_t)sz);
  if (r) { prf("wasm: bad module: %s\n", r); return 1; }
  r = m3_LoadModule(c->rt, mod);
  if (r) { m3_FreeModule(mod); prf("wasm: load: %s\n", r); return 1; }   // (a failed load leaves the module ours to free)

  for (size_t i = 0; i < sizeof(kHostFns) / sizeof(kHostFns[0]); i++) {
    r = m3_LinkRawFunction(mod, "koa", kHostFns[i].name, kHostFns[i].sig, kHostFns[i].fn);
    if (r && r != m3Err_functionLookupFailed) { prf("wasm: link %s: %s\n", kHostFns[i].name, r); return 1; }   // "not imported" is fine
  }

  IM3Function fn;
  r = m3_FindFunction(&fn, c->rt, "_start");
  if (r) r = m3_FindFunction(&fn, c->rt, "main");
  if (r) { prf("wasm: no _start or main: %s\n", r); return 1; }

  uint32_t na = m3_GetArgCount(fn);
  if (na == 0) r = m3_CallV(fn);
  else if (na == 2) r = m3_CallV(fn, (int32_t)0, (int32_t)0);   // main(argc, argv) with no arguments
  else { pr("wasm: entry takes unexpected arguments\n"); return 1; }

  if (!r) return 0;
  if (r == kExitMsg) return c->exitcode;
  if (r == kKilledMsg) return 130;
  prf("wasm: %s\n(psram %uK of %uK)\n", r, (unsigned)(c->peak / 1024), (unsigned)(c->limit / 1024));
  return 1;
}

int p_wasm(int argc, char** argv) {
  Proc* p = procSelf();
  if (!p || argc < 2) return 1;
  if (!wasmAvailable()) { pr("wasm: no PSRAM\n"); return 1; }
  WasmCtx* c = (WasmCtx*)calloc(1, sizeof(WasmCtx));
  if (!c) { pr("wasm: out of memory\n"); return 1; }
  c->p = p;
  c->limit = WASM_APP_MEM;
  p->wctx = c;
  int rc = wasmRun(c, argv[1]);
  wasmTeardown(p);                                  // runtime, module, program image: all back to PSRAM
  return rc;
}

// ================= APRS (LoRa) =================
#define APRS_FREQ  915.0   // MHz
#define APRS_BW    125.0
#define APRS_SF    9
#define APRS_CR    5       // 4/5
#define APRS_PWR   10      // dBm at the chip (the V4 PA boosts this)

struct Heard { char call[10]; char text[48]; float rssi, snr; unsigned long ms; };

SemaphoreHandle_t aprsMx = NULL;      // created in setup()
Heard heard[6];
int heardN = 0, aprsRx = 0, aprsTx = 0;
char aprsId[10] = "KOANODE";
char aprsTxMsg[64];
volatile bool aprsTxPending = false;
unsigned long aprsLogBytes = 0;

void aprsLog(const char* tag, float rssi, const char* txt) {
  int fd = sys_open("/aprs.log", K_APP);
  if (fd < 0) return;
  char b[160];
  int n = snprintf(b, sizeof(b), "%lu %s %.0f %s\n", millis() / 1000, tag, rssi, txt);
  if (n > (int)sizeof(b) - 1) n = sizeof(b) - 1;
  sys_write(fd, b, n);
  sys_close(fd);
  aprsLogBytes += n;
  if (aprsLogBytes > 20000) { sys_unlink("/aprs.log"); aprsLogBytes = 0; }   // crude rotate
}

// LoRa-APRS frame = "<" 0xFF 0x01 + TNC2 text
int aprsPosPacket(char* out, int max, const GpsInfo& g, const char* cmt) {
  double lat = fabs(g.lat), lng = fabs(g.lng);
  int la = (int)lat, lo = (int)lng;
  double lm = (lat - la) * 60.0, om = (lng - lo) * 60.0;
  int kn = (int)(g.mph * 0.868976 + 0.5);
  return snprintf(out, max, "<\xFF\x01%s>APRS:!%02d%05.2f%c/%03d%05.2f%c>%03d/%03d%s",
                  aprsId, la, lm, g.lat >= 0 ? 'N' : 'S', lo, om, g.lng >= 0 ? 'E' : 'W',
                  (int)g.crs, kn, cmt);
}

unsigned long aprsInterval(double mph) {     // poor man's smart beaconing
  if (mph < 3)  return 300000;
  if (mph < 25) return 90000;
  return 45000;
}

bool aprsTryTx(const char* s, int n) {       // CAD first; false = channel busy or tx failed, retry later
  if (sys_lora_cad() != 0) return false;
  if (sys_lora_tx(s, n) < 0) return false;
  aprsTx++;
  aprsLog("TX", 0, s + 3);
  return true;
}

void aprsHandleRx(uint8_t* buf, int n, float rssi, float snr) {   // buf[n] must be 0
  char* t = (char*)buf;
  if (n >= 3 && buf[0] == '<' && buf[1] == 0xFF && buf[2] == 0x01) t += 3;
  char* gt = strchr(t, '>');
  if (!gt) return;                                  // not TNC2-looking, ignore
  aprsRx++;
  aprsLog("RX", rssi, t);

  char call[10];
  int cl = gt - t; if (cl > 9) cl = 9;
  memcpy(call, t, cl); call[cl] = 0;

  xSemaphoreTake(aprsMx, portMAX_DELAY);
  int slot = -1;
  for (int i = 0; i < heardN; i++) if (!strcmp(heard[i].call, call)) { slot = i; break; }
  if (slot < 0) {
    if (heardN < 6) slot = heardN++;
    else { slot = 0; for (int i = 1; i < 6; i++) if (heard[i].ms < heard[slot].ms) slot = i; }
  }
  strlcpy(heard[slot].call, call, sizeof(heard[slot].call));
  strlcpy(heard[slot].text, gt + 1, sizeof(heard[slot].text));
  heard[slot].rssi = rssi; heard[slot].snr = snr; heard[slot].ms = millis();
  xSemaphoreGive(aprsMx);
}

// daemon: start with "aprs start [ID]". It takes the radio and the GPS through the
// resource API (both are released automatically when it exits).
int p_aprsd(int argc, char** argv) {
  if (argc > 1) strlcpy(aprsId, argv[1], sizeof(aprsId));

  int rc = resAcquire(KC_LORA);
  if (rc != KE_OK) { prf("aprsd: radio %s\n", rc == KE_BUSY ? "is busy (another app owns it)" : "init failed"); return 1; }
  resAcquire(KC_GPS);                        // for beacons; failure just means no beacons

  koa_lora_cfg_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.freq_mhz = APRS_FREQ; cfg.bw_khz = APRS_BW;
  cfg.sf = APRS_SF; cfg.cr = APRS_CR; cfg.sync_word = 0x12;
  cfg.power_dbm = APRS_PWR; cfg.preamble = 8;
  if (sys_lora_config(&cfg) != KE_OK) { pr("aprsd: radio config rejected\n"); return 1; }
  heardN = 0; aprsRx = 0; aprsTx = 0; aprsTxPending = false;

  unsigned long nextBeacon = millis() + 10000;
  unsigned long nextTry = 0;
  uint8_t rb[200];
  for (;;) {
    float rssi = 0, snr = 0;
    int rn = sys_lora_rx(rb, sizeof(rb) - 1, 20, &rssi, &snr);   // also where a kill request is noticed
    if (rn > 0) { rb[rn] = 0; aprsHandleRx(rb, rn, rssi, snr); }
    unsigned long now = millis();
    if ((long)(now - nextTry) < 0) continue;

    char pkt[160];
    if (aprsTxPending) {
      int n = snprintf(pkt, sizeof(pkt), "<\xFF\x01%s>APRS:>%s", aprsId, aprsTxMsg);
      if (aprsTryTx(pkt, n)) aprsTxPending = false;
      else nextTry = now + 200 + random(800);
    } else if ((long)(now - nextBeacon) >= 0) {
      GpsInfo g; sys_gpsinfo(&g);
      if (!g.locValid) { nextBeacon = now + 15000; continue; }
      int n = aprsPosPacket(pkt, sizeof(pkt), g, " KoaOS");
      if (aprsTryTx(pkt, n)) nextBeacon = now + aprsInterval(g.mph) + random(5000);
      else nextTry = now + 200 + random(800);
    }
  }
}

// viewer
void aprsDraw() {
  char l[40];
  String ser = "\x1b[H\x1b[2J";
  display.clearBuffer();
  display.setFont(u8g2_font_5x8_tr);
  snprintf(l, sizeof(l), "%.8s rx%d tx%d", aprsId, aprsRx, aprsTx);
  display.drawStr(0, 7, l);
  ser += l; ser += "\r\n";
  xSemaphoreTake(aprsMx, portMAX_DELAY);
  for (int i = 0; i < heardN; i++) {
    Heard& h = heard[i];
    snprintf(l, sizeof(l), "%-9.9s %4d %+3d %4lus", h.call, (int)h.rssi, (int)h.snr, (millis() - h.ms) / 1000);
    display.drawStr(0, (i + 1) * 8 + 7, l);
    ser += l; ser += "  "; ser += h.text; ser += "\r\n";
  }
  xSemaphoreGive(aprsMx);
  display.sendBuffer();
  ser += "q = back";
  sys_write(1, ser.c_str(), ser.length());
}

int aprsPid() {
  int pid, ppid; char st, name[25];
  for (int i = 0; sys_psinfo(i, &pid, &ppid, &st, name) == 1; i++)
    if (!strcmp(name, "aprsd")) return pid;
  return 0;
}

int p_aprs(int argc, char** argv) {
  const char* cmd = argc > 1 ? argv[1] : "status";
  int pid = aprsPid();

  if (!strcmp(cmd, "start")) {
    if (pid) { pr("aprsd already running\n"); return 1; }
    int np = sys_spawn("aprsd", argc > 2 ? argv[2] : "", -1, true);
    if (np < 0) { pr("aprs: cannot spawn\n"); return 1; }
    prf("aprsd started, pid %d\n", np);
    return 0;
  }
  if (!strcmp(cmd, "stop")) {
    if (!pid) { pr("aprsd not running\n"); return 1; }
    sys_kill(pid);
    pr("aprsd stopped\n");
    return 0;
  }
  if (!strcmp(cmd, "send")) {
    if (!pid) { pr("aprsd not running\n"); return 1; }
    if (argc < 3) { pr("usage: aprs send \"text\"\n"); return 1; }
    if (aprsTxPending) { pr("aprs: previous message still queued\n"); return 1; }
    strlcpy(aprsTxMsg, argv[2], sizeof(aprsTxMsg));
    aprsTxPending = true;
    return 0;
  }
  if (!strcmp(cmd, "view")) {
    if (!pid) { pr("aprsd not running\n"); return 1; }
    sys_ttyctl(TTY_RAW | TTY_NOSIG | TTY_NOSCREEN | TTY_GFX);
    for (;;) {
      aprsDraw();
      int c = sys_getc_t(1000);
      if (c == 'q' || c == 3) break;
    }
    sys_ttyctl(0);
    pr("\x1b[2J\x1b[H");
    return 0;
  }
  if (pid) prf("aprsd running, pid %d  rx%d tx%d\n", pid, aprsRx, aprsTx);
  else pr("aprsd not running\n");
  return 0;
}

// ================= PROGRAM TABLE =================
// ================= PACKAGE INSTALLER =================
#define MAXPKG 1048576   // largest single download (bytes)

// Download url -> LittleFS file dest. Returns bytes written, or:
//  -1 can't reach server, -2 can't write file, -3 cancelled (Ctrl-C),
//  -4 timeout / connection dropped, -5 too big, -6 not enough free space, -<100..599> = HTTP error code
// Uses LittleFS directly (not sys_write) so a Ctrl-C can't leave the socket open.
int dlFile(const char* url, const char* dest, int spaceFactor) {   // spaceFactor: free space needed = size * factor + 8K
  Proc* me = procSelf();
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  if (!http.begin(url)) return -1;
  int code = http.GET();
  if (code != 200) { http.end(); return code > 0 ? -code : -1; }
  int total = http.getSize();            // -1 if the server didn't say
  if (total > 0) {
    unsigned long fr = LittleFS.totalBytes() - LittleFS.usedBytes();
    if ((unsigned long)total * spaceFactor + 8192 > fr) { http.end(); return -6; }   // keep a little headroom
  }
  WiFiClient* st = http.getStreamPtr();
  File f = LittleFS.open(dest, "w");
  if (!f) { http.end(); return -2; }
  uint8_t buf[512];
  int got = 0, rc = 0;
  unsigned long last = millis();
  while (http.connected() && (total < 0 || got < total)) {
    if (me && me->killed) { rc = -3; break; }
    int av = st->available();
    if (av > 0) {
      int n = st->readBytes(buf, av > (int)sizeof(buf) ? (int)sizeof(buf) : av);
      if (n > 0) {
        f.write(buf, n);
        got += n;
        last = millis();
        if (got > MAXPKG) { rc = -5; break; }
      }
    } else {
      if (millis() - last > 8000) { rc = -4; break; }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  if (rc == 0 && total >= 0 && got < total) rc = -4;
  f.close();
  http.end();
  if (rc < 0) { LittleFS.remove(dest); return rc; }
  return got;
}

bool repoGet(char* out, int n) {
  int fd = sys_open("/config/repo", K_RD);
  if (fd < 0) return false;
  int r = sys_read(fd, out, n - 1);
  sys_close(fd);
  if (r <= 0) return false;
  out[r] = 0;
  while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == '\r' || out[r - 1] == ' ' || out[r - 1] == '/')) out[--r] = 0;
  return out[0] != 0;
}

void dlError(int r) {
  if (r == -1) pr("can't reach server\n");
  else if (r == -2) pr("can't write file (disk full?)\n");
  else if (r == -3) pr("cancelled\n");
  else if (r == -4) pr("timeout / connection dropped\n");
  else if (r == -5) pr("file too big\n");
  else if (r == -6) pr("not enough free space\n");
  else if (r <= -100) prf("server said HTTP %d\n", -r);
  else pr("download failed\n");
}

// ---- app packages ----
static void rmTree(const char* path) {          // removes a file, or a directory and the files in it (packages are flat)
  File d = LittleFS.open(path);
  if (!d) return;
  if (!d.isDirectory()) { d.close(); LittleFS.remove(path); return; }
  std::vector<String> names;
  for (File e = d.openNextFile(); e; e = d.openNextFile()) {
    const char* n = e.name();
    const char* leaf = strrchr(n, '/');
    names.push_back(String(leaf ? leaf + 1 : n));
    e.close();
  }
  d.close();
  for (size_t i = 0; i < names.size(); i++) LittleFS.remove(String(path) + "/" + names[i]);
  LittleFS.rmdir(path);
}

static int appRemove(const char* name) {        // 0 = removed, -1 = wasn't installed
  char p[48];
  snprintf(p, sizeof(p), "/apps/%s", name);
  if (!LittleFS.exists(p)) return -1;
  rmTree(p);
  return 0;
}

static int pidByName(const char* want) {
  int pid, ppid;
  char st, name[25];
  for (int i = 0; sys_psinfo(i, &pid, &ppid, &st, name) == 1; i++) if (!strcmp(name, want)) return pid;
  return 0;
}

static int fioRead(void* c, uint8_t* b, int n) { return ((File*)c)->read(b, n); }
static int fioSeek(void* c, uint32_t pos) { return ((File*)c)->seek(pos) ? 0 : -1; }

// Verify and unpack an .esp from tmp into /apps/<name>/, and create /data/<name>/.
// Unpacks into /apps/.new first, so a failed or cancelled install never damages an existing app.
int pkgInstall(const char* tmp, const char* expect, char* err, int errn, esp_pkg_t* pk) {
  File f = LittleFS.open(tmp, "r");
  if (!f) { esp_err(err, errn, "can't read download"); return -1; }
  esp_io_t io;
  io.ctx = &f; io.read = fioRead; io.seek = fioSeek; io.size = f.size();
  if (esp_pkg_parse(&io, pk, err, errn) != 0) { f.close(); return -1; }
  if (strcmp(pk->m.name, expect)) {
    char m[56];
    snprintf(m, sizeof(m), "package is '%.20s', not '%.20s'", pk->m.name, expect);
    esp_err(err, errn, m);
    f.close();
    return -1;
  }
  if (findProg(pk->m.name)) { esp_err(err, errn, "built-in command"); f.close(); return -1; }

  uint32_t need = pk->manifest_len;
  for (int i = 0; i < pk->nfiles; i++) need += pk->files[i].size;
  unsigned long fr = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (need + 8192 > fr) { esp_err(err, errn, "not enough free space"); f.close(); return -1; }

  rmTree("/apps/.new");
  LittleFS.mkdir("/apps/.new");
  Proc* me = procSelf();
  uint8_t buf[256];
  bool ok = true;
  for (int i = 0; i < pk->nfiles && ok; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/apps/.new/%s", pk->files[i].name);
    File o = LittleFS.open(path, "w");
    if (!o) { esp_err(err, errn, "can't write file"); ok = false; break; }
    uint32_t left = pk->files[i].size;
    while (left) {
      if (me && me->killed) { esp_err(err, errn, "cancelled"); ok = false; break; }
      int want = left > sizeof(buf) ? (int)sizeof(buf) : (int)left;
      int n = f.read(buf, want);
      if (n <= 0) { esp_err(err, errn, "package truncated"); ok = false; break; }
      if (o.write(buf, n) != (size_t)n) { esp_err(err, errn, "disk full"); ok = false; break; }
      left -= n;
    }
    o.close();
  }
  f.close();
  if (ok) {
    File mf = LittleFS.open("/apps/.new/manifest", "w");
    if (!mf || mf.write((const uint8_t*)pk->manifest, pk->manifest_len) != (size_t)pk->manifest_len) {
      esp_err(err, errn, "can't write manifest");
      ok = false;
    }
    if (mf) mf.close();
  }
  if (!ok) { rmTree("/apps/.new"); return -1; }

  appRemove(expect);                              // replace an earlier install of the same name
  char dest[48];
  snprintf(dest, sizeof(dest), "/apps/%s", expect);
  if (!LittleFS.rename("/apps/.new", dest)) {
    rmTree("/apps/.new");
    esp_err(err, errn, "can't finish install");
    return -1;
  }
  char dd[48];
  snprintf(dd, sizeof(dd), "/data/%s", expect);
  LittleFS.mkdir(dd);
  return 0;
}

int p_install(int argc, char** argv) {
  char repo[96] = "";
  bool haveRepo = false, list = false;
  const char* name = NULL;
  const char* setRepo = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-l")) list = true;
    else if (!strcmp(argv[i], "-s") && i + 1 < argc) setRepo = argv[++i];
    else if (!strcmp(argv[i], "-r") && i + 1 < argc) { strlcpy(repo, argv[++i], sizeof(repo)); haveRepo = true; }
    else if (argv[i][0] != '-') name = argv[i];
  }

  if (setRepo) {   // install -s <url> : remember the default server
    char u[96];
    if (strstr(setRepo, "://")) strlcpy(u, setRepo, sizeof(u));
    else snprintf(u, sizeof(u), "http://%s", setRepo);
    int fd = sys_open("/config/repo", K_WR);
    if (fd < 0) { pr("install: can't save\n"); return 1; }
    sys_write(fd, u, strlen(u));
    sys_close(fd);
    prf("repo: %s\n", u);
    return 0;
  }

  if (!list && !name) {
    pr("install <name>\ninstall -l  (list)\ninstall -s <url> set repo\ninstall -r <url> <name>\n");
    return 1;
  }

  if (haveRepo && !strstr(repo, "://")) {
    char u[96];
    snprintf(u, sizeof(u), "http://%s", repo);
    strlcpy(repo, u, sizeof(repo));
  }
  if (!haveRepo && !repoGet(repo, sizeof(repo))) {
    pr("no repo set. use:\ninstall -s <ip:port>\n");
    return 1;
  }

  char ssid[33], ip[20];
  int rssi;
  if (!sys_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi)) {
    pr("wifi not connected\n(see: wifi)\n");
    return 1;
  }

  char url[160];
  if (list) {
    snprintf(url, sizeof(url), "%s/index.txt", repo);
    int r = dlFile(url, "/config/index.tmp", 1);
    if (r < 0) { dlError(r); return 1; }
    int fd = sys_open("/config/index.tmp", K_RD);
    if (fd >= 0) {
      char b[64];
      int n;
      while ((n = sys_read(fd, b, sizeof(b))) > 0) sys_write(1, b, n);
      sys_close(fd);
    }
    sys_unlink("/config/index.tmp");
    return 0;
  }

  if (!validAppName(name)) { pr("install: bad name\n"); return 1; }
  if (findProg(name)) { pr("install: built-in command\n"); return 1; }
  if (pidByName(name)) { pr("install: it's running, kill it first\n"); return 1; }

  prf("getting %s...\n", name);
  snprintf(url, sizeof(url), "%s/%s.esp", repo, name);
  int r = dlFile(url, "/config/dl.tmp", 2);            // a package needs room for itself plus its unpacked copy
  if (r == -404) {                                      // no package by that name: fall back to a plain script
    snprintf(url, sizeof(url), "%s/%s", repo, name);
    r = dlFile(url, "/config/dl.tmp", 1);
    if (r < 0) { dlError(r); return 1; }
    appRemove(name);
    char dest[48];
    snprintf(dest, sizeof(dest), "/apps/%s", name);
    if (!LittleFS.rename("/config/dl.tmp", dest)) {
      LittleFS.remove("/config/dl.tmp");
      pr("install: can't save app\n");
      return 1;
    }
    prf("installed %s (script, %d bytes)\n", name, r);
    return 0;
  }
  if (r < 0) { dlError(r); return 1; }

  esp_pkg_t pk;
  char err[56] = "";
  int rc = pkgInstall("/config/dl.tmp", name, err, sizeof(err), &pk);
  LittleFS.remove("/config/dl.tmp");
  if (rc != 0) { prf("install: %s\n", err); return 1; }
  prf("installed %s %s (%s, %d files)\n", name, pk.m.version, pk.m.type, pk.nfiles);
  if (pk.m.reqs[0]) prf("it uses: %s\n", pk.m.reqs);
  return 0;
}

int p_uninstall(int argc, char** argv) {
  bool purge = false;
  const char* name = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-p")) purge = true;
    else name = argv[i];
  }
  if (!name) { pr("usage: uninstall [-p] <name>\n-p also deletes its /data\n"); return 1; }
  if (!validAppName(name)) { pr("uninstall: bad name\n"); return 1; }
  if (pidByName(name)) { pr("uninstall: it's running, kill it first\n"); return 1; }
  if (appRemove(name) != 0) { pr("uninstall: not installed\n"); return 1; }
  char dd[48];
  snprintf(dd, sizeof(dd), "/data/%s", name);
  if (purge) { rmTree(dd); prf("removed %s and its data\n", name); }
  else if (LittleFS.exists(dd)) prf("removed %s (data kept in %s, -p deletes it)\n", name, dd);
  else prf("removed %s\n", name);
  return 0;
}

// apps : what is installed
int p_apps(int, char**) {
  char name[48];
  int size;
  if (sys_readdir("/apps", 0, name, sizeof(name), &size) < 0) { pr("apps: no /apps\n"); return 1; }
  int shown = 0;
  for (int i = 0; sys_readdir("/apps", i, name, sizeof(name), &size) == 1; i++) {
    const char* leaf = strrchr(name, '/');
    leaf = leaf ? leaf + 1 : name;
    if (leaf[0] == '.') continue;                       // staging leftovers
    char path[64];
    snprintf(path, sizeof(path), "/apps/%s", leaf);
    File f = LittleFS.open(path, "r");
    bool isdir = f && f.isDirectory();
    f.close();
    shown++;
    if (!isdir) { prf("%-14s script\n", leaf); continue; }
    esp_manifest_t m;
    if (pkgLoadManifest(leaf, &m) == 0) prf("%-14s %-6s %s%s%s\n", leaf, m.version, m.type, m.reqs[0] ? "  uses " : "", m.reqs);
    else prf("%-14s damaged package\n", leaf);
  }
  if (!shown) pr("no apps installed\n");
  return 0;
}

// ================= WEATHER =================
// weather            : GPS fix (or last saved position) -> current conditions
// weather <lat> <lon>: use these coordinates instead
// weather -c         : metric (C, km/h); default is F and mph
// Data from open-meteo.com (free, no key). https without certificate checking.

const char* wxName(int c) {
  if (c == 0) return "Clear sky";
  if (c == 1) return "Mostly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Rain showers";
  if (c == 85 || c == 86) return "Snow showers";
  if (c >= 95) return "Thunderstorm";
  return "Unknown";
}

// find "key": after position 'from' in a JSON string and read the number
bool jnum(const String& s, int from, const char* key, double* out) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k, from);
  if (i < 0) return false;
  *out = s.substring(i + k.length()).toFloat();
  return true;
}

bool locLoad(double* lat, double* lng) {
  int fd = sys_open("/config/loc", K_RD);
  if (fd < 0) return false;
  char b[48];
  int n = sys_read(fd, b, sizeof(b) - 1);
  sys_close(fd);
  if (n <= 0) return false;
  b[n] = 0;
  char* comma = strchr(b, ',');
  if (!comma) return false;
  *lat = atof(b);
  *lng = atof(comma + 1);
  return true;
}

void locSave(double lat, double lng) {
  char b[48];
  snprintf(b, sizeof(b), "%.5f,%.5f", lat, lng);
  int fd = sys_open("/config/loc", K_WR);
  if (fd < 0) return;
  sys_write(fd, b, strlen(b));
  sys_close(fd);
}

int p_weather(int argc, char** argv) {
  bool metric = false;
  int nums = 0;
  double nv[2] = {0, 0};
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-c")) { metric = true; continue; }
    if (nums < 2 && strspn(argv[i], "0123456789.-+") == strlen(argv[i])) { nv[nums++] = atof(argv[i]); continue; }
    pr("usage: weather [-c] [lat lon]\n");
    return 1;
  }
  if (nums == 1) { pr("usage: weather [-c] [lat lon]\n"); return 1; }

  char ssid[33], ip[20];
  int rssi;
  if (!sys_wifi_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi)) {
    pr("wifi not connected\n(see: wifi)\n");
    return 1;
  }

  double lat = 0, lng = 0;
  const char* src = "";
  if (nums == 2) {
    lat = nv[0]; lng = nv[1]; src = "manual";
    if (lat < -90 || lat > 90 || lng < -180 || lng > 180) { pr("weather: bad coordinates\n"); return 1; }
  } else {
    Proc* me = procSelf();
    GpsInfo g;
    sys_gpsinfo(&g);
    bool haveGps = resAcquire(KC_GPS) == KE_OK;   // powers it on if needed
    sys_gpsinfo(&g);
    if (!g.locValid) pr("waiting for gps...\n");
    for (int t = 0; t < 40; t++) {            // up to ~20 s
      sys_gpsinfo(&g);
      if (g.locValid) break;
      if (me && me->killed) return 130;       // procExit() hands the gps back
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (haveGps) resRelease(KC_GPS);          // don't keep it powered during the http request
    if (g.locValid) {
      lat = g.lat; lng = g.lng; src = "gps";
      locSave(lat, lng);
    } else if (locLoad(&lat, &lng)) {
      src = "saved";
    } else {
      pr("no gps fix, nothing saved\ntry: weather <lat> <lon>\n");
      return 1;
    }
  }

  char url[300];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m%s",
    lat, lng, metric ? "" : "&temperature_unit=fahrenheit&wind_speed_unit=mph");

  String body = "";
  int code = -1;
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    if (http.begin(client, url)) {
      code = http.GET();
      if (code == 200) body = http.getString();
      http.end();
    }
  }
  if (code != 200) {
    if (code < 0) prf("weather: %s\n", HTTPClient::errorToString(code).c_str());
    else prf("weather: server said HTTP %d\n", code);
    return 1;
  }

  int cur = body.indexOf("\"current\":{");
  double t, ft, hum, wc, wind;
  if (cur < 0 || !jnum(body, cur, "temperature_2m", &t) || !jnum(body, cur, "apparent_temperature", &ft) ||
      !jnum(body, cur, "relative_humidity_2m", &hum) || !jnum(body, cur, "weather_code", &wc) ||
      !jnum(body, cur, "wind_speed_10m", &wind)) {
    pr("weather: bad reply\n");
    return 1;
  }

  const char* u = metric ? "C" : "F";
  prf("Weather %.2f,%.2f\n", lat, lng);
  prf("%s (%s)\n", wxName((int)wc), src);
  prf("Temp %.0f%s Feels %.0f%s\n", t, u, ft, u);
  prf("Humidity %.0f%%\n", hum);
  prf("Wind %.0f %s\n", wind, metric ? "km/h" : "mph");
  pr("Data: open-meteo.com\n");
  return 0;
}

const Prog progs[] = {
  {"help", p_help, 0}, {"clear", p_clear, 0}, {"uname", p_uname, 0},
  {"whoami", p_whoami, 0}, {"uptime", p_uptime, 0}, {"free", p_free, 0},
  {"df", p_df, 0}, {"ls", p_ls, 0}, {"cat", p_cat, 0}, {"rm", p_rm, 0},
  {"echo", p_echo, 0}, {"date", p_date, 0}, {"reboot", p_reboot, 0},
  {"gps", p_gps, 0}, {"edit", p_edit, 0}, {"nano", p_edit, 0}, {"vi", p_edit, 0},
  {"neofetch", p_neofetch, 0}, {"fetch", p_neofetch, 0},
  {"wifi", p_wifi, 0}, {"aprs", p_aprs, 0}, {"aprsd", p_aprsd, 0},
  {"ps", p_ps, 0}, {"pins", p_pins, 0}, {"kill", p_kill, 0}, {"sleep", p_sleep, 0},
  {"run", p_run, 0}, {"install", p_install, PF_BIGSTACK}, {"uninstall", p_uninstall, 0}, {"apps", p_apps, 0},
  {"weather", p_weather, PF_BIGSTACK},
  {"login", p_login, PF_NOKILL}, {"sh", p_sh, PF_NOKILL},
  {NULL, NULL, 0}
};

const Prog* findProg(const char* n) {
  for (int i = 0; progs[i].name; i++) if (!strcmp(progs[i].name, n)) return &progs[i];
  return NULL;
}

// ================= BOOT =================
void bootMsg(const char* msg, int ms) {
  unsigned long t = millis();
  kprintf("[%2lu.%03lu] %s\n", t / 1000, t % 1000, msg);
  drawTerm();
  delay(ms);
}

void kstartInit() {
  Proc* p = &procs[0];
  p->used = true;
  p->pid = nextPid++;
  p->ppid = 0;
  strlcpy(p->name, "init", sizeof(p->name));
  p->args[0] = 0;
  p->killed = false;
  p->allowed = 0xFFFFFFFFu;
  p->held = 0;
  p->state = P_RUN;
  p->flags = PF_NOKILL;
  p->entry = p_init;
  for (int i = 0; i < 3; i++) p->fd[i].type = FD_TTY;
  fgpid = p->pid;
  TaskHandle_t h;
  xTaskCreatePinnedToCore(procMain, "init", 8192, p, 1, &h, 0);
}

void setup() {
  Serial.begin(115200);
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);
  pinMode(GNSS_PWR, OUTPUT);
  digitalWrite(GNSS_PWR, HIGH);  // GPS off until "gps start"
  delay(100);

  display.begin();
  memset(scr, 0, sizeof(scr));
  ttyMx = xSemaphoreCreateMutex();
  ttyIn = xQueueCreate(256, sizeof(char));
  aprsMx = xSemaphoreCreateMutex();
  gpsMx = xSemaphoreCreateMutex();
  resMx = xSemaphoreCreateMutex();
  ipcMx = xSemaphoreCreateMutex();

  bootMsg(OSNAME " kernel " OSVER, 300);
  bootMsg("cpu: ESP32-S3 2 cores", 200);
  if (wasmAvailable()) { char m[32]; snprintf(m, sizeof(m), "mem: psram %uK", (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024)); bootMsg(m, 200); }
  else bootMsg("mem: no psram, wasm off", 200);
  bootMsg("tty0: oled+usb ready", 200);
  bool ok = LittleFS.begin(true);
  bootMsg(ok ? "vfs: rootfs mounted" : "vfs: mount FAILED", 300);
  if (ok) { LittleFS.mkdir("/apps"); LittleFS.mkdir("/config"); LittleFS.mkdir("/data"); rmTree("/apps/.new"); }   // .new = an install that was interrupted
  bootMsg("gps: standby", 200);
  bootMsg("proc: starting init", 300);
  kprintf("\n%s %s tty1\n\n", OSNAME, OSVER);
  kstartInit();
}

void loop() {
  if (gpsOn && xSemaphoreTake(gpsMx, 0) == pdTRUE) {      // never block the main loop; skip a round if someone holds the lock
    while (gpsOn && GPSSerial.available()) {
      char c = GPSSerial.read();
      gps.encode(c);
      if (gpsReaders > 0) { gpsRing[gWritten % 512] = (uint8_t)c; gWritten++; }
    }
    xSemaphoreGive(gpsMx);
  }

  while (Serial.available()) ttyInputChar((char)Serial.read());

  unsigned long now = millis();
  static unsigned long lastReap = 0;
  if (now - lastReap > 250) { lastReap = now; wasmReaper(); }   // enforces kill on wasm apps stuck in pure computation
  if (now - lastBlink > 500) { lastBlink = now; blinkOn = !blinkOn; dirty = true; }

  if (dirty && !(ttyFlags & TTY_GFX) && now - lastDraw > 40) {
    xSemaphoreTake(ttyMx, portMAX_DELAY);
    if (!(ttyFlags & TTY_GFX)) {
      dirty = false;
      lastDraw = now;
      drawTerm();
    }
    xSemaphoreGive(ttyMx);
  }
  delay(1);
}
