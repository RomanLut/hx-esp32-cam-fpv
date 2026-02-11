#pragma once

#include <cassert>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#if CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/ets_sys.h"  // will be removed in idf v5.0
#elif CONFIG_IDF_TARGET_ESP32S2
#include "esp32s2/rom/ets_sys.h"
#elif CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/ets_sys.h"
#elif CONFIG_IDF_TARGET_ESP32C5
#include "esp32c5/rom/ets_sys.h"
#endif

extern SemaphoreHandle_t s_safe_printf_mux;

#define SAFE_PRINTF(...)  \
    do { xSemaphoreTake(s_safe_printf_mux, portMAX_DELAY); \
        ets_printf(__VA_ARGS__); \
        xSemaphoreGive(s_safe_printf_mux); \
    } while (false)
