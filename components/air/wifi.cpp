#include "main.h"

#include "wifi.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi_types.h"
#include "structures.h"
#include "fec_codec.h"
#include "crc.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>
#include <unistd.h>

#include "vcd_profiler.h"



static const char * TAG="wifi_task";

#define TX_COMPLETION_CB

Ground2Air_Data_Packet s_ground2air_data_packet;

//we apply settings in two steps after receiving config packet:
//1) we apply changed non-camera related settings by comparing config and s_ground2air_config_packet
//2) we assign config to s_ground2air_config_packet
//3) we apply camera-related settings at the end of the frame by comparing s_ground2air_config_packet with s_ground2air_config_packet2
//4) we assign s_ground2air_config_packet to s_ground2air_config_packet2
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

static void (*ground2air_config_packet_handler)(Ground2Air_Config_Packet& src) = nullptr;
static void (*ground2air_connect_packet_handler)(Ground2Air_Config_Packet& src) = nullptr;
static void (*ground2air_data_packet_handler)(Ground2Air_Data_Packet& src) = nullptr;
static Transport_Payload_Received_CB s_transport_packet_received_handler = nullptr;
WIFI_Rate s_wlan_rate = s_ground2air_config_packet.dataChannel.wifi_rate;
float s_wlan_power_dBm = s_ground2air_config_packet.dataChannel.wifi_power;

static volatile WifiTransportMode s_wifi_transport_mode = WifiTransportMode::Raw80211;
static bool s_transport_runtime_initialized = false;
static bool s_wifi_event_loop_initialized = false;
static bool s_netif_initialized = false;
static uint16_t s_device_id = 0;

static TaskHandle_t s_udp_rx_task = nullptr;
static int s_udp_socket = -1;
static esp_netif_t* s_ap_netif = nullptr;
static sockaddr_in s_udp_peer_addr = {};
static bool s_udp_peer_known = false;

constexpr uint16_t APFPV_UDP_PORT = 5600;
static constexpr const char* APFPV_IP = "192.168.4.1";
static constexpr const char* APFPV_NETMASK = "255.255.255.0";
static constexpr int APFPV_UDP_SEND_TIMEOUT_US = 20000;
static constexpr int APFPV_UDP_SEND_BUFFER_SIZE = 8 * 1024;

#ifdef TX_COMPLETION_CB
static void wifi_tx_done(uint8_t ifidx, uint8_t *data, uint16_t *data_len, bool txStatus);
#endif

//===========================================================================================
//===========================================================================================
void set_ground2air_config_packet_handler(void (*handler)(Ground2Air_Config_Packet& src))
{
    ground2air_config_packet_handler = handler;
}

//===========================================================================================
//===========================================================================================
void set_ground2air_connect_packet_handler(void (*handler)(Ground2Air_Config_Packet& src))
{
    ground2air_connect_packet_handler = handler;
}

//===========================================================================================
//===========================================================================================
void set_ground2air_data_packet_handler(void (*handler)(Ground2Air_Data_Packet& src))
{
    ground2air_data_packet_handler=handler;
}

static esp_err_t ensure_event_loop()
{
    if (s_wifi_event_loop_initialized)
        return ESP_OK;

    //allocates 118-32 kb RAM!!!
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    {
        s_wifi_event_loop_initialized = true;
        return ESP_OK;
    }
    return err;
}

static esp_err_t ensure_netif()
{
    if (s_netif_initialized)
        return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    {
        s_netif_initialized = true;
        return ESP_OK;
    }
    return err;
}

static esp_err_t ensure_wifi_init()
{
    esp_err_t err = ensure_event_loop();
    if (err != ESP_OK)
        return err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE)
        return err;

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    return ESP_OK;
}

static void clear_udp_peer()
{
    memset(&s_udp_peer_addr, 0, sizeof(s_udp_peer_addr));
    s_udp_peer_known = false;
}

static void close_udp_socket()
{
    if (s_udp_socket >= 0)
    {
        shutdown(s_udp_socket, SHUT_RDWR);
        close(s_udp_socket);
        s_udp_socket = -1;
    }
    clear_udp_peer();
}

