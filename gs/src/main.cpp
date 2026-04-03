#include "main.h"

#include <sys/statvfs.h>
#include <cerrno>
#include <cstdlib>
#include <fstream>

#include "Comms.h"
#include "core/transport.h"
#include "Clock.h"
#include "IHAL.h"
#include "PI_HAL.h"
#include "imgui.h"
#include "desktop_osd.h"
#include "gs_desktop_bootstrap.h"
#include "Video_Decoder.h" 
#include "gs_runtime_state.h"
#include "desktop_osd.h"

/*

Changes on the PI:

- Disable the compositor from raspi-config. This will increase FPS
- Change from fake to real driver: dtoverlay=vc4-fkms-v3d to dtoverlay=vc4-kms-v3d

*/

std::unique_ptr<IHAL> s_hal;
Video_Decoder s_decoder;

#ifdef USE_MAVLINK
int fdUART = -1;
std::string serialPortName = "";
#endif

/* This prints an "Assertion failed" message and aborts.  */
void __assert_fail(const char* __assertion, const char* __file, unsigned int __line, const char* __function)
{
    printf("assert: %s:%d: %s: %s", __file, __line, __function, __assertion);
    fflush(stdout);
    //    abort();
}

bool bRestart = false;
bool bRestartRequired = false;
Clock::time_point restart_tp;

uint64_t s_GSSDTotalSpaceBytes = 0;
uint64_t s_GSSDFreeSpaceBytes = 0;

bool s_debugWindowVisisble = false;

Clock::time_point s_change_channel = Clock::now() + std::chrono::hours(10000);

uint8_t s_avi_fps;
uint16_t s_avi_frameWidth;
uint16_t s_avi_frameHeight;
uint32_t s_avi_frameCnt;
bool s_avi_ov2640HighFPS;
bool s_avi_ov5640HighFPS;

int main(int argc, const char* argv[])
{
    return runDesktopBootstrap(argc, argv);
}

