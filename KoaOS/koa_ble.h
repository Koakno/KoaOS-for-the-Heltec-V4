// koa_ble.h - Bluetooth LE keyboard for the console (EXPERIMENTAL).
//
// The V4 acts as a BLE central: it pairs with a BLE HID keyboard, subscribes to its input reports and turns them into the same
// bytes a serial terminal sends (koa_hid.h), so the shell, the editor, the gui and every app work with it unchanged. Works on the
// OLED, on VGA and over USB at the same time. BLE is OFF until you type "ble on" (or "ble boot on"), so it costs nothing otherwise.
//
// Needs the NimBLE-Arduino library, version 2.5.1 (Library Manager). Build with  #define KOA_NO_BLE  to leave it all out.
//
//   ble on | off            start / stop the BLE stack
//   ble scan [all]          list keyboards in range (put the keyboard in pairing mode first)
//   ble pair <n>            pair with entry n of the last scan; the keyboard is remembered and reconnects by itself
//   ble status | forget | boot on|off
//
// Threading: the BLE host and a small manager task (koable) do all the Bluetooth work. They never touch the console input;
// they only push 8-byte reports into a queue. bleKeyPump(), called from loop(), turns those into bytes and calls ttyInputChar()
// from the same thread that reads the USB serial port, so the line editor needs no extra locking.
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "koa_hid.h"

void kprintf(const char* fmt, ...);       // defined in KoaOS.ino
void ttyInputChar(char c);

#define BLE_ADDR_FILE  "/config/blekbd"   // "AA:BB:CC:DD:EE:FF <addr type>" of the paired keyboard
#define BLE_BOOT_FILE  "/config/ble"      // exists = start BLE at boot

#ifndef KOA_NO_BLE
#include <NimBLEDevice.h>

struct BleFound { char addr[18]; uint8_t type; int8_t rssi; char name[24]; };
static BleFound bleFound[8];
static volatile int  bleNFound = 0;

static volatile bool bleEnabled = false;
static volatile int  bleReq = 0;           // 0 idle, 1 scan, 2 pair, 3 forget: set by the shell, cleared by the task when done
static volatile int  blePairIdx = -1;
static volatile bool bleScanAll = false;
static volatile bool bleLinked = false;
static volatile bool bleHidReset = false;  // the link dropped or came up: forget held keys
static volatile int  bleBatt = -1;         // keyboard battery %, -1 unknown
static volatile bool bleInited = false;
static char    bleAddr[18];
static uint8_t bleAddrType = 0;
static bool    bleHaveAddr = false;
static QueueHandle_t bleQ = NULL;
static TaskHandle_t  bleTaskH = NULL;
static NimBLEClient* bleCli = NULL;
static koa_hid_t     bleHid;

#define BLE_MITM 1   // 1: ask for authenticated pairing (the keyboard types a passkey we show). Keyboards without keys still pair as Just Works.

// ---- notifications: runs on the BLE host task, only queues the raw report ----
static void bleNotify(NimBLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {
  uint8_t r[8];
  if (n == 8) memcpy(r, d, 8);
  else if (n == 9) memcpy(r, d + 1, 8);    // a report ID in front of the 8 bytes
  else return;                              // consumer-control / system-control reports: not handled
  xQueueSend(bleQ, r, 0);
}

class BleCb : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int) override { bleLinked = false; bleBatt = -1; bleHidReset = true; }
  uint32_t onPassKeyDisplay(NimBLEConnInfo&) override {
    uint32_t pin = esp_random() % 1000000UL;
    kprintf("ble: type %06u on the keyboard, then press Enter\n", (unsigned)pin);
    return pin;
  }
  void onConfirmPasskey(NimBLEConnInfo& ci, uint32_t) override {   // numeric comparison: a keyboard cannot show or confirm a number
    NimBLEDevice::injectConfirmPasskey(ci, false);
    kprintf("ble: this device wants a number comparison, which a keyboard can't do\n");
  }
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    if (!ci.isEncrypted()) kprintf("ble: pairing failed (link not encrypted)\n");
  }
};
static BleCb bleCb;

static bool bleLoadAddr() {
  File f = LittleFS.open(BLE_ADDR_FILE, "r");
  if (!f) return false;
  String s = f.readStringUntil('\n');
  f.close();
  int sp = s.indexOf(' ');
  if (sp != 17) return false;
  s.substring(0, 17).toCharArray(bleAddr, sizeof(bleAddr));
  bleAddrType = (uint8_t)s.substring(sp + 1).toInt();
  bleHaveAddr = true;
  return true;
}
static void bleSaveAddr(const char* a, uint8_t type) {
  strncpy(bleAddr, a, sizeof(bleAddr) - 1); bleAddr[sizeof(bleAddr) - 1] = 0;
  bleAddrType = type; bleHaveAddr = true;
  File f = LittleFS.open(BLE_ADDR_FILE, "w");
  if (f) { f.printf("%s %u\n", bleAddr, (unsigned)type); f.close(); }
}

