#pragma once

#include <stdint.h>

#define HX_MAVLINK_MAX_PAYLOAD_LEN 255 ///< Maximum payload length
#define HX_MAVLINK_CORE_HEADER_LEN 9 ///< Length of core header (of the comm. layer)
#define HX_MAVLINK_CORE_HEADER_MAVLINK1_LEN 5 ///< Length of MAVLink1 core header (of the comm. layer)
#define HX_MAVLINK_NUM_HEADER_BYTES (HX_MAVLINK_CORE_HEADER_LEN + 1) ///< Length of all header bytes, including core and stx
#define HX_MAVLINK_NUM_CHECKSUM_BYTES 2
#define HX_MAVLINK_NUM_NON_PAYLOAD_BYTES (HX_MAVLINK_NUM_HEADER_BYTES + HX_MAVLINK_NUM_CHECKSUM_BYTES)

#define HX_MAVLINK_SIGNATURE_BLOCK_LEN 13

#define HX_MAVLINK_MAX_PACKET_LEN (HX_MAVLINK_MAX_PAYLOAD_LEN + HX_MAVLINK_NUM_NON_PAYLOAD_BYTES + HX_MAVLINK_SIGNATURE_BLOCK_LEN) ///< Maximum packet length

//=====================================================================
//=====================================================================
struct HXMAVLinkMsgHeader
{
    // MAVLink v2 framing byte 0xFD
    uint8_t stx; 

    // Payload length (fixed at 38 bytes for RC_CHANNELS_OVERRIDE with 18 channels)
    uint8_t payload_length;

    // Incompatibility flags (typically 0x00)
    uint8_t incompatibility_flags;

    // Compatibility flags (typically 0x00)
    uint8_t compatibility_flags;

    // Sequence number (increments with each message sent)
    uint8_t sequence_number;

    // System ID (identifies the sending system, e.g., GCS or UAV)
    uint8_t system_id;

    // Component ID (identifies the sending component, e.g., autopilot)
    uint8_t component_id;

    // Message ID for RC_CHANNELS_OVERRIDE (70 in little-endian format)
    uint8_t message_id[3] = {0x46, 0x00, 0x00};
};

#define HX_MAXLINK_RC_CHANNELS_OVERRIDE 70
//=====================================================================
//=====================================================================
struct HXMAVLinkRCChannelsOverride 
{
    // MAVLink v2 framing byte 0xFD
    uint8_t stx; 

    // Payload length (fixed at 38 bytes for RC_CHANNELS_OVERRIDE with 18 channels)
    uint8_t payload_length;

    // Incompatibility flags (typically 0x00)
    uint8_t incompatibility_flags;

    // Compatibility flags (typically 0x00)
    uint8_t compatibility_flags;

    // Sequence number (increments with each message sent)
    uint8_t sequence_number;

    // System ID (identifies the sending system, e.g., GCS or UAV)
    uint8_t system_id;

    // Component ID (identifies the sending component, e.g., autopilot)
    uint8_t component_id;

    // Message ID for RC_CHANNELS_OVERRIDE (70 in little-endian format)
    uint8_t message_id[3] = {0x46, 0x00, 0x00};

    // Payload: Override values for 18 RC channels, and target system/component
    uint16_t chan1_raw;  // Channel 1 value (0 to 65535)
    uint16_t chan2_raw;  // Channel 2 value (0 to 65535)
    uint16_t chan3_raw;  // Channel 3 value (0 to 65535)
    uint16_t chan4_raw;  // Channel 4 value (0 to 65535)
    uint16_t chan5_raw;  // Channel 5 value (0 to 65535)
    uint16_t chan6_raw;  // Channel 6 value (0 to 65535)
    uint16_t chan7_raw;  // Channel 7 value (0 to 65535)
    uint16_t chan8_raw;  // Channel 8 value (0 to 65535)
    uint16_t chan9_raw;  // Channel 9 value (0 to 65535)
    uint16_t chan10_raw; // Channel 10 value (0 to 65535)
    uint16_t chan11_raw; // Channel 11 value (0 to 65535)
    uint16_t chan12_raw; // Channel 12 value (0 to 65535)
    uint16_t chan13_raw; // Channel 13 value (0 to 65535)
    uint16_t chan14_raw; // Channel 14 value (0 to 65535)
    uint16_t chan15_raw; // Channel 15 value (0 to 65535)
    uint16_t chan16_raw; // Channel 16 value (0 to 65535)
    uint16_t chan17_raw; // Channel 17 value (0 to 65535)
    uint16_t chan18_raw; // Channel 18 value (0 to 65535)
    uint8_t target_system;    // Target system ID
    uint8_t target_component; // Target component ID

    // Checksum (calculated over the message starting from payload_length)
    uint16_t checksum;
};

//=====================================================================
//=====================================================================
class HXMavlinkParser
{
private:

    typedef enum
    {
        MPS_IDLE,
        MPS_LENGTH,
        MPS_BODY
    } TMavParserState;
  
    bool v2;

    uint8_t state;
    int packetLength;
    int expectedLength;

    uint8_t sbuf[HX_MAVLINK_MAX_PACKET_LEN]; 
    int sbufIndex;

    bool bGotPacket;

    void crc_init(uint16_t* crcAccum);
    uint16_t crc_calculate(const uint8_t* pBuffer, uint16_t length);
    void crc_accumulate(uint8_t data, uint16_t *crcAccum);
public:
    HXMavlinkParser(bool mavlinkV2);

    void processByte(uint8_t byte);

    bool gotPacket();
    const uint8_t* getPacketBuffer();
    int getPacketLength();

    int getMessageId();

    template<typename t>
    const t* getMsg()
    {
        return (t*)this->sbuf;
    }
};



