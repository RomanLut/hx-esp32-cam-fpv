#include "main.h"

#include "wifi.h"
#include "esp_log.h"
#include "structures.h"
#include "fec_codec.h"
#include "crc.h"
#include "lwip/inet.h"

#include "vcd_profiler.h"

static const char * TAG="wifi_task";

#define TX_COMPLETION_CB

#define LOG(...) do { if (s_uart_verbose > 0) SAFE_PRINTF(__VA_ARGS__); } while (false) 

Ground2Air_Data_Packet s_ground2air_data_packet;
Ground2Air_Config_Packet s_ground2air_config_packet;  
Ground2Air_Config_Packet s_ground2air_config_packet2;  

SemaphoreHandle_t s_wlan_incoming_mux = xSemaphoreCreateBinary();
SemaphoreHandle_t s_wlan_outgoing_mux = xSemaphoreCreateBinary();

#ifdef TX_COMPLETION_CB
SemaphoreHandle_t s_wifi_tx_done_semaphore = xSemaphoreCreateBinary();
#endif

TaskHandle_t s_wifi_tx_task = nullptr;
TaskHandle_t s_wifi_rx_task = nullptr;
Stats s_stats;
Stats s_last_stats;
uint8_t s_wlan_outgoing_queue_usage = 0;

static void (*ground2air_config_packet_handler)(Ground2Air_Config_Packet& src)=nullptr;
static void (*ground2air_data_packet_handler)(Ground2Air_Data_Packet& src)=nullptr;
WIFI_Rate s_wlan_rate = s_ground2air_config_packet.dataChannel.wifi_rate;
float s_wlan_power_dBm = s_ground2air_config_packet.dataChannel.wifi_power;

//===========================================================================================
//===========================================================================================
void set_ground2air_config_packet_handler(void (*handler)(Ground2Air_Config_Packet& src))
{
    ground2air_config_packet_handler = handler;
}

//===========================================================================================
//===========================================================================================
void set_ground2air_data_packet_handler(void (*handler)(Ground2Air_Data_Packet& src))
{
    ground2air_data_packet_handler=handler;
}


//===========================================================================================
//===========================================================================================
IRAM_ATTR void add_to_wlan_incoming_queue(const void* data, size_t size)
{
    Wlan_Incoming_Packet packet;

    xSemaphoreTake(s_wlan_incoming_mux, portMAX_DELAY);
    start_writing_wlan_incoming_packet(packet, size);

    if (packet.ptr)
        memcpy(packet.ptr, data, size);

    //LOG("Sending packet of size %d\n", packet.size);

    end_writing_wlan_incoming_packet(packet);
    xSemaphoreGive(s_wlan_incoming_mux);

    if (s_wifi_rx_task)
        xTaskNotifyGive(s_wifi_rx_task); //notify task
}

//===========================================================================================
//===========================================================================================
IRAM_ATTR bool add_to_wlan_outgoing_queue(const void* data, size_t size)
{
    if (s_ground2air_config_packet.dataChannel.wifi_power == 0) return true;

    bool res = true;

    Wlan_Outgoing_Packet packet;

    xSemaphoreTake(s_wlan_outgoing_mux, portMAX_DELAY);
    start_writing_wlan_outgoing_packet(packet, size);

    if (packet.ptr)
    {
        memcpy(packet.payload_ptr, data, size);
        //LOG("Sending packet of size %d\n", packet.size);
    }
    else
    {
        res = false;
    }

    end_writing_wlan_outgoing_packet(packet);

    size_t qs = s_wlan_outgoing_queue.size();
    size_t c = s_wlan_outgoing_queue.capacity();
    s_wlan_outgoing_queue_usage = qs * 100 / c;

#ifdef PROFILE_CAMERA_DATA    
    s_profiler.set(PF_CAMERA_WIFI_QUEUE, qs / 1024);
#endif

    if ( s_max_wlan_outgoing_queue_size_frame < qs )
    {
        s_max_wlan_outgoing_queue_size_frame = qs;
    }

    xSemaphoreGive(s_wlan_outgoing_mux);

    //xSemaphoreGive(s_wifi_semaphore);
    if (s_wifi_tx_task)
        xTaskNotifyGive(s_wifi_tx_task); //notify task
    //LOG("gave semaphore\n");

    return res;
}

