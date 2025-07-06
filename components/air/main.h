#pragma once

#include <cassert>
#include <cstring>

#define UART_RX_BUFFER_SIZE_MSP_OSD      512
#define UART_TX_BUFFER_SIZE_MSP_OSD      256

#define UART_RX_BUFFER_SIZE_MAVLINK      512
#define UART_TX_BUFFER_SIZE_MAVLINK      256

//===============================================================
//For esp32cam

//for development - enabled debug output on normal UART pins (1,3)
//#define USBUART_DEBUG_OUTPUT

#ifdef BOARD_ESP32CAM

//   Debug log on pin 33 (existing LED)
//   UART0:  Mavlink RX=3 TX=1 
//   UART2:  MSP-OSD RX=13 TX=12
//   STATUS LED: 4 (existing FLASH LED)

#define UART_MAVLINK UART_NUM_1
#define UART1_RX_BUFFER_SIZE UART_RX_BUFFER_SIZE_MAVLINK
#define UART1_TX_BUFFER_SIZE UART_TX_BUFFER_SIZE_MAVLINK

#define UART_MSP_OSD UART_NUM_2
#define UART2_RX_BUFFER_SIZE UART_RX_BUFFER_SIZE_MSP_OSD
#define UART2_TX_BUFFER_SIZE UART_TX_BUFFER_SIZE_MSP_OSD

#define CAMERA_MODEL_AI_THINKER
#define DVR_SUPPORT

#define INIT_UART_0
#ifdef USBUART_DEBUG_OUTPUT
#define TXD0_PIN    1
#define RXD0_PIN    3
#else
//#define TXD0_PIN    33
//#define RXD0_PIN    33
#define TXD0_PIN    UART_PIN_NO_CHANGE
#define RXD0_PIN    UART_PIN_NO_CHANGE
#endif

#define UART0_BAUDRATE 115200

#define INIT_UART_1
#ifdef USBUART_DEBUG_OUTPUT
#define TXD1_PIN    UART_PIN_NO_CHANGE
#define RXD1_PIN    UART_PIN_NO_CHANGE
#else
#define TXD1_PIN    1
#define RXD1_PIN    3
#endif

#define UART1_BAUDRATE 115200

#define INIT_UART_2
#define TXD2_PIN    12   //should be low at boot!!!
#define RXD2_PIN    13 
#define UART2_BAUDRATE 115200

#define STATUS_LED_PIN GPIO_NUM_33
#define STATUS_LED_ON 0
#define STATUS_LED_OFF 1

#define ESP32CAM_FLASH_LED_PIN GPIO_NUM_4  //shared pin for LED and REC button

#endif

//===============================================================
//===============================================================
//for XIAO ESP32S3 Sense
//https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_SCH_v1.1.pdf
//https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/XIAO_ESP32S3_ExpBoard_v1.0_SCH.pdf

#ifdef BOARD_XIAOS3SENSE

//  Debug is on USB UART
//  UART1:  MSP-OSD  RX=D3 TX=D1
//  UART2:  MAVLINK  RX=D6 TX=D&
//  REC BUTTON: GPIO0  (existing flash button)
//  STATUS LED: GPIO1

//define to use DisplayPort OSD on UART1
#define UART_MSP_OSD UART_NUM_1
#define UART1_RX_BUFFER_SIZE UART_RX_BUFFER_SIZE_MSP_OSD
#define UART1_TX_BUFFER_SIZE UART_TX_BUFFER_SIZE_MSP_OSD

//define to use mavlink telemetry on UART2 
#define UART_MAVLINK UART_NUM_2
#define UART2_RX_BUFFER_SIZE UART_RX_BUFFER_SIZE_MAVLINK
#define UART2_TX_BUFFER_SIZE UART_TX_BUFFER_SIZE_MAVLINK

#define CAMERA_MODEL_XIAO_ESP32S3
#define DVR_SUPPORT
#define STATUS_LED_PIN GPIO_NUM_1
#define STATUS_LED_ON 1
#define STATUS_LED_OFF 0
#define REC_BUTTON_PIN  GPIO_NUM_0 //dedicated REC button pin

#define INIT_UART_1
#define TXD1_PIN    GPIO_NUM_2 //D1
#define RXD1_PIN    GPIO_NUM_4 //D3
#define UART1_BAUDRATE 115200

