/*
  =====================================================================
   INKUBATOR MANGGOT (BSF) IoT  -  ESP32
  =====================================================================
  PERANGKAT & PIN (sama dengan program awal)
    LCD I2C 20x4 ........ alamat 0x27   (SDA GPIO21, SCL GPIO22)
    Sensor SHT30 ........ alamat 0x44   (bus I2C yang sama)
    Relay 1  GPIO26 ..... Peltier + kipas  (PENDINGIN)
    Relay 2  GPIO27 ..... Lampu pemanas    (PEMANAS)
    LED      GPIO2 ...... nyala = WiFi OK, kedip cepat = mencari WiFi,
                          kedip lambat = hotspot setup aktif

  FITUR
    - Mode AUTO  : relay mengikuti target suhu (histeresis, interlock, jeda 10 dtk)
    - Mode MANUAL: pemanas / pendingin dinyalakan dari aplikasi
                   (tetap ada interlock + batas aman suhu)
    - Firebase Realtime Database lewat REST (tanpa library Firebase)
    - Semua setelan disimpan permanen di Preferences (NVS = "EEPROM" ESP32)
    - Riwayat disimpan dulu di flash (LittleFS, +-6000 data). Saat online
      riwayat dikirim ke aplikasi, jadi tidak ada data yang hilang
    - Multi WiFi (maks 5) tersimpan di memori. Urutan daftar = prioritas:
      jika WiFi utama tidak ada, langsung mencoba WiFi berikutnya.
      Jika 60 dtk tidak ada WiFi, hotspot terbuka "MANGGOT-SETUP"
      (192.168.4.1) aktif untuk menambah WiFi
    - Relay bisa ditukar & polaritasnya diubah dari aplikasi (disimpan)
    - Target kelembapan ideal (RH) dari aplikasi
    - Reboot otomatis (tiap N jam / jam tertentu) + reboot dari aplikasi
    - Update firmware (OTA) dari aplikasi: pilih file .bin langsung, atau dari GitHub,
      atau upload .bin di halaman lokal (tanpa password)
    - Sensor tidak terbaca > 10 dtk -> semua relay OFF (fail-safe)

  LIBRARY (Library Manager)
    - "LiquidCrystal I2C" oleh Frank de Brabander
    - "ArduinoJson" oleh Benoit Blanchon (versi 7.x)

  BOARD (Arduino IDE)
    - ESP32 Dev Module, core esp32 by Espressif versi 3.x
    - Tools > Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)"
      (wajib, supaya muat dan OTA bisa jalan)

  Perintah Serial (115200 baud, Newline):
    SET 31 | MODE AUTO | MODE MANUAL | HEAT ON/OFF | COOL ON/OFF
    WIFI LIST | WIFI ADD nama;password | WIFI DEL 2 | REBOOT | STATUS | HELP
*/

#define WOKWI_SIM 1            // 1 = untuk simulasi Wokwi (relay aktif HIGH)
#define FW_VERSION "2.1.0"     // naikkan setiap kali update firmware lewat OTA

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <LiquidCrystal_I2C.h>
#include "secrets.h"

typedef uint8_t Row[20];   // harus di atas semua fungsi (Arduino membuat prototipe otomatis)

// ================= KONFIGURASI PIN =================
#define PIN_SDA          21
#define PIN_SCL          22
#define PIN_RELAY_PELT   26          // Relay 1: Peltier + kipas
#define PIN_RELAY_LAMP   27          // Relay 2: Lampu pemanas
#define PIN_LED          2           // LED status WiFi

#define LCD_ADDR         0x27
#define SHT_ADDR         0x44

// Polaritas & penukaran relay sekarang diatur dari aplikasi (Sistem > Pengaturan Relay)
// dan disimpan di memori. Nilai di bawah hanya bawaan saat memori masih kosong.
#if WOKWI_SIM
  #define RELAY_ACTIVE_HIGH_DEFAULT 1   // modul relay Wokwi aktif HIGH
#else
  #define RELAY_ACTIVE_HIGH_DEFAULT 0   // modul relay asli umumnya active LOW
#endif

const long     GMT_OFFSET_SEC    = 7 * 3600;   // WIB (UTC+7)
const uint32_t MIN_SWITCH_MS     = 10000;      // jeda minimum relay menyala lagi (lindungi relay & peltier)
const uint32_t SENSOR_TIMEOUT_MS = 10000;
const char    *AP_SSID           = "MANGGOT-SETUP";

LiquidCrystal_I2C lcd(LCD_ADDR, 20, 4);

// ================= SETELAN (disimpan di NVS) =================
#define MAX_WIFI     5
#define MODE_AUTO    0
#define MODE_MANUAL  1
#define CFG_MAGIC_V2 0x4D473202UL
#define CFG_MAGIC    0x4D473203UL

// Struktur lama (v2.0.x), dipakai untuk memindahkan setelan saat update
struct ConfigV2 {
  uint32_t magic;
  float    tempSet, hyst, warnTL, warnTH, warnHL, warnHH, safeMax, safeMin;
  uint8_t  mode, manCool, manHeat, rbEn, rbMode, rbHour, rbMin;
  uint16_t rbHours, histSec;
  uint8_t  nWifi;
  char     ssid[5][33];
  char     pass[5][65];
};

struct Config {
  uint32_t magic;
  float    tempSet, hyst;                    // target & histeresis
  float    warnTL, warnTH, warnHL, warnHH;   // batas peringatan
  float    safeMax, safeMin;                 // batas aman mode manual
  uint8_t  mode, manCool, manHeat;           // mode & perintah manual
  uint8_t  rbEn, rbMode, rbHour, rbMin;      // reboot otomatis (0 = tiap N jam, 1 = harian)
  uint16_t rbHours;
  uint16_t histSec;                          // interval simpan riwayat (detik)
  uint8_t  nWifi;
  char     ssid[MAX_WIFI][33];
  char     pass[MAX_WIFI][65];
  // ---- v2.1 ----
  float    humSet;                           // target kelembapan ideal (%RH)
  uint8_t  relaySwap;                        // 1 = relay pemanas & pendingin ditukar
  uint8_t  relayHigh;                        // 1 = modul relay aktif HIGH, 0 = aktif LOW
};

Config       cfg;
portMUX_TYPE cfgMux = portMUX_INITIALIZER_UNLOCKED;
Preferences  prefs;
volatile bool     cfgDirty = false;
uint32_t cfgDirtyMs = 0;
volatile bool     cfgPushPending = false;   // setelan lokal perlu dikirim ke Firebase
volatile bool     sysPushPending = false;   // daftar WiFi / reboot perlu dikirim ke Firebase
volatile bool     wifiListChanged = false;
uint32_t          bootCount = 0;

// ================= RIWAYAT (antrian di flash) =================
struct __attribute__((packed)) Rec {
  uint32_t ts;      // epoch (atau detik sejak boot jika jam belum sinkron)
  uint16_t boot;
  int16_t  t100;    // suhu x100
  uint16_t h10;     // RH x10
  uint8_t  f;       // bit0 pendingin, bit1 pemanas, bit2 manual, bit3 waktu relatif, bit4 sensor ok
  uint8_t  a;       // bitmask peringatan
};
#define QCAP   6000
#define QRAM   200
#define F_COOL 1
#define F_HEAT 2
#define F_MAN  4
#define F_REL  8
#define F_SOK   16
Rec      qRam[QRAM];
bool     qFs = false;
uint32_t qHead = 0, qCount = 0;

// ================= KARAKTER KUSTOM =================
enum { CH_WIFI = 0, CH_TEMP, CH_DROP, CH_SNOW, CH_FLAME, CH_WARN, CH_LARVA1, CH_LARVA2 };

byte gWifi[8]   = {0b00000, 0b01110, 0b10001, 0b00100, 0b01010, 0b00000, 0b00100, 0b00000};
byte gTemp[8]   = {0b00100, 0b01010, 0b01010, 0b01010, 0b01110, 0b11111, 0b11111, 0b01110};
byte gDrop[8]   = {0b00100, 0b00100, 0b01010, 0b01010, 0b10001, 0b10001, 0b10001, 0b01110};
byte gSnow[8]   = {0b00100, 0b10101, 0b01110, 0b11111, 0b01110, 0b10101, 0b00100, 0b00000};
byte gFlame[8]  = {0b00100, 0b00100, 0b01100, 0b01110, 0b11111, 0b11111, 0b11111, 0b01110};
byte gWarn[8]   = {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100, 0b00000};
byte gLarva1[8] = {0b00000, 0b00000, 0b00000, 0b01110, 0b11111, 0b01110, 0b01010, 0b00000};
byte gLarva2[8] = {0b00000, 0b00000, 0b01110, 0b11111, 0b11111, 0b01010, 0b10001, 0b00000};

// ================= STATE =================
float    temperature = NAN;
float    humidity    = NAN;
bool     shtOk       = false;
uint32_t lastGoodMs = 0;

bool coolOn = false, heatOn = false;
uint32_t lastCoolChange = 0, lastHeatChange = 0;
uint8_t  safetyFlags = 0;          // 1 = pemanas diputus batas aman, 2 = pendingin diputus
uint32_t statusRev = 0;            // naik setiap ada perubahan penting -> status dikirim segera

#define A_TEMP_LOW   1
#define A_TEMP_HIGH  2
#define A_HUM_LOW    4
#define A_HUM_HIGH   8
#define A_SENSOR     16
uint8_t alerts = 0;
bool    newAlertFlag = false;
uint8_t alertShown = 0;
uint8_t alertIdx = 4;

// Waktu
struct tm timeNow;
bool timeOk = false;
bool ntpStarted = false;

// WiFi
enum { WS_IDLE, WS_SCAN, WS_CONNECTING, WS_CONNECTED };
uint8_t  wState = WS_IDLE;
uint32_t wStateMs = 0, wLastTry = 0, wOfflineSince = 0, wConnectedMs = 0;
int      wNext = 0;
int      wCand[MAX_WIFI];                     // urutan WiFi yang akan dicoba (prioritas daftar)
int      wCandN = 0, wCandI = 0;
bool     apOn = false;
DNSServer dns;
WebServer web(80);

// Cloud
WiFiClientSecure tls;
HTTPClient       http;
String           idToken;
uint32_t         tokenMs = 0;
volatile bool    fbReady = false;
volatile bool    cloudOk = false;
volatile bool    otaActive = false;
int     otaProgress = 0;
char             otaState[12] = "idle";
char             otaMsg[64]   = "";
bool             otaReqPending = false;
char             otaUrl[200]  = "";
char             otaVer[20]   = "";
bool             otaFromFile  = false;
uint32_t         otaSize = 0, otaChunks = 0;
#define OTA_CHUNK 12288                     // byte per potongan file .bin di Firebase

// Event log ke aplikasi
#define EVQ 6
char         evQ[EVQ][80];
int evN = 0;
portMUX_TYPE evMux = portMUX_INITIALIZER_UNLOCKED;

// Reboot
char     rebootReason[48] = "";
uint32_t rebootAt = 0;

