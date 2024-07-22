#include "main.h"

#include <sys/statvfs.h>

#include "Comms.h"
#include "Clock.h"
#include "IHAL.h"
#include "PI_HAL.h"
#include "imgui.h"
#include "osd.h"
#include "osd_menu.h"
#include "Video_Decoder.h" 
#include "crc.h"
#include "packets.h"
#include <thread>
#include "imgui_impl_opengl3.h"
#include "util.h"

#include "stats.h"

#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include <random>

#include "socket.h"
#include "avi.h"
#include "jpeg_parser.h"

#ifdef TEST_LATENCY
extern "C"
{
#include "pigpio.h"
}
#endif
/*

Changes on the PI:

- Disable the compositor from raspi-config. This will increase FPS
- Change from fake to real driver: dtoverlay=vc4-fkms-v3d to dtoverlay=vc4-kms-v3d

*/

const char* resolutionName[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480",
    "640x360",
    "800x600",
    "800x456",
    "1024x768",
    "1024x576",
    "1280x960",
    "1280x720",
    "1600x1200"
};

const char* resolutionName2640[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 30fps",
    "640x360 30fps",
    "800x600 30fps",
    "800x456 30fps",
    "1024x768",
    "1024x576 13fps",
    "1280x960",
    "1280x720 13fps",
    "1600x1200"
};

const char* resolutionName2640Hi[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 40fps",
    "640x360 40fps",
    "800x600 30fps",
    "800x456 40fps",
    "1024x768",
    "1024x576 13fps",
    "1280x960",
    "1280x720 13fps",
    "1600x1200"
};

const char* resolutionName5640[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 30fps",
    "640x360 30fps",
    "800x600 30fps",
    "800x456 30fps",
    "1024x768",
    "1024x576 30fps",
    "1280x960",
    "1280x720 30fps",
    "1600x1200"
};

const char* resolutionName5640Hi[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 40fps",
    "640x360 50fps",
    "800x600 30fps",
    "800x456 50fps",
    "1024x768",
    "1024x576 30fps",
    "1280x960",
    "1280x720 30fps",
    "1600x1200"
};

const char* resolutionName2640a[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 30fps (4:3)",
    "640x360 30fps (16:9)",
    "800x600 30fps (4:3)",
    "800x456 30fps (16:9)",
    "1024x768",
    "1024x576 13fps (16:9)",
    "1280x960",
    "1280x720 13fps (16:9)",
    "1600x1200"
};

const char* resolutionName2640Hia[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 40fps (4:3)",
    "640x360 40fps (16:9)",
    "800x600 30fps (4:3)",
    "800x456 40fps (16:9)",
    "1024x768",
    "1024x576 13fps (16:9)",
    "1280x960",
    "1280x720 13fps (16:9)",
    "1600x1200"
};

const char* resolutionName5640a[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 30fps (4:3)",
    "640x360 30fps (16:9)",
    "800x600 30fps (4:3)",
    "800x456 30fps (16:9)",
    "1024x768",
    "1024x576 30fps (16:9)",
    "1280x960",
    "1280x720 30fps (16:9)",
    "1600x1200"
};

const char* resolutionName5640Hia[] =
{
    "320x240",
    "400x296",
    "480x320",
    "640x480 40fps (4:3)",
    "640x360 50fps (16:9)",
    "800x600 30fps (4:3)",
    "800x456 50fps (16:9)",
    "1024x768",
    "1024x576 30fps (16:9)",
    "1280x960",
    "1280x720 30fps (16:9)",
    "1600x1200"
};

const char* rateName[] =
{
    "2M_L",
    "2M_S",
    "5M_L",
    "5M_S",
    "11M_L",
    "11M_S",

    "6M",
    "9M",
    "12M",
    "18M",
    "24M",
    "36M",
    "48M",
    "54M",

    "MCS0_6.5ML",
    "MCS0_7.2MS",
    "MCS1L_13M",
    "MCS1S_14.4M",
    "MCS2L_19.5M",
    "MCS2S_21.7M",
    "MCS3L_26M",
    "MCS3S_28.9M",
    "MCS4L_39M",
    "MCS4S_43.3M",
    "MCS5L_52M",

    "MCS5S_57.8M",
    "MCS6L_58.5M",
    "MCS6S_65M",
    "MCS7L_65",
    "MCS7S_72.2"
};

static const Resolution resolutionsList[] = { Resolution::VGA16, Resolution::VGA, Resolution::SVGA16, Resolution::SVGA, Resolution::XGA16, Resolution::HD };
#define RESOLUTOINS_LIST_SIZE 6

std::unique_ptr<IHAL> s_hal;
Comms s_comms;
Video_Decoder s_decoder;

#ifdef USE_MAVLINK
int fdUART = -1;
std::string serialPortName = "/dev/serial0";
#endif

/* This prints an "Assertion failed" message and aborts.  */
void __assert_fail(const char* __assertion, const char* __file, unsigned int __line, const char* __function)
{
    printf("assert: %s:%d: %s: %s", __file, __line, __function, __assertion);
    fflush(stdout);
    //    abort();
}

static std::thread s_comms_thread;

static std::mutex s_ground2air_config_packet_mutex;
static Ground2Air_Config_Packet s_ground2air_config_packet;

static std::mutex s_ground2air_data_packet_mutex;
static Ground2Air_Data_Packet s_ground2air_data_packet;
int s_tlm_size = 0;

#ifdef TEST_LATENCY
static uint32_t s_test_latency_gpio_value = 0;
static Clock::time_point s_test_latency_gpio_last_tp = Clock::now();
#endif

TGroundstationConfig s_groundstation_config;

mINI::INIStructure ini;
mINI::INIFile s_iniFile("gs.ini");

float video_fps = 0;
int s_min_rssi = 0;
int s_total_data = 0;
int s_lost_frame_count = 0;
WIFI_Rate s_curr_wifi_rate = WIFI_Rate::RATE_B_2M_CCK;
int s_wifi_queue_min = 0;
int s_wifi_queue_max = 0;
uint8_t s_curr_quality = 0;
bool bRestart = false;
bool bRestartRequired = false;
Clock::time_point restart_tp;
uint16_t s_SDTotalSpaceGB16 = 0;
uint16_t s_SDFreeSpaceGB16 = 0;
bool s_air_record = false;
bool s_wifi_ovf =false;
bool s_SDDetected = false;
bool s_SDSlow = false;
bool s_SDError = false;
bool s_isOV5640 = false;
bool s_isDual = false;

uint64_t s_GSSDTotalSpaceBytes = 0;
uint64_t s_GSSDFreeSpaceBytes = 0;

bool s_debugWindowVisisble = false;

bool s_noPing = false;

Clock::time_point s_incompatibleFirmwareTime = Clock::now() - std::chrono::milliseconds(10000);
Clock::time_point s_last_packet_tp = Clock::now();

Stats s_frame_stats;
Stats s_frameParts_stats;
Stats s_frameTime_stats;
Stats s_frameQuality_stats;
Stats s_dataSize_stats;
Stats s_queueUsage_stats;

static AirStats s_last_airStats;

GSStats s_gs_stats;
GSStats s_last_gs_stats;

static Clock::time_point s_change_channel = Clock::now() + std::chrono::hours(10000);

uint8_t s_avi_fps;
uint16_t s_avi_frameWidth;
uint16_t s_avi_frameHeight;
uint32_t s_avi_frameCnt;
bool s_avi_ov2640HighFPS;
bool s_avi_ov5640HighFPS;

// Function to get filesystem statistics
void getFilesystemStats(const char *path, unsigned long long *total, unsigned long long *free) {
    struct statvfs stat;

    // Perform statvfs on the given path
    if (statvfs(path, &stat) != 0) {
        perror("statvfs");
        exit(EXIT_FAILURE);
    }

    // Calculate total and free space
    *total = (unsigned long long) stat.f_frsize * stat.f_blocks;
    *free = (unsigned long long) stat.f_frsize * stat.f_bfree;
}
//===================================================================================
//===================================================================================
void updateGSSdFreeSpace() 
{
    struct statvfs stat;
    statvfs(".", &stat);
    s_GSSDTotalSpaceBytes = (unsigned long long) stat.f_frsize * stat.f_blocks;
    s_GSSDFreeSpaceBytes = (unsigned long long) stat.f_frsize * stat.f_bfree;
}

