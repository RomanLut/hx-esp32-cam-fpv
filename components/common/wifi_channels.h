#pragma once

#include "stdint.h"
#include "esp_attr.h"

#define DEFAULT_WIFI_CHANNEL 7

#define MIN_WIFI_CHANNEL 1
#define MAX_WIFI_2D4GHZ_CHANNEL 13
#define MAX_WIFI_CHANNEL 165

//0...WIFI_CHANNELS_COUNT-1 =>wifi channel
extern const uint8_t WIFI_CHANNELS_BY_INDEX[];
#define WIFI_CHANNELS_COUNT 38
#define MAX_WIFI_2D4GHZ_CHANNEL_INDEX 12


//return valid channel number (validate)
//checks soc 2.4GHZ and 5GHz support
//does not check legal channels range for current country
int getValidWifiChannel(int channels);
