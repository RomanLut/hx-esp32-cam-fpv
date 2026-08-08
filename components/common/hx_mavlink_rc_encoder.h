#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ARDUINO
#include <Arduino.h>
#endif

#define MAVLINK_RC_CHANNELS_COUNT_V1        8 //Mavlink v1 can handle 8 channels only
#define MAVLINK_RC_CHANNELS_COUNT           16
#define MAVLINK_RADIO_STATUS_FRAME_SIZE     21

//===================================================================================
//===================================================================================
// Encodes MAVLink RC commands and platform-neutral RADIO_STATUS frames.
class HXMavlinkRCEncoder
{
private:
#ifdef ARDUINO
    bool mavlink_v1;
    bool failsafe;
    uint16_t packetRateMS;
    uint16_t channels[MAVLINK_RC_CHANNELS_COUNT];
    unsigned long lastPacketTime;

    void initChannels();
#endif

    static void mavlinkCrcAccumulate(uint8_t data, uint16_t& checksum);

public:
    HXMavlinkRCEncoder();

#ifdef ARDUINO
    //packetRateMS - write packet every ?ms
    void init( uint16_t packetRateMS, bool mavlink_v1 );

    void setFailsafe( bool failsafe );
    void setChannelValue( uint8_t index, uint16_t value );
    bool loop( Stream& serial );
#endif

    static size_t buildRadioStatus(
        uint8_t* frame,
        size_t frameCapacity,
        uint8_t sequence,
        uint8_t rssi,
        uint8_t txBuffer,
        uint8_t remoteRssi = UINT8_MAX,
        uint8_t noise = UINT8_MAX,
        uint8_t remoteNoise = UINT8_MAX,
        uint16_t rxErrors = 0,
        uint16_t fixed = 0,
        uint8_t systemId = 1,
        uint8_t componentId = 68);
};