static esp_err_t ensure_ap_netif()
{
    esp_err_t err = ensure_netif();
    if (err != ESP_OK)
        return err;

    if (!s_ap_netif)
        s_ap_netif = esp_netif_create_default_wifi_ap();

    return s_ap_netif ? ESP_OK : ESP_FAIL;
}

static esp_err_t configure_ap_netif()
{
    esp_err_t err = ensure_ap_netif();
    if (err != ESP_OK)
        return err;

    esp_netif_ip_info_t ip = {};
    ip.ip.addr = ipaddr_addr(APFPV_IP);
    ip.netmask.addr = ipaddr_addr(APFPV_NETMASK);
    ip.gw.addr = ipaddr_addr(APFPV_IP);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));
    return ESP_OK;
}

static void get_ap_ssid(char* ssid, size_t size)
{
    snprintf(ssid, size, "esp32cam-fpv-%04x", s_device_id);
}

static void deliver_transport_payload(const uint8_t* data, size_t size, int8_t rssi_dbm, int8_t noise_floor_dbm)
{
    if (s_transport_packet_received_handler)
        s_transport_packet_received_handler(data, size, rssi_dbm, noise_floor_dbm);
}

static void raw_packet_received_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA)
    {
        s_stats.inRejectedPacketCounter++;
        return;
    }

    wifi_promiscuous_pkt_t* pkt = reinterpret_cast<wifi_promiscuous_pkt_t*>(buf);

    int channel = getValidWifiChannel(s_ground2air_config_packet.dataChannel.wifi_channel);
    if (pkt->rx_ctrl.channel != channel)
    {
        s_stats.inRejectedPacketCounter++;
        return;
    }

    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len <= WLAN_IEEE_HEADER_SIZE)
    {
        s_stats.wlan_error_count++;
        return;
    }

    uint8_t* data = pkt->payload;
    if (memcmp(data + 10, WLAN_IEEE_HEADER_GROUND2AIR + 10, 6) != 0)
    {
        s_stats.inRejectedPacketCounter++;
        return;
    }

    data += WLAN_IEEE_HEADER_SIZE;
    len -= WLAN_IEEE_HEADER_SIZE;

    if (len < 4)
    {
        s_stats.wlan_error_count++;
        return;
    }
    len -= 4;

    deliver_transport_payload(data, std::min<size_t>(len, WLAN_MAX_PAYLOAD_SIZE), -pkt->rx_ctrl.rssi, -pkt->rx_ctrl.noise_floor);
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
//here comes packet starting from Packet_Header + user_data
//size is sizeof(Packet_header) + sizeof(user_data)
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

//===========================================================================================
//===========================================================================================
inline bool init_queues(size_t wlan_incoming_queue_size, size_t wlan_outgoing_queue_size)
{
  //SPI RAM is too slow, can not handle more then 2Mb/s bandwidth
  //s_wlan_outgoing_queue.init(new uint8_t[wlan_outgoing_queue_size], wlan_outgoing_queue_size);
  s_wlan_outgoing_queue.init( (uint8_t*)heap_caps_malloc(wlan_outgoing_queue_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL ),wlan_outgoing_queue_size );
  //s_wlan_outgoing_queue.init( (uint8_t*)heap_caps_malloc(wlan_outgoing_queue_size, MALLOC_CAP_SPIRAM ),wlan_outgoing_queue_size*4 );

  s_wlan_incoming_queue.init(new uint8_t[wlan_incoming_queue_size], wlan_incoming_queue_size);

  return true;
}

//===========================================================================================
//===========================================================================================
//free queues memory to give some space for fileserver and OTA
void deinitQueues()
{
  free( s_wlan_outgoing_queue.getBuffer() );
  delete[] s_wlan_incoming_queue.getBuffer();
}

static void udp_rx_proc(void*)
{
    uint8_t buffer[sizeof(Packet_Header) + AIR2GROUND_MAX_MTU];

    while (true)
    {
        if (s_wifi_transport_mode != WifiTransportMode::ApUdp || s_udp_socket < 0)
        {
            vTaskDelay(20 / portTICK_PERIOD_MS);
            continue;
        }

        sockaddr_in from_addr = {};
        socklen_t from_len = sizeof(from_addr);
        int len = recvfrom(s_udp_socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from_addr), &from_len);
        if (len <= 0)
        {
            vTaskDelay(1);
            continue;
        }

        s_udp_peer_addr = from_addr;
        s_udp_peer_known = true;

        deliver_transport_payload(buffer, static_cast<size_t>(len), 0, 0);
    }
}

