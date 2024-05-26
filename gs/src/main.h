#pragma once

#include <iostream>
#include <string>
#include <deque>
#include <mutex>
#include <algorithm>
#include <cstdio>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "Clock.h"

#include "Log.h"

#include "packets.h"


#define USE_MAVLINK

//When enabled, it will output a 4Hz pulse (50ms ON, 200ms OFF) on GPIO 17. This can be used to blink a LED pointing inside the camera.
//This is used with a photodiode on the screen to measure with an oscilloscope the delay between the GPIO 17 pulse and the pixels on screen
#if defined(RASPBERRY_PI)
	//#define TEST_LATENCY
#else
	//#define TEST_LATENCY
#endif

//When enabled (together with TEST_LATENCY), it will output a 4Hz pulse (50ms ON, 200ms OFF) on GPIO 17. 
//On top of this, together with the on/off pulse it will send a white/black frame to the decoding thread.
//This is used with a photodiode on the screen to measure with an oscilloscope the delay between the GPIO 17 pulse and the pixels on screen
//This measurement excludes the air unit and is used to measure the latency of the ground station alone.
//#define TEST_DISPLAY_LATENCY

#define CHECK_GL_ERRORS

#if defined(CHECK_GL_ERRORS)
#define GLCHK(X) \
do { \
    GLenum err = GL_NO_ERROR; \
    X; \
   while ((err = glGetError())) \
   { \
      LOGE("GL error {} in " #X " file {} line {}", err, __FILE__,__LINE__); \
   } \
} while(0)
#define SDLCHK(X) \
do { \
    int err = X; \
    if (err != 0) LOGE("SDL error {} in " #X " file {} line {}", err, __FILE__,__LINE__); \
} while (0)
#else
#define GLCHK(X) X
#define SDLCHK(X) X
#endif

//===================================================================================
//===================================================================================
enum class ScreenAspectRatio : int
{
    STRETCH = 0,
    ASPECT4X3 = 1,
    ASPECT16X9 = 2
};

//===================================================================================
//===================================================================================
struct TGroundstationConfig
{
    int socket_fd;
    bool record;
    FILE * record_file=nullptr;
    std::mutex record_mutex;
    int wifi_channel;
    ScreenAspectRatio screenAspectRatio;
    bool stats;
};

extern TGroundstationConfig s_groundstation_config;

//===================================================================================
//===================================================================================
struct GSStats
{
    uint16_t outPacketCounter = 0;
    uint16_t inPacketCounter = 0;
    uint16_t inRejectedPacketCounter = 0;

    uint8_t rssiDbm = 0;
    uint8_t noiseFloorDbm = 0;
    uint8_t antena1PacketsCounter = 0;
    uint8_t antena2PacketsCounter = 0;

    uint8_t brokenFrames = 0;  //JPEG decoding errors

    int pingMinMS = 0;
    int pingMaxMS = 0;
};

extern GSStats s_gs_stats;
extern GSStats s_last_gs_stats;


extern void calculateLetterBoxAndBorder( int width, int height, int& x, int& y, int& w, int& h);
extern void saveGroundStationConfig();
extern void saveGround2AirConfig(const Ground2Air_Config_Packet& config);
extern void exitApp();

extern bool s_isOV5640;
extern uint16_t s_SDTotalSpaceGB16;
extern uint16_t s_SDFreeSpaceGB16;
extern bool s_air_record;
extern bool s_SDDetected;
extern bool s_SDSlow;
extern bool s_SDError;
extern bool bRestartRequired;
extern bool bRestart;
extern Clock::time_point restart_tp;


extern const char* resolutionName[];