inline bool init_queues(size_t wlan_incoming_queue_size, size_t wlan_outgoing_queue_size)
{
  //SPI RAM is too slow, can not handle more then 2Mb/s bandwidth
  //s_wlan_outgoing_queue.init(new uint8_t[wlan_outgoing_queue_size], wlan_outgoing_queue_size);
  s_wlan_outgoing_queue.init( (uint8_t*)heap_caps_malloc(wlan_outgoing_queue_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL ),wlan_outgoing_queue_size );
  //s_wlan_outgoing_queue.init( (uint8_t*)heap_caps_malloc(wlan_outgoing_queue_size, MALLOC_CAP_SPIRAM ),wlan_outgoing_queue_size*4 );

  s_wlan_incoming_queue.init(new uint8_t[wlan_incoming_queue_size], wlan_incoming_queue_size);

  return true;
}

IRAM_ATTR static void wifi_tx_proc(void *)
{
    Wlan_Outgoing_Packet packet;

    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); //wait for notification
        //xSemaphoreTake(s_wifi_semaphore, portMAX_DELAY);

        //LOG("received semaphore\n");

        while (true)
        {
            //send pending wlan packets
            xSemaphoreTake(s_wlan_outgoing_mux, portMAX_DELAY);

            start_reading_wlan_outgoing_packet(packet);
            xSemaphoreGive(s_wlan_outgoing_mux);

            if (packet.ptr)
            {
                memcpy(packet.ptr, WLAN_IEEE_HEADER_AIR2GROUND, WLAN_IEEE_HEADER_SIZE);

                size_t spins = isHQDVRMode() ? 10000 : 0;
                while (packet.ptr)
                {
#ifdef PROFILE_CAMERA_DATA    
                    s_profiler.set(PF_CAMERA_WIFI_TX,1);
#endif

#ifdef TX_COMPLETION_CB                    
                    xSemaphoreTake(s_wifi_tx_done_semaphore, 0); //clear the notif
#endif

                    esp_err_t res = esp_wifi_80211_tx(ESP_WIFI_IF, packet.ptr, WLAN_IEEE_HEADER_SIZE + packet.size, false);
                    if (res == ESP_OK)
                    {
                        s_stats.wlan_data_sent += packet.size;
                        s_stats.outPacketCounter++;

                        xSemaphoreTake(s_wlan_outgoing_mux, portMAX_DELAY);
                        end_reading_wlan_outgoing_packet(packet);

                        size_t qs = s_wlan_outgoing_queue.size();
                        size_t c = s_wlan_outgoing_queue.capacity();
                        s_wlan_outgoing_queue_usage = qs * 100 / c;

#ifdef PROFILE_CAMERA_DATA    
                        s_profiler.set(PF_CAMERA_WIFI_QUEUE, qs / 1024);
#endif
                        if ( (s_min_wlan_outgoing_queue_size_frame == -1) || ( s_min_wlan_outgoing_queue_size_frame > qs ) )
                        {
                            s_min_wlan_outgoing_queue_size_frame = qs;
                        }

                        xSemaphoreGive(s_wlan_outgoing_mux);

#ifdef TX_COMPLETION_CB
                        xSemaphoreTake(s_wifi_tx_done_semaphore, portMAX_DELAY); //wait for the tx_done notification
#endif

#ifdef PROFILE_CAMERA_DATA    
                        s_profiler.set(PF_CAMERA_WIFI_TX,0);
#endif

                    }
                    else if (res == ESP_ERR_NO_MEM) //No TX buffers available, need to poll.
                    {

#ifdef PROFILE_CAMERA_DATA    
                        s_profiler.set(PF_CAMERA_WIFI_SPIN,1);
#endif
                        //s_stats.wlan_error_count++;
                        spins++;
                        if (spins > 1000)
                            vTaskDelay(1);
                        else
                            taskYIELD();
#ifdef PROFILE_CAMERA_DATA    
                        s_profiler.set(PF_CAMERA_WIFI_SPIN,0);
#endif
                    }
                    else //other errors
                    {
                        //ESP_LOGE(TAG,"Wlan err: %d\n", res);
                        s_stats.wlan_error_count++;
#ifdef PROFILE_CAMERA_DATA    
    s_profiler.toggle(PF_CAMERA_WIFI_OVF);
#endif
                        xSemaphoreTake(s_wlan_outgoing_mux, portMAX_DELAY);
                        end_reading_wlan_outgoing_packet(packet);
                        xSemaphoreGive(s_wlan_outgoing_mux);
                    }
                }
            }
            else
                break;
        }
    }
}

