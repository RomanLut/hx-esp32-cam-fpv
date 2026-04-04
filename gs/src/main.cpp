#include "main.h"

#include <sys/statvfs.h>
#include <cerrno>
#include <cstdlib>
#include <fstream>

#include "Comms.h"
#include "core/transport.h"
#include "Clock.h"
#include "flight_osd.h"
#include "IHAL.h"
#include "PI_HAL.h"
#include "gs_recordings_storage.h"
#include "gs_linux_recordings_storage.h"
#include "gs_linux_runtime_platform_services.h"
#include "imgui.h"
#include "gs_linux_osd_font_storage.h"
#include "gs_linux_bootstrap.h"
#include "Video_Decoder.h"
#include "gs_runtime_state.h"
#include "ISerialTelemetry.h"
#include "linux_serial_telemetry.h"
#include "gs_udp_broadcast.h"
#include "gs_linux_udp_broadcast.h"

/*

Changes on the PI:

- Disable the compositor from raspi-config. This will increase FPS
- Change from fake to real driver: dtoverlay=vc4-fkms-v3d to dtoverlay=vc4-kms-v3d

*/

std::unique_ptr<IHAL> s_hal;
Video_Decoder s_decoder;

static LinuxSerialTelemetry s_linuxSerialTelemetry;
ISerialTelemetry* g_serialTelemetry = &s_linuxSerialTelemetry;

static LinuxGSUDPBroadcast s_linuxGSUDPBroadcast;
IGSUDPBroadcast* g_gsUDPBroadcast = &s_linuxGSUDPBroadcast;
std::string serialPortName = "";
bool bRestart = false;
bool bRestartRequired = false;
Clock::time_point restart_tp;

Clock::time_point s_change_channel = Clock::now() + std::chrono::hours(10000);
IRuntimePlatformServices* s_RuntimePlatformServices = nullptr;
IOSDFontStorage* s_OSDFontStorage = nullptr;
RecordingsStorage* s_recordingsStorage = nullptr;
FlightOSD s_flightOSD;

//===================================================================================
//===================================================================================
// Application entry point. Initializes platform services and starts the Linux bootstrap.
int main(int argc, const char* argv[])
{
    s_RuntimePlatformServices = &getLinuxRuntimePlatformServices();
    s_OSDFontStorage = &getLinuxOsdFontStorage();
    s_recordingsStorage = &getLinuxRecordingsStorage();
    return runLinuxBootstrap(argc, argv);
}