const char *DAY_SHORT[] = {"Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"};
const char *DAY_FULL[]  = {"Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"};
const char *MON_SHORT[] = {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun", "Jul", "Agu", "Sep", "Okt", "Nov", "Des"};

// ================= SETELAN: DEFAULT, VALIDASI, SIMPAN =================
void defaultConfig(Config &c) {
  memset(&c, 0, sizeof(Config));
  c.magic   = CFG_MAGIC;
  c.tempSet = 30.0f;  c.hyst   = 0.5f;
  c.warnTL  = 27.0f;  c.warnTH = 33.0f;
  c.warnHL  = 60.0f;  c.warnHH = 85.0f;
  c.safeMax = 38.0f;  c.safeMin = 20.0f;
  c.mode    = MODE_AUTO;
  c.rbEn    = 0;  c.rbMode = 0;  c.rbHours = 24;  c.rbHour = 3;  c.rbMin = 0;
  c.histSec = 60;
  c.humSet  = 70.0f;
  c.relaySwap = 0;
  c.relayHigh = RELAY_ACTIVE_HIGH_DEFAULT;
  c.nWifi   = 1;
  strlcpy(c.ssid[0], WIFI_DEFAULT_SSID, 33);
  strlcpy(c.pass[0], WIFI_DEFAULT_PASS, 65);
}

float clampF(float v, float lo, float hi) {
  if (isnan(v)) return lo;
  return v < lo ? lo : (v > hi ? hi : v);
}

void sanitize(Config &c) {
  c.tempSet = clampF(c.tempSet, 20.0f, 40.0f);
  c.hyst    = clampF(c.hyst, 0.1f, 3.0f);
  c.warnTL  = clampF(c.warnTL, 0.0f, 50.0f);
  c.warnTH  = clampF(c.warnTH, 0.0f, 60.0f);
  c.warnHL  = clampF(c.warnHL, 0.0f, 100.0f);
  c.warnHH  = clampF(c.warnHH, 0.0f, 100.0f);
  c.safeMax = clampF(c.safeMax, 25.0f, 50.0f);
  c.safeMin = clampF(c.safeMin, 5.0f, 30.0f);
  c.mode    = c.mode ? MODE_MANUAL : MODE_AUTO;
  c.manCool = c.manCool ? 1 : 0;
  c.manHeat = c.manHeat ? 1 : 0;
  if (c.manCool && c.manHeat) c.manCool = c.manHeat = 0;   // tidak boleh dua-duanya
  c.rbEn    = c.rbEn ? 1 : 0;
  c.rbMode  = c.rbMode ? 1 : 0;
  if (c.rbHours < 1) c.rbHours = 1;
  if (c.rbHours > 720) c.rbHours = 720;
  c.rbHour %= 24;  c.rbMin %= 60;
  if (c.histSec < 10) c.histSec = 10;
  if (c.histSec > 3600) c.histSec = 3600;
  if (c.nWifi > MAX_WIFI) c.nWifi = MAX_WIFI;
  c.humSet    = clampF(c.humSet, 30.0f, 95.0f);
  c.relaySwap = c.relaySwap ? 1 : 0;
  c.relayHigh = c.relayHigh ? 1 : 0;
  for (int i = 0; i < MAX_WIFI; i++) { c.ssid[i][32] = 0; c.pass[i][64] = 0; }
}

void loadConfig() {
  defaultConfig(cfg);
  size_t len = prefs.getBytesLength("cfg");
  if (len == sizeof(Config)) {
    Config t;
    prefs.getBytes("cfg", &t, sizeof(Config));
    if (t.magic == CFG_MAGIC) cfg = t;
  } else if (len == sizeof(ConfigV2)) {          // pindahkan setelan dari firmware v2.0.x
    ConfigV2 o;
    prefs.getBytes("cfg", &o, sizeof(ConfigV2));
    if (o.magic == CFG_MAGIC_V2) {
      cfg.tempSet = o.tempSet; cfg.hyst = o.hyst; cfg.warnTL = o.warnTL; cfg.warnTH = o.warnTH;
      cfg.warnHL = o.warnHL; cfg.warnHH = o.warnHH; cfg.safeMax = o.safeMax; cfg.safeMin = o.safeMin;
      cfg.mode = o.mode; cfg.rbEn = o.rbEn; cfg.rbMode = o.rbMode; cfg.rbHour = o.rbHour; cfg.rbMin = o.rbMin;
      cfg.rbHours = o.rbHours; cfg.histSec = o.histSec; cfg.nWifi = o.nWifi;
      memcpy(cfg.ssid, o.ssid, sizeof cfg.ssid);
      memcpy(cfg.pass, o.pass, sizeof cfg.pass);
      cfgDirty = true;                            // simpan ulang dengan format baru
      Serial.println(F("[NVS] Setelan lama dipindahkan ke format v2.1"));
    }
  }
  sanitize(cfg);
  if (cfg.nWifi == 0) {
    cfg.nWifi = 1;
    strlcpy(cfg.ssid[0], WIFI_DEFAULT_SSID, 33);
    strlcpy(cfg.pass[0], WIFI_DEFAULT_PASS, 65);
  }
}

void saveConfig() {
  Config t;
  portENTER_CRITICAL(&cfgMux);
  t = cfg;
  portEXIT_CRITICAL(&cfgMux);
  prefs.putBytes("cfg", &t, sizeof(Config));
}

// toCloud: perubahan lokal (Serial/portal) yang perlu dikirim ke Firebase
void cfgChanged(bool toCloud, bool systemPart) {
  cfgDirty = true;
  cfgDirtyMs = millis();
  statusRev++;
  if (toCloud) {
    if (systemPart) sysPushPending = true;
    else cfgPushPending = true;
  }
}

void queueEvent(const char *msg) {
  portENTER_CRITICAL(&evMux);
  if (evN < EVQ) { strlcpy(evQ[evN], msg, 80); evN++; }
  portEXIT_CRITICAL(&evMux);
}

// ================= UTIL SENSOR =================
bool sensorValid() {
  return lastGoodMs != 0 && (millis() - lastGoodMs) < SENSOR_TIMEOUT_MS;
}

bool epochOk() { return time(nullptr) > 1700000000; }

uint8_t crc8(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

uint8_t  shtState = 0;
uint32_t shtT = 0;

void shtUpdate() {
  uint32_t now = millis();
  if (shtState == 0 && now - shtT >= 1000) {
    Wire.beginTransmission(SHT_ADDR);
    Wire.write(0x24);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
      shtOk = false;
      shtT = now;
      return;
    }
    shtT = now;
    shtState = 1;
  } else if (shtState == 1 && now - shtT >= 20) {
    shtState = 0;
    shtT = now;
    if (Wire.requestFrom((uint8_t)SHT_ADDR, (uint8_t)6) != 6) {
      shtOk = false;
      return;
    }
    uint8_t d[6];
    for (int i = 0; i < 6; i++) d[i] = Wire.read();
    if (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) {
      shtOk = false;
      return;
    }
    uint16_t rawT = (d[0] << 8) | d[1];
    uint16_t rawH = (d[3] << 8) | d[4];
    temperature = -45.0f + 175.0f * rawT / 65535.0f;
    humidity    = 100.0f * rawH / 65535.0f;
    shtOk = true;
    lastGoodMs = now ? now : 1;
  }
}

// ================= RELAY & KONTROL =================
uint8_t pinCool()  { return cfg.relaySwap ? PIN_RELAY_LAMP : PIN_RELAY_PELT; }
uint8_t pinHeat()  { return cfg.relaySwap ? PIN_RELAY_PELT : PIN_RELAY_LAMP; }
uint8_t lvl(bool on) { return (on == (bool)cfg.relayHigh) ? HIGH : LOW; }
volatile bool relayCfgChanged = false;

void setCool(bool on, bool force) {
  if (on == coolOn) return;
  if (on && heatOn) return;                                   // interlock
  if (!force && millis() - lastCoolChange < MIN_SWITCH_MS) return;
  coolOn = on;
  lastCoolChange = millis();
  digitalWrite(pinCool(), lvl(on));
  statusRev++;
  Serial.printf("[AKSI] Peltier+Kipas -> %s\n", on ? "ON" : "OFF");
}

void setHeat(bool on, bool force) {
  if (on == heatOn) return;
  if (on && coolOn) return;                                   // interlock
  if (!force && millis() - lastHeatChange < MIN_SWITCH_MS) return;
  heatOn = on;
  lastHeatChange = millis();
  digitalWrite(pinHeat(), lvl(on));
  statusRev++;
  Serial.printf("[AKSI] Lampu Pemanas -> %s\n", on ? "ON" : "OFF");
}

void allRelaysOff() {
  coolOn = heatOn = false;
  digitalWrite(PIN_RELAY_PELT, lvl(false));
  digitalWrite(PIN_RELAY_LAMP, lvl(false));
}

// Dipanggil saat pengaturan relay (tukar / polaritas) diubah dari aplikasi
void applyRelayConfig() {
  relayCfgChanged = false;
  allRelaysOff();
  lastCoolChange = lastHeatChange = millis();     // jeda aman sebelum relay boleh menyala lagi
  statusRev++;
  Serial.printf("[RELAY] Pendingin=GPIO%d  Pemanas=GPIO%d  aktif %s\n", pinCool(), pinHeat(), cfg.relayHigh ? "HIGH" : "LOW");
}

void controlUpdate() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();

  if (!sensorValid()) {            // fail-safe (berlaku di AUTO dan MANUAL)
    setCool(false, true);
    setHeat(false, true);
    return;
  }

  float t = temperature;
  uint8_t sf = 0;

  if (cfg.mode == MODE_MANUAL) {
    bool wantCool = cfg.manCool, wantHeat = cfg.manHeat;
    if (wantCool && wantHeat) wantCool = wantHeat = false;
    if (wantHeat && t >= cfg.safeMax) { wantHeat = false; sf |= 1; }
    if (wantCool && t <= cfg.safeMin) { wantCool = false; sf |= 2; }
    if (!wantCool) setCool(false, true);
    if (!wantHeat) setHeat(false, true);
    if (wantCool) setCool(true, false);
    if (wantHeat) setHeat(true, false);
  } else {
    float sp = cfg.tempSet, hy = cfg.hyst;
    if (t >= sp + hy) {
      setHeat(false, true);          // interlock: pemanas dimatikan dulu
      setCool(true, false);
    } else if (t <= sp - hy) {
      setCool(false, true);
      setHeat(true, false);
    } else {
      if (coolOn && t <= sp) setCool(false, false);
      if (heatOn && t >= sp) setHeat(false, false);
    }
  }
  if (sf != safetyFlags) { safetyFlags = sf; statusRev++; }
}

// ================= ALERT =================
void alertUpdate() {
  static uint32_t last = 0;
  if (millis() - last < 200) return;
  last = millis();

  uint8_t a = 0;
  if (!sensorValid()) {
    a |= A_SENSOR;
  } else {
    float t = temperature, h = humidity;
    if (t < ((alerts & A_TEMP_LOW)  ? cfg.warnTL + 0.3f : cfg.warnTL)) a |= A_TEMP_LOW;
    if (t > ((alerts & A_TEMP_HIGH) ? cfg.warnTH - 0.3f : cfg.warnTH)) a |= A_TEMP_HIGH;
    if (h < ((alerts & A_HUM_LOW)   ? cfg.warnHL + 1.0f : cfg.warnHL)) a |= A_HUM_LOW;
    if (h > ((alerts & A_HUM_HIGH)  ? cfg.warnHH - 1.0f : cfg.warnHH)) a |= A_HUM_HIGH;
  }
  uint8_t added = a & ~alerts;
  if (added) {
    newAlertFlag = true;
    Serial.printf("[PERINGATAN] Baru: 0x%02X\n", added);
  }
  if (a != alerts) statusRev++;
  alerts = a;
}