IRAM_ATTR static void wifi_rx_proc(void *)
{
    Wlan_Incoming_Packet packet;

    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); //wait for notification

        //LOG("received semaphore\n");

        while (true)
        {
            xSemaphoreTake(s_wlan_incoming_mux, portMAX_DELAY);
            start_reading_wlan_incoming_packet(packet);
            xSemaphoreGive(s_wlan_incoming_mux);

            if (packet.ptr)
            {
                if (packet.size >= sizeof(Ground2Air_Header))
                {
                    Ground2Air_Header& header = *(Ground2Air_Header*)packet.ptr;
                    if (header.size <= packet.size)
                    {
                        uint8_t crc = header.crc;
                        header.crc = 0;
                        uint8_t computed_crc = crc8(0, packet.ptr, header.size);
                        if (computed_crc != crc)
                            ESP_LOGE(TAG,"Bad incoming packet %d: bad crc %d != %d\n", (int)header.type, (int)crc, (int)computed_crc);
                        else
                        {
                            switch (header.type)
                            {
                                case Ground2Air_Header::Type::Telemetry:
                                    if(ground2air_data_packet_handler){
                                        ground2air_data_packet_handler(*(Ground2Air_Data_Packet*)packet.ptr);
                                    }
                                break;
                                case Ground2Air_Header::Type::Config:
                                    if(ground2air_config_packet_handler){
                                        ground2air_config_packet_handler(*(Ground2Air_Config_Packet*)packet.ptr);
                                    }
                                break;
                                default:
                                    ESP_LOGE(TAG,"Bad incoming packet: unknown type %d\n", (int)header.type);
                                break;
                            }
                        }
                    }
                    else
                        ESP_LOGE(TAG,"Bad incoming packet: header size too big %d > %d\n", (int)header.size, (int)packet.size);
                }
                else
                   ESP_LOGE(TAG,"Bad incoming packet: size too small %d < %d\n", (int)packet.size, (int)sizeof(Ground2Air_Header));


                xSemaphoreTake(s_wlan_incoming_mux, portMAX_DELAY);
                end_reading_wlan_incoming_packet(packet);
                xSemaphoreGive(s_wlan_incoming_mux);
            }
            else
                break;
        }
    }
}

#ifdef TX_COMPLETION_CB
IRAM_ATTR static void wifi_tx_done(uint8_t ifidx, uint8_t *data, uint16_t *data_len, bool txStatus)
{
    xSemaphoreGive(s_wifi_tx_done_semaphore);

#ifdef PROFILE_CAMERA_DATA    
    s_profiler.toggle(PF_CAMERA_WIFI_DONE_CB);
#endif

}
#endif 

void setup_wifi(WIFI_Rate wifi_rate,uint8_t chn,float power_dbm,void (*packet_received_cb)(void* buf, wifi_promiscuous_pkt_type_t type))
{
    printf("Setup WIFI...\n");

    xSemaphoreGive(s_wlan_incoming_mux);
    xSemaphoreGive(s_wlan_outgoing_mux);


    //allocates WLAN_INCOMING_BUFFER_SIZE(1kb) + WLAN_OUTGOING_BUFFER_SIZE(65kb) RAM
    init_queues(WLAN_INCOMING_BUFFER_SIZE, WLAN_OUTGOING_BUFFER_SIZE);

    //~30kb + ~65KB PSRAM
    setup_fec(s_ground2air_config_packet.dataChannel.fec_codec_k, s_ground2air_config_packet.dataChannel.fec_codec_n, s_ground2air_config_packet.dataChannel.fec_codec_mtu,
                add_to_wlan_outgoing_queue,add_to_wlan_incoming_queue);


    //allocates 118-32 kb RAM!!!
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    //ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_ap_handler, NULL, NULL));

    esp_wifi_internal_set_log_level(WIFI_LOG_NONE); //to try in increase bandwidth when we spam the send function and there are no more slots available

    {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(ESP_WIFI_MODE));
    }

    //~1.5kb RAM
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    //this reduces throughput for some reason
    //update: do not see any bad effect. Contrary, without completion cb, wifi_tx() tends to completely fail with ESP_ERR_NO_MEM error in crowded wifi environment
