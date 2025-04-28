#pragma once

#include "structures.h"
#include "fec.h"

#define DEFAULT_WIFI_CHANNEL 7

#define FW_VERSION "0.3"

#define FEC_K 6
#define FEC_N 12

constexpr size_t AIR2GROUND_MTU = WLAN_MAX_PAYLOAD_SIZE - PACKET_OVERHEAD; //6 is the fec header size
constexpr size_t GROUND2AIR_DATA_MAX_SIZE = 64;

#pragma pack(push, 1) // exact fit - no padding

//======================================================
//======================================================
enum class WIFI_Rate : uint8_t
{
    /*  0 */ RATE_B_2M_CCK,
    /*  1 */ RATE_B_2M_CCK_S,
    /*  2 */ RATE_B_5_5M_CCK,
    /*  3 */ RATE_B_5_5M_CCK_S,
    /*  4 */ RATE_B_11M_CCK,
    /*  5 */ RATE_B_11M_CCK_S,

    /*  6 */ RATE_G_6M_ODFM,
    /*  7 */ RATE_G_9M_ODFM,
    /*  8 */ RATE_G_12M_ODFM,
    /*  9 */ RATE_G_18M_ODFM,
    /* 10 */ RATE_G_24M_ODFM,
    /* 11 */ RATE_G_36M_ODFM,
    /* 12 */ RATE_G_48M_ODFM,
    /* 13 */ RATE_G_54M_ODFM,

    /* 14 */ RATE_N_6_5M_MCS0,
    /* 15 */ RATE_N_7_2M_MCS0_S,
    /* 16 */ RATE_N_13M_MCS1,
    /* 17 */ RATE_N_14_4M_MCS1_S,
    /* 18 */ RATE_N_19_5M_MCS2,
    /* 19 */ RATE_N_21_7M_MCS2_S,
    /* 20 */ RATE_N_26M_MCS3,
    /* 21 */ RATE_N_28_9M_MCS3_S,
    /* 22 */ RATE_N_39M_MCS4,
    /* 23 */ RATE_N_43_3M_MCS4_S,
    /* 24 */ RATE_N_52M_MCS5,
    /* 25 */ RATE_N_57_8M_MCS5_S,
    /* 26 */ RATE_N_58M_MCS6,
    /* 27 */ RATE_N_65M_MCS6_S,
    /* 28 */ RATE_N_65M_MCS7,
    /* 29 */ RATE_N_72M_MCS7_S,
};

#define DEFAULT_WIFI_RATE WIFI_Rate::RATE_N_26M_MCS3

//======================================================
//======================================================
enum class Resolution : uint8_t
{
    QVGA,   //320x240
    CIF,    //400x296
    HVGA,   //480x320
    VGA,    //640x480
    VGA16,    //640x360
    SVGA,   //800x600
    SVGA16,  //800x456
    XGA,    //1024x768
    XGA16,    //1024x576
    SXGA,   //1280x960
    HD,   //1280x720
    UXGA,   //1600x1200
    COUNT
};

//======================================================
//======================================================
typedef struct 
{
    uint16_t width;
    uint16_t height;
    uint8_t FPS2640;
    uint8_t FPS5640;
    uint8_t highFPS2640;
    uint8_t highFPS5640;
} TVMode;

extern TVMode vmodes[];

//======================================================
//======================================================
//Description of some settings:
//https://heyrick.eu/blog/index.php?diary=20210418&keitai=0
struct CameraConfig
{
    Resolution resolution = Resolution::SVGA;
    uint8_t fps_limit = 60;
    uint8_t quality = 0;//0 - 63  0-auto
    int8_t brightness = 0;//-2 - 2
    int8_t contrast = 0;//-2 - 2
    int8_t saturation = 1;//-2 - 2
    int8_t sharpness = 0;//-2 - 3
    uint8_t denoise = 0;  //0..8, ov5640 only
    uint8_t special_effect = 0;//0 - 6
    bool awb = true;
    bool awb_gain = true;
    uint8_t wb_mode = 0;//0 - 4
    bool aec = true; //automatic exposure control
    bool aec2 = true; //enable aec DSP (better processing?). "Nigth mode" for ov5640.
    int8_t ae_level = 1;//-2 - 2, for aec=true
    uint16_t aec_value = 204;//0 - 1200 ISO, for aec=false
    bool agc = true;  //automatic gain control
    uint8_t agc_gain = 0;//30 - 6, for agc=false
    uint8_t gainceiling = 0;//0 - 6, for agc=true. 0=2x, 1=4x, 2=8x,3=16x,4=32x,5=64x,6=128x
    bool bpc = true;
    bool wpc = true;
    bool raw_gma = true;
    bool lenc = true;
    bool hmirror = false;
    bool vflip = false;
    bool dcw = true;
    bool ov2640HighFPS = false;
    bool ov5640HighFPS = false;
    bool ov5640NightMode = false;
};

//======================================================
//======================================================
struct DataChannelConfig
{
    int8_t wifi_power = 20;//dBm
    WIFI_Rate wifi_rate = DEFAULT_WIFI_RATE;
    uint8_t wifi_channel = DEFAULT_WIFI_CHANNEL;
    uint8_t fec_codec_k = FEC_K;
    uint8_t fec_codec_n = FEC_N;
    uint16_t fec_codec_mtu = AIR2GROUND_MTU;
    uint8_t air_record_btn = 0; //incremented each time button is pressed on gs
    uint8_t profile1_btn = 0; //incremented each time button is pressed on gs
    uint8_t profile2_btn = 0; //incremented each time button is pressed on gs
};

//======================================================
//======================================================
struct Ground2Air_Header
{
    enum class Type : uint8_t
    {
        Telemetry,
        Config,
        Connect  //Packet is sent to initialize connection. GS will send this packet instead of Config Packet untill any packet with GS id is received from Air unit
    };