//===================================================================================
//===================================================================================
void toggleGSRecording( int width, int height)
{
    std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);

    s_groundstation_config.record = !s_groundstation_config.record;

    if(s_groundstation_config.record)
    {
        if ( s_GSSDFreeSpaceBytes < GS_SD_MIN_FREE_SPACE_BYTES) 
        {
            s_groundstation_config.record = false;
            return;
        }

        auto time=std::time({});
#ifdef WRITE_RAW_MJPEG_STREAM 
        char filename[]="yyyy-mm-dd-hh-mm-ss.mjpeg";
        std::strftime(filename, sizeof(filename), "%Y-%m-%d-%H-%M-%S.mjpeg", std::localtime(&time));
        s_groundstation_config.record_file=fopen(filename,"wb+");
#else        
        char filename[]="yyyy-mm-dd-hh-mm-ss.avi";
        std::strftime(filename, sizeof(filename), "%Y-%m-%d-%H-%M-%S.avi", std::localtime(&time));

        prepAviIndex();

        s_avi_frameCnt = 0;

        const TVMode* v = &vmodes[clamp((int)s_ground2air_config_packet.camera.resolution, 0, (int)(Resolution::COUNT)-1)];

        if ( width != 0 )
        {
            for (size_t i = 0; i < (int)Resolution::COUNT; i++) 
            {
                if (vmodes[i].width == width && vmodes[i].height == height) 
                {
                    v = &vmodes[i];
                    break;
                }
            }
        }

        if (s_isOV5640)
        {
            s_avi_fps = s_ground2air_config_packet.camera.ov5640HighFPS ? v->highFPS5640 : v->FPS5640;
        }
        else
        {
            s_avi_fps = s_ground2air_config_packet.camera.ov2640HighFPS ? v->highFPS2640 : v->FPS2640;
        }

        s_avi_frameWidth = v->width;
        s_avi_frameHeight = v->height;
        s_avi_ov2640HighFPS = s_ground2air_config_packet.camera.ov2640HighFPS;
        s_avi_ov5640HighFPS = s_ground2air_config_packet.camera.ov5640HighFPS;

        LOGI("{}x{} {}fps\n", s_avi_frameWidth, s_avi_frameHeight, s_avi_fps);

        s_groundstation_config.record_file = fopen(filename,"wb+");
        fwrite(aviHeader, AVI_HEADER_LEN, 1, s_groundstation_config.record_file); 

#endif        
        LOGI("start record:{}",std::string(filename));
    }
    else
    {
#ifdef WRITE_RAW_MJPEG_STREAM 
#else
        finalizeAviIndex(s_avi_frameCnt);

        size_t SD_WRITE_BLOCK_SIZE = 8192;
        uint8_t sd_write_block[SD_WRITE_BLOCK_SIZE];
        while(true)
        {
            size_t sz = writeAviIndex(sd_write_block, SD_WRITE_BLOCK_SIZE);
            if ( sz == 0) break;
            fwrite(sd_write_block, sz, 1, s_groundstation_config.record_file);
        }

        // save avi header at start of file
        buildAviHdr( s_avi_fps, s_avi_frameWidth, s_avi_frameHeight, s_avi_frameCnt );

        fseek(s_groundstation_config.record_file, 0, SEEK_SET); // start of file
        fwrite(aviHeader, AVI_HEADER_LEN, 1, s_groundstation_config.record_file) ; 
#endif

        fflush(s_groundstation_config.record_file);
        fclose(s_groundstation_config.record_file);
        s_groundstation_config.record_file=nullptr;
    }

    updateGSSdFreeSpace();
}