#ifdef TX_COMPLETION_CB
    ESP_ERROR_CHECK(esp_wifi_set_tx_done_cb(wifi_tx_done));
    xSemaphoreGive(s_wifi_tx_done_semaphore);
#endif

    ESP_ERROR_CHECK(set_wifi_fixed_rate(wifi_rate));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(ESP_WIFI_IF, WIFI_BW_HT20 ));
    ESP_ERROR_CHECK(esp_wifi_set_channel(chn, WIFI_SECOND_CHAN_NONE));

    wifi_promiscuous_filter_t filter = 
    {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_ctrl_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(packet_received_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));


    set_wlan_power_dBm(power_dbm);

    //esp_log_level_set("*", ESP_LOG_DEBUG);

    ESP_LOGI(TAG,"MEMORY After WIFI: ");
    heap_caps_print_heap_info(MALLOC_CAP_8BIT);

    {
        int core = tskNO_AFFINITY;
        BaseType_t res = xTaskCreatePinnedToCore(&wifi_tx_proc, "Wifi TX", 2048, nullptr, 1, &s_wifi_tx_task, core);
        if (res != pdPASS)
            ESP_LOGE(TAG, "Failed wifi tx task: %d\n", res);
    }
    {
        int core = tskNO_AFFINITY;
        BaseType_t res = xTaskCreatePinnedToCore(&wifi_rx_proc, "Wifi RX", 2048, nullptr, 1, &s_wifi_rx_task, core);
        if (res != pdPASS)
            ESP_LOGE(TAG, "Failed wifi rx task: %d\n", res);
    }

    ESP_LOGI(TAG,"Initialized");
}


esp_err_t set_wifi_fixed_rate(WIFI_Rate value)
{
    uint8_t rates[] = 
    {
        WIFI_PHY_RATE_2M_L,
        WIFI_PHY_RATE_2M_S,
        WIFI_PHY_RATE_5M_L,
        WIFI_PHY_RATE_5M_S,
        WIFI_PHY_RATE_11M_L,
        WIFI_PHY_RATE_11M_S,

        WIFI_PHY_RATE_6M,
        WIFI_PHY_RATE_9M,
        WIFI_PHY_RATE_12M,
        WIFI_PHY_RATE_18M,
        WIFI_PHY_RATE_24M,
        WIFI_PHY_RATE_36M,
        WIFI_PHY_RATE_48M,
        WIFI_PHY_RATE_54M,

        WIFI_PHY_RATE_MCS0_LGI,
        WIFI_PHY_RATE_MCS0_SGI,
        WIFI_PHY_RATE_MCS1_LGI,
        WIFI_PHY_RATE_MCS1_SGI,
        WIFI_PHY_RATE_MCS2_LGI,
        WIFI_PHY_RATE_MCS2_SGI,
        WIFI_PHY_RATE_MCS3_LGI,
        WIFI_PHY_RATE_MCS3_SGI,
        WIFI_PHY_RATE_MCS4_LGI,
        WIFI_PHY_RATE_MCS4_SGI,
        WIFI_PHY_RATE_MCS5_LGI,

        WIFI_PHY_RATE_MCS5_SGI,
        WIFI_PHY_RATE_MCS6_LGI,
        WIFI_PHY_RATE_MCS6_SGI,
        WIFI_PHY_RATE_MCS7_LGI,
        WIFI_PHY_RATE_MCS7_SGI,
    };
    //esp_err_t err = esp_wifi_internal_set_fix_rate(ESP_WIFI_IF, true, (wifi_phy_rate_t)rates[(int)value]);
    esp_err_t err = esp_wifi_config_80211_tx_rate(ESP_WIFI_IF, (wifi_phy_rate_t)rates[(int)value]);
    //esp_err_t err = esp_wifi_internal_set_fix_rate(ESP_WIFI_IF, true, (wifi_phy_rate_t)value);
    if (err == ESP_OK)
        s_wlan_rate = value;
    return err;
}

