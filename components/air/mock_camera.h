#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_camera.h"

void init_mock_camera();
void mock_camera_process();
sensor_t* mock_camera_sensor_get();