//===================================================================================
//===================================================================================
static void comms_thread_proc()
{
    Clock::time_point last_stats_tp = Clock::now();
    Clock::time_point last_stats_tp10 = Clock::now();
    Clock::time_point last_comms_sent_tp = Clock::now();
    Clock::time_point last_data_sent_tp = Clock::now();
    uint8_t last_sent_ping = 0;
    Clock::time_point last_ping_sent_tp = Clock::now();
    Clock::time_point last_frame_decoded = Clock::now();
    Clock::duration ping_min = std::chrono::seconds(999);
    Clock::duration ping_max = std::chrono::seconds(0);
    Clock::duration ping_avg = std::chrono::seconds(0);
    size_t ping_count = 0;
    size_t sent_count = 0;
    size_t in_tlm_size = 0;
    size_t out_tlm_size = 0;
    size_t total_data = 0;
    size_t total_data10 = 0;
    int16_t min_rssi = 0;

    std::vector<uint8_t> video_frame;
    uint32_t video_frame_index = 0;
    uint8_t video_next_part_index = 0;
    bool video_restoredByFEC = false;

    struct RX_Data
    {
        std::array<uint8_t, AIR2GROUND_MTU> data;
        size_t size;
        int16_t rssi = 0;
    };

    RX_Data rx_data;


    while (true)
    {
        if (Clock::now() - last_stats_tp >= std::chrono::milliseconds(1000))
        {
            LOGI("Sent: {}, RX len: {}, TlmIn: {}, TlmOut: {}, RSSI: {}, Latency: {}/{}/{},vfps:{}", sent_count, total_data, in_tlm_size, out_tlm_size, min_rssi, 
                std::chrono::duration_cast<std::chrono::milliseconds>(ping_min).count(),
                std::chrono::duration_cast<std::chrono::milliseconds>(ping_max).count(),
                ping_count > 0 ? std::chrono::duration_cast<std::chrono::milliseconds>(ping_avg).count() / ping_count : 0,
                video_fps);

            s_min_rssi = min_rssi;
            s_total_data = total_data;

            s_noPing = (ping_count == 0) || (std::chrono::duration_cast<std::chrono::milliseconds>(ping_avg).count() / ping_count > 2000);
            s_gs_stats.pingMinMS = std::chrono::duration_cast<std::chrono::milliseconds>(ping_min).count();
            s_gs_stats.pingMaxMS = std::chrono::duration_cast<std::chrono::milliseconds>(ping_max).count();

            ping_min = std::chrono::seconds(999);
            ping_max = std::chrono::seconds(0);
            ping_avg = std::chrono::seconds(0);
            sent_count = 0;
            in_tlm_size = 0;
            out_tlm_size = 0;
            ping_count = 0;
            total_data = 0;
            min_rssi = 0;

            s_gs_stats.brokenFrames += s_last_gs_stats.brokenFrames;
            s_last_gs_stats = s_gs_stats;
            s_gs_stats = GSStats();
            s_gs_stats.statsPacketIndex = s_last_gs_stats.lastPacketIndex;

            last_stats_tp = Clock::now();
        }

        if (Clock::now() - last_stats_tp10 >= std::chrono::milliseconds(100))
        {
            total_data10 /= 1024;
            if ( total_data10 > 255 ) total_data10 = 255;
            s_dataSize_stats.add(total_data10);
            total_data10 = 0;
            last_stats_tp10 = Clock::now();
        }

        if (Clock::now() - last_comms_sent_tp >= std::chrono::milliseconds(500))
        {
            std::lock_guard<std::mutex> lg(s_ground2air_config_packet_mutex);
            auto& config = s_ground2air_config_packet;
            config.ping = last_sent_ping; 
            config.type = Ground2Air_Header::Type::Config;
            config.size = sizeof(config);
            config.crc = 0;
            config.crc = crc8(0, &config, sizeof(config)); 
            s_comms.send(&config, sizeof(config), true);
            last_comms_sent_tp = Clock::now();
            last_ping_sent_tp = Clock::now();
            sent_count++;
        }

#ifdef USE_MAVLINK
        if (fdUART != -1)
        {
            std::lock_guard<std::mutex> lg(s_ground2air_data_packet_mutex);
            auto& data = s_ground2air_data_packet;

            int frb = GROUND2AIR_DATA_MAX_PAYLOAD_SIZE - s_tlm_size;
            int n = read(fdUART, &(data.payload[s_tlm_size]), frb);

            if ( n > 0 )
            {
                s_tlm_size += n;
                in_tlm_size += n;
            }

            if ( 
                (s_tlm_size == GROUND2AIR_DATA_MAX_PAYLOAD_SIZE) ||
                ( 
                    ( (s_tlm_size > 0 ) && (Clock::now() - last_data_sent_tp) >= std::chrono::milliseconds(50)) 
                )
            )
            {
                data.type = Ground2Air_Header::Type::Telemetry;
                data.size = sizeof(Ground2Air_Header) + s_tlm_size;
                data.crc = 0;
                data.crc = crc8(0, &data, data.size); 
                s_comms.send(&data, data.size, true);
                last_data_sent_tp = Clock::now();
                sent_count++;
                s_tlm_size = 0;
            }
        }
#endif

#ifdef TEST_LATENCY
        if (s_test_latency_gpio_value == 0 && Clock::now() - s_test_latency_gpio_last_tp >= std::chrono::milliseconds(200))
        {
            s_test_latency_gpio_value = 1;
            gpioWrite(17, s_test_latency_gpio_value);
            s_test_latency_gpio_last_tp = Clock::now();
#   ifdef TEST_DISPLAY_LATENCY
            s_decoder.inject_test_data(s_test_latency_gpio_value);
#   endif
        }
        if (s_test_latency_gpio_value != 0 && Clock::now() - s_test_latency_gpio_last_tp >= std::chrono::milliseconds(50))
        {
            s_test_latency_gpio_value = 0;
            gpioWrite(17, s_test_latency_gpio_value);
            s_test_latency_gpio_last_tp = Clock::now();
#   ifdef TEST_DISPLAY_LATENCY
            s_decoder.inject_test_data(s_test_latency_gpio_value);
#   endif
        }
#endif        

#ifdef TEST_DISPLAY_LATENCY
        std::this_thread::yield();

        //pump the comms to avoid packages accumulating
        s_comms.process();

        bool restoredByFEC;
        s_comms.receive(rx_data.data.data(), rx_data.size, restoredByFEC);
#else
        //receive new packets
        do
        {
            s_comms.process();
            bool restoredByFEC;
            if (!s_comms.receive(rx_data.data.data(), rx_data.size, restoredByFEC))
            {
                std::this_thread::yield();
                break;
            }

            s_last_packet_tp = Clock::now();

            rx_data.rssi = (int16_t)s_comms.get_input_dBm();

            //filter bad packets
            Air2Ground_Header& air2ground_header = *(Air2Ground_Header*)rx_data.data.data();

            if ( air2ground_header.version != PACKET_VERSION )
            {
                s_incompatibleFirmwareTime = Clock::now();
                break;
            }

            uint32_t packet_size = air2ground_header.size;
            if (air2ground_header.type == Air2Ground_Header::Type::Video)
            {
                if (packet_size > rx_data.size)
                {
                    LOGE("Video frame {}: data too big: {} > {}", video_frame_index, packet_size, rx_data.size);
                    break;
                }
                if (packet_size < sizeof(Air2Ground_Video_Packet))
                {
                    LOGE("Video frame {}: data too small: {} > {}", video_frame_index, packet_size, sizeof(Air2Ground_Video_Packet));
                    break;
                }

                size_t payload_size = packet_size - sizeof(Air2Ground_Video_Packet);
                Air2Ground_Video_Packet& air2ground_video_packet = *(Air2Ground_Video_Packet*)rx_data.data.data();
                uint8_t crc = air2ground_video_packet.crc;
                air2ground_video_packet.crc = 0;
                uint8_t computed_crc = crc8(0, rx_data.data.data(), sizeof(Air2Ground_Video_Packet));
                if (crc != computed_crc)
                {
                    LOGE("Video frame {}, {} {}: crc mismatch: {} != {}", air2ground_video_packet.frame_index, (int)air2ground_video_packet.part_index, payload_size, crc, computed_crc);
                    break;
                }

                if (air2ground_video_packet.pong == last_sent_ping)
                {
                    last_sent_ping++;
                    auto d = (Clock::now() - last_ping_sent_tp) / 2;
                    ping_min = std::min(ping_min, d);
                    ping_max = std::max(ping_max, d);
                    ping_avg += d;
                    ping_count++;
                }

                total_data += rx_data.size;
                total_data10 += rx_data.size;
                min_rssi = std::min(min_rssi, rx_data.rssi);
                //LOGI("OK Video frame {}, {} {} - CRC OK {}. {}", air2ground_video_packet.frame_index, (int)air2ground_video_packet.part_index, payload_size, crc, rx_queue.size());

                if ((air2ground_video_packet.frame_index + 200u < video_frame_index) ||                 //frame from the distant past? TX was restarted
                    (air2ground_video_packet.frame_index > video_frame_index)) //frame from the future and we still have other frames enqueued? Stale data
                {
                    //if (video_next_part_index > 0) //incomplete frame
                    //   s_decoder.decode_data(video_frame.data(), video_frame.size());

                    //if (video_next_part_index > 0)
                    //    LOGE("Aborting video frame {}, {}", video_frame_index, video_next_part_index);

                    video_frame.clear();

                    if ( video_next_part_index != 0 )
                    {
                        //video_frame_index - not all parts are received, frame is lost
                        s_lost_frame_count++;
                        s_frame_stats.add(0);
                        s_frameTime_stats.add(0);
                        s_frameQuality_stats.add(0);
                        s_frameParts_stats.add(video_next_part_index);
                        s_queueUsage_stats.add(s_wifi_queue_max);
                    }

                    //frames [video_frame_index + 1 ... untill air2ground_video_packet.frame_index) are lost completely
                    int df = air2ground_video_packet.frame_index - video_frame_index;
                    if ( df > 1)
                    {
                        df--;
                        s_lost_frame_count += df;
                        s_frame_stats.addMultiple( 0, df );
                        s_frameTime_stats.addMultiple( 0, df );
                        s_frameQuality_stats.addMultiple( 0, df );
                        s_frameParts_stats.addMultiple( 0, df );
                        s_queueUsage_stats.addMultiple(0,df);
                    }

                    video_frame_index = air2ground_video_packet.frame_index;
                    video_next_part_index = 0;
                    video_restoredByFEC = false;
                }
                if (air2ground_video_packet.frame_index == video_frame_index && air2ground_video_packet.part_index == video_next_part_index)
                {
                    video_restoredByFEC |= restoredByFEC;
                    video_next_part_index++;
                    size_t offset = video_frame.size();
                    video_frame.resize(offset + payload_size);
                    memcpy(video_frame.data() + offset, rx_data.data.data() + sizeof(Air2Ground_Video_Packet), payload_size);

                    if (air2ground_video_packet.last_part != 0)
                    {
                        //LOGI("Received frame {}, {}, size {}", video_frame_index, video_next_part_index, video_frame.size());
                        s_decoder.decode_data(video_frame.data(), video_frame.size());
                        if(s_groundstation_config.record)
                        {
#ifdef WRITE_RAW_MJPEG_STREAM                            
                            std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);
                            fwrite(video_frame.data(),video_frame.size(),1,s_groundstation_config.record_file);
#else

                            int width, height;
                            if ( getJPEGDimensions(video_frame.data(), width, height, 2048) )
                            {
                                //LOGI("Received frame {}, {}", width, height);

                                if ( 
                                    (width != s_avi_frameWidth) || 
                                    (height != s_avi_frameHeight) ||
                                    ( s_avi_ov2640HighFPS != s_ground2air_config_packet.camera.ov2640HighFPS ) ||
                                    ( s_avi_ov5640HighFPS != s_ground2air_config_packet.camera.ov5640HighFPS )
                                )
                                {
                                    toggleGSRecording(0,0); //stop
                                    toggleGSRecording(width,height); //start
                                }

                                {
                                    std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);
                                    uint16_t jpegSize = video_frame.size();
                                    uint16_t filler = (4 - (jpegSize & 0x3)) & 0x3; 
                                    size_t jpegSize1 = jpegSize + filler;
                                    uint8_t buf[8];
                                    memcpy(buf, dcBuf, 4); 
                                    memcpy(&buf[4], &jpegSize1, 4);

                                    fwrite(buf,8,1,s_groundstation_config.record_file);
                                    fwrite(video_frame.data(),video_frame.size(),1,s_groundstation_config.record_file);

                                    memset(buf, 0, 4); 
                                    fwrite(buf,filler,1,s_groundstation_config.record_file);
                                    
                                    buildAviIdx(jpegSize1); // save avi index for frame
                                    s_avi_frameCnt++;
                                }

                                if ( (s_avi_frameCnt == (DVR_MAX_FRAMES-1)) || (moviSize > 50*1024*1024))
                                {
                                    toggleGSRecording(0,0); //stop
                                    toggleGSRecording(width,height); //start
                                }
                            }
                            else
                            {
                                LOGI("Received frame - unknown size!");
                            }
#endif                            
                        }

                        if(s_groundstation_config.socket_fd>0)
                        {
                            send_data_to_udp(s_groundstation_config.socket_fd,video_frame.data(),video_frame.size());
                        }
                        video_next_part_index = 0;
                        video_frame.clear();

                        s_frame_stats.add(video_restoredByFEC ? 1 : 3);
                        s_frameParts_stats.add(air2ground_video_packet.part_index);
                        s_frameQuality_stats.add(s_curr_quality);
                        s_queueUsage_stats.add(s_curr_quality);

                        auto current_time = Clock::now();
                        auto duration_since_last_frame = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_frame_decoded);
                        auto milliseconds_since_last_frame = duration_since_last_frame.count();
                        if( milliseconds_since_last_frame > 100) milliseconds_since_last_frame = 100;
                        s_frameTime_stats.add((uint8_t)milliseconds_since_last_frame);
                        last_frame_decoded = current_time;

                        video_restoredByFEC = false;
                    }
                }

            }
            else if (air2ground_header.type == Air2Ground_Header::Type::Telemetry)
            {
#ifdef USE_MAVLINK
              if (fdUART != -1)
              {
                if (packet_size > rx_data.size)
                {
                    LOGE("Telemetry frame: data too big: {} > {}", packet_size, rx_data.size);
                    break;
                }
                if (packet_size < (sizeof(Air2Ground_Data_Packet) + 1))
                {
                    LOGE("Telemetry frame: data too small: {} < {}", packet_size, sizeof(Air2Ground_Data_Packet) + 1);
                    break;
                }

                size_t payload_size = packet_size - sizeof(Air2Ground_Data_Packet);
                Air2Ground_Data_Packet& air2ground_data_packet = *(Air2Ground_Data_Packet*)rx_data.data.data();
                uint8_t crc = air2ground_data_packet.crc;
                air2ground_data_packet.crc = 0;
                uint8_t computed_crc = crc8(0, rx_data.data.data(), sizeof(Air2Ground_Data_Packet));
                if (crc != computed_crc)
                {
                    LOGE("Telemetry frame: crc mismatch {}: {} != {}", payload_size, crc, computed_crc);
                    break;
                }

                total_data10 += rx_data.size;
                min_rssi = std::min(min_rssi, rx_data.rssi);
                //LOGI("OK Telemetry frame {} - CRC OK {}. {}", payload_size, crc, rx_queue.size());

                write(fdUART, ((uint8_t*)&air2ground_data_packet) + sizeof(Air2Ground_Data_Packet), payload_size);
                out_tlm_size += payload_size;
              }
#endif
            }
            else if (air2ground_header.type == Air2Ground_Header::Type::OSD)
            {
                if (packet_size > rx_data.size)
                {
                    LOGE("OSD frame: data too big: {} > {}", packet_size, rx_data.size);
                    break;
                }
                if (packet_size < (sizeof(Air2Ground_OSD_Packet)))
                {
                    LOGE("OSD frame: data too small: {} > {}", packet_size, sizeof(Air2Ground_OSD_Packet));
                    break;
                }

                Air2Ground_OSD_Packet& air2ground_osd_packet = *(Air2Ground_OSD_Packet*)rx_data.data.data();
                uint8_t crc = air2ground_osd_packet.crc;
                air2ground_osd_packet.crc = 0;
                uint8_t computed_crc = crc8(0, rx_data.data.data(), sizeof(Air2Ground_OSD_Packet));
                if (crc != computed_crc)
                {
                    LOGE("OSD frame: crc mismatch: {} != {}", crc, computed_crc);
                    break;
                }

                s_last_airStats = air2ground_osd_packet.stats;

                total_data += rx_data.size;
                total_data10 += rx_data.size;

                //TODO: remove all these, use s_last_airStats
                s_curr_wifi_rate = (WIFI_Rate)air2ground_osd_packet.stats.curr_wifi_rate;
                s_curr_quality = air2ground_osd_packet.stats.curr_quality;
                s_wifi_queue_min = air2ground_osd_packet.stats.wifi_queue_min;
                s_wifi_queue_max = air2ground_osd_packet.stats.wifi_queue_max;
                s_SDTotalSpaceGB16 = air2ground_osd_packet.stats.SDTotalSpaceGB16;
                s_SDFreeSpaceGB16 = air2ground_osd_packet.stats.SDFreeSpaceGB16;
                s_air_record = air2ground_osd_packet.stats.air_record_state != 0;
                s_wifi_ovf = air2ground_osd_packet.stats.wifi_ovf !=0;
                s_SDDetected = air2ground_osd_packet.stats.SDDetected != 0;
                s_SDError = air2ground_osd_packet.stats.SDError != 0;
                s_SDSlow = air2ground_osd_packet.stats.SDSlow != 0;
                s_isOV5640 = air2ground_osd_packet.stats.isOV5640 != 0;

                g_osd.update( &air2ground_osd_packet.buffer );
            }
            else
            {
                LOGE("Unknown air packet: {}", air2ground_header.type);
                break;
            }


        } 
        while (false);