static bool bleStackUp() {
  if (bleInited) return true;
  if (!NimBLEDevice::init("KoaOS")) return false;
  NimBLEDevice::setSecurityAuth(true, BLE_MITM, true);          // bonding, MITM, LE Secure Connections (legacy pairing is used if the keyboard lacks it)
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);       // we display the passkey, the keyboard types it
  bleCli = NimBLEDevice::createClient();
  bleCli->setClientCallbacks(&bleCb, false);
  bleCli->setConnectTimeout(4000);
  bleInited = true;
  return true;
}
static void bleStackDown() {
  if (!bleInited) return;
  if (bleCli && bleCli->isConnected()) bleCli->disconnect();
  NimBLEDevice::deinit(true);                                   // also frees the client
  bleCli = NULL; bleInited = false; bleLinked = false; bleBatt = -1; bleHidReset = true;
}

// Connect to the keyboard at a, encrypt the link, subscribe to its key reports. verbose: print why it failed.
static bool bleOpen(const NimBLEAddress& a, bool verbose) {
  if (!bleCli->connect(a, true)) { if (verbose) kprintf("ble: could not connect (is it in pairing mode?)\n"); return false; }
  if (!bleCli->secureConnection()) {
    if (verbose) kprintf("ble: pairing failed\n");
    bleCli->disconnect();
    if (NimBLEDevice::isBonded(a)) NimBLEDevice::deleteBond(a);  // the keyboard forgot us: drop our stale key so the next try pairs afresh
    return false;
  }
  NimBLERemoteService* hid = bleCli->getService(NimBLEUUID((uint16_t)0x1812));
  if (!hid) { if (verbose) kprintf("ble: no HID service, not a keyboard\n"); bleCli->disconnect(); return false; }
  int subs = 0;
  for (NimBLERemoteCharacteristic* c : hid->getCharacteristics(true)) {
    if (!(c->getUUID() == NimBLEUUID((uint16_t)0x2A22) || c->getUUID() == NimBLEUUID((uint16_t)0x2A4D))) continue;   // boot input / report
    if (c->canNotify() && c->subscribe(true, bleNotify)) subs++;
  }
  if (!subs) { if (verbose) kprintf("ble: keyboard offers no input reports\n"); bleCli->disconnect(); return false; }
  bleBatt = -1;
  NimBLERemoteService* bs = bleCli->getService(NimBLEUUID((uint16_t)0x180F));
  if (bs) {
    NimBLERemoteCharacteristic* bc = bs->getCharacteristic(NimBLEUUID((uint16_t)0x2A19));
    if (bc && bc->canRead()) { NimBLEAttValue v = bc->readValue(); if (v.size() >= 1) bleBatt = v.data()[0]; }
  }
  bleSaveAddr(a.toString().c_str(), a.getType());
  bleHidReset = true;
  bleLinked = true;
  if (bleBatt >= 0) kprintf("ble: keyboard connected (battery %d%%)\n", (int)bleBatt);
  else kprintf("ble: keyboard connected\n");
  return true;
}

static void bleDoScan() {
  bleNFound = 0;
  NimBLEScan* sc = NimBLEDevice::getScan();
  sc->setActiveScan(true);
  sc->setInterval(100);
  sc->setWindow(90);
  sc->clearResults();
  NimBLEScanResults res = sc->getResults(6000, false);          // blocks for 6 s
  for (int i = 0; i < res.getCount() && bleNFound < 8; i++) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    bool kb = d->isAdvertisingService(NimBLEUUID((uint16_t)0x1812)) || (d->haveAppearance() && d->getAppearance() == 0x03C1);
    if (!kb && !bleScanAll) continue;
    BleFound& f = bleFound[bleNFound++];
    strncpy(f.addr, d->getAddress().toString().c_str(), sizeof(f.addr) - 1); f.addr[sizeof(f.addr) - 1] = 0;
    f.type = d->getAddressType();
    f.rssi = d->getRSSI();
    std::string nm = d->getName();
    strncpy(f.name, nm.empty() ? "(no name)" : nm.c_str(), sizeof(f.name) - 1); f.name[sizeof(f.name) - 1] = 0;
  }
  sc->clearResults();
  if (!bleNFound) kprintf("ble: nothing found. Put the keyboard in pairing mode (and disconnect it from other devices), then retry. \"ble scan all\" lists every device.\n");
  for (int i = 0; i < bleNFound; i++) kprintf("%d  %s  %s  rssi %d\n", i, bleFound[i].addr, bleFound[i].name, (int)bleFound[i].rssi);
}