void pickAlertToShow() {
  for (int i = 1; i <= 5; i++) {
    uint8_t idx = (alertIdx + i) % 5;
    uint8_t b = 1 << idx;
    if (alerts & b) {
      alertShown = b;
      alertIdx = idx;
      return;
    }
  }
}

void alertNames(char *out, size_t n) {
  out[0] = 0;
  if (alerts & A_TEMP_LOW)  strlcat(out, "SUHU-RENDAH ", n);
  if (alerts & A_TEMP_HIGH) strlcat(out, "SUHU-TINGGI ", n);
  if (alerts & A_HUM_LOW)   strlcat(out, "RH-RENDAH ", n);
  if (alerts & A_HUM_HIGH)  strlcat(out, "RH-TINGGI ", n);
  if (alerts & A_SENSOR)    strlcat(out, "SENSOR-ERROR ", n);
  if (!alerts) strlcpy(out, "-", n);
}

// ================= ANTRIAN RIWAYAT (LittleFS ring buffer) =================
void qSaveMeta() {
  if (!qFs) return;
  File f = LittleFS.open("/hq.meta", "w");
  if (!f) return;
  uint32_t m[2] = {qHead, qCount};
  f.write((uint8_t *)m, sizeof m);
  f.close();
}

void qInit() {
  qFs = LittleFS.begin(true);
  if (!qFs) {
    Serial.println(F("[FS] LittleFS gagal, riwayat offline hanya di RAM (200 data)"));
    return;
  }
  const size_t need = (size_t)QCAP * sizeof(Rec);
  bool fresh = true;
  if (LittleFS.exists("/hq.bin")) {
    File f = LittleFS.open("/hq.bin", "r");
    fresh = !f || f.size() != need;
    if (f) f.close();
  }
  if (fresh) {
    Serial.println(F("[FS] Menyiapkan file riwayat..."));
    File f = LittleFS.open("/hq.bin", "w");
    uint8_t zero[256];
    memset(zero, 0, sizeof zero);
    for (size_t w = 0; w < need; w += sizeof zero) f.write(zero, min(sizeof zero, need - w));
    f.close();
    qHead = qCount = 0;
    qSaveMeta();
  } else {
    File m = LittleFS.open("/hq.meta", "r");
    uint32_t v[2] = {0, 0};
    if (m && m.read((uint8_t *)v, sizeof v) == sizeof v) { qHead = v[0]; qCount = v[1]; }
    if (m) m.close();
    if (qHead >= QCAP || qCount > QCAP) qHead = qCount = 0;
  }
  Serial.printf("[FS] Antrian riwayat: %lu data belum terkirim\n", (unsigned long)qCount);
}

uint32_t qCap() { return qFs ? QCAP : QRAM; }

void qPush(const Rec &r) {
  uint32_t cap = qCap();
  uint32_t idx = (qHead + qCount) % cap;
  if (qFs) {
    File f = LittleFS.open("/hq.bin", "r+");
    if (f) { f.seek(idx * sizeof(Rec)); f.write((const uint8_t *)&r, sizeof(Rec)); f.close(); }
  } else {
    qRam[idx] = r;
  }
  if (qCount < cap) qCount++;
  else qHead = (qHead + 1) % cap;      // penuh: data paling lama ditimpa
  qSaveMeta();
}

int qPeek(Rec *out, int maxN) {
  int n = min((uint32_t)maxN, (uint32_t)qCount);
  uint32_t cap = qCap();
  if (qFs) {
    File f = LittleFS.open("/hq.bin", "r");
    if (!f) return 0;
    for (int i = 0; i < n; i++) {
      f.seek(((qHead + i) % cap) * sizeof(Rec));
      f.read((uint8_t *)&out[i], sizeof(Rec));
    }
    f.close();
  } else {
    for (int i = 0; i < n; i++) out[i] = qRam[(qHead + i) % cap];
  }
  return n;
}

void qPop(int n) {
  if (n <= 0) return;
  if ((uint32_t)n > qCount) n = qCount;
  qHead = (qHead + n) % qCap();
  qCount -= n;
  qSaveMeta();
}

void histTick() {
  static uint32_t last = 0;
  uint32_t iv = (uint32_t)cfg.histSec * 1000UL;
  if (last != 0 && millis() - last < iv) return;
  if (last == 0 && millis() < 8000) return;          // tunggu sensor siap setelah boot
  last = millis();

  Rec r;
  bool ok = sensorValid();
  if (epochOk()) { r.ts = (uint32_t)time(nullptr); r.f = 0; }
  else           { r.ts = millis() / 1000;          r.f = F_REL; }
  r.boot = (uint16_t)bootCount;
  r.t100 = ok ? (int16_t)lroundf(temperature * 100) : 0;
  r.h10  = ok ? (uint16_t)lroundf(humidity * 10) : 0;
  if (coolOn) r.f |= F_COOL;
  if (heatOn) r.f |= F_HEAT;
  if (cfg.mode == MODE_MANUAL) r.f |= F_MAN;
  if (ok) r.f |= F_SOK;
  r.a = alerts;
  qPush(r);
}

// ================= WIFI & WAKTU =================
void startNtp() {
  if (ntpStarted) return;
  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com", "id.pool.ntp.org");
  ntpStarted = true;
}

void updateTime() {
  time_t now = time(nullptr);
  localtime_r(&now, &timeNow);
  timeOk = (timeNow.tm_year >= (2020 - 1900));
}

void startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);                      // hotspot setup tanpa password
  dns.start(53, "*", WiFi.softAPIP());
  apOn = true;
  statusRev++;
  Serial.printf("[WIFI] Hotspot setup aktif: %s  (http://%s)\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void stopAP() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  statusRev++;
  Serial.println(F("[WIFI] Hotspot setup dimatikan"));
}

void wifiConnectTo(int i) {
  Serial.printf("[WIFI] Menghubungkan ke \"%s\"...\n", cfg.ssid[i]);
  WiFi.begin(cfg.ssid[i], cfg.pass[i]);
  wState = WS_CONNECTING;
  wStateMs = millis();
}

void wifiTick() {
  uint32_t now = millis();
  bool con = (WiFi.status() == WL_CONNECTED);

  if (wifiListChanged) {
    wifiListChanged = false;
    wCandN = wCandI = 0;
    if (!con && wState != WS_SCAN) { wState = WS_IDLE; wLastTry = 0; }
  }

  switch (wState) {
    case WS_CONNECTED:
      if (!con) {
        Serial.println(F("[WIFI] Terputus, mencari WiFi dari daftar..."));
        wState = WS_IDLE;
        wCandN = wCandI = 0;
        wLastTry = now - 7000;                      // scan ulang 3 detik lagi
        statusRev++;
      }
      break;

    case WS_IDLE: {
      if (con) { wState = WS_CONNECTED; wConnectedMs = now; startNtp(); statusRev++; break; }
      uint32_t retry = (apOn && WiFi.softAPgetStationNum() > 0) ? 60000 : 10000;
      if (wLastTry != 0 && now - wLastTry < retry) break;
      wLastTry = now;
      if (cfg.nWifi == 0) break;
      if (wCandI < wCandN) { wifiConnectTo(wCand[wCandI++]); break; }   // coba WiFi berikutnya
      if (cfg.nWifi == 1) { wCandN = 1; wCand[0] = 0; wCandI = 1; wifiConnectTo(0); break; }
      WiFi.scanNetworks(true);                 // scan tanpa memblokir
      wState = WS_SCAN;
      wStateMs = now;
      break;
    }

    case WS_SCAN: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) {
        if (now - wStateMs > 15000) { WiFi.scanDelete(); wState = WS_IDLE; wCandN = wCandI = 0; }
        break;
      }
      // Urutan prioritas = urutan daftar di aplikasi (nomor 1 = WiFi utama).
      // WiFi yang terlihat dicoba lebih dulu, sisanya (mungkin tersembunyi) dicoba setelahnya.
      wCandN = 0;
      bool seen[MAX_WIFI] = {false};
      for (int i = 0; i < cfg.nWifi; i++)
        for (int k = 0; k < n; k++)
          if (WiFi.SSID(k) == cfg.ssid[i]) { seen[i] = true; break; }
      for (int i = 0; i < cfg.nWifi; i++) if (seen[i])  wCand[wCandN++] = i;
      for (int i = 0; i < cfg.nWifi; i++) if (!seen[i]) wCand[wCandN++] = i;
      WiFi.scanDelete();
      wCandI = 0;
      if (wCandN > 0) wifiConnectTo(wCand[wCandI++]);
      else wState = WS_IDLE;
      break;
    }

    case WS_CONNECTING:
      if (con) {
        wState = WS_CONNECTED;
        wConnectedMs = now;
        Serial.printf("[WIFI] Terhubung ke %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        startNtp();
        statusRev++;
      } else if (now - wStateMs > 12000) {
        Serial.println(F("[WIFI] Gagal, mencoba WiFi berikutnya"));
        WiFi.disconnect();
        wState = WS_IDLE;
        wLastTry = (wCandI < wCandN) ? 0 : now;    // masih ada kandidat: langsung coba berikutnya
      }
      break;
  }

  // Hotspot setup otomatis
  if (!con) {
    if (wOfflineSince == 0) wOfflineSince = now ? now : 1;
    if (!apOn && now - wOfflineSince > 60000) startAP();
  } else {
    wOfflineSince = 0;
    if (apOn && WiFi.softAPgetStationNum() == 0 && now - wConnectedMs > 30000) stopAP();
  }
}

void ledUpdate() {
  if (WiFi.status() == WL_CONNECTED) digitalWrite(PIN_LED, HIGH);
  else if (apOn) digitalWrite(PIN_LED, ((millis() / 1000) & 1) ? HIGH : LOW);
  else digitalWrite(PIN_LED, ((millis() / 250) & 1) ? HIGH : LOW);
}

// ================= FIREBASE (REST) =================
bool fbSignIn() {
  tls.stop();
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FB_API_KEY;
  JsonDocument body;
  body["email"] = FB_DEVICE_EMAIL;
  body["password"] = FB_DEVICE_PASS;
  body["returnSecureToken"] = true;
  String b;
  serializeJson(body, b);
  http.begin(tls, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(b);
  String s = http.getString();
  http.end();
  tls.stop();
  if (code != 200) {
    fbReady = false;
    Serial.printf("[CLOUD] Login Firebase gagal (%d): %s\n", code, s.substring(0, 160).c_str());
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, s)) { fbReady = false; return false; }
  idToken = d["idToken"].as<String>();
  tokenMs = millis();
  fbReady = idToken.length() > 20;
  Serial.println(fbReady ? F("[CLOUD] Login Firebase OK") : F("[CLOUD] Token kosong"));
  return fbReady;
}

int fbReq(const char *method, const String &path, const String &body, String *out) {
  String url = String(FB_DB_URL) + "/devices/" + DEVICE_ID + path + ".json?auth=" + idToken;
  if (!http.begin(tls, url)) return -1;
  int code;
  if (strcmp(method, "GET") == 0) {
    code = http.GET();
  } else {
    http.addHeader("Content-Type", "application/json");
    code = http.sendRequest(method, body);
  }
  if (out && code > 0) *out = http.getString();
  http.end();
  if (code == 401) { fbReady = false; Serial.println(F("[CLOUD] Token kedaluwarsa / ditolak (401)")); }
  if (code < 0) tls.stop();
  return code;
}

String num(float v, int d) { return isnan(v) ? String("null") : String(v, d); }