#endif
    }
}

//===================================================================================
//===================================================================================
static inline ImVec2 operator+(const ImVec2& lhs, const ImVec2& rhs)
{
    return ImVec2(lhs.x + rhs.x, lhs.y + rhs.y);
}

//===================================================================================
//===================================================================================
static inline ImVec2 ImRotate(const ImVec2& v, float cos_a, float sin_a)
{
    return ImVec2(v.x * cos_a - v.y * sin_a, v.x * sin_a + v.y * cos_a);
}

//===================================================================================
//===================================================================================
void calculateLetterBoxAndBorder( int width, int height, int& x1, int& y1, int& x2, int& y2)
{
    bool videoAspect16x9 = s_decoder.isAspect16x9();

    float videoAspect = videoAspect16x9 ? 16.0f / 9.0f : 4.0f/3.0f;

    ImVec2 screenSize = ImGui::GetIO().DisplaySize;
    float screenAspect = screenSize.x/screenSize.y;
    
    if (  s_groundstation_config.screenAspectRatio == ScreenAspectRatio::ASPECT5X4 )
    {
        screenAspect =  5.0f / 4.0f;
    }
    else if (  s_groundstation_config.screenAspectRatio == ScreenAspectRatio::ASPECT4X3 )
    {
        screenAspect =  4.0f / 3.0f;
    }
    else if (  s_groundstation_config.screenAspectRatio == ScreenAspectRatio::ASPECT16X9 )
    {
        screenAspect =  16.0f / 9.0f;
    }
    else if (  s_groundstation_config.screenAspectRatio == ScreenAspectRatio::ASPECT16X10 )
    {
        screenAspect =  16.0f / 10.0f;
    } 
    
    if (  (s_groundstation_config.screenAspectRatio == ScreenAspectRatio::STRETCH) ||  ((int)(videoAspect*100 + 0.5) == (int)(screenAspect*100 + 0.5f)) )
    {
        //no scale or stretch
        x1 = 0; y1 = 0; x2 = width; y2 = height;
    }
    else if ( videoAspect > screenAspect )
    {
        //fe.e 16x9 on 4x3
        int h1 = (int)(height * screenAspect / videoAspect + 0.5f);
        x1 = 0; y1 = (height - h1) / 2; x2 = width; y2 = y1 + h1;
    }
    else
    {
        //f.e. 4x3 on 16x9
        int w1 = (int)(width * videoAspect / screenAspect + 0.5f);
        x1 = (width - w1) / 2; y1 = 0; x2 = x1 + w1; y2 = height;
    }
}

//===================================================================================
//===================================================================================
void ImageRotated(ImTextureID tex_id, ImVec2 center, ImVec2 size, float angle, float uvAngle)
{
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    float cos_a = cosf(angle);
    float sin_a = sinf(angle);
    ImVec2 pos[4] =
        {
            center + ImRotate(ImVec2(-size.x * 0.5f, -size.y * 0.5f), cos_a, sin_a),
            center + ImRotate(ImVec2(+size.x * 0.5f, -size.y * 0.5f), cos_a, sin_a),
            center + ImRotate(ImVec2(+size.x * 0.5f, +size.y * 0.5f), cos_a, sin_a),
            center + ImRotate(ImVec2(-size.x * 0.5f, +size.y * 0.5f), cos_a, sin_a)};

    cos_a = cosf(uvAngle);
    sin_a = sinf(uvAngle);
    ImVec2 uvCenter(0.5f, 0.5f);
    ImVec2 uvs[4] =
        {
            uvCenter + ImRotate(ImVec2(-0.5f, -0.5f), cos_a, sin_a),
            uvCenter + ImRotate(ImVec2(+0.5f, -0.5f), cos_a, sin_a),
            uvCenter + ImRotate(ImVec2(+0.5f, +0.5f), cos_a, sin_a),
            uvCenter + ImRotate(ImVec2(-0.5f, +0.5f), cos_a, sin_a)};

    draw_list->AddImageQuad(tex_id, pos[0], pos[1], pos[2], pos[3], uvs[0], uvs[1], uvs[2], uvs[3], IM_COL32_WHITE);
}