    Type type = Type::Telemetry; 
    uint32_t size = 0;
    uint8_t crc = 0;
    uint8_t packet_version = PACKET_VERSION;
    uint16_t airDeviceId; //unique id of target AIR unit. 
    uint16_t gsDeviceId;  //ID of GS. Assigned permanently on first boot.

};

//======================================================
//======================================================
struct Ground2Air_Connect_Packet : Ground2Air_Header
{
};

constexpr size_t  GROUND2AIR_DATA_MAX_PAYLOAD_SIZE = GROUND2AIR_DATA_MAX_SIZE - sizeof(Ground2Air_Header);

//======================================================
//======================================================
struct Ground2Air_Data_Packet : Ground2Air_Header
{
    uint8_t payload[GROUND2AIR_DATA_MAX_PAYLOAD_SIZE];
};
static_assert(sizeof(Ground2Air_Data_Packet) <= GROUND2AIR_DATA_MAX_SIZE, "");

//======================================================
//======================================================
struct Ground2Air_Config_Packet : Ground2Air_Header
{
    uint8_t ping = 0; //used for latency measurement

    CameraConfig camera;
    DataChannelConfig dataChannel;
};
static_assert(sizeof(Ground2Air_Config_Packet) <= GROUND2AIR_DATA_MAX_SIZE, "");

//======================================================
//======================================================
struct Air2Ground_Header
{
    enum class Type : uint8_t
    {
        Video,
        Telemetry,
        OSD,
        Config
    };

    Type type = Type::Video; 
    uint32_t size = 0;
    uint8_t pong = 0; //used for latency measurement
    uint8_t version; //PACKET_VERSION
    uint8_t crc = 0;
    uint16_t airDeviceId; //unique id of this AIR unit. Assigned permanently on first boot.
    uint16_t gsDeviceId;  //ID of GS this unit is connected to cuurently. 0 - not connected currently. Will accept 
};

//======================================================
//======================================================
struct Air2Ground_Config_Packet : Air2Ground_Header
{
    CameraConfig camera;
    DataChannelConfig dataChannel;
};

static_assert(sizeof(Air2Ground_Config_Packet) <= AIR2GROUND_MTU, "");

//======================================================
//======================================================
struct Air2Ground_Video_Packet : Air2Ground_Header
{
    Resolution resolution;
    uint8_t part_index : 7;
    uint8_t last_part : 1;
    uint32_t frame_index = 0;
    //data follows
};

static_assert(sizeof(Air2Ground_Video_Packet) == 18, "");

//======================================================
//======================================================
struct Air2Ground_Data_Packet : Air2Ground_Header
{
};


#define OSD_COLS 53
#define OSD_COLS_H 7 //56 bits
#define OSD_ROWS 20

#define OSD_BUFFER_SIZE (OSD_ROWS*OSD_COLS + OSD_ROWS*OSD_COLS_H)

//======================================================
//======================================================
struct OSDBuffer
{
    uint8_t screenLow[OSD_ROWS][OSD_COLS];
    uint8_t screenHigh[OSD_ROWS][OSD_COLS_H];
};

//======================================================
//======================================================
struct AirStats
{
    uint8_t SDDetected : 1;
    uint8_t SDSlow : 1;
    uint8_t SDError : 1;
    uint8_t curr_wifi_rate :5; //WIFI_Rate
//1
    uint8_t wifi_queue_min : 7;
    uint8_t air_record_state : 1;
//2
    uint8_t wifi_queue_max;
//3
    uint32_t SDFreeSpaceGB16 : 12;
    uint32_t SDTotalSpaceGB16 : 12;
    uint32_t curr_quality : 6;
    uint32_t wifi_ovf : 1;
    uint32_t isOV5640 : 1;
//7
    uint16_t outPacketRate;
    uint16_t inPacketRate;
    uint16_t inRejectedPacketRate;
    uint8_t rssiDbm;  //positive value in dbm
    uint8_t noiseFloorDbm; //positive value in dbm
    uint8_t captureFPS;
    uint8_t cam_ovf_count;
    uint16_t cam_frame_size_min; //bytes
    uint16_t cam_frame_size_max; //bytes
    uint16_t inMavlinkRate; //b/s
    uint16_t outMavlinkRate; //b/s
//25
    uint8_t RCPeriodMax;  //0 - no RC Packets detected, <1...100 - value in ms, 101...254 - period = (v - 101) * 10
//26
    uint8_t wifiChannel : 4; //1...14
    uint8_t resolution : 4;
//27
    uint8_t temperature : 7;  //degree C. 0 - does not have temp sensor.
    uint8_t overheatTrottling : 1;
//28
    uint8_t suspend_ch : 5; //0 - disabled, 1...18
    uint8_t highFps: 1;
    uint8_t mavlink2mspRC : 1;
    uint8_t suspended : 1;
//29
    uint8_t fec_codec_k : 4;
    uint8_t in_session : 1; //1 if camera is currently actively communicating with some GS. Used in search.
    uint8_t screenAspectRatio: 3;  
//30
    int16_t brightness : 3;  //-2 - 2
    int16_t contrast : 3;    //-2 - 2
    int16_t saturation : 3;  //-2 - 2
    int16_t sharpness : 3;   //-2 - 3
    int16_t ae_level: 3;     //-2 - 2, for aec=true
    int16_t reserved: 1;  //-1...0
//32

//..39
};

//======================================================
//======================================================
struct Air2Ground_OSD_Packet : Air2Ground_Header
{
    AirStats stats;
    OSDBuffer buffer;
};

static_assert(sizeof(Air2Ground_OSD_Packet) <= AIR2GROUND_MTU, "");

#pragma pack(pop)

