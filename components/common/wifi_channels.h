#pragma once

#include "stdint.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif

#define DEFAULT_WIFI_CHANNEL_2_4GHZ 7
#define DEFAULT_WIFI_CHANNEL_5_8_GHZ 44

#define GS_WIFI_BAND_2_4_GHZ 0
#define GS_WIFI_BAND_5_8_GHZ 1
#define GS_WIFI_BAND_DUAL 2
#define DEFAULT_GS_WIFI_BAND GS_WIFI_BAND_2_4_GHZ

#define MIN_WIFI_CHANNEL 1
#define MAX_WIFI_2D4GHZ_CHANNEL 13
#define MAX_WIFI_CHANNEL 165

//0...WIFI_CHANNELS_COUNT-1 =>wifi channel
extern const uint8_t WIFI_CHANNELS_BY_INDEX[];
#define WIFI_CHANNELS_COUNT 38
#define MAX_WIFI_2D4GHZ_CHANNEL_INDEX 12  //channel 13
#define DEFAULT_WIFI_CHANNEL_INDEX 6 //channel 7

//returns index of channel in WIFI_CHANNELS_BY_INDEX
int getWifiChannelIndex(int channels);


//return valid channel number (validate channel)
//checks soc 2.4GHZ and 5GHz support
//does not check legal channels range for current country
int getValidWifiChannel(int channel);

// checks channel compatibility with gs wifiBand values: 0=2.4GHz, 1=5.8GHz, 2=dual
bool isWifiChannelAllowedByBand(int channel, uint8_t wifiBand);

// returns first valid index in WIFI_CHANNELS_BY_INDEX[] for selected band
int getFirstWifiChannelIndexForBand(uint8_t wifiBand);

// returns a valid channel for selected band; falls back to first channel in that band
int getBandAwareWifiChannel(int channel, uint8_t wifiBand);

// returns index within a band-filtered channel menu list
int getBandAwareWifiChannelMenuIndex(int channel, uint8_t wifiBand);
