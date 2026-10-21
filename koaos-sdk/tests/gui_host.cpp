/*
 * gui_host.cpp -- renders the KoaOS GUI on a PC. Compiles ../../KoaOS/koa_gui.h against stand-ins for the OS calls,
 * feeds it a scripted key sequence and writes a screenshot (PPM, 640x480) whenever the script says so.
 *   g++ -std=c++17 -Wall -I../../KoaOS gui_host.cpp -o gui_host && ./gui_host shots/   (then see gui_shots.py)
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <algorithm>
#include <string>
#include <map>
#include <set>
#include <vector>

#define OSNAME "KoaOS"
#define OSVER  "0.12"
#define MAXPROC 8
#define PF_WASM 8
#define TTY_RAW 1
#define TTY_NOECHO 2
#define TTY_NOSIG 4
#define TTY_NOSCREEN 8
#define TTY_GFX 16
#define K_RD 0
#define K_WR 1
#define K_APP 2
#define KC_GPS 1
#define KE_OK 0
#define KE_BUSY (-5)
struct GpsInfo { bool on, locValid, timeValid; int sats, y, mo, d, h, mi; double lat, lng, mph, alt, crs; };
struct Proc { bool used = false; int pid = 0; char name[25] = ""; int flags = 0; };
struct esp_manifest_t { char version[12]; char type[8]; };
static Proc procs[MAXPROC];
static volatile int fgpid = 0, guiPid = 0;

// ---- fake file system ----
static std::map<std::string, std::string> files;
static std::set<std::string> dirs = {"/", "/apps", "/config", "/data", "/apps/gpswatch", "/apps/meshcore"};
static std::map<int, std::pair<std::string, size_t>> fds;
static int nextFd = 3;
static std::string parentOf(const std::string& p) { size_t i = p.rfind('/'); return i == 0 ? "/" : p.substr(0, i); }
int sys_open(const char* p, int mode) {
  std::string s = p;
  if (mode == K_RD && !files.count(s)) return -1;
  if (mode == K_WR) files[s] = "";
  fds[nextFd] = {s, 0};
  return nextFd++;
}
int sys_read(int fd, char* b, int n) {
  auto& f = fds[fd]; const std::string& d = files[f.first];
  int k = (int)std::min<size_t>(n, d.size() - f.second);
  memcpy(b, d.data() + f.second, k); f.second += k; return k;
}
int sys_write(int fd, const char* b, int n) { files[fds[fd].first].append(b, n); return n; }
void sys_close(int fd) { fds.erase(fd); }
int sys_unlink(const char* p) { return files.erase(p) ? 0 : -1; }
int sys_readdir(const char* dir, int idx, char* name, int nmax, int* size) {
  std::vector<std::pair<std::string, int>> ents;
  for (auto& d : dirs) if (d != "/" && parentOf(d) == dir) ents.push_back({d.substr(d.rfind('/') + 1), 0});
  for (auto& f : files) if (parentOf(f.first) == dir) ents.push_back({f.first.substr(f.first.rfind('/') + 1), (int)f.second.size()});
  if (idx >= (int)ents.size()) return 0;
  strlcpy(name, ents[idx].first.c_str(), nmax); *size = ents[idx].second; return 1;
}
static bool guiIsDir(const char* p) { return dirs.count(p) > 0; }
static void* guiAlloc(size_t n) { return malloc(n); }
static void guiFree(void* p) { free(p); }
static void guiPsram(unsigned long* f, unsigned long* t) { *f = 7800 * 1024; *t = 8192 * 1024; }
static void guiOled() {}

#include "koa_gfx.h"

// ---- scripted keys + screenshots ----
static std::vector<int> script;
static size_t sp = 0;
static const char* outDir = ".";
static int shotNo = 0;
enum { SHOT = -2, TIMEOUT = -3 };
static void screenshot() {
  static const uint8_t pal[8][3] = {{0,0,0},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255}};
  char fn[256]; snprintf(fn, sizeof(fn), "%s/shot%02d.ppm", outDir, shotNo++);
  FILE* f = fopen(fn, "wb"); fprintf(f, "P6\n640 480\n255\n");
  for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) {
    int sx = x / 2, sy = y / 2; uint8_t b = gfxFb[sy * GFX_STRIDE + (sx >> 1)];
    int c = (sx & 1) ? (b >> 4) & 7 : b & 7; fwrite(pal[c], 1, 3, f);
  }
  fclose(f);
}

int sys_getc_t(int) {
  for (;;) {
    if (sp >= script.size()) return 3;                      // script over: Ctrl-C leaves the desktop
    int v = script[sp++];
    if (v == SHOT) { screenshot(); continue; }
    if (v == TIMEOUT) return -1;
    return v;
  }
}
unsigned long sys_uptime() { return 3723000; }
void sys_meminfo(unsigned long* f, unsigned long* t) { *f = 150 * 1024; *t = 330 * 1024; }
void sys_fsinfo(unsigned long* u, unsigned long* t) { *u = 180 * 1024; *t = 2000 * 1024; }
void sys_gpsinfo(GpsInfo* g) { *g = GpsInfo{true, true, true, 9, 2026, 10, 17, 14, 32, 37.77493, -122.41942, 23.4, 16, 270}; }
static bool wifiUp = false;
int sys_wifi_scan() { return 4; }
int sys_wifi_entry(int i, char* ssid, int n, int* rssi, int* open) {
  static const char* nm[4] = {"HomeNet", "CoffeeShop-Guest", "", "Neighbor_5G"};
  static const int r[4] = {-48, -63, -71, -80}; if (i < 0 || i >= 4) return 0;
  strlcpy(ssid, nm[i], n); *rssi = r[i]; *open = (i == 1); return 1; }
int sys_wifi_connect(const char*, const char*, int) { wifiUp = true; return 0; }
int sys_wifi_status(char* s, int n, char* ip, int m, int* rssi) { if (!wifiUp) return 0; strlcpy(s, "HomeNet", n); strlcpy(ip, "192.168.1.44", m); *rssi = -48; return 1; }
void sys_wifi_off() { wifiUp = false; }
void sys_reboot() {}
void sys_sleep(int) {}
int sys_getpid() { return 5; }
int sys_spawn(const char*, const char*, int, bool) { return -2; }
int sys_wait(int) { return 0; }
void sys_ttyctl(int) {}
int resAcquire(uint32_t) { return 0; }
int resRelease(uint32_t) { return 0; }
void pr(const char* s) { printf("[console] %s", s); }
void prf(const char* f, ...) { va_list a; va_start(a, f); printf("[console] "); vprintf(f, a); va_end(a); }
int readline(char* b, int) { b[0] = 0; return 0; }
static bool vgaOn = true;
static inline bool vgaActive() { return vgaOn; }
static int vgaEnable(const char**) { return 0; }
static void vgaDisable() {}
static bool vgaGfxBegin() { gfxFb = (uint8_t*)calloc(GFX_BYTES, 1); return gfxFb; }
static void vgaGfxEnd() {}
int pkgLoadManifest(const char*, esp_manifest_t* m) { strcpy(m->version, "1.0"); strcpy(m->type, "wasm"); return 0; }

#include "koa_gui.h"

static void K(int c) { script.push_back(c); }
static void keys(const char* s) { for (; *s; s++) K(*s); }
static void UP() { K(27); K('['); K('A'); }
static void DOWN() { K(27); K('['); K('B'); }
static void RIGHT() { K(27); K('['); K('C'); }
static void LEFT() { K(27); K('['); K('D'); }
static void ESCK() { K(27); K(TIMEOUT); }
static void ENTER() { K('\r'); }
static void SH() { K(SHOT); }

int main(int argc, char** argv) {
  if (argc > 1) outDir = argv[1];
  files["/notes.txt"] = "Shopping list\n- batteries\n- coax cable\n- VGA adapter\n\nRemember to flash the V4 again.\n";
  files["/hello"] = "echo hello from KoaOS\n";
  files["/config/vga"] = "1";
  SH();                                                       // 0 desktop
  ENTER(); SH(); DOWN(); DOWN(); DOWN(); DOWN(); SH(); ENTER(); SH();   // 1 files, 2 selected, 3 editor
  keys("Hi! "); SH(); K(19); ESCK(); ESCK();                  // 4 typed + saved
  RIGHT(); ENTER(); SH(); DOWN(); DOWN(); ENTER(); keys("secret"); SH(); ENTER(); SH(); K('x'); SH(); ESCK();   // 5 wifi list, 6 password, 7 connected msg, 8 list
  RIGHT(); ENTER(); SH(); ESCK();                             // 8 gps
  RIGHT(); ENTER(); SH(); ESCK();                             // 9 system
  DOWN(); ENTER(); SH(); ESCK();                              // 10 power menu
  LEFT(); LEFT(); LEFT(); ENTER(); SH(); ESCK();              // 11 apps
  RIGHT(); RIGHT(); ENTER(); ENTER(); SH(); ESCK(); SH();     // 12 editor opened on notes.txt, 13 desktop again
  guiPid = 5;
  if (!guiEnterGfx()) return 1;
  int rc = guiRun();
  printf("rc=%d shots=%d notes.txt=[%s]\n", rc, shotNo, files["/notes.txt"].c_str());
  return 0;
}
