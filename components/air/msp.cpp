#include "msp.h"

#include "driver/uart.h"
#include "esp_timer.h"
#include "osd.h"

#ifdef UART_MSP_OSD

#define MSP_PING_TIMEOUT_US           125000

#define MSP_PROTOCOL_LOG_ERRORS
#define JUMBO_FRAME_MIN_SIZE  255

#define SYM_BEGIN       '$'
#define SYM_PROTO_V1    'M'
#define SYM_PROTO_V2    'X'
#define SYM_FROM_MWC    '>'
#define SYM_TO_MWC      '<'
#define SYM_UNSUPPORTED '!'

#include "safe_printf.h"
#define LOG(...) do { if (true) SAFE_PRINTF(__VA_ARGS__); } while (false) 

MSP g_msp;

//======================================================
//======================================================
MSP::MSP()
{
  this->lastPing = 0;
  this->lastLoop = 0;
  this->lastRC = 0;
  this->lastRealRC = 0;
  this->gotRCChannels = false;
}

//======================================================
//======================================================
MSP::~MSP()
{
}

//======================================================
//======================================================
void MSP::dispatchMessage(uint8_t expected_checksum)
{
  if (this->message_checksum == expected_checksum)
  {
    //LOG("code: %d - crc Ok, len=%d\n", this->code, this->message_length_expected);
    // message received, process
    this->processMessage();
  }
  else
  {
#ifdef MSP_PROTOCOL_LOG_ERRORS
    LOG("code: %d - crc failed, len=%d\n", this->code, this->message_length_expected);
#endif
    //this.packet_error++;
    //$('span.packet-error').html(this.packet_error);
  }

  this->decoderState = DS_IDLE;
}