//===================================================================================
//===================================================================================
void exitApp()
{
    if (s_groundstation_config.record)
    {
        toggleGSRecording(0,0);
        /*
        std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);
        fflush(s_groundstation_config.record_file);
        fclose(s_groundstation_config.record_file); 
        */
    }
    abort();
}

//===================================================================================
//===================================================================================
float calcLossRatio( int outCount, int inCount)
{
    if ( outCount == 0 ) return 0;
    int loss = outCount - inCount;
    if ( loss <= 0 ) return 0;
    return (loss * 100.0f)/ outCount;
}

//===================================================================================
//===================================================================================
void applyWifiChannel(Ground2Air_Config_Packet& config)
{
    config.wifi_channel = s_groundstation_config.wifi_channel;
    s_change_channel = Clock::now() + std::chrono::milliseconds(3000);
}

//===================================================================================
//===================================================================================
void applyWifiChannelInstant(Ground2Air_Config_Packet& config)
{
    config.wifi_channel = s_groundstation_config.wifi_channel;
    s_comms.setChannel (s_groundstation_config.wifi_channel);
}

//===================================================================================
//===================================================================================
bool isHQDVRMode()
{
    return s_ground2air_config_packet.camera.resolution == Resolution::HD;
}

//===================================================================================
//===================================================================================
int run(char* argv[])
{
    ImGuiIO& io = ImGui::GetIO();

    s_decoder.init(*s_hal);

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    s_comms_thread = std::thread(&comms_thread_proc);

    Ground2Air_Config_Packet config = s_ground2air_config_packet;

    size_t video_frame_count = 0;

    {
        g_osd.init();

        std::string& temp = ini["gs"]["osd_font"] ;
        g_osd.loadFont( temp != "" ? temp.c_str() : "INAV_default_24.png" );
    }

    Clock::time_point last_stats_tp = Clock::now();
    Clock::time_point last_tp = Clock::now();

    auto f = [&config,&argv]
    {
        bool ignoreKeys = g_osdMenu.visible;
        //---------- fullscreen window
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("fullscreen", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav  | ImGuiWindowFlags_NoFocusOnAppearing);
        {

            {
                //RSSI
                char buf[32];
                sprintf(buf, "%d", (int)s_min_rssi );

                ImGui::PushID(0);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::Button(buf, ImVec2(60.0f, 0));
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            {
                //queue usage
                char buf[32];
                sprintf(buf, "%d%%", (int)s_wifi_queue_max );
                ImGui::SameLine();
                ImGui::PushID(0);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, s_wifi_ovf ? 0.6f : 0, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::Button(buf, ImVec2(55.0f, 0));
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            {
                //video bitrate
                char buf[32];
                sprintf(buf, "%.1fMb", (int)s_total_data*8.0f/(1024*1024));
                ImGui::SameLine();
                ImGui::PushID(0);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::Button(buf, ImVec2(90.0f, 0));
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            {
                //resolution
                ImGui::SameLine();
                ImGui::PushID(0);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                //ImGui::Button(resolutionName[(int)config.camera.resolution], ImVec2(120.0f, 0));
                ImGui::Button(resolutionName[(int)config.camera.resolution]);
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            {
                //fps
                char buf[32];
                sprintf(buf, "%02d", (int)video_fps);
                ImGui::SameLine();
                ImGui::PushID(0);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.0f, 0.6f));
                ImGui::Button(buf, ImVec2(45.0f, 0));
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            if ( s_noPing )
            {
                //NO PING!
                ImGui::SameLine();
                ImGui::PushID(0);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::Button("!NO PING!");
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            if ( s_SDSlow )
            {
                //!SD SLOW!
                ImGui::SameLine();
                ImGui::PushID(0);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::Button("!SD SLOW!");
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            if ( s_air_record )
            {
                //AIR REC
                ImGui::SameLine();
                ImGui::PushID(0);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::Button("AIR");
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            //GS REC
            if ( s_groundstation_config.record )
            {
                ImGui::SameLine();
                ImGui::PushID(1);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::Button("GS");
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            //GS REC
            if ( isHQDVRMode() )
            {
                ImGui::SameLine();
                ImGui::PushID(1);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::Button("HQ DVR");
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            //Incompatible firmware
            if (Clock::now() - s_incompatibleFirmwareTime < std::chrono::milliseconds(5000))
            {
                ImGui::PushID(1);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::Button("!Incompatible Air Unit firmware. Please update!");
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            //OSD Font error
            if (g_osd.isFontError())
            {
                ImGui::PushID(1);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::Button("!Displayport OSD Font Unexpected Format!");
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            if ( s_groundstation_config.stats )
            {
                char overlay[32];

                ImGui::PushItemWidth(ImGui::GetWindowWidth() * 0.25f);
                ImGui::PlotHistogram("Frames", Stats::getter, &s_frame_stats, s_frame_stats.count(), 0, NULL, 0, 3.0f, ImVec2(0, 24));            

                sprintf(overlay, "max: %d", (int)s_frameParts_stats.max());
                ImGui::PlotHistogram("Parts", Stats::getter, &s_frameParts_stats, s_frameParts_stats.count(), 0, overlay, 0, s_frameParts_stats.average()*2 + 1.0f, ImVec2(0, 60));
                ImGui::PlotHistogram("Period", Stats::getter, &s_frameTime_stats, s_frameTime_stats.count(), 0, NULL, 0, 100.0f, ImVec2(0, 60));

                sprintf(overlay, "cur: %d", s_curr_quality);
                ImGui::PlotHistogram("Quality", Stats::getter, &s_frameQuality_stats, s_frameQuality_stats.count(), 0, overlay, 0, 64.0f, ImVec2(0, 60));

                sprintf(overlay, "avg: %d KB/sec", ((int)(s_dataSize_stats.average()+0.5f) )*10);
                ImGui::PlotHistogram("DataSize", Stats::getter, &s_dataSize_stats, s_dataSize_stats.count(), 0, overlay, 0, 100.0f, ImVec2(0, 60));

                sprintf(overlay, "%d%%", (int)(s_wifi_queue_max));
                ImGui::PlotHistogram("Wifi Load", Stats::getter, &s_queueUsage_stats, s_queueUsage_stats.count(), 0, overlay, 0, 100.0f, ImVec2(0, 60));

                ImGui::PopItemWidth();

                const float table_width = 420.0f;
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - table_width);
                ImGui::SetCursorPosY(10);

                if (ImGui::BeginTable("table1", 2, 0, ImVec2(table_width, 24.0f) ))
                {
                    ImGuiStyle& style = ImGui::GetStyle();
                    ImU32 c = ImGui::ColorConvertFloat4ToU32(style.Colors[ImGuiCol_FrameBg] );
 
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 270.0f); 

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("AirOutPacketRate");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d pkt/s", s_last_airStats.outPacketRate);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("AirInPacketRate");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d pkt/s", s_last_airStats.inPacketRate);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("AirOthersPacketRate");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d pkt/s", s_last_airStats.inRejectedPacketRate);
                    }


                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("AirPacketLossRatio");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%.1f%%", calcLossRatio(s_last_gs_stats.outPacketCounter, s_last_airStats.inPacketRate) );
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GSOutPacketRate");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d pkt/s", s_last_gs_stats.outPacketCounter);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GSInPacketRate");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d+%d", s_last_gs_stats.inPacketCounter[0], s_last_gs_stats.inPacketCounter[1]);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GSPacketLossRatio1");

                        ImGui::TableSetColumnIndex(1);

                        ImGui::Text("%.1f,%.1f%%", calcLossRatio(s_last_airStats.outPacketRate, s_last_gs_stats.inPacketCounter[0]),
                            calcLossRatio(s_last_airStats.outPacketRate, s_last_gs_stats.inPacketCounter[1]));
                    }
/*
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GSExpectedPktRate");

                        ImGui::TableSetColumnIndex(1);

                        ImGui::Text("%d", (s_last_gs_stats.lastPacketIndex - s_last_gs_stats.statsPacketIndex) /12 * config.fec_codec_n);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GSUniquePktRate");

                        ImGui::TableSetColumnIndex(1);

                        ImGui::Text("%d", s_last_gs_stats.inUniquePacketCounter);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GSDublicatedPktRate");

                        ImGui::TableSetColumnIndex(1);

                        ImGui::Text("%d", s_last_gs_stats.inDublicatedPacketCounter);
                    }
*/
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GSPacketLossRatio2");

                        ImGui::TableSetColumnIndex(1);

                        ImGui::Text("%.1f%%", calcLossRatio((s_last_gs_stats.lastPacketIndex - s_last_gs_stats.statsPacketIndex)/12*config.fec_codec_n, s_last_gs_stats.inUniquePacketCounter));
                    }
/*
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("FECSuccIndex");

                        ImGui::TableSetColumnIndex(1);

                        uint32_t blocksCount = s_last_gs_stats.FECBlocksCounter;
                        if ( blocksCount == 0 ) blocksCount = 1;
                        ImGui::Text("%.1f", s_last_gs_stats.FECSuccPacketIndexCounter * 1.0f / blocksCount);
                    }
*/
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Air RSSI");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d dbm", -s_last_airStats.rssiDbm);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Air Noise Floor");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d dbm", -s_last_airStats.noiseFloorDbm);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Air SNR");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d db", (int)s_last_airStats.noiseFloorDbm - s_last_airStats.rssiDbm );
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GS RSSI 1");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d dbm", -s_last_gs_stats.rssiDbm[0]);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GS RSSI 2");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d dbm", s_last_gs_stats.rssiDbm[1]);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GS Noise Floor");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d dbm", -s_last_gs_stats.noiseFloorDbm);
                    }