void pushStatus() {
  JsonDocument d;
  bool ok = sensorValid();
  if (ok) {
    d["t"] = serialized(num(temperature, 2));
    d["h"] = serialized(num(humidity, 1));
  }
  d["ok"]   = ok;
  d["cool"] = (bool)coolOn;
  d["heat"] = (bool)heatOn;
  d["mode"] = cfg.mode;
  d["al"]   = alerts;
  d["sf"]   = safetyFlags;
  d["rssi"] = WiFi.RSSI();
  d["ssid"] = WiFi.SSID();
  d["ip"]   = WiFi.localIP().toString();
  d["up"]   = millis() / 1000;
  d["fw"]   = FW_VERSION;
  d["q"]    = qCount;
  d["ap"]   = apOn;
  d["boot"] = bootCount;
  d["heap"] = ESP.getFreeHeap();
  if (epochOk()) d["ts"] = (uint32_t)time(nullptr);
  d["seen"][".sv"] = "timestamp";
  JsonObject o = d["ota"].to<JsonObject>();
  o["st"]  = otaState;
  o["msg"] = otaMsg;
  o["p"]   = otaProgress;
  String s;
  serializeJson(d, s);
  fbReq("PUT", "/status", s, nullptr);
}

void pushConfig() {
  cfgPushPending = false;
  JsonDocument d;
  d["mode"] = cfg.mode;
  d["sp"]   = serialized(num(cfg.tempSet, 1));
  d["hy"]   = serialized(num(cfg.hyst, 1));
  d["wtl"]  = serialized(num(cfg.warnTL, 1));
  d["wth"]  = serialized(num(cfg.warnTH, 1));
  d["whl"]  = serialized(num(cfg.warnHL, 0));
  d["whh"]  = serialized(num(cfg.warnHH, 0));
  d["smax"] = serialized(num(cfg.safeMax, 1));
  d["smin"] = serialized(num(cfg.safeMin, 1));
  d["mc"]   = (bool)cfg.manCool;
  d["mh"]   = (bool)cfg.manHeat;
  d["hs"]   = cfg.histSec;
  d["hsp"]  = serialized(num(cfg.humSet, 0));
  String s;
  serializeJson(d, s);
  if (fbReq("PATCH", "/config", s, nullptr) != 200) cfgPushPending = true;
  else Serial.println(F("[CLOUD] Setelan lokal dikirim ke aplikasi"));
}

void pushSystem() {
  sysPushPending = false;
  JsonDocument d;
  JsonArray a = d["wifi"].to<JsonArray>();
  for (int i = 0; i < cfg.nWifi; i++) {
    JsonObject w = a.add<JsonObject>();
    w["s"] = cfg.ssid[i];
    w["p"] = cfg.pass[i];
  }
  d["rbEn"]    = (bool)cfg.rbEn;
  d["rbMode"]  = cfg.rbMode;
  d["rbHours"] = cfg.rbHours;
  char at[6];
  snprintf(at, sizeof at, "%02d:%02d", cfg.rbHour, cfg.rbMin);
  d["rbAt"] = at;
  d["rlSwap"] = (bool)cfg.relaySwap;
  d["rlHigh"] = (bool)cfg.relayHigh;
  String s;
  serializeJson(d, s);
  if (fbReq("PATCH", "/system", s, nullptr) != 200) sysPushPending = true;
}

void readF(JsonDocument &d, const char *k, float &v) {
  if (!d[k].isNull()) v = d[k].as<float>();
}

bool pollConfig() {
  String s;
  int code = fbReq("GET", "/config", "", &s);
  if (code != 200) return false;
  s.trim();
  if (s == "null") { cfgPushPending = true; return true; }   // database masih kosong
  if (cfgPushPending) return true;                            // ada perubahan lokal yang belum terkirim
  JsonDocument d;
  if (deserializeJson(d, s)) return false;

  Config n;
  portENTER_CRITICAL(&cfgMux);
  n = cfg;
  portEXIT_CRITICAL(&cfgMux);
  if (!d["mode"].isNull()) n.mode = d["mode"].as<int>();
  readF(d, "sp", n.tempSet);
  readF(d, "hy", n.hyst);
  readF(d, "wtl", n.warnTL);
  readF(d, "wth", n.warnTH);
  readF(d, "whl", n.warnHL);
  readF(d, "whh", n.warnHH);
  readF(d, "smax", n.safeMax);
  readF(d, "smin", n.safeMin);
  if (!d["mc"].isNull()) n.manCool = d["mc"].as<bool>();
  if (!d["mh"].isNull()) n.manHeat = d["mh"].as<bool>();
  if (!d["hs"].isNull()) n.histSec = d["hs"].as<int>();
  readF(d, "hsp", n.humSet);
  sanitize(n);
  if (memcmp(&n, &cfg, sizeof(Config)) != 0) {
    portENTER_CRITICAL(&cfgMux);
    cfg = n;
    portEXIT_CRITICAL(&cfgMux);
    cfgChanged(false, false);
    Serial.printf("[CLOUD] Setelan baru: mode=%s target=%.1f\n", n.mode ? "MANUAL" : "AUTO", n.tempSet);
  }
  return true;
}

void pollSystem() {
  String s;
  if (fbReq("GET", "/system", "", &s) != 200) return;
  s.trim();
  if (s == "null") { sysPushPending = true; return; }
  if (sysPushPending) return;
  JsonDocument d;
  if (deserializeJson(d, s)) return;

  Config n;
  portENTER_CRITICAL(&cfgMux);
  n = cfg;
  portEXIT_CRITICAL(&cfgMux);

  // --- daftar WiFi (diabaikan jika kosong, supaya perangkat tidak terkunci) ---
  JsonVariant w = d["wifi"];
  Config tmp = n;
  int cnt = 0;
  auto addW = [&](JsonVariant e) {
    const char *ss = e["s"] | "";
    if (cnt < MAX_WIFI && strlen(ss) > 0) {
      strlcpy(tmp.ssid[cnt], ss, 33);
      strlcpy(tmp.pass[cnt], e["p"] | "", 65);
      cnt++;
    }
  };
  if (w.is<JsonArray>())       for (JsonVariant e : w.as<JsonArray>()) addW(e);
  else if (w.is<JsonObject>()) for (JsonPair p : w.as<JsonObject>()) addW(p.value());
  if (cnt > 0) {
    memset(n.ssid, 0, sizeof n.ssid);
    memset(n.pass, 0, sizeof n.pass);
    memcpy(n.ssid, tmp.ssid, sizeof n.ssid);
    memcpy(n.pass, tmp.pass, sizeof n.pass);
    for (int i = cnt; i < MAX_WIFI; i++) { n.ssid[i][0] = 0; n.pass[i][0] = 0; }
    n.nWifi = cnt;
  }

  // --- reboot otomatis ---
  if (!d["rbEn"].isNull())    n.rbEn = d["rbEn"].as<bool>();
  if (!d["rbMode"].isNull())  n.rbMode = d["rbMode"].as<int>();
  if (!d["rbHours"].isNull()) n.rbHours = d["rbHours"].as<int>();
  const char *at = d["rbAt"] | "";
  int hh, mm;
  if (sscanf(at, "%d:%d", &hh, &mm) == 2) { n.rbHour = hh; n.rbMin = mm; }
  if (!d["rlSwap"].isNull()) n.relaySwap = d["rlSwap"].as<bool>();
  if (!d["rlHigh"].isNull()) n.relayHigh = d["rlHigh"].as<bool>();
  sanitize(n);

  if (memcmp(&n, &cfg, sizeof(Config)) != 0) {
    bool relayDiff = n.relaySwap != cfg.relaySwap || n.relayHigh != cfg.relayHigh;
    if (relayDiff) {
      // matikan relay dengan pengaturan lama dulu, baru ganti pengaturan
      coolOn = heatOn = false;
      digitalWrite(PIN_RELAY_PELT, lvl(false));
      digitalWrite(PIN_RELAY_LAMP, lvl(false));
    }
    bool wifiDiff = memcmp(n.ssid, cfg.ssid, sizeof n.ssid) || memcmp(n.pass, cfg.pass, sizeof n.pass) || n.nWifi != cfg.nWifi;
    portENTER_CRITICAL(&cfgMux);
    cfg = n;
    portEXIT_CRITICAL(&cfgMux);
    cfgChanged(false, true);
    if (wifiDiff) { wifiListChanged = true; Serial.printf("[CLOUD] Daftar WiFi diperbarui (%d jaringan)\n", n.nWifi); }
    if (relayDiff) { relayCfgChanged = true; queueEvent(n.relaySwap ? "Relay ditukar (pemanas <-> pendingin)" : "Relay kembali ke susunan awal"); }
  }

  if (!epochOk()) return;                       // permintaan reboot/OTA butuh jam yang benar
  uint32_t nowS = (uint32_t)time(nullptr);

  // --- reboot dari aplikasi ---
  uint64_t rq = (uint64_t)(d["rebootReq"] | 0.0);
  if (rq > 0 && rq != prefs.getULong64("rbAck", 0)) {
    prefs.putULong64("rbAck", rq);
    if (rq / 1000 + 180 >= nowS) {
      queueEvent("Reboot diminta dari aplikasi");
      strlcpy(rebootReason, "Reboot dari aplikasi", sizeof rebootReason);
      rebootAt = millis() + 2500;
    }
  }

  // --- update firmware dari aplikasi ---
  uint64_t oq = (uint64_t)(d["ota"]["req"] | 0.0);
  if (oq > 0 && oq != prefs.getULong64("otaAck", 0)) {
    prefs.putULong64("otaAck", oq);
    const char *u = d["ota"]["url"] | "";
    const char *src = d["ota"]["src"] | "url";
    if (oq / 1000 + 600 >= nowS) {
      strlcpy(otaVer, d["ota"]["ver"] | "?", sizeof otaVer);
      if (strcmp(src, "file") == 0) {             // file .bin dipilih di aplikasi, dititipkan di Firebase
        otaFromFile = true;
        otaSize   = d["ota"]["size"] | 0;
        otaChunks = d["ota"]["n"] | 0;
        otaUrl[0] = 0;
        otaReqPending = otaSize > 0 && otaChunks > 0;
      } else if (strlen(u) > 10) {
        otaFromFile = false;
        strlcpy(otaUrl, u, sizeof otaUrl);
        otaReqPending = true;
      }
    }
  }
}

bool drainQueue() {
  static Rec recs[40];
  int n = qPeek(recs, 40);
  if (n <= 0) return true;
  uint32_t nowS = (uint32_t)time(nullptr);
  uint32_t upS  = millis() / 1000;
  String body;
  body.reserve(n * 70 + 4);
  body = "{";
  bool any = false;
  char item[96];
  for (int i = 0; i < n; i++) {
    Rec &r = recs[i];
    uint32_t ts = r.ts;
    if (r.f & F_REL) {
      if (r.boot != (uint16_t)bootCount || r.ts > upS) continue;   // tidak bisa dipastikan waktunya: dibuang
      ts = nowS - (upS - r.ts);
    }
    if (ts < 1700000000UL) continue;
    uint8_t f = r.f & ~F_REL;
    if (r.f & F_SOK)
      snprintf(item, sizeof item, "%s\"t%lu\":{\"t\":%.2f,\"h\":%.1f,\"f\":%u,\"a\":%u}", any ? "," : "",
               (unsigned long)ts, r.t100 / 100.0f, r.h10 / 10.0f, f, r.a);
    else
      snprintf(item, sizeof item, "%s\"t%lu\":{\"f\":%u,\"a\":%u}", any ? "," : "", (unsigned long)ts, f, r.a);
    body += item;
    any = true;
  }
  body += "}";
  if (!any) { qPop(n); return true; }
  int code = fbReq("PATCH", "/history", body, nullptr);
  if (code == 200) {
    qPop(n);
    if (qCount > 0) Serial.printf("[CLOUD] Riwayat terkirim %d, sisa %lu\n", n, (unsigned long)qCount);
    return true;
  }
  Serial.printf("[CLOUD] Kirim riwayat gagal (%d)\n", code);
  return false;
}