#define INIT_UART_2
#define TXD2_PIN    GPIO_NUM_43 //D6
#define RXD2_PIN    GPIO_NUM_44 //D7
#define UART2_BAUDRATE 115200
//----------------------

#endif
//===============================================================

//write raw MJPEG stream instead of avi
//#define WRITE_RAW_MJPEG_STREAM

//#define TEST_AVI_FRAMES

//===============================================================

#define MAX_SD_WRITE_SPEED_ESP32   (800*1024) //esp32 can hadle 1.9mb writes, but in this project it's 0.8mb max due to overal system load (otherwise we miss camera data callback)
#define MAX_SD_WRITE_SPEED_ESP32S3 (1800*1024) //can  write 1900 but we set to 1800 due to overal system load

#if defined(CAMERA_MODEL_WROVER_KIT)
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    21
#define SIOD_GPIO_NUM    26
#define SIOC_GPIO_NUM    27

#define Y9_GPIO_NUM      35
#define Y8_GPIO_NUM      34
#define Y7_GPIO_NUM      39
#define Y6_GPIO_NUM      36
#define Y5_GPIO_NUM      19
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM       5
#define Y2_GPIO_NUM       4
#define VSYNC_GPIO_NUM   25
#define HREF_GPIO_NUM    23
#define PCLK_GPIO_NUM    22

#elif defined(CAMERA_MODEL_ESP_EYE)
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    4
#define SIOD_GPIO_NUM    18
#define SIOC_GPIO_NUM    23

#define Y9_GPIO_NUM      36
#define Y8_GPIO_NUM      37
#define Y7_GPIO_NUM      38
#define Y6_GPIO_NUM      39
#define Y5_GPIO_NUM      35
#define Y4_GPIO_NUM      14
#define Y3_GPIO_NUM      13
#define Y2_GPIO_NUM      34
#define VSYNC_GPIO_NUM   5
#define HREF_GPIO_NUM    27
#define PCLK_GPIO_NUM    25

#elif defined(CAMERA_MODEL_M5STACK_PSRAM)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    15
#define XCLK_GPIO_NUM     27
#define SIOD_GPIO_NUM     25
#define SIOC_GPIO_NUM     23

#define Y9_GPIO_NUM       19
#define Y8_GPIO_NUM       36
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       39
#define Y5_GPIO_NUM        5
#define Y4_GPIO_NUM       34
#define Y3_GPIO_NUM       35
#define Y2_GPIO_NUM       32
#define VSYNC_GPIO_NUM    22
#define HREF_GPIO_NUM     26
#define PCLK_GPIO_NUM     21

#elif defined(CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#elif defined(CAMERA_MODEL_M5STACK)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    15
#define XCLK_GPIO_NUM     27
#define SIOD_GPIO_NUM     25
#define SIOC_GPIO_NUM     23

#define Y9_GPIO_NUM       19
#define Y8_GPIO_NUM       36
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       39
#define Y5_GPIO_NUM        5
#define Y4_GPIO_NUM       34
#define Y3_GPIO_NUM       35
#define Y2_GPIO_NUM       17
#define VSYNC_GPIO_NUM    22
#define HREF_GPIO_NUM     26
#define PCLK_GPIO_NUM     21
#elif defined(CAMERA_MODEL_ESP_VTX)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     23
#define SIOD_GPIO_NUM     27
#define SIOC_GPIO_NUM     26

#define Y9_GPIO_NUM       18
#define Y8_GPIO_NUM       19
#define Y7_GPIO_NUM       22
#define Y6_GPIO_NUM       35
#define Y5_GPIO_NUM       39
#define Y4_GPIO_NUM       36
#define Y3_GPIO_NUM       38
#define Y2_GPIO_NUM       34
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     5
#define PCLK_GPIO_NUM     21

#elif defined(CAMERA_MODEL_XIAO_ESP32S3)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

#define LED_GPIO_NUM      21  //also used as SDCard CS
#else
#error "Camera model not selected"
#endif

extern bool isHQDVRMode();

extern bool SDError;
extern uint16_t SDTotalSpaceGB16;
extern uint16_t SDFreeSpaceGB16;
extern bool s_sd_initialized;

void updateSDInfo();