/*
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("GS SNR");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d db", (int)s_last_gs_stats.noiseFloorDbm - s_last_gs_stats.rssiDbm );
                    }
*/

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Ping min");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d ms", s_last_gs_stats.pingMinMS);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Ping max");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d ms", s_last_gs_stats.pingMaxMS);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Capture FPS");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d FPS", s_last_airStats.captureFPS);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Frame size min");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d b", s_last_airStats.cam_frame_size_min);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Frame size max");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d b", s_last_airStats.cam_frame_size_max);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Camera OVF");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d", s_last_airStats.cam_ovf_count);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Broken frames");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d", s_last_gs_stats.brokenFrames);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Mavlink Up");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d b/s", s_last_airStats.inMavlinkRate);
                    }

                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, c );

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("Mavlink Down");

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d b/s", s_last_airStats.outMavlinkRate);
                    }


/*
    uint16_t outPacketRate;
    uint16_t inPacketRate;
    uint8_t inPacketLostRatio;
    uint8_t rssiDbm;
    uint8_t snrDb;
    uint8_t noiseFloorDbm;
    uint8_t captureFPS;
    uint8_t cam_ovf_count;
    uint16_t inMavlinkRate; //b/s
    uint16_t outMavlinkRate; //b/s
*/
                    ImGui::EndTable();
                }
            }

            g_osd.draw();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        //------------ osd menu
        g_osdMenu.draw(config);

        //------------ debug window
        if ( s_debugWindowVisisble )
        {
            char buf[256];
            sprintf(buf, "RSSI:%d FPS:%1.0f/%d %dKB/S %d%%..%d%% AQ:%d %s/%s###HAL", 
            s_min_rssi, video_fps, s_lost_frame_count, 
            s_total_data/1024, 
            s_wifi_queue_min,s_wifi_queue_max,
            s_curr_quality,
            rateName[(int)s_curr_wifi_rate], rateName[(int)config.wifi_rate]);

            static const float SLIDER_WIDTH = 480.0f;

            ImGui::SetNextWindowCollapsed(true, ImGuiCond_Once); 
            ImGui::Begin(buf);
            {
                {
                    int value = config.wifi_power;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("WIFI Power", &value, 0, 20); 
                    config.wifi_power = value;
                }
                {
                    int value = (int)config.wifi_rate;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("WIFI Rate", &value, (int)WIFI_Rate::RATE_B_2M_CCK, (int)WIFI_Rate::RATE_N_72M_MCS7_S);
                    if (config.wifi_rate != (WIFI_Rate)value) 
                    {
                        config.wifi_rate = (WIFI_Rate)value;
                        saveGround2AirConfig(config);
                    }
                }
                {
                    bool pending = s_change_channel < Clock::now() + std::chrono::hours(1);
                    ImGui::BeginDisabled( pending);
                    if (ImGui::Button("APPLY"))
                    {
                        //start sending new channel to air, restart after 2 seconds
                        applyWifiChannel(config);
                    }
                    ImGui::EndDisabled();

                    int ch = s_groundstation_config.wifi_channel;
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(SLIDER_WIDTH - 93); 
                    ImGui::SliderInt("WIFI Channel", &s_groundstation_config.wifi_channel, 1, 13);
                    if ( ch != s_groundstation_config.wifi_channel)
                    {
                        saveGroundStationConfig();
                    }
                }
                {
                    int value = config.fec_codec_n;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("FEC_N", &value,FEC_K+1, FEC_N);
                    if (config.fec_codec_n != (int8_t)value)
                    {
                        config.fec_codec_n = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }
                
                if ( s_isOV5640 )

                {
                    if ( ImGui::Checkbox("ov5640HiFPS", &config.camera.ov5640HighFPS) )
                    {
                        saveGround2AirConfig(config);
                    }
                }
                else
                {
                    if ( ImGui::Checkbox("ov2640HiFPS", &config.camera.ov2640HighFPS) )
                    {
                        saveGround2AirConfig(config);
                    }
                }
                {
                    ImGui::SameLine();
                    int value = (int)config.camera.resolution;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH - 198); 
                    ImGui::SliderInt("Resolution", &value, 0, 11);
                    if ( config.camera.resolution != (Resolution)value )
                    {
                        config.camera.resolution = (Resolution)value;
                        saveGround2AirConfig(config);
                    }
                }
                {
                    int value = (int)config.camera.fps_limit;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("FPS Limit", &value, 0, 100);
                    config.camera.fps_limit = (uint8_t)value;
                }
                {
                    int value = config.camera.quality;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("Quality(0-auto)", &value, 0, 63);
                    config.camera.quality = value;
                }

                ImGui::Checkbox("AGC", &config.camera.agc);
                ImGui::SameLine();            
                ImGui::Checkbox("AEC", &config.camera.aec);
                if ( config.camera.aec && !s_isOV5640)
                {
                    ImGui::SameLine();            
                    ImGui::Checkbox("AEC DSP", &config.camera.aec2);
                }
                ImGui::SameLine();            
                {
                    bool prev = config.camera.vflip;
                    ImGui::Checkbox("VFLIP", &config.camera.vflip);
                    if ( prev != config.camera.vflip )
                    {
                        saveGround2AirConfig(config);
                    }
                }
                ImGui::SameLine();            
                ImGui::Checkbox("HMIRROR", &config.camera.hmirror);

                if ( !config.camera.agc )
                {
                    int value = config.camera.agc_gain;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("AGC Gain", &value, 0, 30);
                    config.camera.agc_gain = (int8_t)value;
                }
                else 
                {
                    int value = config.camera.gainceiling;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("GainCeiling", &value, 0, 6);
                    config.camera.gainceiling = (uint8_t)value;
                }

                if ( config.camera.aec )
                {
                    int value = config.camera.ae_level;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("AEC Level", &value, -2, 2);
                    if ( config.camera.ae_level != (int8_t)value)
                    {
                        config.camera.ae_level = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }
                else 
                {
                    int value = config.camera.aec_value;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("AEC Value", &value, 0, 1200);
                    config.camera.aec_value = (uint16_t)value;
                }

                {
                    int value = config.camera.brightness;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("Brightness", &value, -2, 2);
                    if (config.camera.brightness != (int8_t)value)
                    {
                        config.camera.brightness = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }

                {
                    int value = config.camera.contrast;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("Contrast", &value, -2, 2);
                    if (config.camera.contrast != (int8_t)value)
                    {
                        config.camera.contrast = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }

                {
                    int value = config.camera.saturation;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("Saturation", &value, -2, 2);
                    if (config.camera.saturation != (int8_t)value)
                    {
                        config.camera.saturation = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }

                {
                    int value = config.camera.sharpness;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("Sharpness(3-auto)", &value, -2, 3);
                    if (config.camera.sharpness != (int8_t)value)
                    {
                        config.camera.sharpness = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }

    /* does nothing ?
                if ( s_isOV5640 )
                {
                    int value = config.camera.denoise;
                    ImGui::SliderInt("Denoise", &value, 0, 8);
                    config.camera.denoise = (int8_t)value;
                }
    */
                {
                    int ch = (int)s_groundstation_config.screenAspectRatio;
                    ImGui::SetNextItemWidth(SLIDER_WIDTH); 
                    ImGui::SliderInt("Letterbox", &ch, 0, 5);
                    if ( ch != (int)s_groundstation_config.screenAspectRatio)
                    {
                        s_groundstation_config.screenAspectRatio = (ScreenAspectRatio)ch;
                        saveGroundStationConfig();
                    }
                }

                if ( bRestartRequired)
                {
                    ImGui::Text("*Restart to apply!");
                }

                {
                    //ImGui::Checkbox("LC", &config.camera.lenc);
                    //ImGui::SameLine();
                    //ImGui::Checkbox("DCW", &config.camera.dcw);
                    //ImGui::SameLine();
                    //ImGui::Checkbox("H", &config.camera.hmirror);
                    //ImGui::SameLine();
                    //ImGui::Checkbox("V", &config.camera.vflip);
                    //ImGui::SameLine();
                    //ImGui::Checkbox("Raw", &config.camera.raw_gma);
                    //ImGui::SameLine();
                }
                if ( ImGui::Button("Profile 500ms") )
                {
                    config.profile1_btn++;
                }
                ImGui::SameLine();
                if ( ImGui::Button("Profile 3s") )
                {
                    config.profile2_btn++;
                }
                ImGui::SameLine();
                if ( ImGui::Checkbox("VSync", &s_groundstation_config.vsync) )
                {
                    s_hal->set_vsync(s_groundstation_config.vsync, true);
                    saveGroundStationConfig();
                }
                ImGui::SameLine();
                ImGui::Checkbox("Stats", &s_groundstation_config.stats);

                if ( ImGui::Button("Air Record") )
                {
                    config.air_record_btn++;
                }

                ImGui::SameLine();
                if (ImGui::Button("GS Record"))
                {
                    toggleGSRecording(0,0);
                }

                ImGui::SameLine();
                if (ImGui::Button("Restart"))
                {
                    restart_tp = Clock::now();
                    bRestart = true;
                }

                ImGui::SameLine();
                if (ImGui::Button("Exit"))
                {
                    exitApp();
                }
                
                ImGui::Text("%.3f ms/frame (%.1f FPS) %.1f VFPS", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate, video_fps);
                ImGui::Text("AIR SD: %s%s%s %.2fGB/%.2fGB %s", 
                    s_SDDetected ? "Detected" : "Not detected", s_SDError ? " Error" :"",  s_SDSlow ? " Slow" : "",
                    s_SDFreeSpaceGB16 / 16.0f, s_SDTotalSpaceGB16 / 16.0f,
                    s_isOV5640 ? "OV5640" : "OV2640");
                ImGui::Text("GS SD: %s %.2fGB/%.2fGB", 
                        s_GSSDFreeSpaceBytes >= GS_SD_MIN_FREE_SPACE_BYTES ? "Ok" : "Low space",
                        s_GSSDFreeSpaceBytes/(1024.0f*1024*1024), s_GSSDTotalSpaceBytes/(1024.0f*1024*1024));
            }
            ImGui::End();
        } //debug window
        
        if ( ImGui::IsKeyPressed(ImGuiKey_D) || ImGui::IsKeyPressed(ImGuiKey_MouseMiddle))
        {
            s_debugWindowVisisble = !s_debugWindowVisisble;
        }

        if ( ImGui::IsKeyPressed(ImGuiKey_S))
        {
            s_groundstation_config.stats = !s_groundstation_config.stats;
        }

        bool resetRes = false;
        if ( !ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_LeftArrow) )
        {
            bool found = false;
            for ( int i = 0; i < RESOLUTOINS_LIST_SIZE; i++ )
            {
                if ( config.camera.resolution == resolutionsList[i])
                {
                    if ( i!=0)
                    {
                        config.camera.resolution = resolutionsList[i-1];
                        saveGround2AirConfig(config);
                    }
                    found = true;
                    break;
                }
            }
            resetRes |=  !found;
        }
        if ( !ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
        {
            bool found = false;
            for ( int i = 0; i < RESOLUTOINS_LIST_SIZE; i++ )
            {
                if ( config.camera.resolution == resolutionsList[i])
                {
                    if ( i != RESOLUTOINS_LIST_SIZE-1 )
                    {
                        config.camera.resolution = resolutionsList[i+1];
                        saveGround2AirConfig(config);
                    }
                    found = true;
                    break;
                }
            }
            resetRes |= !found;
        }

        if ( resetRes )
        {
            config.camera.resolution = Resolution::SVGA16;
            saveGround2AirConfig(config);
        }

        if ( !ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_R))
        {
            config.air_record_btn++;
        }

        if (!ignoreKeys &&  ImGui::IsKeyPressed(ImGuiKey_G))
        {
            toggleGSRecording(0,0);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Space) || (!ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_Escape)))
        {
            exitApp();
        }

        if ( bRestart ) 
        {
            if (Clock::now() - restart_tp >= std::chrono::milliseconds(100)) 
            {
                bRestart = false;
                execv(argv[0],argv);
            }
        } 

        if ( Clock::now() > s_change_channel )
        {
            if ( std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_last_packet_tp).count() < 300 )
            {
                //still not changed - wait
                s_change_channel = Clock::now() + std::chrono::milliseconds(3000);
            }
            else
            {
                s_change_channel = Clock::now() + std::chrono::hours(10000);
                s_comms.setChannel (s_groundstation_config.wifi_channel);
            }
        }

        std::lock_guard<std::mutex> lg(s_ground2air_config_packet_mutex);
        s_ground2air_config_packet = config;
    };

    s_hal->add_render_callback(f);

    while (true)
    {
        s_decoder.unlock_output();
        size_t count = s_decoder.lock_output();
        if ( count == 0)
        {
            //std::this_thread::yield();
        }

        video_frame_count += count;
        s_hal->set_video_channel(s_decoder.get_video_texture_id());

        s_hal->process();

        if (Clock::now() - last_stats_tp >= std::chrono::milliseconds(1000))
        {
            last_stats_tp = Clock::now();
            video_fps = video_frame_count;
            video_frame_count = 0;
            s_lost_frame_count = 0;
        }

        Clock::time_point now = Clock::now();
        Clock::duration dt = now - last_tp;
        last_tp = now;
        io.DeltaTime = std::chrono::duration_cast<std::chrono::duration<float> >(dt).count();
    }

    return 0;
}

