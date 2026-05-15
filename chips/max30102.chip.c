#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>

const int ADDRESS = 0x22;

typedef struct {
  pin_t pin_int;
  uint32_t heart_rate_attr;
  uint32_t spo2_attr;
  uint8_t selected_register;
} chip_state_t;

static bool on_i2c_connect(void *user_data, uint32_t address, bool connect);
static uint8_t on_i2c_read(void *user_data);
static bool on_i2c_write(void *user_data, uint8_t data);
static void on_i2c_disconnect(void *user_data);

void chip_init() {
  chip_state_t *chip = malloc(sizeof(chip_state_t));

  chip->pin_int = pin_init("INT", INPUT);

  chip->heart_rate_attr = attr_init("heartRate", 75);
  chip->spo2_attr = attr_init("spo2", 98);

  chip->selected_register = 0;

  const i2c_config_t i2c_config = {
    .user_data = chip,
    .address = ADDRESS,
    .scl = pin_init("SCL", INPUT),
    .sda = pin_init("SDA", INPUT),
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
    .disconnect = on_i2c_disconnect,
  };

  i2c_init(&i2c_config);

  printf("MAX30102 custom chip initialized: register 0=HR, register 1=SpO2\n");
}

bool on_i2c_connect(void *user_data, uint32_t address, bool connect) {
  return true;
}

uint8_t on_i2c_read(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;

  uint32_t heart_rate = attr_read(chip->heart_rate_attr);
  uint32_t spo2 = attr_read(chip->spo2_attr);

  if (chip->selected_register == 0) {
    return (uint8_t)heart_rate;
  }

  if (chip->selected_register == 1) {
    return (uint8_t)spo2;
  }

  return 0;
}

bool on_i2c_write(void *user_data, uint8_t data) {
  chip_state_t *chip = (chip_state_t *)user_data;

  chip->selected_register = data;

  return true;
}

void on_i2c_disconnect(void *user_data) {
}