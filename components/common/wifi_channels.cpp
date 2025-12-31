#include "wifi_channels.h"
#ifdef ESP_PLATFORM
#include "esp_system.h"
#else
#define IRAM_ATTR
#endif


const uint8_t WIFI_CHANNELS_BY_INDEX[] = 
{
    1,2,3,4,5,6,7,8,9,10,11,12,13,

    36, 40, 44, 48,
    52, 56, 60, 64,   
    100, 104, 108, 112,
    116, 120, 124, 128,
    132, 136, 140, 144,
    149, 153, 157, 161, 165
};

const int WIFI_5GHZ_20MHZ_CHANNELS_ALL[] = {
    36, 40, 44, 48,
    52, 56, 60, 64,  
    100, 104, 108, 112,
    116, 120, 124, 128, 
    132, 136, 140, 144,
    149, 153, 157, 161, 165
};


const int WIFI_5GHZ_20MHZ_NONDFS_US[] = {
    36, 40, 44, 48,
    149, 153, 157, 161, 165
};

const int WIFI_5GHZ_20MHZ_NONDFS_EU[] = {
    36, 40, 44, 48
};


//==========================================================================
//==========================================================================
IRAM_ATTR int getValidWifiChannel(int channel)
{
    if ((channel < MIN_WIFI_CHANNEL) || (channel > MAX_WIFI_CHANNEL)) 
    {
        channel = DEFAULT_WIFI_CHANNEL;
    }

#if CONFIG_IDF_TARGET_ESP32C5
    //all channels are valid on esp32c5
#elif !defined(ESP_PLATFORM)
    //all channels are valid on GS
#else
    //2.4GHZ only on esp32, esp32s3
    if ( channel > MAX_WIFI_2D4GHZ_CHANNEL )
    {
        channel = DEFAULT_WIFI_CHANNEL;
    }
#endif

    //if channel does not exists in WIFI_CHANNELS_BY_INDEX, assing DEFAULT_WIFI_CHANNEL
    bool valid = false;
    for(int i = 0; i < WIFI_CHANNELS_COUNT; i++) {
        if(WIFI_CHANNELS_BY_INDEX[i] == channel) {
            valid = true;
            break;
        }
    }
    if(!valid) {
        channel = DEFAULT_WIFI_CHANNEL;
    }

    return channel;
}

//returns index of channel in WIFI_CHANNELS_BY_INDEX
int getWifiChannelIndex(int channel)
{
    for(int i = 0; i < WIFI_CHANNELS_COUNT; i++) 
    {
        if(WIFI_CHANNELS_BY_INDEX[i] == channel) 
        {
            return i;
        }
    }
    return DEFAULT_WIFI_CHANNEL_INDEX;
}