esp_err_t set_wlan_power_dBm(float dBm)
{
    constexpr float k_min = 2.f;
    constexpr float k_max = 20.f;

    dBm = std::max(std::min(dBm, k_max), k_min);
    s_wlan_power_dBm = dBm;
    int8_t power = static_cast<int8_t>(((dBm - k_min) / (k_max - k_min)) * 80) + 8;
    return esp_wifi_set_max_tx_power(power);
}

static esp_err_t esp_netif_set_static_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip;
    esp_netif_dhcps_stop(netif);

    memset(&ip, 0 , sizeof(esp_netif_ip_info_t));
    ip.ip.addr = ipaddr_addr("192.168.4.1");
    ip.netmask.addr = ipaddr_addr("255.255.255.0");
    ip.gw.addr = ipaddr_addr("192.168.4.1");
    if (esp_netif_set_ip_info(netif, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set ip info");
        esp_netif_dhcps_start(netif);
        return ESP_FAIL;
    }

    esp_netif_dhcps_start(netif);
    return ESP_OK;
}



esp_err_t start_file_server(const char *base_path);



void setup_wifi_file_server(void)
{
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_netif_set_static_ip(netif));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config;
    strcpy((char *)wifi_config.ap.ssid,"espvtx"); 
    wifi_config.ap.ssid_len = strlen("espvtx"); 
    wifi_config.ap.channel = 5;
    wifi_config.ap.max_connection = 5;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_file_server("/sdcard");

}

uint8_t getMaxWlanOutgoingQueueUsage()
{
    size_t v;
    size_t c;

    xSemaphoreTake(s_wlan_outgoing_mux, portMAX_DELAY);
    v = s_max_wlan_outgoing_queue_size;
    c = s_wlan_outgoing_queue.capacity();
    s_max_wlan_outgoing_queue_size = 0;
    xSemaphoreGive(s_wlan_outgoing_mux);

    return v * 100 / c;
}

uint8_t getMinWlanOutgoingQueueUsageSeen()
{
    size_t v;
    size_t c;

    xSemaphoreTake(s_wlan_outgoing_mux, portMAX_DELAY);
    v = s_min_wlan_outgoing_queue_size_seen;
    c = s_wlan_outgoing_queue.capacity();
    s_min_wlan_outgoing_queue_size_seen = 0;
    xSemaphoreGive(s_wlan_outgoing_mux);

    return v * 100 / c;
}

uint8_t getMaxWlanOutgoingQueueUsageFrame()
{
    size_t v;
    size_t c;

    xSemaphoreTake(s_wlan_outgoing_mux, portMAX_DELAY);

    v = s_max_wlan_outgoing_queue_size_frame;

    if ( s_max_wlan_outgoing_queue_size < v )
    {
        s_max_wlan_outgoing_queue_size = v;
    }

    c = s_wlan_outgoing_queue.capacity();
    s_max_wlan_outgoing_queue_size_frame = 0;

    xSemaphoreGive(s_wlan_outgoing_mux);

    return v * 100 / c;
}

uint8_t getMinWlanOutgoingQueueUsageFrame()
{
    size_t v;
    size_t c;

    xSemaphoreTake(s_wlan_outgoing_mux, portMAX_DELAY);

    v = s_min_wlan_outgoing_queue_size_frame;

    if ( ( v !=-1 ) && (s_min_wlan_outgoing_queue_size_seen < v) )
    {
        s_min_wlan_outgoing_queue_size_seen = v;
    }

    c = s_wlan_outgoing_queue.capacity();
    s_min_wlan_outgoing_queue_size_frame = -1;
    xSemaphoreGive(s_wlan_outgoing_mux);

    return v == -1? 0 : v * 100 / c;
}

