# Manggot IoT, versi PlatformIO

Program ESP32 inkubator manggot yang sama dengan versi Arduino IDE (firmware v2.1.0), disusun untuk **PlatformIO** (VS Code).

```
platformio/
├── platformio.ini          board, partisi, library, 2 environment
├── src/main.cpp            program utama
├── include/secrets.h       WiFi dan Firebase (tidak ikut ke GitHub)
├── include/secrets.example.h
├── scripts/copy_bin.py     menyalin hasil build ke bin/manggot-v<versi>.bin
└── bin/                    file .bin siap OTA (dibuat otomatis)
```

## Cara pakai (VS Code)

1. Pasang **VS Code**, lalu extension **PlatformIO IDE**.
2. **File › Open Folder**, pilih folder `platformio` (di dalam folder proyek).
3. Tunggu PlatformIO mengunduh platform ESP32 dan library (sekali saja, beberapa menit).
4. Klik ikon PlatformIO di kiri, lalu pilih:
   * **esp32dev › Build** untuk compile
   * **esp32dev › Upload** untuk upload ke ESP32 lewat kabel
   * **esp32dev › Monitor** untuk Serial Monitor 115200

Atau lewat terminal:

```bash
pio run -e esp32dev                 # build
pio run -e esp32dev -t upload       # upload
pio device monitor                  # serial monitor
```

## Environment

| Env | Untuk | Catatan |
|---|---|---|
| `esp32dev` | ESP32 asli | relay active LOW, WiFi dari `secrets.h`, partisi `min_spiffs.csv` |
| `wokwi` | simulasi Wokwi | relay aktif HIGH, WiFi `Wokwi-GUEST`, partisi default |

## Update firmware (OTA)

1. Naikkan `FW_VERSION` di `src/main.cpp`, misalnya `"2.1.1"`.
2. Klik **Build**.
3. File `bin/manggot-v2.1.1.bin` dibuat otomatis.
4. Buka aplikasi › **Sistem › Update Firmware** › pilih file itu › **Kirim & update**.

## Catatan

* Platform yang dipakai adalah **pioarduino** (Arduino core ESP32 versi 3.x). Platform resmi `espressif32` di PlatformIO masih memakai core 2.x.
* `include/secrets.h` berisi password, jangan di-upload ke GitHub (sudah ada di `.gitignore`).
* Library: `LiquidCrystal_I2C` (Frank de Brabander, paket `marcoschwartz/LiquidCrystal_I2C`) dan `ArduinoJson` v7, diunduh otomatis.
* Kalau upload gagal di 921600 baud, ubah `upload_speed` di `platformio.ini` menjadi `115200`.
