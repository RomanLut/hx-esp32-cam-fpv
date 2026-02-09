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
bool isWifiChannelAllowedByBand(int channel, uint8_t wifiBand)
{
    if (wifiBand == 0)
    {
        return channel >= MIN_WIFI_CHANNEL && channel <= MAX_WIFI_2D4GHZ_CHANNEL;
    }
    if (wifiBand == 1)
    {
        return channel > MAX_WIFI_2D4GHZ_CHANNEL;
    }
    return true;
}

//==========================================================================
//==========================================================================
int getFirstWifiChannelIndexForBand(uint8_t wifiBand)
{
    for (int i = 0; i < WIFI_CHANNELS_COUNT; i++)
    {
        if (isWifiChannelAllowedByBand(WIFI_CHANNELS_BY_INDEX[i], wifiBand))
        {
            return i;
        }
    }
    return DEFAULT_WIFI_CHANNEL_INDEX;
}

//==========================================================================
//==========================================================================
int getBandAwareWifiChannel(int channel, uint8_t wifiBand)
{
    channel = getValidWifiChannel(channel);
    if (isWifiChannelAllowedByBand(channel, wifiBand))
    {
        return channel;
    }

    int index = getFirstWifiChannelIndexForBand(wifiBand);
    return WIFI_CHANNELS_BY_INDEX[index];
}

//==========================================================================
//==========================================================================
int getBandAwareWifiChannelMenuIndex(int channel, uint8_t wifiBand)
{
    int filteredIndex = 0;
    for (int i = 0; i < WIFI_CHANNELS_COUNT; i++)
    {
        int current = WIFI_CHANNELS_BY_INDEX[i];
        if (!isWifiChannelAllowedByBand(current, wifiBand))
        {
            continue;
        }
        if (current == channel)
        {
            return filteredIndex;
        }
        filteredIndex++;
    }
    return 0;
}


//==========================================================================
//==========================================================================
IRAM_ATTR int getValidWifiChannel(int channel)
{
    if ((channel < MIN_WIFI_CHANNEL) || (channel > MAX_WIFI_CHANNEL)) 
    {
        channel = DEFAULT_WIFI_CHANNEL_2_4GHZ;
    }

#if CONFIG_IDF_TARGET_ESP32C5
    //all channels are valid on esp32c5
#elif !defined(ESP_PLATFORM)
    //all channels are valid on GS
#else
    //2.4GHZ only on esp32, esp32s3
    if ( channel > MAX_WIFI_2D4GHZ_CHANNEL )
    {
        channel = DEFAULT_WIFI_CHANNEL_2_4GHZ;
    }
#endif

    //if channel does not exists in WIFI_CHANNELS_BY_INDEX, assing DEFAULT_WIFI_CHANNEL_2_4GHZ
    bool valid = false;
    for(int i = 0; i < WIFI_CHANNELS_COUNT; i++) {
        if(WIFI_CHANNELS_BY_INDEX[i] == channel) {
            valid = true;
            break;
        }
    }
    if(!valid) {
        channel = DEFAULT_WIFI_CHANNEL_2_4GHZ;
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
