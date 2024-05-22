#include "main.h"

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

#include "socket.h"

#include "ini.h"

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
    "1280x1024",
    "1280x720",
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

    "MCS0_6.5M_L",
    "MCS0_7.2M_S",
    "MCS1_13M_L",
    "MCS1_14.4M_S",
    "MCS2_19.5M_L",
    "MCS2_21.7M_S",
    "MCS3_26.3M_L",
    "MCS3_28.9M_S",
    "MCS4_39M_L",
    "MCS4_43.3M_S",
    "MCS5_52M_L",

    "MCS5_57M_S",
    "MCS6_58M_L",
    "MCS6_65M_S",
    "MCS7_65_L",
    "MCS7_72_S"
};

std::unique_ptr<IHAL> s_hal;
Comms s_comms;
Video_Decoder s_decoder;

#ifdef USE_MAVLINK
int fdUART;
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
int s_wifi_queue = 0;
uint8_t s_curr_quality = 0;
bool bRestart = false;
bool bRestartRequired = false;
Clock::time_point restart_tp;
uint16_t s_SDTotalSpaceGB16 = 0;
uint16_t s_SDFreeSpaceGB16 = 0;
bool s_air_record = false;
bool s_SDDetected = false;
bool s_SDSlow = false;
bool s_SDError = false;
bool s_isOV5640 = false;

bool s_debugWindowVisisble = false;

Stats s_frame_stats;
Stats s_frameParts_stats;
Stats s_frameTime_stats;
Stats s_frameQuality_stats;
Stats s_dataSize_stats;
Stats s_queueUsage_stats;

OSD g_osd;

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
            if (ping_count == 0)
            {
                ping_count = 0;
                ping_min = std::chrono::seconds(0);
                ping_max = std::chrono::seconds(0);
                ping_avg = std::chrono::seconds(0);
            }

            LOGI("Sent: {}, RX len: {}, TlmIn: {}, TlmOut: {}, RSSI: {}, Latency: {}/{}/{},vfps:{}", sent_count, total_data, in_tlm_size, out_tlm_size, min_rssi, 
                std::chrono::duration_cast<std::chrono::milliseconds>(ping_min).count(),
                std::chrono::duration_cast<std::chrono::milliseconds>(ping_max).count(),
                std::chrono::duration_cast<std::chrono::milliseconds>(ping_avg).count() / ping_count,
                video_fps);

            s_min_rssi = min_rssi;
            s_total_data = total_data;

            ping_min = std::chrono::seconds(999);
            ping_max = std::chrono::seconds(0);
            ping_avg = std::chrono::seconds(0);
            sent_count = 0;
            in_tlm_size = 0;
            out_tlm_size = 0;
            ping_count = 0;
            total_data = 0;
            min_rssi = 0;
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

            rx_data.rssi = (int16_t)s_comms.get_input_dBm();

            //filter bad packets
            Air2Ground_Header& air2ground_header = *(Air2Ground_Header*)rx_data.data.data();

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
                        s_queueUsage_stats.add(s_wifi_queue);
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
                            std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);
                            fwrite(video_frame.data(),video_frame.size(),1,s_groundstation_config.record_file);
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
                total_data += rx_data.size;
                total_data10 += rx_data.size;

                s_curr_wifi_rate = (WIFI_Rate)air2ground_osd_packet.curr_wifi_rate;
                s_curr_quality = air2ground_osd_packet.curr_quality;
                s_wifi_queue = air2ground_osd_packet.wifi_queue;
                s_SDTotalSpaceGB16 = air2ground_osd_packet.SDTotalSpaceGB16;
                s_SDFreeSpaceGB16 = air2ground_osd_packet.SDFreeSpaceGB16;
                s_air_record = air2ground_osd_packet.air_record_state != 0;
                s_SDDetected = air2ground_osd_packet.SDDetected != 0;
                s_SDError = air2ground_osd_packet.SDError != 0;
                s_SDSlow = air2ground_osd_packet.SDSlow != 0;
                s_isOV5640 = air2ground_osd_packet.isOV5640 != 0;

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
    
    if ( 
        (s_groundstation_config.screenAspectRatio == ScreenAspectRatio::STRETCH) || 
        (videoAspect16x9 == (s_groundstation_config.screenAspectRatio == ScreenAspectRatio::ASPECT16X9)) 
    )
    {
        //no scale or stretch
        x1 = 0; y1 = 0; x2 = width; y2 = height;
    }
    else if ( videoAspect16x9 )
    {
        //16x9 on 4x3 screen
        //16/9   16/12
        //screen = 12 image 9
        int h1 = height * 9 / 12;
        x1 = 0; y1 = (height - h1) / 2; x2 = width; y2 = y1 + h1;
    }
    else
    {
        //4x3 on 16x9 screen
        //12/9   16/9   
        //screen = 16 image 12
        int w1 = width * 12 / 16;
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
void toggleGSRecording()
{
    s_groundstation_config.record = !s_groundstation_config.record;

    std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);
    if(s_groundstation_config.record)
    {
        auto time=std::time({});
        char filename[]="yyyy-mm-dd-hh:mm:ss.mjpeg";
        std::strftime(filename, sizeof(filename), "%F-%T.mjpeg", std::localtime(&time));
        s_groundstation_config.record_file=fopen(filename,"wb+");

        LOGI("start record:{}",std::string(filename));
    }
    else
    {
        fflush(s_groundstation_config.record_file);
        fclose(s_groundstation_config.record_file);
        s_groundstation_config.record_file=nullptr;
    }
}