//======================================================
//======================================================
void MSP::decode()
{
  static unsigned char data[UART_RX_BUFFER_SIZE_MSP_OSD];
  size_t rs = 0;
  ESP_ERROR_CHECK( uart_get_buffered_data_len(UART_MSP_OSD, &rs) );
  if ( rs == 0 ) return;

#ifdef MSP_PROTOCOL_LOG_ERRORS
  if ( rs == UART_RX_BUFFER_SIZE_MSP_OSD )
  {
    LOG("MSP Data len=%d\n", rs);  //likely overflow
  }
#endif

  if ( rs > UART_RX_BUFFER_SIZE_MSP_OSD) rs = UART_RX_BUFFER_SIZE_MSP_OSD;
  //Note: if uart_read_bytes() can not read specified number of bytes, it returns error.
  int len = uart_read_bytes(UART_MSP_OSD, data,rs, 0);

  if ( len <=0 ) LOG("ret len=%d\n", rs);  //likely overflow

  //ESP_ERROR_CHECK(len);  sometimes returns error
  if (len <= 0) return;

  for (int i = 0; i < len; i++)
  {
    switch (this->decoderState)
    {
    case DS_IDLE: // sync char 1
      if (data[i] == SYM_BEGIN)
      {
        this->decoderState = DS_PROTO_IDENTIFIER;
      }
#ifdef MSP_PROTOCOL_LOG_ERRORS
      else
      {
        LOG("Garbage data=%d\n", data[i]);
      }
#endif
      break;

    case DS_PROTO_IDENTIFIER: // sync char 2
      switch (data[i])
      {
      case SYM_PROTO_V1:
        this->decoderState = DS_DIRECTION_V1;
        break;
      case SYM_PROTO_V2:
        this->decoderState = DS_DIRECTION_V2;
        break;
      default:
        //unknown protocol
        this->decoderState = DS_IDLE;
      }
      break;

    case DS_DIRECTION_V1: // direction (should be >)

    case DS_DIRECTION_V2:
      this->unsupported = 0;
      switch (data[i])
      {
      case SYM_FROM_MWC:
        this->message_direction = 1;
        break;
      case SYM_TO_MWC:
        this->message_direction = 0;
        break;
      case SYM_UNSUPPORTED:
        this->unsupported = 1;
        break;
      }
      this->decoderState = this->decoderState == DS_DIRECTION_V1 ? DS_PAYLOAD_LENGTH_V1 : DS_FLAG_V2;
      break;

    case DS_FLAG_V2:
      // Ignored for now
      this->decoderState = DS_CODE_V2_LOW;
      break;
    case DS_PAYLOAD_LENGTH_V1:
      this->message_length_expected = data[i];

      if (this->message_length_expected == JUMBO_FRAME_MIN_SIZE)
      {
        this->decoderState = DS_CODE_JUMBO_V1;
      }
      else
      {
        this->message_length_received = 0;
        this->decoderState = DS_CODE_V1;
      }
      break;

    case DS_PAYLOAD_LENGTH_V2_LOW:
      this->message_length_expected = data[i];
      this->decoderState = DS_PAYLOAD_LENGTH_V2_HIGH;
      break;

    case DS_PAYLOAD_LENGTH_V2_HIGH:
      this->message_length_expected |= data[i] << 8;
      this->message_length_received = 0;
      if (this->message_length_expected <= MAX_MSP_MESSAGE)
      {
        this->decoderState = this->message_length_expected > 0 ? DS_PAYLOAD_V2 : DS_CHECKSUM_V2;
      }
      else
      {
        //too large payload
#ifdef MSP_PROTOCOL_LOG_ERRORS
        LOG("Too large payload, len=%d\n", this->message_length_expected); 
#endif
        this->decoderState = DS_IDLE;
      }
      break;

    case DS_CODE_V1:
    case DS_CODE_JUMBO_V1:
      this->code = data[i];
      if (this->message_length_expected > 0)
      {
        // process payload
        if (this->decoderState == DS_CODE_JUMBO_V1)
        {
          this->decoderState = DS_PAYLOAD_LENGTH_JUMBO_LOW;
        }
        else
        {
          this->decoderState = DS_PAYLOAD_V1;
        }
      }
      else
      {
        // no payload
        this->decoderState = DS_CHECKSUM_V1;
      }
      break;

    case DS_CODE_V2_LOW:
      this->code = data[i];
      this->decoderState = DS_CODE_V2_HIGH;
      break;

    case DS_CODE_V2_HIGH:
      this->code |= data[i] << 8;
      this->decoderState = DS_PAYLOAD_LENGTH_V2_LOW;
      break;

    case DS_PAYLOAD_LENGTH_JUMBO_LOW:
      this->message_length_expected = data[i];
      this->decoderState = DS_PAYLOAD_LENGTH_JUMBO_HIGH;
      break;

    case DS_PAYLOAD_LENGTH_JUMBO_HIGH:
      this->message_length_expected |= data[i] << 8;
      this->message_length_received = 0;
      this->decoderState = DS_PAYLOAD_V1;
      break;

    case DS_PAYLOAD_V1:
    case DS_PAYLOAD_V2:
      this->message_buffer[this->message_length_received] = data[i];
      this->message_length_received++;

      if (this->message_length_received >= this->message_length_expected)
      {
        this->decoderState = this->decoderState == DS_PAYLOAD_V1 ? DS_CHECKSUM_V1 : DS_CHECKSUM_V2;
      }
      break;

    case DS_CHECKSUM_V1:
      if (this->message_length_expected >= JUMBO_FRAME_MIN_SIZE)
      {
        this->message_checksum = JUMBO_FRAME_MIN_SIZE;
      }
      else
      {
        this->message_checksum = this->message_length_expected;
      }
      this->message_checksum ^= this->code;
      if (this->message_length_expected >= JUMBO_FRAME_MIN_SIZE)
      {
        this->message_checksum ^= this->message_length_expected & 0xFF;
        this->message_checksum ^= (this->message_length_expected & 0xFF00) >> 8;
      }
      for (int ii = 0; ii < this->message_length_received; ii++)
      {
        this->message_checksum ^= this->message_buffer[ii];
      }
      this->dispatchMessage(data[i]);
      break;

    case DS_CHECKSUM_V2:
      this->message_checksum = 0;
      this->message_checksum = this->crc8_dvb_s2(this->message_checksum, 0); // flag
      this->message_checksum = this->crc8_dvb_s2(this->message_checksum, this->code & 0xFF);
      this->message_checksum = this->crc8_dvb_s2(this->message_checksum, (this->code & 0xFF00) >> 8);
      this->message_checksum = this->crc8_dvb_s2(this->message_checksum, this->message_length_expected & 0xFF);
      this->message_checksum = this->crc8_dvb_s2(this->message_checksum, (this->message_length_expected & 0xFF00) >> 8);
      for (int ii = 0; ii < this->message_length_received; ii++)
      {
        this->message_checksum = this->crc8_dvb_s2(this->message_checksum, this->message_buffer[ii]);
      }
      this->dispatchMessage(data[i]);
      break;

    default:
      break;
    }
  }
}

//======================================================
//======================================================
uint8_t MSP::crc8_dvb_s2(uint8_t crc, unsigned char a)
{
  crc ^= a;
  for (int ii = 0; ii < 8; ++ii) {
    if (crc & 0x80) {
      crc = (crc << 1) ^ 0xD5;
    }
    else {
      crc = crc << 1;
    }
  }
  return crc;
}