static esp_err_t stop_udp_rx_task()
{
    close_udp_socket();

    if (s_udp_rx_task)
    {
        vTaskDelete(s_udp_rx_task);
        s_udp_rx_task = nullptr;
    }

    return ESP_OK;
}

static esp_err_t start_udp_socket()
{
    close_udp_socket();

    s_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_socket < 0)
        return ESP_FAIL;

    timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    setsockopt(s_udp_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    timeval send_timeout = {};
    send_timeout.tv_sec = 0;
    send_timeout.tv_usec = APFPV_UDP_SEND_TIMEOUT_US;
    setsockopt(s_udp_socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    int send_buffer_size = APFPV_UDP_SEND_BUFFER_SIZE;
    setsockopt(s_udp_socket, SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size));

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(APFPV_UDP_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_udp_socket, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0)
    {
        close_udp_socket();
        return ESP_FAIL;
    }

    if (!s_udp_rx_task)
    {
        BaseType_t res = xTaskCreatePinnedToCore(&udp_rx_proc, "UDP RX", 3072, nullptr, 1, &s_udp_rx_task, tskNO_AFFINITY);
        if (res != pdPASS)
        {
            close_udp_socket();
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t start_raw_transport(WIFI_Rate wifi_rate, uint8_t chn, float power_dbm)
{
    ESP_ERROR_CHECK(stop_udp_rx_task());
    ESP_ERROR_CHECK(ensure_wifi_init());

    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    //~1.5kb RAM
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

#ifdef TX_COMPLETION_CB
    //this reduces throughput for some reason
    //update: do not see any bad effect. Contrary, without completion cb, wifi_tx() tends to completely fail with ESP_ERR_NO_MEM error in crowded wifi environment
    ESP_ERROR_CHECK(esp_wifi_set_tx_done_cb(wifi_tx_done));
    xSemaphoreGive(s_wifi_tx_done_semaphore);
#endif

#ifdef BOARD_ESP32C5
    // Enable both 2.4GHz and 5GHz bands by default
    wifi_country_t country_config = {
        .cc = "01",  // World-wide safe mode
        .schan = 1,
        .nchan = 14,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    country_config.wifi_5g_channel_mask = 0; // Allow all 5GHz channels
    ESP_ERROR_CHECK(esp_wifi_set_country(&country_config));
#endif

    //set channel before seting rate and bandwidth to select correct band (2.4/5Ghz)
    ESP_ERROR_CHECK(esp_wifi_set_channel(chn, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(set_wifi_fixed_rate(wifi_rate));

#if SOC_WIFI_SUPPORT_5G
    wifi_bandwidths_t bw = {
        .ghz_2g = WIFI_BW_HT20,
        .ghz_5g = WIFI_BW_HT20
    };
    ESP_ERROR_CHECK(esp_wifi_set_bandwidths(WIFI_IF_STA, &bw));
#else
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
#endif

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_ctrl_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(raw_packet_received_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_ERROR_CHECK(set_wlan_power_dBm(power_dbm));
    s_wifi_transport_mode = WifiTransportMode::Raw80211;
    return ESP_OK;
}

static esp_err_t start_ap_udp_transport(uint8_t chn, float power_dbm)
{
    ESP_ERROR_CHECK(ensure_wifi_init());
    ESP_ERROR_CHECK(configure_ap_netif());

    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();

    wifi_config_t wifi_config = {};
    get_ap_ssid(reinterpret_cast<char*>(wifi_config.ap.ssid), sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(reinterpret_cast<const char*>(wifi_config.ap.ssid));
    wifi_config.ap.channel = chn;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.max_connection = 1;
    wifi_config.ap.beacon_interval = 100;
    wifi_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_NONE;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(set_wlan_power_dBm(power_dbm));
    ESP_ERROR_CHECK(start_udp_socket());

    s_wifi_transport_mode = WifiTransportMode::ApUdp;
    return ESP_OK;
}

//===========================================================================================
//===========================================================================================
//A task which sends encoded packets (Air2Ground) after FEC
//from s_wlan_outgoing_queue
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
                size_t spins = isHQDVRMode() ? 10000 : 0;
                while (packet.ptr)
                {
#ifdef PROFILE_CAMERA_DATA    
                    s_profiler.set(PF_CAMERA_WIFI_TX,1);
#endif

                    esp_err_t res = ESP_OK;
                    bool packet_sent = false;
                    if (s_wifi_transport_mode == WifiTransportMode::Raw80211)
                    {
                        memcpy(packet.ptr, WLAN_IEEE_HEADER_AIR2GROUND, WLAN_IEEE_HEADER_SIZE);

#ifdef TX_COMPLETION_CB                    
                        xSemaphoreTake(s_wifi_tx_done_semaphore, 0);
#endif

                        res = esp_wifi_80211_tx(WIFI_IF_STA, packet.ptr, WLAN_IEEE_HEADER_SIZE + packet.size, false);
                        packet_sent = (res == ESP_OK);
                    }
                    else
                    {
                        if (!s_udp_peer_known || s_udp_socket < 0)
                        {
                            packet_sent = true;
                        }
                        else
                        {
                            int sent = sendto(s_udp_socket,
                                              reinterpret_cast<const char*>(packet.payload_ptr),
                                              packet.size,
                                              0,
                                              reinterpret_cast<const sockaddr*>(&s_udp_peer_addr),
                                              sizeof(s_udp_peer_addr));
                            res = sent == static_cast<int>(packet.size) ? ESP_OK : ESP_FAIL;
                            packet_sent = (res == ESP_OK);

                            if (!packet_sent)
                            {
                                const int udp_errno = errno;
                                const bool udp_backpressure =
                                    udp_errno == EAGAIN ||
                                    udp_errno == EWOULDBLOCK ||
                                    udp_errno == ENOMEM ||
                                    udp_errno == ENOBUFS;

                                if (udp_backpressure)
                                {
                                    s_stats.wlan_error_count++;

                                    // Keep the packet in the local queue so queue-based adaptive
                                    // quality can see the congestion instead of hiding it in the
                                    // socket/AP buffers.
                                    if (spins > 1000)
                                        vTaskDelay(1);
                                    else
                                        taskYIELD();
                                    spins++;
                                    continue;
                                }
                            }
                        }
                    }
                    if (packet_sent)
                    {
                        if (s_udp_peer_known || s_wifi_transport_mode == WifiTransportMode::Raw80211)
                        {
                            s_stats.wlan_data_sent += packet.size;
                            s_stats.outPacketCounter++;
                        }

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
                        if (s_wifi_transport_mode == WifiTransportMode::Raw80211)
                            xSemaphoreTake(s_wifi_tx_done_semaphore, portMAX_DELAY);
#endif

#ifdef PROFILE_CAMERA_DATA    
                        s_profiler.set(PF_CAMERA_WIFI_TX,0);
#endif

                    }
                    else if (s_wifi_transport_mode == WifiTransportMode::Raw80211 && res == ESP_ERR_NO_MEM)
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

//===========================================================================================
//===========================================================================================
// A task which reads incoming decoded packets (Ground2Air) after FEC
//from s_wlan_incoming_queue queue
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
                        {
                            //LOGGING from rxtask crashes esp32s!!!
                            //ESP_LOGE(TAG,"Bad incoming packet %d: bad crc %d != %d\n", (int)header.type, (int)crc, (int)computed_crc);
                            s_stats.wlan_received_packets_bad++;
                        }
                        else
                        {
                            switch (header.type)
                            {
                                case Ground2Air_Header::Type::Telemetry:
                                    if (ground2air_data_packet_handler)
                                    {
                                        ground2air_data_packet_handler(*(Ground2Air_Data_Packet*)packet.ptr);
                                    }
                                break;
                                
                                case Ground2Air_Header::Type::Config:
                                    if (ground2air_config_packet_handler)
                                    {
                                        ground2air_config_packet_handler(*(Ground2Air_Config_Packet*)packet.ptr);
                                    }
                                break;

                                case Ground2Air_Header::Type::Connect:
                                    if (ground2air_connect_packet_handler)
                                    {
                                        ground2air_connect_packet_handler(*(Ground2Air_Config_Packet*)packet.ptr);
                                    }
                                break;

                                default:
                                    //ESP_LOGE(TAG,"Bad incoming packet: unknown type %d\n", (int)header.type);
                                    s_stats.wlan_received_packets_bad++;
                                break;
                            }
                        }
                    }
                    else
                    {
                        //ESP_LOGE(TAG,"Bad incoming packet: header size too big %d > %d\n", (int)header.size, (int)packet.size);
                        s_stats.wlan_received_packets_bad++;
                    }
                }
                else
                {
                   //ESP_LOGE(TAG,"Bad incoming packet: size too small %d < %d\n", (int)packet.size, (int)sizeof(Ground2Air_Header));
                   s_stats.wlan_received_packets_bad++;
                }


                xSemaphoreTake(s_wlan_incoming_mux, portMAX_DELAY);
                end_reading_wlan_incoming_packet(packet);
                xSemaphoreGive(s_wlan_incoming_mux);
            }
            else
            {
                break;
            }
        }
    }
}

//===========================================================================================
//===========================================================================================
#ifdef TX_COMPLETION_CB
IRAM_ATTR static void wifi_tx_done(uint8_t ifidx, uint8_t *data, uint16_t *data_len, bool txStatus)
{
    xSemaphoreGive(s_wifi_tx_done_semaphore);

#ifdef PROFILE_CAMERA_DATA    
    s_profiler.toggle(PF_CAMERA_WIFI_DONE_CB);
#endif

}
#endif 

//===========================================================================================
//===========================================================================================
void setup_wifi(WIFI_Rate wifi_rate, uint8_t chn, float power_dbm, uint16_t device_id, bool apfpv, Transport_Payload_Received_CB packet_received_cb)
{
    printf("Setup WIFI...\n");

    s_transport_packet_received_handler = packet_received_cb;
    s_device_id = device_id;

    if (!s_transport_runtime_initialized)
    {
        xSemaphoreGive(s_wlan_incoming_mux);
        xSemaphoreGive(s_wlan_outgoing_mux);

		//allocates WLAN_INCOMING_BUFFER_SIZE(1kb) + WLAN_OUTGOING_BUFFER_SIZE(65kb) RAM
        init_queues(WLAN_INCOMING_BUFFER_SIZE, WLAN_OUTGOING_BUFFER_SIZE);

		//~30kb + ~65KB PSRAM
        setup_fec(s_ground2air_config_packet.dataChannel.fec_codec_k,
                  s_ground2air_config_packet.dataChannel.fec_codec_n,
                  s_ground2air_config_packet.dataChannel.fec_codec_mtu,
                  add_to_wlan_outgoing_queue,
                  add_to_wlan_incoming_queue);

 		//to try in increase bandwidth when we spam the send function and there are no more slots available
        esp_wifi_internal_set_log_level(WIFI_LOG_NONE);
        s_transport_runtime_initialized = true;
    }

    //esp_log_level_set("*", ESP_LOG_DEBUG);

    ESP_LOGI(TAG,"MEMORY After WIFI: ");
    heap_caps_print_heap_info(MALLOC_CAP_8BIT);

    if (!s_wifi_tx_task)
    {
        int core = tskNO_AFFINITY;
        BaseType_t res = xTaskCreatePinnedToCore(&wifi_tx_proc, "Wifi TX", 2048, nullptr, 1, &s_wifi_tx_task, core);
        if (res != pdPASS)
            ESP_LOGE(TAG, "Failed wifi tx task: %d\n", res);
    }
    if (!s_wifi_rx_task)
    {
        int core = tskNO_AFFINITY;
        BaseType_t res = xTaskCreatePinnedToCore(&wifi_rx_proc, "Wifi RX", 2048, nullptr, 1, &s_wifi_rx_task, core);
        if (res != pdPASS)
            ESP_LOGE(TAG, "Failed wifi rx task: %d\n", res);
    }

    ESP_ERROR_CHECK(switch_wifi_transport(apfpv, wifi_rate, chn, power_dbm));

    ESP_LOGI(TAG,"Initialized");
}

esp_err_t stop_wifi_transport()
{
    stop_udp_rx_task();
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    s_wifi_transport_mode = WifiTransportMode::Raw80211;
    return ESP_OK;
}

esp_err_t switch_wifi_transport(bool apfpv, WIFI_Rate wifi_rate, uint8_t channel, float power_dbm)
{
    return apfpv
        ? start_ap_udp_transport(channel, power_dbm)
        : start_raw_transport(wifi_rate, channel, power_dbm);
}

esp_err_t set_wifi_channel(uint8_t channel)
{
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

WifiTransportMode get_wifi_transport_mode()
{
    return s_wifi_transport_mode;
}

bool is_apfpv_mode()
{
    return s_wifi_transport_mode == WifiTransportMode::ApUdp;
}

esp_err_t set_wifi_fixed_rate(WIFI_Rate value)
{
    s_wlan_rate = value;
    if (is_apfpv_mode())
        return ESP_OK;

#if SOC_WIFI_SUPPORT_5G
    // For dual-band capable chips, we must use esp_wifi_set_protocols()
    // to set protocols for both bands.
    // We enable 11b/g/n on 2.4GHz and 11n on 5GHz.
    // We do not enable 11ac on 5Ghz because setting rate below will fail
    // We do not enable older 11a on 5Ghz
    wifi_protocols_t protocols = {.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N, .ghz_5g = WIFI_PROTOCOL_11N};
    ESP_ERROR_CHECK(esp_wifi_set_protocols(WIFI_IF_STA, &protocols));
#else
    // For single-band chips, use esp_wifi_set_protocol().
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
#endif

    wifi_phy_rate_t rates[] =
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

    LOG("Setting rate: %d\n %d", (int)value, (int)rates[(int)value] );

#if SOC_WIFI_SUPPORT_5G
    //esp_wifi_config_80211_tx_rate() does not work on esp32c. SDK Bug? 
    //It is noted that wifi_config_80211_tx_rate() does not work of ax protocol, but we disabled it...    
    //use esp_wifi_config_80211_tx() to change all packets
    wifi_phy_mode_t phy_mode;
    if (value <= WIFI_Rate::RATE_B_11M_CCK_S) 
    {
        phy_mode = WIFI_PHY_MODE_11B;
    } 
    else if (value <= WIFI_Rate::RATE_G_54M_ODFM) 
    {
        phy_mode = WIFI_PHY_MODE_11G;
    } 
    else 
    {
        phy_mode = WIFI_PHY_MODE_HT20;
    }
    wifi_tx_rate_config_t config = { .phymode = phy_mode, .rate = rates[(int)value] };
    esp_err_t err = esp_wifi_config_80211_tx(WIFI_IF_STA, &config);
#else
    esp_err_t err = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, rates[(int)value]);
#endif
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
    if (esp_netif_set_ip_info(netif, &ip) != ESP_OK) 
    {
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
    ESP_ERROR_CHECK(stop_wifi_transport());
    ESP_ERROR_CHECK(ensure_wifi_init());
    ESP_ERROR_CHECK(ensure_ap_netif());
    ESP_ERROR_CHECK(esp_netif_set_static_ip(s_ap_netif));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = {0},
            .password = {0},
            .ssid_len = 0,
            .channel = 5,
            .authmode = WIFI_AUTH_OPEN,
            .ssid_hidden = 0,
            .max_connection = 5,
            .beacon_interval = 100,
            .csa_count = 0,
            .dtim_period = 1,
            .pairwise_cipher = WIFI_CIPHER_TYPE_NONE,
            .ftm_responder = false,
            .pmf_cfg = {
                .capable = false,
                .required = false
            },
            .sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED
            }
        };
    strcpy((char *)wifi_config.ap.ssid,"esp32cam-fpv-config"); 
    wifi_config.ap.ssid_len = strlen("esp32cam-fpv-config"); 

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
