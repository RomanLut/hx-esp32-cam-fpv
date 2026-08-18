#include "hx_mavlink_rc_encoder.h"

#ifdef ARDUINO
#include <common/mavlink.h>

#if defined(ESP8266)
#include <esp8266_peri.h>
#elif defined(ESP32)
#endif
#endif

//===================================================================================
//===================================================================================
// Initializes the MAVLink encoder object.
HXMavlinkRCEncoder::HXMavlinkRCEncoder()
{
}

#ifdef ARDUINO
//===================================================================================
//===================================================================================
// Initializes all RC channels to their minimum value.
void HXMavlinkRCEncoder::initChannels()
{
    for ( int i = 0; i < MAVLINK_RC_CHANNELS_COUNT; i++ )
    {
        this->channels[i] = 1000;
    }
}

//===================================================================================
//===================================================================================
// Configures the RC packet interval and MAVLink protocol version.
void HXMavlinkRCEncoder::init( uint16_t packetRateMS, bool mavlink_v1 )
{
    this->mavlink_v1 = mavlink_v1;
    this->packetRateMS = packetRateMS;

    this->initChannels();
    
    this->lastPacketTime = millis();
    this->failsafe = true;
}

//===================================================================================
//===================================================================================
// Sends a due MAVLink RC override packet to an Arduino stream.
bool HXMavlinkRCEncoder::loop( Stream& serial )
{
    if (serial.availableForWrite() < 34 ) return false;

    unsigned long t = millis();
    if ( (t - this->lastPacketTime)  < this->packetRateMS ) return false;

    if ( this->failsafe ) return false;

    mavlink_message_t msg;
    mavlink_msg_rc_channels_override_pack( 
            1 , MAV_COMP_ID_USER1, 
            &msg,
            1, MAV_COMP_ID_AUTOPILOT1,
            this->channels[0], this->channels[1], this->channels[2], this->channels[3],
            this->channels[4], this->channels[5], this->channels[6], this->channels[7],
            this->channels[8], this->channels[9], this->channels[10], this->channels[11],
            this->channels[12], this->channels[13], this->channels[14], this->channels[15],
            0, 0
         );

    if ( this->mavlink_v1 )
    {
        //pack MAVLINK_STATUS_FLAG_OUT_MAVLINK1 flag and recalculate CRC
        mavlink_get_channel_status(MAVLINK_COMM_0)->flags = MAVLINK_STATUS_FLAG_OUT_MAVLINK1;

        mavlink_finalize_message_chan(
            &msg, 1, MAV_COMP_ID_USER1,
            MAVLINK_COMM_0, 
            MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE_MIN_LEN, MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE_LEN, MAVLINK_MSG_ID_RC_CHANNELS_OVERRIDE_CRC);
    }

    uint8_t sbuf[MAVLINK_MAX_PACKET_LEN];
    int len = mavlink_msg_to_send_buffer(sbuf, &msg); //len = 18(v1) or 34(v2)

    if (serial.availableForWrite() < len ) return false;

    serial.write( sbuf, len );

    this->lastPacketTime = t;

    return true;
}

//===================================================================================
//===================================================================================
// Updates whether RC packet transmission is suppressed by failsafe.
void HXMavlinkRCEncoder::setFailsafe( bool failsafe )
{
    this->failsafe = failsafe;
}

//===================================================================================
//===================================================================================
// Sets one RC channel value in the 1000..2000 input range.
void HXMavlinkRCEncoder::setChannelValue( uint8_t index, uint16_t value ) 
{
    if ( index < MAVLINK_RC_CHANNELS_COUNT )
    {
        this->channels[index] = value;
    }
}
#endif

//===================================================================================
//===================================================================================
// Adds one byte to a MAVLink X.25 checksum.
void HXMavlinkRCEncoder::mavlinkCrcAccumulate(uint8_t data, uint16_t& checksum)
{
    uint8_t tmp = data ^ static_cast<uint8_t>(checksum & 0xff);
    tmp ^= tmp << 4;
    checksum = (checksum >> 8) ^ (static_cast<uint16_t>(tmp) << 8) ^
        (static_cast<uint16_t>(tmp) << 3) ^ (tmp >> 4);
}

//===================================================================================
//===================================================================================
// Builds a complete MAVLink 2 RADIO_STATUS frame in a caller-provided buffer.
size_t HXMavlinkRCEncoder::buildRadioStatus(
    uint8_t* frame,
    size_t frameCapacity,
    uint8_t sequence,
    uint8_t rssi,
    uint8_t txBuffer,
    uint8_t remoteRssi,
    uint8_t noise,
    uint8_t remoteNoise,
    uint16_t rxErrors,
    uint16_t fixed,
    uint8_t systemId,
    uint8_t componentId)
{
    if (frame == nullptr || frameCapacity < MAVLINK_RADIO_STATUS_FRAME_SIZE)
    {
        return 0;
    }

    static constexpr uint8_t payloadLength = 9;
    static constexpr uint8_t messageId = 109;
    static constexpr uint8_t crcExtra = 185;

    frame[0] = 0xfd;
    frame[1] = payloadLength;
    frame[2] = 0;
    frame[3] = 0;
    frame[4] = sequence;
    frame[5] = systemId;
    frame[6] = componentId;
    frame[7] = messageId;
    frame[8] = 0;
    frame[9] = 0;
    frame[10] = static_cast<uint8_t>(rxErrors);
    frame[11] = static_cast<uint8_t>(rxErrors >> 8);
    frame[12] = static_cast<uint8_t>(fixed);
    frame[13] = static_cast<uint8_t>(fixed >> 8);
    frame[14] = rssi;
    frame[15] = remoteRssi;
    frame[16] = txBuffer;
    frame[17] = noise;
    frame[18] = remoteNoise;

    uint16_t checksum = UINT16_MAX;
    for (size_t i = 1; i < MAVLINK_RADIO_STATUS_FRAME_SIZE - 2; i++)
    {
        mavlinkCrcAccumulate(frame[i], checksum);
    }
    mavlinkCrcAccumulate(crcExtra, checksum);
    frame[19] = static_cast<uint8_t>(checksum);
    frame[20] = static_cast<uint8_t>(checksum >> 8);

    return MAVLINK_RADIO_STATUS_FRAME_SIZE;
}
