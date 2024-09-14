#pragma once 

#include <stdint.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#else
#define IRAM_ATTR
#endif

#include "main.h"

#ifdef UART_MSP_OSD

#define MSP_API_VERSION   1

#define MSP_FC_VERSION    3
#define MSP_FC_VARIANT    2
#define MSP_DISPLAYPORT   182

#define MSP_SET_RAW_RC    200

#define MSP_RC_CHANNELS_COUNT  18


#define MAX_MSP_MESSAGE 1024

typedef enum
{
  DPDS_SUBCMD,
  DPDS_POSITION_Y,
  DPDS_POSITION_X,
  DPDS_STRING_INFO,
  DPDS_STRING
} DPDState;

#define DPSC_CLEAR_SCREEN 2
#define DPSC_WRITE_STRING 3
#define DPSC_DRAW_SCREEN  4
#define DPSC_SET_OPTIONS  5

//======================================================
//======================================================
class MSP
{
public:
	MSP();
  ~MSP();

  void loop();

  bool sendCommand(uint16_t messageID, void * payload, uint16_t size);

  int64_t lastPing;
  int64_t lastLoop;
  int64_t lastRC;
  int64_t lastRealRC;

  bool gotRCChannels;
  uint16_t rcChannels[MSP_RC_CHANNELS_COUNT];
  void setRCChannels(const uint16_t* data); //MSP_RC_CHANNELS_COUNT values

private:

  typedef enum
  {
    DS_IDLE,
    DS_PROTO_IDENTIFIER,
    DS_DIRECTION_V1,
    DS_DIRECTION_V2,
    DS_FLAG_V2,
    DS_PAYLOAD_LENGTH_V1,
    DS_PAYLOAD_LENGTH_JUMBO_LOW,
    DS_PAYLOAD_LENGTH_JUMBO_HIGH,
    DS_PAYLOAD_LENGTH_V2_LOW,
    DS_PAYLOAD_LENGTH_V2_HIGH,
    DS_CODE_V1,
    DS_CODE_JUMBO_V1,
    DS_CODE_V2_LOW,
    DS_CODE_V2_HIGH,
    DS_PAYLOAD_V1,
    DS_PAYLOAD_V2,
    DS_CHECKSUM_V1,
    DS_CHECKSUM_V2,
  } TDecoderState;

  TDecoderState decoderState = DS_IDLE;

  uint8_t crc8_dvb_s2(uint8_t crc, unsigned char a);

  int portId;
  unsigned long probeTime;

  int unsupported;
  int message_direction;
  int message_length_expected;
  unsigned char message_buffer[MAX_MSP_MESSAGE];
  int message_length_received;
  int code;
  uint8_t message_checksum;

  void decode();
  void dispatchMessage(uint8_t expected_checksum);
  void processMessage();
  void sendPing();
};

extern MSP g_msp;

#endif // UART_MSP_OSD