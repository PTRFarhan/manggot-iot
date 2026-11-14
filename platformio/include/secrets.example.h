// =====================================================================
//  SALIN file ini menjadi  secrets.h  lalu isi sesuai project kamu.
//  secrets.h TIDAK ikut ter-upload ke GitHub (sudah ada di .gitignore)
// =====================================================================
#pragma once

// --- Firebase (Project settings > General > Web API Key) ---
#define FB_API_KEY       "ISI_WEB_API_KEY"
// --- Realtime Database URL (tanpa garis miring di akhir) ---
#define FB_DB_URL        "https://NAMA-PROJECT-default-rtdb.asia-southeast1.firebasedatabase.app"

// --- Akun khusus perangkat (dibuat di Authentication > Users) ---
//     Email ini HARUS sama dengan email di database.rules.json
#define FB_DEVICE_EMAIL  "esp32@manggot.app"
#define FB_DEVICE_PASS   "GANTI_PASSWORD_PERANGKAT"

// --- ID perangkat (harus sama dengan DEVICE_ID di firebase-config.js) ---
#define DEVICE_ID        "manggot01"

// --- WiFi bawaan (dipakai kalau memori masih kosong) ---
#if WOKWI_SIM
#define WIFI_DEFAULT_SSID "Wokwi-GUEST"   // simulasi Wokwi
#define WIFI_DEFAULT_PASS ""
#else
#define WIFI_DEFAULT_SSID "Redmi Note 14 5G"
#define WIFI_DEFAULT_PASS "Farhan2005"
#endif

// --- Password hotspot setup "MANGGOT-SETUP" & halaman web lokal (min 8 karakter) ---
#define AP_PASS          "manggot123"
