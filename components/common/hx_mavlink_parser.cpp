#include "hx_mavlink_parser.h"

#include <cstddef>

//define timeout value so parser does not stuck if no data is sent in stream
#define MAX_PACKET_TIME_MS 300

#define MAVLINK_FRAME_START_V1  0xFE
#define MAVLINK_FRAME_START_V2  0xFD

static uint8_t HX_MAVLINK_MESSAGE_CRCS[] = {50, 124, 137, 0, 237, 217, 104, 119, 0, 0, 0, 89, 0, 0, 0, 0, 0, 0, 0, 0, 214, 159, 220, 168, 24, 23, 170, 144, 67, 115, 39, 246, 185, 104, 237, 244, 222, 212, 9, 254, 230, 28, 28, 132, 221, 232, 11, 153, 41, 39, 78, 196, 0, 0, 15, 3, 0, 0, 0, 0, 0, 167, 183, 119, 191, 118, 148, 21, 0, 243, 124, 0, 0, 38, 20, 158, 152, 143, 0, 0, 0, 106, 49, 22, 143, 140, 5, 150, 0, 231, 183, 63, 54, 47, 0, 0, 0, 0, 0, 0, 175, 102, 158, 208, 56, 93, 138, 108, 32, 185, 84, 34, 174, 124, 237, 4, 76, 128, 56, 116, 134, 237, 203, 250, 87, 203, 220, 25, 226, 46, 29, 223, 85, 6, 229, 203, 1, 195, 109, 168, 181, 47, 72, 131, 127, 0, 103, 154, 178, 200, 134, 219, 208, 188, 84, 22, 19, 21, 134, 0, 78, 68, 189, 127, 154, 21, 21, 144, 1, 234, 73, 181, 22, 83, 167, 138, 234, 240, 47, 189, 52, 174, 229, 85, 159, 186, 72, 0, 0, 0, 0, 92, 36, 71, 98, 120, 0, 0, 0, 0, 134, 205, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 69, 101, 50, 202, 17, 162, 0, 0, 0, 0, 0, 0, 207, 0, 0, 0, 163, 105, 151, 35, 150, 0, 0, 0, 0, 0, 0, 90, 104, 85, 95, 130, 184, 81, 8, 204, 49, 170, 44, 83, 46, 0};

//=====================================================================
//=====================================================================
HXMavlinkParser::HXMavlinkParser(bool mavlinkV2)
{
    this->v2 = mavlinkV2;
    this->state = MPS_IDLE;
    this->bGotPacket = false;
}

//=====================================================================
//=====================================================================
void HXMavlinkParser::crc_init(uint16_t* crcAccum)
{
    *crcAccum = 0xffff;
}

//=====================================================================
//=====================================================================
uint16_t HXMavlinkParser::crc_calculate(const uint8_t* pBuffer, uint16_t length)
{
    uint16_t crcTmp;
    this->crc_init(&crcTmp);
	while (length--) 
    {
        this->crc_accumulate(*pBuffer++, &crcTmp);
    }
    return crcTmp;
}

//=====================================================================
//=====================================================================
void HXMavlinkParser::crc_accumulate(uint8_t data, uint16_t *crcAccum)
{
        uint8_t tmp;

        tmp = data ^ (uint8_t)(*crcAccum &0xff);
        tmp ^= (tmp<<4);
        *crcAccum = (*crcAccum>>8) ^ (tmp<<8) ^ (tmp <<3) ^ (tmp>>4);
}

//=====================================================================
//=====================================================================
void HXMavlinkParser::processByte(uint8_t byte)
{
    switch ( this->state )
    {
        case MPS_IDLE:
            if ( byte == ( this->v2 ? MAVLINK_FRAME_START_V2 : MAVLINK_FRAME_START_V1) ) 
            {
                this->sbuf[0] = byte;

                this->state = MPS_LENGTH;
                this->bGotPacket = false;
            }
            break;

        case MPS_LENGTH:
            this->sbuf[1] = byte;

            this->state = MPS_BODY;
            this->sbufIndex = 2;
            this->expectedLength = this->v2 ? (int)byte + 8 + 2: (int)byte + 4 + 2;

            break;

       case MPS_BODY:
            this->sbuf[this->sbufIndex++] = byte;
            this->expectedLength--;

            if ( this->expectedLength == 0 )
            {
                int crc1 = (this->sbuf[this->sbufIndex-1] << 8) | this->sbuf[this->sbufIndex-2];

                int msgId;
                int header_len;
                if ( this->v2 )
                {
                    msgId = this->sbuf[7] | (this->sbuf[8] << 8) | (this->sbuf[9] << 16);
                    header_len = 9;
                }
                else
                {
                    msgId = this->sbuf[5];
                    header_len = 5;
                }

                int len = this->sbuf[1];

	            uint16_t checksum = crc_calculate(&this->sbuf[1], header_len + len);
                uint8_t crc_extra = 0;
                if ( msgId < 255)
                {
                    crc_extra = HX_MAVLINK_MESSAGE_CRCS[msgId];
                }
	            crc_accumulate(crc_extra, &checksum);

                if ( crc1 == checksum )
                {
                    this->bGotPacket = true;
                    //Serial.print("Got Packet:");
                    //Serial.print(msgId);
                    //Serial.println("");
                }
                else
                {
                    //Serial.println("invalid crc");
                }
                
                this->state = MPS_IDLE;
            }

            break;
    }
}

//=====================================================================
//=====================================================================
bool HXMavlinkParser::gotPacket()
{
    return this->bGotPacket;
}

//=====================================================================
//=====================================================================

const uint8_t* HXMavlinkParser::getPacketBuffer()
{
    return this->bGotPacket ? &(this->sbuf[0]) : NULL;
}

//=====================================================================
//=====================================================================
int HXMavlinkParser::getPacketLength()
{
    return this->bGotPacket ? this->sbufIndex : 0;
}

//=====================================================================
//=====================================================================
int HXMavlinkParser::getMessageId()
{
    const HXMAVLinkMsgHeader* h = this->getMsg<HXMAVLinkMsgHeader>();
    return h->message_id[0] + ((int)(h->message_id[1]) << 8) + ((int)(h->message_id[2]) << 16);
}

