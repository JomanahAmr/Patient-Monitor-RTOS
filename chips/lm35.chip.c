#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  pin_t pin_out;
  uint32_t temperature_attr;
  timer_t timer;
} chip_state_t;

static void update_output(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;

  float temp_c = attr_read_float(chip->temperature_attr);

  /*
    Wokwi-friendly LM35 output.

    This maps the slider directly to a wider analog voltage range:
    0°C   -> 0.0V
    100°C -> 3.3V

    Examples:
    15°C   -> 0.495V
    36.7°C -> 1.211V
    41°C   -> 1.353V

    This avoids the very small real LM35 voltage range that was causing
    large wrong readings in the ESP32 ADC simulation.
  */
  float voltage = (temp_c / 100.0f) * 3.3f;

  if (voltage < 0.0f) {
    voltage = 0.0f;
  }

  if (voltage > 3.3f) {
    voltage = 3.3f;
  }

  pin_dac_write(chip->pin_out, voltage);
}

void chip_init() {
  chip_state_t *chip = malloc(sizeof(chip_state_t));

  chip->pin_out = pin_init("OUT", ANALOG);
  chip->temperature_attr = attr_init_float("temperature", 36.7f);

  const timer_config_t timer_config = {
    .callback = update_output,
    .user_data = chip
  };

  chip->timer = timer_init(&timer_config);
  timer_start(chip->timer, 50000, true);

  printf("Wokwi-friendly LM35 temperature chip initialized\n");
}