#ifdef USE_MAVLINK
//===================================================================================
//===================================================================================
bool init_uart()
{
    fdUART = open(serialPortName.c_str(), O_RDWR);
    if (fdUART == -1)
    {
      printf("Warning: Can not open serial port %s. Telemetry will not be available.\n", serialPortName.c_str());
      return false;
    }

    struct termios tty;
    if(tcgetattr(fdUART, &tty) != 0) 
    {
      printf("Error %i from tcgetattr: %s\n", errno, strerror(errno));
      return false;
    }

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_lflag &= ~ICANON;
    tty.c_lflag &= ~ECHO;
    tty.c_lflag &= ~ECHOE;
    tty.c_lflag &= ~ECHONL;
    tty.c_lflag &= ~ISIG;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VTIME] = 0;
    tty.c_cc[VMIN] = 0;
    
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
  
    if (tcsetattr(fdUART, TCSANOW, &tty) != 0) 
    {
      printf("Error %i from tcsetattr: %s\n", errno, strerror(errno));
      return false;
    }

    return true;
}
#endif 

//===================================================================================
//===================================================================================
void saveGroundStationConfig()
{
    ini["gs"]["wifi_channel"] = std::to_string(s_groundstation_config.wifi_channel);
    ini["gs"]["screen_aspect_ratio"] = std::to_string((int)s_groundstation_config.screenAspectRatio);
    s_iniFile.write(ini);
}

//===================================================================================
//===================================================================================
void saveGround2AirConfig(const Ground2Air_Config_Packet& config)
{
    ini["gs"]["brightness"] = std::to_string(config.camera.brightness);
    ini["gs"]["contrast"] = std::to_string(config.camera.contrast);
    ini["gs"]["saturation"] = std::to_string(config.camera.saturation);
    ini["gs"]["ae_level"] = std::to_string(config.camera.ae_level);
    ini["gs"]["sharpness"] = std::to_string(config.camera.sharpness);
    ini["gs"]["vflip"] = std::to_string(config.camera.vflip ? 1 : 0);
    ini["gs"]["resolution"] = std::to_string((int)config.camera.resolution);
    ini["gs"]["wifi_rate"] = std::to_string((int)config.wifi_rate);
    ini["gs"]["fec_n"] = std::to_string((int)config.fec_codec_n);
    ini["gs"]["ov2640_high_fps"] = std::to_string((int)config.camera.ov2640HighFPS ? 1 : 0);
    ini["gs"]["ov5640_high_fps"] = std::to_string((int)config.camera.ov5640HighFPS ? 1 : 0);
    s_iniFile.write(ini);
}