void flushEvents() {
  while (evN > 0) {
    char m[80];
    portENTER_CRITICAL(&evMux);
    strlcpy(m, evQ[0], sizeof m);
    portEXIT_CRITICAL(&evMux);
    JsonDocument d;
    d["ts"][".sv"] = "timestamp";
    d["u"] = "ESP32";
    d["m"] = m;
    String s;
    serializeJson(d, s);
    if (fbReq("POST", "/events", s, nullptr) != 200) return;
    portENTER_CRITICAL(&evMux);
    for (int i = 1; i < evN; i++) memcpy(evQ[i - 1], evQ[i], 80);
    evN--;
    portEXIT_CRITICAL(&evMux);
  }
}

// Update firmware dari file .bin yang dipilih di aplikasi (dititipkan di Firebase per potongan)
void doOtaFile() {
  otaReqPending = false;
  char ev[80];
  snprintf(ev, sizeof ev, "Mulai update dari file: v%s -> v%s", FW_VERSION, otaVer);
  queueEvent(ev);
  flushEvents();
  otaActive = true;
  otaProgress = 0;
  strlcpy(otaState, "unduh", sizeof otaState);
  snprintf(otaMsg, sizeof otaMsg, "Mengunduh v%s", otaVer);
  pushStatus();
  Serial.printf("[OTA] File %lu byte, %lu potongan\n", (unsigned long)otaSize, (unsigned long)otaChunks);

  static uint8_t bin[OTA_CHUNK + 16];
  const char *err = nullptr;
  uint32_t written = 0;
  if (!Update.begin(otaSize)) err = "Ruang flash tidak cukup (cek Partition Scheme)";
  for (uint32_t i = 0; !err && i < otaChunks; i++) {
    String s;
    int code = -1;
    for (int tr = 0; tr < 4 && code != 200; tr++) {
      if (!fbReady) fbSignIn();
      code = fbReq("GET", String("/otabin/c") + i, "", &s);
      if (code != 200) delay(600);
    }
    if (code != 200) { err = "Gagal mengunduh potongan file"; break; }
    int a = s.indexOf('"'), b = s.lastIndexOf('"');
    if (a < 0 || b <= a) { err = "Potongan file rusak"; break; }
    size_t olen = 0;
    if (mbedtls_base64_decode(bin, sizeof bin, &olen, (const unsigned char *)s.c_str() + a + 1, b - a - 1) != 0) { err = "Data base64 rusak"; break; }
    s = String();
    if (Update.write(bin, olen) != olen) { err = Update.errorString(); break; }
    written += olen;
    otaProgress = (int)((i + 1) * 100 / otaChunks);
    if (i % 8 == 7) pushStatus();
  }
  if (!err && written != otaSize) err = "Ukuran file tidak cocok";
  if (!err && !Update.end(true)) err = Update.errorString();

  if (!err) {
    strlcpy(otaState, "ok", sizeof otaState);
    snprintf(otaMsg, sizeof otaMsg, "Berhasil v%s, restart", otaVer);
    otaProgress = 100;
    pushStatus();
    fbReq("DELETE", "/otabin", "", nullptr);      // hapus file titipan
    strlcpy(rebootReason, "Update firmware selesai", sizeof rebootReason);
    rebootAt = millis() + 1500;
  } else {
    Update.abort();
    otaActive = false;
    strlcpy(otaState, "gagal", sizeof otaState);
    strlcpy(otaMsg, err, sizeof otaMsg);
    Serial.printf("[OTA] GAGAL: %s\n", err);
    snprintf(ev, sizeof ev, "Update firmware gagal: %.55s", err);
    queueEvent(ev);
    pushStatus();
  }
}

void doOta() {
  otaReqPending = false;
  char ev[80];
  snprintf(ev, sizeof ev, "Mulai update firmware v%s -> v%s", FW_VERSION, otaVer);
  queueEvent(ev);
  flushEvents();
  otaActive = true;
  otaProgress = 0;
  strlcpy(otaState, "unduh", sizeof otaState);
  snprintf(otaMsg, sizeof otaMsg, "Mengunduh v%s", otaVer);
  pushStatus();
  Serial.printf("[OTA] Unduh %s\n", otaUrl);

  http.end();
  tls.stop();
  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(20);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onProgress([](int cur, int total) {
    if (total > 0) otaProgress = (int)((int64_t)cur * 100 / total);
  });
  t_httpUpdate_return r = httpUpdate.update(c, otaUrl);
  c.stop();

  if (r == HTTP_UPDATE_OK) {
    strlcpy(otaState, "ok", sizeof otaState);
    snprintf(otaMsg, sizeof otaMsg, "Berhasil v%s, restart", otaVer);
    otaProgress = 100;
    pushStatus();
    strlcpy(rebootReason, "Update firmware selesai", sizeof rebootReason);
    rebootAt = millis() + 1500;
  } else {
    otaActive = false;
    strlcpy(otaState, "gagal", sizeof otaState);
    snprintf(otaMsg, sizeof otaMsg, "%s", httpUpdate.getLastErrorString().c_str());
    Serial.printf("[OTA] GAGAL: %s\n", otaMsg);
    snprintf(ev, sizeof ev, "Update firmware gagal: %.55s", otaMsg);
    queueEvent(ev);
    pushStatus();
  }
}

// Tugas cloud berjalan di core 0, terpisah dari kontrol & LCD (core 1)
void cloudTask(void *) {
  uint32_t tCfg = 0, tSys = 0, tSt = 0, tDrain = 0, tAuth = 0, lastRev = 0;
  for (;;) {
    histTick();

    if (WiFi.status() != WL_CONNECTED) {
      cloudOk = false;
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }
    uint32_t now = millis();
    if (!fbReady || now - tokenMs > 50UL * 60UL * 1000UL) {
      if (tAuth == 0 || now - tAuth > 20000) { tAuth = now; fbSignIn(); }
      if (!fbReady) { cloudOk = false; vTaskDelay(pdMS_TO_TICKS(300)); continue; }
    }

    if (cfgPushPending) pushConfig();
    if (sysPushPending) pushSystem();
    if (now - tCfg >= 2000) { tCfg = now; cloudOk = pollConfig(); }
    if (now - tSys >= 6000) { tSys = now; pollSystem(); }
    if (statusRev != lastRev || now - tSt >= 5000) { lastRev = statusRev; tSt = millis(); pushStatus(); }
    if (qCount > 0 && epochOk() && millis() - tDrain >= 800) { tDrain = millis(); drainQueue(); }
    if (evN > 0) flushEvents();
    if (otaReqPending && !otaActive) { if (otaFromFile) doOtaFile(); else doOta(); }

    vTaskDelay(pdMS_TO_TICKS(60));
  }
}

// ================= REBOOT =================
void requestReboot(const char *why) {
  if (rebootReason[0]) return;
  strlcpy(rebootReason, why, sizeof rebootReason);
  queueEvent(why);
  rebootAt = millis() + 2500;          // beri waktu event terkirim
}

void doReboot() {
  Serial.printf("[SISTEM] Reboot: %s\n", rebootReason);
  allRelaysOff();
  if (cfgDirty) saveConfig();
  lcd.clear();
  lcd.setCursor(2, 1);
  lcd.print("MENYALAKAN ULANG");
  lcd.setCursor(0, 2);
  char l[21];
  snprintf(l, sizeof l, "%-20.20s", rebootReason);
  lcd.print(l);
  delay(400);
  ESP.restart();
}

void autoRebootCheck() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  if (!cfg.rbEn || otaActive || rebootReason[0]) return;
  if (cfg.rbMode == 0) {
    if ((uint64_t)millis() >= (uint64_t)cfg.rbHours * 3600000ULL) requestReboot("Reboot otomatis (interval)");
  } else if (timeOk && millis() > 120000UL && timeNow.tm_hour == cfg.rbHour && timeNow.tm_min == cfg.rbMin) {
    requestReboot("Reboot otomatis harian");
  }
}

