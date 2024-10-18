#pragma once

#include "esp_wifi_types.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

esp_err_t nvs_args_init();

uint32_t nvs_args_read(const char *key);
esp_err_t nvs_args_set(const char *key,uint32_t value);
