// Chip kustom Wokwi: sensor SHT30 (I2C 0x44)
// Mendukung perintah single shot 0x24 0x00 dan mengirim 6 byte + CRC seperti sensor asli.
#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct {
  uint32_t temp_attr;
  uint32_t hum_attr;
  uint8_t  cmd[2];
  int      cmd_len;
  uint8_t  out[6];
  int      out_idx;
} chip_state_t;

static uint8_t crc8(const uint8_t *d, int n) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

static void measure(chip_state_t *c) {
  float t = attr_read_float(c->temp_attr);
  float h = attr_read_float(c->hum_attr);
  if (h < 0) h = 0;
  if (h > 100) h = 100;
  uint16_t rt = (uint16_t)((t + 45.0f) / 175.0f * 65535.0f);
  uint16_t rh = (uint16_t)(h / 100.0f * 65535.0f);
  c->out[0] = rt >> 8; c->out[1] = rt & 0xFF; c->out[2] = crc8(c->out, 2);
  c->out[3] = rh >> 8; c->out[4] = rh & 0xFF; c->out[5] = crc8(c->out + 3, 2);
  c->out_idx = 0;
}

static bool on_i2c_connect(void *user_data, uint32_t address, bool read) {
  chip_state_t *c = (chip_state_t *)user_data;
  if (read) c->out_idx = 0; else c->cmd_len = 0;
  return true;
}

static uint8_t on_i2c_read(void *user_data) {
  chip_state_t *c = (chip_state_t *)user_data;
  if (c->out_idx < 6) return c->out[c->out_idx++];
  return 0xFF;
}

static bool on_i2c_write(void *user_data, uint8_t data) {
  chip_state_t *c = (chip_state_t *)user_data;
  if (c->cmd_len < 2) c->cmd[c->cmd_len++] = data;
  if (c->cmd_len == 2 && c->cmd[0] == 0x24) measure(c);
  return true;
}

static void on_i2c_disconnect(void *user_data) {}

void chip_init(void) {
  chip_state_t *c = malloc(sizeof(chip_state_t));
  c->temp_attr = attr_init_float("temperature", 31.0);
  c->hum_attr  = attr_init_float("humidity", 70.0);
  c->cmd_len = 0;
  c->out_idx = 0;
  measure(c);
  const i2c_config_t cfg = {
    .user_data  = c,
    .address    = 0x44,
    .scl        = pin_init("SCL", INPUT_PULLUP),
    .sda        = pin_init("SDA", INPUT_PULLUP),
    .connect    = on_i2c_connect,
    .read       = on_i2c_read,
    .write      = on_i2c_write,
    .disconnect = on_i2c_disconnect,
  };
  i2c_init(&cfg);
  printf("SHT30 siap di alamat 0x44\n");
}