// ================= HALAMAN WEB LOKAL / PORTAL =================
const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html><html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Manggot Setup</title>
<style>body{font-family:system-ui,sans-serif;background:#0f1512;color:#e7efe9;margin:0;padding:16px;max-width:520px;margin:auto}
h1{font-size:20px;color:#a3e635}.c{background:#18211c;border:1px solid #26332b;border-radius:14px;padding:14px;margin:12px 0}
input,select,button{width:100%;box-sizing:border-box;padding:11px;margin:5px 0;border-radius:10px;border:1px solid #2f3f35;background:#0f1512;color:#e7efe9;font-size:15px}
button{background:#a3e635;color:#0f1512;font-weight:700;border:0}.d{background:#f43f5e;color:#fff}
.r{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #26332b}
.r button{width:auto;padding:6px 12px;margin:0}small{color:#93a39a}</style></head><body>
<h1>&#128027; Inkubator Manggot</h1><div class="c" id="st">Memuat...</div>
<div class="c"><b>WiFi tersimpan</b><div id="wl"></div><br><b>Tambah WiFi</b>
<input id="s" placeholder="Nama WiFi (SSID)"><input id="p" placeholder="Password" type="password">
<button onclick="addW()">Simpan WiFi</button><small>Maksimal 5 jaringan. Perangkat memilih sinyal terkuat.</small></div>
<div class="c"><b>Target suhu &amp; mode</b><input id="sp" type="number" step="0.5" min="20" max="40">
<select id="md"><option value="0">AUTO</option><option value="1">MANUAL</option></select>
<button onclick="setT()">Simpan</button></div>
<div class="c"><b>Update firmware (.bin)</b><input type="file" id="f" accept=".bin"><button onclick="up()">Upload</button><small id="pr"></small></div>
<div class="c"><button class="d" onclick="if(confirm('Reboot perangkat?'))post('/reboot','')">Reboot perangkat</button></div>
<script>
function post(u,b){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(r=>r.text()).then(t=>{alert(t);load()})}
function e(x){return encodeURIComponent(x)}
function addW(){post('/wifi/add','s='+e(s.value)+'&p='+e(p.value))}
function delW(i){if(confirm('Hapus WiFi ini?'))post('/wifi/del','i='+i)}
function setT(){post('/set','sp='+sp.value+'&md='+md.value)}
function up(){var x=f.files[0];if(!x)return;var fd=new FormData();fd.append('f',x);var q=new XMLHttpRequest();
q.upload.onprogress=function(v){pr.textContent='Upload '+Math.round(v.loaded*100/v.total)+'%'};q.onload=function(){pr.textContent=q.responseText};
q.open('POST','/update');q.send(fd)}
function load(){fetch('/api').then(r=>r.json()).then(d=>{
st.innerHTML='Suhu <b>'+(d.ok?d.t.toFixed(1)+' &deg;C':'--')+'</b> &middot; RH <b>'+(d.ok?d.h.toFixed(0)+'%':'--')+'</b><br>Mode <b>'+(d.mode?'MANUAL':'AUTO')+'</b> &middot; Pemanas '+(d.heat?'ON':'OFF')+' &middot; Pendingin '+(d.cool?'ON':'OFF')+'<br><small>WiFi: '+(d.ssid||'tidak terhubung')+' '+d.ip+' &middot; FW v'+d.fw+' &middot; antrian '+d.q+'</small>';
wl.innerHTML=d.wifi.map((w,i)=>'<div class="r"><span>'+w+'</span><button class="d" onclick="delW('+i+')">Hapus</button></div>').join('')||'<small>kosong</small>';
if(document.activeElement!==sp)sp.value=d.sp;md.value=d.mode;})}
load();setInterval(load,3000);
</script></body></html>)HTML";

bool portalAuth() { return true; }        // halaman lokal tanpa password

bool uploadOk = false;

void setupWeb() {
  web.on("/", HTTP_GET, []() {
    if (!portalAuth()) return;
    web.send_P(200, "text/html", PORTAL_HTML);
  });
  web.on("/api", HTTP_GET, []() {
    if (!portalAuth()) return;
    JsonDocument d;
    bool ok = sensorValid();
    d["ok"] = ok;
    if (ok) { d["t"] = temperature; d["h"] = humidity; }
    d["sp"] = cfg.tempSet;  d["mode"] = cfg.mode;
    d["heat"] = (bool)heatOn;  d["cool"] = (bool)coolOn;
    d["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
    d["ip"] = WiFi.localIP().toString();
    d["fw"] = FW_VERSION;  d["q"] = qCount;
    JsonArray a = d["wifi"].to<JsonArray>();
    for (int i = 0; i < cfg.nWifi; i++) a.add(cfg.ssid[i]);
    String s;
    serializeJson(d, s);
    web.send(200, "application/json", s);
  });
  web.on("/wifi/add", HTTP_POST, []() {
    if (!portalAuth()) return;
    String s = web.arg("s"), p = web.arg("p");
    s.trim();
    if (s.length() == 0 || s.length() > 32 || p.length() > 64) { web.send(400, "text/plain", "SSID/password tidak valid"); return; }
    portENTER_CRITICAL(&cfgMux);
    int idx = -1;
    for (int i = 0; i < cfg.nWifi; i++) if (s == cfg.ssid[i]) idx = i;
    if (idx < 0) idx = (cfg.nWifi < MAX_WIFI) ? cfg.nWifi++ : MAX_WIFI - 1;
    strlcpy(cfg.ssid[idx], s.c_str(), 33);
    strlcpy(cfg.pass[idx], p.c_str(), 65);
    portEXIT_CRITICAL(&cfgMux);
    wifiListChanged = true;
    cfgChanged(true, true);
    queueEvent("WiFi ditambah lewat halaman lokal");
    web.send(200, "text/plain", "WiFi disimpan");
  });
  web.on("/wifi/del", HTTP_POST, []() {
    if (!portalAuth()) return;
    int i = web.arg("i").toInt();
    if (cfg.nWifi <= 1 || i < 0 || i >= cfg.nWifi) { web.send(400, "text/plain", "Minimal harus ada 1 WiFi"); return; }
    portENTER_CRITICAL(&cfgMux);
    for (int k = i; k < cfg.nWifi - 1; k++) {
      memcpy(cfg.ssid[k], cfg.ssid[k + 1], 33);
      memcpy(cfg.pass[k], cfg.pass[k + 1], 65);
    }
    cfg.nWifi--;
    memset(cfg.ssid[cfg.nWifi], 0, 33);
    memset(cfg.pass[cfg.nWifi], 0, 65);
    portEXIT_CRITICAL(&cfgMux);
    wifiListChanged = true;
    cfgChanged(true, true);
    web.send(200, "text/plain", "WiFi dihapus");
  });
  web.on("/set", HTTP_POST, []() {
    if (!portalAuth()) return;
    float sp = web.arg("sp").toFloat();
    if (sp < 20 || sp > 40) { web.send(400, "text/plain", "Target harus 20 - 40"); return; }
    cfg.tempSet = sp;
    cfg.mode = web.arg("md").toInt() ? MODE_MANUAL : MODE_AUTO;
    cfgChanged(true, false);
    web.send(200, "text/plain", "Tersimpan");
  });
  web.on("/reboot", HTTP_POST, []() {
    if (!portalAuth()) return;
    web.send(200, "text/plain", "Perangkat akan reboot");
    requestReboot("Reboot dari halaman lokal");
  });
  web.on("/update", HTTP_POST, []() {
    if (!portalAuth()) return;
    bool ok = uploadOk && !Update.hasError();
    web.send(200, "text/plain", ok ? "Berhasil! Perangkat restart..." : "GAGAL update");
    if (ok) requestReboot("Update firmware lokal selesai");
    else otaActive = false;
  }, []() {
    HTTPUpload &up = web.upload();
    if (up.status == UPLOAD_FILE_START) {
      uploadOk = true;
      if (!uploadOk) return;
      Serial.printf("[OTA] Upload lokal: %s\n", up.filename.c_str());
      otaActive = true;
      otaProgress = 0;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) uploadOk = false;
    } else if (up.status == UPLOAD_FILE_WRITE && uploadOk) {
      if (Update.write(up.buf, up.currentSize) != up.currentSize) uploadOk = false;
      otaProgress = (otaProgress + 1) % 100;
    } else if (up.status == UPLOAD_FILE_END && uploadOk) {
      if (!Update.end(true)) uploadOk = false;
      otaProgress = 100;
    }
  });
  web.onNotFound([]() {
    if (apOn) {
      web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
      web.send(302, "text/plain", "");
    } else {
      web.send(404, "text/plain", "Tidak ditemukan");
    }
  });
  web.begin();
}

// ================= BUFFER LAYAR =================
Row      scr[4];
uint16_t shown[4][20];
bool     blOn = true;

void setBacklight(bool on) {
  if (on == blOn) return;
  blOn = on;
  if (on) lcd.backlight(); else lcd.noBacklight();
}

void put(uint8_t (*b)[20], uint8_t r, uint8_t c, const char *s) {
  while (*s && c < 20) b[r][c++] = (uint8_t)*s++;
}
void putCh(uint8_t (*b)[20], uint8_t r, uint8_t c, uint8_t ch) {
  if (c < 20) b[r][c] = ch;
}
void clearBuf(uint8_t (*b)[20], uint8_t rows) {
  memset(b, ' ', rows * 20);
}
void putCenter(uint8_t (*b)[20], uint8_t r, const char *s) {
  int len = strlen(s);
  int c = (20 - len) / 2;
  if (c < 0) c = 0;
  put(b, r, c, s);
}

void putBar(uint8_t (*b)[20], uint8_t r, float v, float vmin, float vmax, float marker) {
  const int w = 18;
  b[r][0] = '[';
  b[r][19] = ']';
  int n = 0;
  if (!isnan(v)) {
    n = (int)((v - vmin) / (vmax - vmin) * w + 0.5f);
    n = constrain(n, 0, w);
  }
  for (int i = 0; i < w; i++) b[r][1 + i] = (i < n) ? 255 : '-';
  if (!isnan(marker)) {
    int m = (int)((marker - vmin) / (vmax - vmin) * w);
    m = constrain(m, 0, w - 1);
    b[r][1 + m] = '|';
  }
}

void flushScreen() {
  int curR = -1, curC = -1;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 20; c++) {
      if (scr[r][c] != shown[r][c]) {
        if (r != curR || c != curC) lcd.setCursor(c, r);
        lcd.write(scr[r][c]);
        shown[r][c] = scr[r][c];
        curR = r;
        curC = c + 1;
      }
    }
  }
}

void invalidateScreen() {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 20; c++) shown[r][c] = 0xFFFF;
}

// ================= HALAMAN =================
enum { P_TEMP = 0, P_HUM, P_TARGET, P_TIME, P_WIFI, P_ACT, P_CLOUD, N_NORMAL, P_ALERT = N_NORMAL };

const uint16_t PAGE_MS[] = {5000, 5000, 3500, 3500, 3500, 4000, 3500, 3500};

uint8_t  curPage = P_TEMP, normalIdx = 0;
uint32_t pageStart = 0;
bool     trans = false;
uint8_t  transOld = 0, transNew = 0;
int      transStep = 0;
uint32_t lastUiMs = 0;

void renderPage(uint8_t page, uint8_t (*out)[20]) {
  char l[40];
  uint32_t fr = millis() / 350;
  bool valid = sensorValid();

  switch (page) {
    case P_TEMP: {
      putCh(out, 0, 0, CH_TEMP);
      put(out, 0, 2, "SUHU");
      if (valid) snprintf(l, sizeof l, "%4.1f\337C", temperature);
      else       snprintf(l, sizeof l, "--.-\337C");
      put(out, 0, 7, l);
      snprintf(l, sizeof l, "S:%.1f", cfg.tempSet);
      put(out, 0, 14, l);
      putBar(out, 1, valid ? temperature : NAN, 20.0f, 40.0f, cfg.tempSet);
      break;
    }
    case P_HUM: {
      putCh(out, 0, 0, CH_DROP);
      put(out, 0, 2, "RH");
      if (valid) snprintf(l, sizeof l, "%5.1f%%", humidity);
      else       snprintf(l, sizeof l, "---.-%%");
      put(out, 0, 5, l);
      snprintf(l, sizeof l, "S:%.0f%%", cfg.humSet);
      put(out, 0, 14, l);
      putBar(out, 1, valid ? humidity : NAN, 0.0f, 100.0f, cfg.humSet);
      break;
    }
    case P_TARGET: {
      snprintf(l, sizeof l, "TARGET %.1f\337C %s", cfg.tempSet, cfg.mode ? "MANUAL" : "AUTO");
      putCenter(out, 0, l);
      snprintf(l, sizeof l, "RH ideal %.0f%% (%.0f-%.0f)", cfg.humSet, cfg.warnHL, cfg.warnHH);
      putCenter(out, 1, l);
      break;
    }
    case P_TIME: {
      if (timeOk) {
        snprintf(l, sizeof l, "%s, %02d %s %04d", DAY_FULL[timeNow.tm_wday], timeNow.tm_mday,
                 MON_SHORT[timeNow.tm_mon], timeNow.tm_year + 1900);
        putCenter(out, 0, l);
        snprintf(l, sizeof l, "Jam %02d:%02d:%02d WIB", timeNow.tm_hour, timeNow.tm_min, timeNow.tm_sec);
        putCenter(out, 1, l);
      } else {
        putCenter(out, 0, "Waktu belum sinkron");
        putCenter(out, 1, "(butuh WiFi)");
      }
      break;
    }
    case P_WIFI: {
      bool con = (WiFi.status() == WL_CONNECTED);
      if (con || ((fr & 1) == 0)) putCh(out, 0, 0, CH_WIFI);
      if (con) {
        snprintf(l, sizeof l, "%.11s", WiFi.SSID().c_str());
        put(out, 0, 2, l);
        snprintf(l, sizeof l, "%ddB", WiFi.RSSI());
        put(out, 0, 14, l);
      } else {
        put(out, 0, 2, "WiFi mencari...");
      }
      if (apOn) {
        snprintf(l, sizeof l, "AP %s", WiFi.softAPIP().toString().c_str());
      } else {
        uint32_t s = millis() / 1000;
        snprintf(l, sizeof l, "Aktif %luj %02lum %02lud", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
      }
      putCenter(out, 1, l);
      break;
    }
    case P_ACT: {
      if (!coolOn || (fr & 1)) putCh(out, 0, 0, CH_SNOW);
      put(out, 0, 2, "PELTIER+KIPAS");
      put(out, 0, 16, coolOn ? "ON" : "OFF");
      if (!heatOn || (fr & 1)) putCh(out, 1, 0, CH_FLAME);
      put(out, 1, 2, "LAMPU PEMANAS");
      put(out, 1, 16, heatOn ? "ON" : "OFF");
      break;
    }
    case P_CLOUD: {
      bool con = (WiFi.status() == WL_CONNECTED);
      snprintf(l, sizeof l, "CLOUD %s", (con && fbReady && cloudOk) ? "TERHUBUNG" : "OFFLINE");
      putCenter(out, 0, l);
      snprintf(l, sizeof l, "Antri %lu  FW %s", (unsigned long)qCount, FW_VERSION);
      putCenter(out, 1, l);
      break;
    }
    case P_ALERT: {
      putCh(out, 0, 0, CH_WARN);
      putCh(out, 0, 19, CH_WARN);
      bool blink = ((millis() / 400) & 1) == 0;
      const char *title = "KONDISI NORMAL";
      l[0] = 0;
      switch (alertShown) {
        case A_TEMP_LOW:
          title = "SUHU KEDINGINAN";
          snprintf(l, sizeof l, "%.1f\337C  batas %.0f\337C", temperature, cfg.warnTL);
          break;
        case A_TEMP_HIGH:
          title = "SUHU TERLALU PANAS";
          snprintf(l, sizeof l, "%.1f\337C  batas %.0f\337C", temperature, cfg.warnTH);
          break;
        case A_HUM_LOW:
          title = "KELEMBABAN RENDAH";
          snprintf(l, sizeof l, "RH %.1f%%  min %.0f%%", humidity, cfg.warnHL);
          break;
        case A_HUM_HIGH:
          title = "KELEMBABAN TINGGI";
          snprintf(l, sizeof l, "RH %.1f%%  maks %.0f%%", humidity, cfg.warnHH);
          break;
        case A_SENSOR:
          title = "SENSOR ERROR";
          snprintf(l, sizeof l, "Relay dimatikan");
          break;
      }
      if (blink) putCenter(out, 0, title);
      putCenter(out, 1, l);
      break;
    }
  }
}

void drawHeader(uint8_t (*b)[20]) {
  char l[24];
  if (timeOk) {
    char c = (((millis() / 500) & 1) == 0) ? ':' : ' ';
    snprintf(l, sizeof l, "%s %02d/%02d %02d%c%02d%c%02d", DAY_SHORT[timeNow.tm_wday], timeNow.tm_mday,
             timeNow.tm_mon + 1, timeNow.tm_hour, c, timeNow.tm_min, c, timeNow.tm_sec);
  } else {
    snprintf(l, sizeof l, "--- --/-- --:--:--");
  }
  put(b, 0, 0, l);
  putCh(b, 0, 18, cfg.mode ? 'M' : 'A');                       // A = AUTO, M = MANUAL
  bool con = (WiFi.status() == WL_CONNECTED);
  if (con || (((millis() / 500) & 1) == 0)) putCh(b, 0, 19, CH_WIFI);
}

void drawAction(uint8_t (*b)[20]) {
  uint32_t fr = millis() / 350;
  bool f = (fr & 1);
  if (alerts & A_SENSOR) {
    putCh(b, 3, 0, f ? CH_WARN : ' ');
    putCh(b, 3, 19, f ? CH_WARN : ' ');
    put(b, 3, 2, "SENSOR ERROR  OFF");
  } else if (safetyFlags) {
    putCh(b, 3, 0, f ? CH_WARN : ' ');
    putCh(b, 3, 19, f ? CH_WARN : ' ');
    putCenter(b, 3, "BATAS AMAN AKTIF");
  } else if (coolOn) {
    putCh(b, 3, 0, f ? CH_SNOW : ' ');
    putCh(b, 3, 19, f ? ' ' : CH_SNOW);
    put(b, 3, 1, "PENDINGIN MENYALA");
  } else if (heatOn) {
    putCh(b, 3, 0, f ? CH_FLAME : ' ');
    putCh(b, 3, 19, f ? ' ' : CH_FLAME);
    put(b, 3, 2, "PEMANAS MENYALA");
  } else {
    putCh(b, 3, 0, f ? CH_LARVA1 : CH_LARVA2);
    putCh(b, 3, 19, f ? CH_LARVA2 : CH_LARVA1);
    putCenter(b, 3, cfg.mode ? "MODE MANUAL" : "SUHU STABIL");
  }
}

void startTransition(uint8_t target) {
  transOld = curPage;
  transNew = target;
  transStep = 0;
  trans = true;
  if (curPage == P_ALERT) setBacklight(true);
}

uint8_t pickNextPage() {
  if (alerts && curPage != P_ALERT) {
    pickAlertToShow();
    return P_ALERT;
  }
  normalIdx = (normalIdx + 1) % N_NORMAL;
  return normalIdx;
}

void uiUpdate() {
  uint32_t now = millis();
  if (now - lastUiMs < 40) return;
  lastUiMs = now;
  updateTime();

  Row buf[4];
  clearBuf(buf, 4);
  drawHeader(buf);

  // Layar khusus saat update firmware
  if (otaActive) {
    setBacklight(true);
    putCenter(buf, 1, "UPDATE FIRMWARE");
    putBar(buf, 2, (float)otaProgress, 0.0f, 100.0f, NAN);
    putCenter(buf, 3, "JANGAN CABUT DAYA");
    memcpy(scr, buf, sizeof(scr));
    flushScreen();
    return;
  }

  if (newAlertFlag && curPage == P_ALERT) newAlertFlag = false;

  if (trans) {
    transStep += 2;
    if (transStep >= 22) {
      trans = false;
      curPage = transNew;
      pageStart = now;
      if (curPage < N_NORMAL) normalIdx = curPage;
    }
  } else {
    bool leaveAlert = (curPage == P_ALERT && alerts == 0);
    bool goAlert = (newAlertFlag && alerts && curPage != P_ALERT);
    if (goAlert) {
      newAlertFlag = false;
      pickAlertToShow();
      startTransition(P_ALERT);
    } else if (leaveAlert || (now - pageStart >= PAGE_MS[curPage])) {
      startTransition(pickNextPage());
    }
  }

  // Backlight berkedip saat halaman peringatan
  if (!trans && curPage == P_ALERT) setBacklight(((now / 500) & 1) == 0);
  else if (!blOn) setBacklight(true);

  if (trans) {
    Row a[2], b[2];
    clearBuf(a, 2);
    clearBuf(b, 2);
    renderPage(transOld, a);
    renderPage(transNew, b);
    for (int r = 0; r < 2; r++) {
      for (int c = 0; c < 20; c++) {
        if (c < transStep) buf[1 + r][c] = b[r][c];
        else if (c < transStep + 2) buf[1 + r][c] = 255;
        else buf[1 + r][c] = a[r][c];
      }
    }
  } else {
    renderPage(curPage, buf + 1);
  }
  drawAction(buf);
  memcpy(scr, buf, sizeof(scr));
  flushScreen();
}

// ================= ANIMASI BOOT =================
void bootBar(int p, int fr) {
  lcd.setCursor(0, 3);
  for (int i = 0; i < 20; i++) {
    if (i < p) lcd.write((uint8_t)255);
    else if (i == p) lcd.write((uint8_t)(CH_LARVA1 + (fr & 1)));
    else lcd.write((uint8_t)' ');
  }
}

void bootMsg(const char *label, const char *status) {
  char b[28];
  snprintf(b, sizeof b, "%-14.14s%6s", label, status);
  lcd.setCursor(0, 2);
  lcd.print(b);
}

bool condSht()  { shtUpdate(); return shtOk; }
bool condWifi() { wifiTick(); return WiFi.status() == WL_CONNECTED; }
bool condNtp()  { updateTime(); return timeOk; }

bool waitStep(const char *label, int p0, int p1, uint32_t timeoutMs, bool (*cond)(), const char *failText) {
  uint32_t t0 = millis();
  int fr = 0;
  bool ok = false;
  const char *dots[] = {".  ", ".. ", "..."};
  while (millis() - t0 < timeoutMs) {
    if (cond()) { ok = true; break; }
    int p = p0 + (int)((p1 - p0) * (millis() - t0) / timeoutMs);
    bootMsg(label, dots[fr % 3]);
    bootBar(p, fr);
    fr++;
    delay(90);
  }
  bootBar(p1, fr);
  bootMsg(label, ok ? "OK" : failText);
  delay(600);
  return ok;
}

void bootAnimation() {
  lcd.clear();

  // 1. Garis pembuka dari tepi ke tengah
  for (int i = 0; i < 10; i++) {
    lcd.setCursor(i, 0);      lcd.write((uint8_t)255);
    lcd.setCursor(19 - i, 0); lcd.write((uint8_t)255);
    delay(45);
  }

  // 2. Judul mengetik
  const char *title = "INKUBATOR MANGGOT";
  lcd.setCursor(1, 1);
  for (int i = 0; title[i]; i++) {
    lcd.write((uint8_t)title[i]);
    delay(55);
  }

  // 3. Larva merayap dan "memunculkan" subjudul
  const char sub[] = " ~ BSF LARVA CARE ~ ";
  for (int x = 0; x < 20; x++) {
    lcd.setCursor(0, 2);
    for (int i = 0; i < 20; i++) {
      if (i < x) lcd.write((uint8_t)sub[i]);
      else if (i == x) lcd.write((uint8_t)(CH_LARVA1 + (x & 1)));
      else lcd.write((uint8_t)' ');
    }
    delay(70);
  }
  delay(700);

  lcd.setCursor(0, 3);
  lcd.print("    by TeknoBro     ");
  delay(900);
  lcd.setCursor(0, 3);
  lcd.print("                    ");

  // 4. Self-test dengan progress larva
  for (int p = 0; p <= 3; p++) { bootBar(p, p); delay(60); }
  bootMsg("Cek LCD", "OK");
  delay(400);
  char l[24];
  snprintf(l, sizeof l, "Memori %lu antri", (unsigned long)qCount);
  bootMsg(l, qFs ? "OK" : "RAM");
  delay(600);

  shtT = millis() - 1000;               // paksa pembacaan segera
  waitStep("Cek SHT30", 3, 7, 3500, condSht, "GAGAL");

  snprintf(l, sizeof l, "WiFi (%d)", cfg.nWifi);
  bool w = waitStep(l, 7, 12, 20000, condWifi, "OFFLINE");

  if (w) {
    startNtp();
    waitStep("Sinkron Waktu", 12, 16, 4000, condNtp, "GAGAL");
    bootMsg("Login Cloud", "...");
    bootBar(17, 0);
    bool c = fbSignIn();
    bootBar(19, 1);
    bootMsg("Login Cloud", c ? "OK" : "GAGAL");
    delay(600);
  } else {
    bootBar(19, 0);
    bootMsg("Mode Offline", "SIMPAN");
    delay(600);
  }

  // 5. Siap
  lcd.clear();
  lcd.setCursor(4, 1);
  lcd.print("SISTEM SIAP!");
  snprintf(l, sizeof l, "Target %.1f\337C %s", cfg.tempSet, cfg.mode ? "MAN" : "AUTO");
  lcd.setCursor((20 - strlen(l)) / 2, 2);
  lcd.print(l);
  for (int i = 0; i < 3; i++) {
    lcd.noBacklight(); delay(140);
    lcd.backlight();   delay(140);
  }

  // 6. Tirai menutup lalu membuka
  for (int i = 0; i < 10; i++) {
    for (int r = 0; r < 4; r++) {
      lcd.setCursor(i, r);      lcd.write((uint8_t)255);
      lcd.setCursor(19 - i, r); lcd.write((uint8_t)255);
    }
    delay(35);
  }
  for (int i = 9; i >= 0; i--) {
    for (int r = 0; r < 4; r++) {
      lcd.setCursor(i, r);      lcd.write((uint8_t)' ');
      lcd.setCursor(19 - i, r); lcd.write((uint8_t)' ');
    }
    delay(35);
  }
  lcd.clear();
  invalidateScreen();
}

// ================= SERIAL =================
void printStatus() {
  char tstr[12] = "--:--:--";
  if (timeOk) snprintf(tstr, sizeof tstr, "%02d:%02d:%02d", timeNow.tm_hour, timeNow.tm_min, timeNow.tm_sec);
  char an[64];
  alertNames(an, sizeof an);
  const char *wifi = WiFi.status() == WL_CONNECTED ? "OK" : (apOn ? "AP" : "OFF");
  const char *cloud = (fbReady && cloudOk) ? "OK" : "OFF";
  if (sensorValid()) {
    Serial.printf("[%s] T=%.2fC H=%.1f%% SET=%.1fC %s | PELTIER=%s LAMPU=%s | WiFi=%s Cloud=%s Antri=%lu | ALERT=%s\n",
                  tstr, temperature, humidity, cfg.tempSet, cfg.mode ? "MANUAL" : "AUTO", coolOn ? "ON" : "OFF",
                  heatOn ? "ON" : "OFF", wifi, cloud, (unsigned long)qCount, an);
  } else {
    Serial.printf("[%s] SENSOR ERROR %s | PELTIER=%s LAMPU=%s | WiFi=%s Cloud=%s Antri=%lu | ALERT=%s\n",
                  tstr, cfg.mode ? "MANUAL" : "AUTO", coolOn ? "ON" : "OFF", heatOn ? "ON" : "OFF", wifi, cloud,
                  (unsigned long)qCount, an);
  }
}

void handleCommand(String raw) {
  raw.trim();
  if (raw.length() == 0) return;
  String cmd = raw;
  cmd.toUpperCase();

  if (cmd.startsWith("WIFI ADD ")) {                  // WIFI ADD nama;password (huruf besar/kecil dipertahankan)
    String arg = raw.substring(9);
    int sc = arg.indexOf(';');
    String s = sc >= 0 ? arg.substring(0, sc) : arg;
    String p = sc >= 0 ? arg.substring(sc + 1) : "";
    s.trim();
    if (s.length() == 0 || s.length() > 32) { Serial.println(F("SSID tidak valid")); return; }
    portENTER_CRITICAL(&cfgMux);
    int idx = -1;
    for (int i = 0; i < cfg.nWifi; i++) if (s == cfg.ssid[i]) idx = i;
    if (idx < 0) idx = (cfg.nWifi < MAX_WIFI) ? cfg.nWifi++ : MAX_WIFI - 1;
    strlcpy(cfg.ssid[idx], s.c_str(), 33);
    strlcpy(cfg.pass[idx], p.c_str(), 65);
    portEXIT_CRITICAL(&cfgMux);
    wifiListChanged = true;
    cfgChanged(true, true);
    Serial.printf("[CMD] WiFi \"%s\" disimpan di slot %d\n", s.c_str(), idx + 1);
  } else if (cmd.startsWith("WIFI DEL ")) {
    int i = cmd.substring(9).toInt() - 1;
    if (cfg.nWifi <= 1 || i < 0 || i >= cfg.nWifi) { Serial.println(F("Minimal harus ada 1 WiFi")); return; }
    portENTER_CRITICAL(&cfgMux);
    for (int k = i; k < cfg.nWifi - 1; k++) {
      memcpy(cfg.ssid[k], cfg.ssid[k + 1], 33);
      memcpy(cfg.pass[k], cfg.pass[k + 1], 65);
    }
    cfg.nWifi--;
    portEXIT_CRITICAL(&cfgMux);
    wifiListChanged = true;
    cfgChanged(true, true);
    Serial.println(F("[CMD] WiFi dihapus"));
  } else if (cmd == "WIFI LIST") {
    for (int i = 0; i < cfg.nWifi; i++) Serial.printf("  %d. %s\n", i + 1, cfg.ssid[i]);
  } else if (cmd.startsWith("SET")) {
    float v = cmd.substring(3).toFloat();
    if (v < 20.0f || v > 40.0f) {
      Serial.println("Target harus 20 - 40 C. Contoh: SET 30");
      return;
    }
    cfg.tempSet = v;
    cfgChanged(true, false);
    Serial.printf("[CMD] Target suhu = %.1f C\n", cfg.tempSet);
  } else if (cmd == "MODE AUTO" || cmd == "MODE MANUAL") {
    cfg.mode = (cmd == "MODE MANUAL") ? MODE_MANUAL : MODE_AUTO;
    cfgChanged(true, false);
    Serial.printf("[CMD] Mode = %s\n", cfg.mode ? "MANUAL" : "AUTO");
  } else if (cmd == "HEAT ON" || cmd == "HEAT OFF" || cmd == "COOL ON" || cmd == "COOL OFF") {
    if (cfg.mode != MODE_MANUAL) { Serial.println(F("Ubah ke MODE MANUAL dulu")); return; }
    bool on = cmd.endsWith("ON");
    portENTER_CRITICAL(&cfgMux);
    if (cmd.startsWith("HEAT")) { cfg.manHeat = on; if (on) cfg.manCool = 0; }
    else                        { cfg.manCool = on; if (on) cfg.manHeat = 0; }
    portEXIT_CRITICAL(&cfgMux);
    cfgChanged(true, false);
    Serial.println(F("[CMD] OK"));
  } else if (cmd == "RELAY SWAP" || cmd == "RELAY NORMAL" || cmd == "RELAY HIGH" || cmd == "RELAY LOW") {
    allRelaysOff();
    portENTER_CRITICAL(&cfgMux);
    if (cmd == "RELAY SWAP") cfg.relaySwap = 1;
    else if (cmd == "RELAY NORMAL") cfg.relaySwap = 0;
    else cfg.relayHigh = (cmd == "RELAY HIGH");
    portEXIT_CRITICAL(&cfgMux);
    relayCfgChanged = true;
    cfgChanged(true, true);
  } else if (cmd == "REBOOT") {
    requestReboot("Reboot dari Serial");
  } else if (cmd == "STATUS") {
    printStatus();
  } else if (cmd == "HELP") {
    Serial.println(F("SET 31            ubah target (20-40)"));
    Serial.println(F("MODE AUTO|MANUAL  ubah mode"));
    Serial.println(F("HEAT ON|OFF       pemanas (mode manual)"));
    Serial.println(F("COOL ON|OFF       pendingin (mode manual)"));
    Serial.println(F("WIFI LIST | WIFI ADD nama;pass | WIFI DEL 2"));
    Serial.println(F("RELAY SWAP|NORMAL tukar relay, RELAY HIGH|LOW polaritas"));
    Serial.println(F("REBOOT | STATUS"));
  } else {
    Serial.println(F("Perintah tidak dikenal. Ketik HELP"));
  }
}

String rxBuf = "";
void serialUpdate() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      handleCommand(rxBuf);
      rxBuf = "";
    } else if (rxBuf.length() < 110) {
      rxBuf += c;
    }
  }
}

