#pragma once

#include "esp_wifi_types.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_private/wifi.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "queue.h"

#include "packets.h"
#include "stdint.h"
//#define WIFI_AP

#if defined WIFI_AP
    #define ESP_WIFI_MODE WIFI_MODE_AP
    #define ESP_WIFI_IF WIFI_IF_AP
#else
    #define ESP_WIFI_MODE WIFI_MODE_STA
    #define ESP_WIFI_IF WIFI_IF_STA
#endif

extern SemaphoreHandle_t s_wlan_incoming_mux;
extern SemaphoreHandle_t s_wlan_outgoing_mux;
extern Ground2Air_Data_Packet s_ground2air_data_packet;
extern Ground2Air_Config_Packet s_ground2air_config_packet;
extern Ground2Air_Config_Packet s_ground2air_config_packet2;
extern TaskHandle_t s_wifi_tx_task;
extern TaskHandle_t s_wifi_rx_task;

constexpr size_t WLAN_INCOMING_BUFFER_SIZE = 1024;

//use as much memory as available
//leave ~5k for SD library to initialize correctly
#ifdef BOARD_XIAOS3SENSE
constexpr size_t WLAN_OUTGOING_BUFFER_SIZE = 90000;
#else
constexpr size_t WLAN_OUTGOING_BUFFER_SIZE = 85000;
#endif

void setup_wifi(WIFI_Rate wifi_rate,uint8_t chn,float power_dbm,void (*packet_received_cb)(void* buf, wifi_promiscuous_pkt_type_t type));

void set_ground2air_config_packet_handler(void (*handler)(Ground2Air_Config_Packet& src));
void set_ground2air_connect_packet_handler(void (*handler)(Ground2Air_Config_Packet& src));
void set_ground2air_data_packet_handler(void (*handler)(Ground2Air_Data_Packet& src));
esp_err_t set_wifi_fixed_rate(WIFI_Rate value);
esp_err_t set_wlan_power_dBm(float dBm);
void setup_wifi_file_server(void);
uint8_t getMaxWlanOutgoingQueueUsage();
uint8_t getMinWlanOutgoingQueueUsageSeen();
uint8_t getMaxWlanOutgoingQueueUsageFrame();
uint8_t getMinWlanOutgoingQueueUsageFrame();

//todo: global stats, not wifi only. Move to separate header.
struct Stats
{
    uint32_t wlan_data_sent = 0;
    uint32_t wlan_data_received = 0;
    uint16_t wlan_error_count = 0;
    uint16_t fec_spin_count = 0;
    uint16_t wlan_received_packets_dropped = 0;
    uint32_t video_data = 0;
    uint16_t video_frames = 0;
    //video_frames count is not valid if camera overflow happened. 
    //We still need expected vmode capture fps
    //we estimate it as video_frames + ovf_count
    uint16_t video_frames_expected = 0; 
    uint32_t sd_data = 0;
    uint32_t sd_drops = 0;
    
    uint32_t out_telemetry_data = 0;
    uint32_t in_telemetry_data = 0;

    uint16_t outPacketCounter = 0;
    uint16_t inPacketCounter = 0;
    uint16_t inRejectedPacketCounter = 0;

    uint8_t rssiDbm = 0;
    uint8_t noiseFloorDbm = 0;

    uint16_t camera_frame_size_min;
    uint16_t camera_frame_size_max;

    int RCPeriodMaxMS = -1;

    uint16_t wlan_received_packets_bad = 0;

};

extern Stats s_stats;
extern Stats s_last_stats;
extern uint8_t s_last_stats_id;
extern uint8_t s_wlan_outgoing_queue_usage;

