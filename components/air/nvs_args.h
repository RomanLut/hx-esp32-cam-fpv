#pragma once

#include "esp_wifi_types.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <stddef.h>

esp_err_t nvs_args_init();

uint32_t nvs_args_read(const char *key, uint32_t defaultValue);
esp_err_t nvs_args_set(const char *key,uint32_t value);
esp_err_t nvs_args_read_blob(const char* key, void* value, size_t size);
esp_err_t nvs_args_set_blob(const char* key, const void* value, size_t size);