//===================================================================================
//===================================================================================
int main(int argc, const char* argv[])
{
    init_crc8_table();

    std::srand(static_cast<unsigned int>(std::time(0)));

    s_iniFile.read(ini);

    Comms::RX_Descriptor rx_descriptor;
    rx_descriptor.interfaces = {"wlan1"};

    Comms::TX_Descriptor tx_descriptor;
    tx_descriptor.interface = "wlan1";

    s_hal.reset(new PI_HAL());

    memset( &s_last_airStats, 0, sizeof(AirStats) );

    Ground2Air_Config_Packet& config = s_ground2air_config_packet;
    //config.wifi_rate = WIFI_Rate::RATE_G_24M_ODFM;
    //config.camera.resolution = Resolution::SVGA;
    //config.camera.fps_limit = 30;
    //config.camera.quality = 30;

    s_groundstation_config.stats = false;

    s_ground2air_config_packet.sessionId = (uint16_t)std::rand();

    {
        std::string& temp = ini["gs"]["wifi_channel"];
        int channel = atoi(temp.c_str());
        if ((channel >= 1) && (channel <=13) )
        {
            s_groundstation_config.wifi_channel = channel;
            config.wifi_channel = channel;
        }
        else
        {
            s_groundstation_config.wifi_channel = DEFAULT_WIFI_CHANNEL;
            config.wifi_channel = DEFAULT_WIFI_CHANNEL;
        }
    }

    {
        std::string& temp = ini["gs"]["screen_aspect_ratio"];
        if (temp != "") s_groundstation_config.screenAspectRatio = (ScreenAspectRatio)clamp( atoi(temp.c_str()), 0, 5 );
    }

    {
        std::string& temp = ini["gs"]["vsync"];
        if (temp != "") s_groundstation_config.vsync = atoi(temp.c_str()) !=0 ;
    }

    {
        std::string& temp = ini["gs"]["brightness"];
        if (temp != "") s_ground2air_config_packet.camera.brightness = clamp( atoi(temp.c_str()), -2, 2 );
    }

    {
        std::string& temp = ini["gs"]["contrast"];
        if (temp != "") s_ground2air_config_packet.camera.contrast = clamp( atoi(temp.c_str()), -2, 2 );
    }

    {
        std::string& temp = ini["gs"]["saturation"];
        if (temp != "") s_ground2air_config_packet.camera.saturation = clamp( atoi(temp.c_str()), -2, 2 ); 
    }

    {
        std::string& temp = ini["gs"]["ae_level"] ;
        if (temp != "") s_ground2air_config_packet.camera.ae_level = clamp( atoi(temp.c_str()), -2, 2 );
    }

    {
        std::string& temp = ini["gs"]["sharpness"] ;
        if (temp != "") s_ground2air_config_packet.camera.sharpness = clamp( atoi(temp.c_str()), -3, 3 );
    }

    {
        std::string& temp = ini["gs"]["vflip"];
        if (temp != "") s_ground2air_config_packet.camera.vflip = atoi(temp.c_str()) != 0;
    }

    {
        std::string& temp = ini["gs"]["resolution"];
        if (temp != "") s_ground2air_config_packet.camera.resolution = (Resolution) clamp( atoi(temp.c_str()), (int)Resolution::VGA, (int)Resolution::HD );
    }

    {
        std::string& temp = ini["gs"]["wifi_rate"];
        if (temp != "") s_ground2air_config_packet.wifi_rate = (WIFI_Rate)clamp( atoi(temp.c_str()), (int) WIFI_Rate::RATE_G_12M_ODFM, (int)WIFI_Rate::RATE_N_72M_MCS7_S );
    }

    {
        std::string& temp = ini["gs"]["fec_n"];
        if (temp != "") s_ground2air_config_packet.fec_codec_n = (uint8_t)clamp( atoi(temp.c_str()), FEC_K+1, FEC_N );
    }

    {
        std::string& temp = ini["gs"]["ov2640_high_fps"];
        if (temp != "") s_ground2air_config_packet.camera.ov2640HighFPS = atoi(temp.c_str()) != 0;
    }

    {
        std::string& temp = ini["gs"]["ov5640_high_fps"];
        if (temp != "") s_ground2air_config_packet.camera.ov5640HighFPS = atoi(temp.c_str()) != 0;
    }

    for(int i=1;i<argc;++i){
        auto temp = std::string(argv[i]);
        auto next = i!=argc-1? std::string(argv[i+1]):std::string("");
        auto check_argval = [&next](std::string arg_name)
        {
            if(next==""){throw std::string("please input correct ")+arg_name;}
        };
        
        if(temp=="-tx")
        {
            check_argval("tx");
            tx_descriptor.interface = next; 
            i++;
        }
#ifdef USE_MAVLINK
        else if(temp=="-serial")
        {
            check_argval("serial");
            serialPortName = next; 
            i++;
        }
#endif
        else if(temp=="-p")
        {
            check_argval("port");
            s_groundstation_config.socket_fd=udp_socket_init(std::string("127.0.0.1"),std::stoi(next));
            i++;
        }
        else if(temp=="-n")
        {
            check_argval("n");
            s_ground2air_config_packet.fec_codec_n = (uint8_t)clamp( std::stoi(next), FEC_K+1, FEC_N ); 
            i++;
            LOGI("set rx fec_n to {}",s_ground2air_config_packet.fec_codec_n);
        }
        else if(temp=="-rx")
        {
            rx_descriptor.interfaces.clear();
        }
        else if(temp=="-ch")
        {
            check_argval("ch");
            s_groundstation_config.wifi_channel = std::stoi(next);
            config.wifi_channel = s_groundstation_config.wifi_channel;
            i++;
        }
        else if(temp=="-w")
        {
            check_argval("w");
            s_hal->set_width(std::stoi(next));
            i++;
        }
        else if(temp=="-h")
        {
            check_argval("h");
            s_hal->set_height(std::stoi(next));
            i++;
        }
        else if(temp=="-fullscreen")
        {
            check_argval("fullscreen");
            s_hal->set_fullscreen(std::stoi(next) > 0);
            i++;
        }
        else if(temp=="-vsync")
        {
            check_argval("vsync");
            s_groundstation_config.vsync = std::stoi(next) > 0;
            i++;
        }
        else if(temp=="-sm")
        {
            check_argval("sm");
            rx_descriptor.skip_mon_mode_cfg = std::stoi(next) > 0;
            i++;
        }
        else if(temp=="-help"){
            printf("gs -option val -option val\n");
            printf("-rx <rx_interface1> <rx_interface2>, default: wlan1 single interface\n");
            printf("-tx <tx_interface>, default: wlan1\n");
            printf("-p <gd_ip>, default: disabled\n");
            printf("-n <rx_fec_n>, 7...12, default: 12\n");
            printf("-ch <wifi_channel>, default: 7\n");
            printf("-w <width>, default: 1280\n");
            printf("-h <width>, default: 720\n");
            printf("-fullscreen <1/0>, default: 1\n");
            printf("-vsync <1/0>, default: 1\n");
            printf("-sm <1/0>, skip configuring monitor mode, default: 0\n");
#ifdef USE_MAVLINK
            printf("-serial <serial_port>, serial port for telemetry, default: /dev/serial0\n");
#endif            
            printf("-help\n");
            return 0;
        }
        else
        {
            rx_descriptor.interfaces.push_back(temp);
        }
    }

    rx_descriptor.coding_k = s_ground2air_config_packet.fec_codec_k;
    rx_descriptor.coding_n = 12;//s_ground2air_config_packet.fec_codec_n;
    rx_descriptor.mtu = s_ground2air_config_packet.fec_codec_mtu;

    tx_descriptor.coding_k = 2;
    tx_descriptor.coding_n = 3;
    tx_descriptor.mtu = GROUND2AIR_DATA_MAX_SIZE;

#ifdef USE_MAVLINK
    init_uart();
#endif

#ifdef WRITE_RAW_MJPEG_STREAM
#else
    prepAviBuffers();
#endif

     updateGSSdFreeSpace();

     s_hal->set_vsync(s_groundstation_config.vsync, false);

    if (!s_hal->init())
        return -1;

#ifdef TEST_LATENCY
    gpioSetMode(17, PI_OUTPUT);
#endif

    if (!s_comms.init(rx_descriptor, tx_descriptor))
        return -1;

    s_isDual = rx_descriptor.interfaces.size() > 1;

    s_comms.setChannel( s_groundstation_config.wifi_channel );

    int result = run((char **)argv);

    s_hal->shutdown();

    return result;
}