//======================================================
//======================================================
bool MSP::sendCommand(uint16_t messageID, void * payload, uint16_t size)
{
  size_t freeSize = 0;
  ESP_ERROR_CHECK( uart_get_tx_buffer_free_size(UART_MSP_OSD, &freeSize) );

  if ( freeSize < (9 + size) ) return false;

  uint8_t flag = 0;
  int msg_size = 9;
  uint8_t crc = 0;
  uint8_t tmp_buf[2];

  msg_size += (int)size;

  uart_write_bytes( UART_MSP_OSD, (unsigned char*)"$", 1);
  uart_write_bytes( UART_MSP_OSD, (unsigned char*)"X", 1);
  uart_write_bytes( UART_MSP_OSD, (unsigned char*)"<", 1);

  crc = MSP::crc8_dvb_s2(crc, flag);
  uart_write_bytes( UART_MSP_OSD, &flag,1);

  memcpy(tmp_buf, &messageID, 2);
  crc = MSP::crc8_dvb_s2(crc, tmp_buf[0]);
  crc = MSP::crc8_dvb_s2(crc, tmp_buf[1]);
  uart_write_bytes( UART_MSP_OSD, tmp_buf, 2);

  memcpy(tmp_buf, &size, 2);
  crc = MSP::crc8_dvb_s2(crc, tmp_buf[0]);
  crc = MSP::crc8_dvb_s2(crc, tmp_buf[1]);
  uart_write_bytes( UART_MSP_OSD,tmp_buf, 2);

  uint8_t * payloadPtr = (uint8_t*)payload;
  for (uint8_t i = 0; i < size; ++i)
  {
    uint8_t b = *(payloadPtr++);
    crc = MSP::crc8_dvb_s2(crc, b);
    uart_write_bytes( UART_MSP_OSD,&b, 1);
  }

  uart_write_bytes( UART_MSP_OSD, &crc,1);

  return true;
}

//======================================================
//======================================================
void MSP::processMessage()
{
  if ( this->code == MSP_DISPLAYPORT )
  {
    DPDState decoderState = DPDS_SUBCMD;

    int len = this->message_length_received;
    uint8_t* ptr = this->message_buffer;
    int row = 0;
    int col = 0;
    bool isExtChar = false;

    while ( len > 0 )
    {
      switch (decoderState)
      {
        case DPDS_SUBCMD:
          switch (*ptr)
          {
            case DPSC_CLEAR_SCREEN:
              g_osd.clear();
            break;

            case DPSC_WRITE_STRING:
              decoderState = DPDS_POSITION_Y;
            break;
            
            case DPSC_DRAW_SCREEN:
              g_osd.commit();
            break;
            
            case DPSC_SET_OPTIONS:
            break;
          }
        break;

        case DPDS_POSITION_Y:
          row = *ptr;
          decoderState = DPDS_POSITION_X;
        break;

        case DPDS_POSITION_X:
          col = *ptr;
          decoderState = DPDS_STRING_INFO;
        break;

        case DPDS_STRING_INFO:
          isExtChar = *ptr != 0;
          decoderState = DPDS_STRING;
        break;

        case DPDS_STRING:
          g_osd.writeString(row, col, isExtChar, ptr, len);
          len = 0;
        break;
      }

      ptr++;
      len--;
    }

  }
}

//======================================================
//======================================================
void MSP::sendPing()
{
    this->sendCommand(MSP_FC_VARIANT, NULL, 0 );
}

//======================================================
//======================================================
void MSP::loop()
{
  int64_t now = esp_timer_get_time(); 
  
  int64_t delta = now - this->lastLoop;
#ifdef MSP_PROTOCOL_LOG_ERRORS
  if ( this->lastLoop && (delta > 30000) )
  {
    LOG("MSP:Loop() delta=%d", delta);
  }
#endif
  this->lastLoop = now;

  this->decode();
  
  delta = now - this->lastRC;

  if ( this->gotRCChannels || (this->lastRealRC && (delta > 90000) && ( delta < 300000)))
  {
    //int64_t delta2 = now - this->lastRealRC;
    //if ((delta2> 100000) && (delta2 < 500000)) LOG("RC Delta=%d\n", (int)delta2);

    if ( this->gotRCChannels )
    {
      this->gotRCChannels = false;
      this->lastRealRC = now;
    }

    this->sendCommand(MSP_SET_RAW_RC, this->rcChannels, MSP_RC_CHANNELS_COUNT * 2);
    this->lastPing = now + MSP_PING_TIMEOUT_US;
    this->lastRC = now;
    return;
  }

  if ( now > this->lastPing )
  {
    this->sendPing();
    this->lastPing = now + MSP_PING_TIMEOUT_US;
  }
}

//======================================================
//======================================================
void MSP::setRCChannels(const uint16_t* data)
{
  memcpy( this->rcChannels, data, MSP_RC_CHANNELS_COUNT*2);
  this->gotRCChannels = true;
}

#endif // UART_MSP_OSD