static void bleDoPair() {
  int i = blePairIdx;
  if (i < 0 || i >= bleNFound) { kprintf("ble: no such entry, run: ble scan\n"); return; }
  if (bleCli->isConnected()) bleCli->disconnect();
  NimBLEAddress a(std::string(bleFound[i].addr), bleFound[i].type);
  if (NimBLEDevice::isBonded(a)) NimBLEDevice::deleteBond(a);   // a fresh pairing, not a stale key
  kprintf("ble: pairing with %s ...\n", bleFound[i].name);
  bleOpen(a, true);
}

static void bleDoForget() {
  if (bleCli && bleCli->isConnected()) bleCli->disconnect();
  NimBLEDevice::deleteAllBonds();
  LittleFS.remove(BLE_ADDR_FILE);
  bleHaveAddr = false; bleLinked = false;
  kprintf("ble: keyboard forgotten\n");
}

// Manager task: serves shell requests and keeps the keyboard connected. A sleeping keyboard does not advertise, so reconnecting
// simply retries (every 5 s, backing off to 20 s). Pressing a key on the keyboard wakes it and it connects within a few seconds.
static void bleTask(void*) {
  uint32_t lastTry = 0, wait = 5000;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!bleEnabled) { if (bleInited) bleStackDown(); bleReq = 0; continue; }
    if (!bleInited && !bleStackUp()) { kprintf("ble: could not start the BLE stack (out of memory?)\n"); bleEnabled = false; bleReq = 0; continue; }
    if (bleReq) {
      int r = bleReq;
      if (r == 1) bleDoScan(); else if (r == 2) bleDoPair(); else if (r == 3) bleDoForget();
      bleReq = 0; lastTry = millis(); wait = 5000;
      continue;
    }
    if (bleLinked && !bleCli->isConnected()) bleLinked = false;
    if (!bleLinked && bleHaveAddr && millis() - lastTry > wait) {
      lastTry = millis();
      if (bleOpen(NimBLEAddress(std::string(bleAddr), bleAddrType), false)) wait = 5000;
      else if (wait < 20000) wait += 5000;
    }
  }
}

static void bleEnable(bool on) {
  if (on) {
    if (!bleQ) bleQ = xQueueCreate(16, 8);
    if (!bleHaveAddr) bleLoadAddr();
    if (!bleTaskH) xTaskCreatePinnedToCore(bleTask, "koable", 6144, NULL, 2, &bleTaskH, 0);
    bleEnabled = true;
  } else {
    bleEnabled = false;
  }
}

// Called from loop(): report -> bytes -> the console, on the same thread that reads the USB serial port.
static void bleKeyPump() {
  if (!bleQ) return;
  if (bleHidReset) { bleHidReset = false; koa_hid_release(&bleHid); xQueueReset(bleQ); }
  uint8_t r[8];
  char out[32];
  while (xQueueReceive(bleQ, r, 0) == pdTRUE) {
    int n = koa_hid_report(&bleHid, r, millis(), out, sizeof(out));
    for (int i = 0; i < n; i++) ttyInputChar(out[i]);
  }
  int n = koa_hid_tick(&bleHid, millis(), out);
  for (int i = 0; i < n; i++) ttyInputChar(out[i]);
}

static void bleStatusPrint() {
  kprintf("ble: %s\n", bleEnabled ? (bleInited ? "on" : "starting") : "off");
  if (bleHaveAddr) kprintf("keyboard: %s (%s)\n", bleAddr, bleLinked ? "connected" : "not connected");
  else kprintf("keyboard: none paired\n");
  if (bleLinked && bleBatt >= 0) kprintf("battery: %d%%\n", (int)bleBatt);
}

#else   // KOA_NO_BLE: the commands still exist and say so
static bool bleEnabled = false, bleLinked = false, bleInited = false, bleHaveAddr = false;
static volatile int bleReq = 0;
static void bleEnable(bool) {}
static void bleKeyPump() {}
static void bleStatusPrint() { kprintf("ble: this build has KOA_NO_BLE set\n"); }
#endif

static bool bleBootFlag() { return LittleFS.exists(BLE_BOOT_FILE); }
static void bleBootSet(bool on) {
  if (on) { File f = LittleFS.open(BLE_BOOT_FILE, "w"); if (f) { f.print("1"); f.close(); } }
  else LittleFS.remove(BLE_BOOT_FILE);
}