//===================================================================================
//===================================================================================
void exitApp()
{
    if (s_groundstation_config.record)
    {
        std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);
        fflush(s_groundstation_config.record_file);
        fclose(s_groundstation_config.record_file); 
    }
    abort();
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

    g_osd.init();

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
            if ( s_air_record )
            {
                //AIR REC
                ImGui::PushID(0);
                ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0 / 7.0f, 0.6f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.7f, 0.7f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.8f, 0.8f));
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
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0 / 7.0f, 0.7f, 0.7f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0 / 7.0f, 0.8f, 0.8f));
                ImGui::Button("GS");
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

                sprintf(overlay, "%d%%", (int)(s_wifi_queue));
                ImGui::PlotHistogram("Wifi Load", Stats::getter, &s_queueUsage_stats, s_queueUsage_stats.count(), 0, overlay, 0, 100.0f, ImVec2(0, 60));

                ImGui::PopItemWidth();
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
            sprintf(buf, "RSSI:%d FPS:%1.0f/%d %dKB/S %s %d%% AQ:%d %s/%s###HAL", 
            s_min_rssi, video_fps, s_lost_frame_count, 
            s_total_data/1024, 
            resolutionName[(int)config.camera.resolution], 
            s_wifi_queue,
            s_curr_quality,
            rateName[(int)s_curr_wifi_rate], rateName[(int)config.wifi_rate]);

            ImGui::SetNextWindowCollapsed(true, ImGuiCond_Once); 
            ImGui::Begin(buf);
            {
                {
                    int value = config.wifi_power;
                    ImGui::SliderInt("WIFI Power", &value, 0, 20); 
                    config.wifi_power = value;
                }
                {
                    int value = (int)config.wifi_rate;
                    ImGui::SliderInt("WIFI Rate", &value, (int)WIFI_Rate::RATE_B_2M_CCK, (int)WIFI_Rate::RATE_N_72M_MCS7_S);
                    if (config.wifi_rate != (WIFI_Rate)value) 
                    {
                        config.wifi_rate = (WIFI_Rate)value;
                        saveGround2AirConfig(config);
                    }
                }
                {
                    int ch = s_groundstation_config.wifi_channel;
                    ImGui::SliderInt("WIFI Channel", &s_groundstation_config.wifi_channel, 1, 13);
                    if ( ch != s_groundstation_config.wifi_channel)
                    {
                        saveGroundStationConfig();
                        bRestartRequired = true;
                    }
                }
                {
                    int value = config.fec_codec_n;
                    ImGui::SliderInt("FEC_N", &value,FEC_K+1, FEC_N);
                    if (config.fec_codec_n != (int8_t)value)
                    {
                        config.fec_codec_n = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }
                {
                    int value = (int)config.camera.resolution;
                    ImGui::SliderInt("Resolution", &value, 0, 10);
                    if ( config.camera.resolution != (Resolution)value )
                    {
                        config.camera.resolution = (Resolution)value;
                        saveGround2AirConfig(config);
                    }
                }
                {
                    int value = (int)config.camera.fps_limit;
                    ImGui::SliderInt("FPS Limit", &value, 0, 100);
                    config.camera.fps_limit = (uint8_t)value;
                }
                {
                    int value = config.camera.quality;
                    ImGui::SliderInt("Quality(0-auto)", &value, 0, 63);
                    config.camera.quality = value;
                }

                ImGui::Checkbox("AGC", &config.camera.agc);
                ImGui::SameLine();            
                ImGui::Checkbox("AEC", &config.camera.aec);
                if ( config.camera.aec )
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
                    ImGui::SliderInt("AGC Gain", &value, 0, 30);
                    config.camera.agc_gain = (int8_t)value;
                }
                else 
                {
                    int value = config.camera.gainceiling;
                    ImGui::SliderInt("GainCeiling", &value, 0, 6);
                    config.camera.gainceiling = (uint8_t)value;
                }

                if ( config.camera.aec )
                {
                    int value = config.camera.ae_level;
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
                    ImGui::SliderInt("AEC Value", &value, 0, 1200);
                    config.camera.aec_value = (uint16_t)value;
                }

                {
                    int value = config.camera.brightness;
                    ImGui::SliderInt("Brightness", &value, -2, 2);
                    if (config.camera.brightness != (int8_t)value)
                    {
                        config.camera.brightness = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }

                {
                    int value = config.camera.contrast;
                    ImGui::SliderInt("Contrast", &value, -2, 2);
                    if (config.camera.contrast != (int8_t)value)
                    {
                        config.camera.contrast = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }

                {
                    int value = config.camera.saturation;
                    ImGui::SliderInt("Saturation", &value, -2, 2);
                    if (config.camera.saturation != (int8_t)value)
                    {
                        config.camera.saturation = (int8_t)value;
                        saveGround2AirConfig(config);
                    }
                }

                {
                    int value = config.camera.sharpness;
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
                    ImGui::SliderInt("Letterbox", &ch, 0, 2);
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

                    ImGui::Checkbox("Stats", &s_groundstation_config.stats);

                    if ( ImGui::Button("Air Record") )
                    {
                        config.air_record_btn++;
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("GS Record"))
                    {
                        toggleGSRecording();
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Restart"))
                {
                    //send channel change command to receiver, then restart
                    restart_tp = Clock::now();
                    bRestart = true;
                }

                ImGui::SameLine();
                if (ImGui::Button("Exit"))
                {
                    exitApp();
                }
                
                ImGui::Text("%.3f ms/frame (%.1f FPS) %.1f VFPS DBG:%d", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate, video_fps, s_dbg);
                ImGui::Text("AIR SD Card: %s%s%s %.2fGB/%.2fGB %s", 
                    s_SDDetected ? "Detected" : "Not detected", s_SDError ? " Error" :"",  s_SDSlow ? " Slow" : "",
                    s_SDFreeSpaceGB16 / 16.0f, s_SDTotalSpaceGB16 / 16.0f,
                    s_isOV5640 ? "OV5640" : "OV2640");
            }
            ImGui::End();
        } //debug window
        
        if ( ImGui::IsKeyPressed(ImGuiKey_D) || ImGui::IsKeyPressed(ImGuiKey_MouseMiddle))
        {
            s_debugWindowVisisble = !s_debugWindowVisisble;
        }

        if ( !ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && (config.camera.resolution > Resolution::VGA))
        {
            config.camera.resolution = (Resolution)((int)config.camera.resolution - 1);
            if ( config.camera.resolution == Resolution::XGA ) config.camera.resolution = Resolution::SVGA16;
            saveGround2AirConfig(config);
        }
        if ( !ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_RightArrow) && (config.camera.resolution < Resolution::HD))
        {
            config.camera.resolution = (Resolution)((int)config.camera.resolution + 1);
            if ( config.camera.resolution == Resolution::XGA ) config.camera.resolution = Resolution::SXGA;
            saveGround2AirConfig(config);
        }

        if ( !ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_R))
        {
            config.air_record_btn++;
        }

        if (!ignoreKeys &&  ImGui::IsKeyPressed(ImGuiKey_G))
        {
            toggleGSRecording();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Space) || (!ignoreKeys && ImGui::IsKeyPressed(ImGuiKey_Escape)))
        {
            exitApp();
        }

        if ( bRestart ) 
        {
            //start sending new channel to air, restart after 3 seconds
            config.wifi_channel = s_groundstation_config.wifi_channel;
            if (Clock::now() - restart_tp >= std::chrono::milliseconds(3000)) 
            {
                bRestart = false;
                execv(argv[0],argv);
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
    fdUART = open("/dev/serial0", O_RDWR);

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
    s_iniFile.write(ini);
}

//===================================================================================
//===================================================================================
int main(int argc, const char* argv[])
{
    init_crc8_table();

    s_iniFile.read(ini);

    Comms::RX_Descriptor rx_descriptor;
    rx_descriptor.interfaces = {"wlan1mon"};

    Comms::TX_Descriptor tx_descriptor;
    tx_descriptor.interface = "wlan1mon";

    s_hal.reset(new PI_HAL());

    Ground2Air_Config_Packet& config = s_ground2air_config_packet;
    //config.wifi_rate = WIFI_Rate::RATE_G_24M_ODFM;
    //config.camera.resolution = Resolution::SVGA;
    //config.camera.fps_limit = 30;
    //config.camera.quality = 30;

    s_groundstation_config.stats = false;

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
        if (temp != "") s_groundstation_config.screenAspectRatio = (ScreenAspectRatio)clamp( atoi(temp.c_str()), 0, 2 );
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
        if (temp != "") s_ground2air_config_packet.wifi_rate = (WIFI_Rate)clamp( atoi(temp.c_str()), (int) WIFI_Rate::RATE_G_12M_ODFM, (int)WIFI_Rate::RATE_G_36M_ODFM );
    }

    {
        std::string& temp = ini["gs"]["fec_n"];
        if (temp != "") s_ground2air_config_packet.fec_codec_n = (uint8_t)clamp( atoi(temp.c_str()), FEC_K+1, FEC_N );
    }

    for(int i=1;i<argc;++i){
        auto temp = std::string(argv[i]);
        auto next = i!=argc-1? std::string(argv[i+1]):std::string("");
        auto check_argval = [&next](std::string arg_name){
            if(next==""){throw std::string("please input correct ")+arg_name;}
        };
        if(temp=="-tx"){
            check_argval("tx");
            tx_descriptor.interface = next; 
            i++;
        }else if(temp=="-p"){
            check_argval("port");
            s_groundstation_config.socket_fd=udp_socket_init(std::string("127.0.0.1"),std::stoi(next));
            i++;
        }else if(temp=="-n"){
            check_argval("n");
            s_ground2air_config_packet.fec_codec_n = (uint8_t)clamp( std::stoi(next), FEC_K+1, FEC_N ); 
            i++;
            LOGI("set rx fec_n to {}",s_ground2air_config_packet.fec_codec_n);
        }else if(temp=="-rx"){
            rx_descriptor.interfaces.clear();
        }else if(temp=="-ch"){
            check_argval("ch");
            s_groundstation_config.wifi_channel = std::stoi(next);
            config.wifi_channel = s_groundstation_config.wifi_channel;
            i++;
        }else if(temp=="-w"){
            check_argval("w");
            s_hal->set_width(std::stoi(next));
            i++;
        }else if(temp=="-h"){
            check_argval("h");
            s_hal->set_height(std::stoi(next));
            i++;
        }else if(temp=="-fullscreen"){
            check_argval("fullscreen");
            s_hal->set_fullscreen(std::stoi(next) > 0);
            i++;
        }else if(temp=="-vsync"){
            check_argval("vsync");
            s_hal->set_vsync(std::stoi(next) > 0);
            i++;
        }else if(temp=="-sm"){
            check_argval("sm");
            rx_descriptor.skip_mon_mode_cfg = std::stoi(next) > 0;
            i++;
        }else if(temp=="-help"){
            printf("gs -option val -option val\n");
            printf("-rx <rx_interface1> <rx_interface2>, default: wlan0mon single interface\n");
            printf("-tx <tx_interface>, default: wlan0mon\n");
            printf("-p <gd_ip>, default: disabled\n");
            printf("-n <rx_fec_n>, 7...12, default: 12\n");
            printf("-ch <wifi_channel>, default: 7\n");
            printf("-w <width>, default: 1280\n");
            printf("-h <width>, default: 720\n");
            printf("-fullscreen <1/0>, default: 1\n");
            printf("-vsync <1/0>, default: 1\n");
            printf("-sm <1/0>, skip configuring monitor mode, default: 0\n");
            printf("-help\n");
            return 0;
        }else{
            rx_descriptor.interfaces.push_back(temp);
        }
    }

    rx_descriptor.coding_k = s_ground2air_config_packet.fec_codec_k;
    rx_descriptor.coding_n = s_ground2air_config_packet.fec_codec_n;
    rx_descriptor.mtu = s_ground2air_config_packet.fec_codec_mtu;

    tx_descriptor.coding_k = 2;
    tx_descriptor.coding_n = 3;
    tx_descriptor.mtu = GROUND2AIR_DATA_MAX_SIZE;

#ifdef USE_MAVLINK
    if ( !init_uart())
    {
        return -1;
    }
#endif    

    if (!s_hal->init())
        return -1;

#ifdef TEST_LATENCY
    gpioSetMode(17, PI_OUTPUT);
#endif

    if (!s_comms.init(rx_descriptor, tx_descriptor))
        return -1;

    for (const auto& itf: rx_descriptor.interfaces)
    {
        system(fmt::format("iwconfig {} channel {}", itf, s_groundstation_config.wifi_channel).c_str());
    }

    int result = run((char **)argv);

    s_hal->shutdown();

    return result;
}