// ================= SETUP & LOOP =================
void setup() {
  pinMode(PIN_LED, OUTPUT);
  Serial.begin(115200);
  Serial.println();
  Serial.printf("=== INKUBATOR MANGGOT IoT  v%s ===\n", FW_VERSION);

  prefs.begin("manggot", false);
  bootCount = prefs.getULong("boot", 0) + 1;
  prefs.putULong("boot", bootCount);
  loadConfig();
  // relay disiapkan setelah setelan terbaca (polaritas & penukaran dari memori)
  digitalWrite(PIN_RELAY_PELT, lvl(false));
  digitalWrite(PIN_RELAY_LAMP, lvl(false));
  pinMode(PIN_RELAY_PELT, OUTPUT);
  pinMode(PIN_RELAY_LAMP, OUTPUT);
  digitalWrite(PIN_RELAY_PELT, lvl(false));
  digitalWrite(PIN_RELAY_LAMP, lvl(false));
  Serial.printf("[RELAY] Pendingin=GPIO%d  Pemanas=GPIO%d  aktif %s\n", pinCool(), pinHeat(), cfg.relayHigh ? "HIGH" : "LOW");
  Serial.printf("[NVS] Boot ke-%lu, target %.1f C, mode %s, %d WiFi tersimpan\n", (unsigned long)bootCount,
                cfg.tempSet, cfg.mode ? "MANUAL" : "AUTO", cfg.nWifi);

  // Catat event boot / perubahan firmware
  String lastFw = prefs.getString("fw", "");
  char ev[80];
  if (lastFw.length() && lastFw != FW_VERSION) snprintf(ev, sizeof ev, "Firmware diperbarui v%s -> v%s", lastFw.c_str(), FW_VERSION);
  else snprintf(ev, sizeof ev, "Perangkat menyala (boot ke-%lu, v%s)", (unsigned long)bootCount, FW_VERSION);
  queueEvent(ev);
  if (lastFw != FW_VERSION) prefs.putString("fw", FW_VERSION);

  qInit();

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  delay(100);
  lcd.init();
  lcd.backlight();
  lcd.createChar(CH_WIFI, gWifi);
  lcd.createChar(CH_TEMP, gTemp);
  lcd.createChar(CH_DROP, gDrop);
  lcd.createChar(CH_SNOW, gSnow);
  lcd.createChar(CH_FLAME, gFlame);
  lcd.createChar(CH_WARN, gWarn);
  lcd.createChar(CH_LARVA1, gLarva1);
  lcd.createChar(CH_LARVA2, gLarva2);
  lcd.clear();

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("manggot");
  tls.setInsecure();                 // sertifikat tidak diverifikasi (hemat memori)
  http.setReuse(true);
  http.setTimeout(8000);

  bootAnimation();
  setupWeb();

  lastCoolChange = lastHeatChange = millis() - MIN_SWITCH_MS;
  pageStart = millis();
  updateTime();

  xTaskCreatePinnedToCore(cloudTask, "cloud", 16384, nullptr, 1, nullptr, 0);
  Serial.println(F("Sistem berjalan. Ketik HELP untuk perintah."));
}

void loop() {
  shtUpdate();
  wifiTick();
  controlUpdate();
  alertUpdate();
  ledUpdate();
  serialUpdate();
  uiUpdate();
  web.handleClient();
  if (apOn) dns.processNextRequest();

  if (cfgDirty && millis() - cfgDirtyMs > 1500) {
    cfgDirty = false;
    saveConfig();
    Serial.println(F("[NVS] Setelan disimpan"));
  }

  if (relayCfgChanged) applyRelayConfig();
  autoRebootCheck();
  if (rebootReason[0] && (int32_t)(millis() - rebootAt) >= 0) doReboot();

  static uint32_t lastPrint = 0;
  if (millis() - lastPrint >= 5000) {
    lastPrint = millis();
    printStatus();
  }
}
