#pragma once

#include "esp_err.h"

bool temperature_sensor_available();
void temperature_sensor_init(void);
void temperature_sensor_read(float *temperature);

