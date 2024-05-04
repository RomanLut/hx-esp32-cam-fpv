//#include <Arduino.h>
#include <algorithm>

#include "esp_camera.h"
//#include "EEPROM.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include <driver/adc.h>
#include "esp_adc_cal.h"
#include "esp_wifi_types.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
//#include "esp_wifi_internal.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_private/wifi.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
//#include "bt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include <unistd.h>

#include "fec_codec.h"
#include "packets.h"
#include "safe_printf.h"
#include "structures.h"
#include "crc.h"
#include "driver/gpio.h"
#include "main.h"
#include "queue.h"
#include "circular_buffer.h"

#include "ll_cam.h" // cam_obj_t defination, used in camera_data_available

#include "wifi.h"
#include "nvs_args.h"

#include "osd.h"
#include "msp.h"

 static int s_stats_last_tp = -10000;


/////////////////////////////////////////////////////////////////////////

static size_t s_video_frame_data_size = 0;
static uint32_t s_video_frame_index = 0;
static uint8_t s_video_part_index = 0;
static bool s_video_frame_started = false;
static size_t s_video_full_frame_size = 0;
static uint8_t s_osdUpdateCounter = 0;

static int s_quality = 20;
static int s_quality_counter = 0;
static float s_quality_framesize_K1 = 1;
static float s_quality_framesize_K2 = 1;
static float s_quality_framesize_K3 = 1;
static int s_max_frame_size = 0;

static int64_t s_video_last_sent_tp = esp_timer_get_time();
static int64_t s_video_last_acquired_tp = esp_timer_get_time();
static bool s_video_skip_frame = false;
static int64_t s_video_target_frame_dt = 0;
static uint8_t s_max_wlan_outgoing_queue_usage = 0;

static int64_t s_restart_time = 0;

extern WIFI_Rate s_wlan_rate;

static bool SDError = false;
static uint16_t SDTotalSpaceGB16 = 0;
static uint16_t SDFreeSpaceGB16 = 0;

/////////////////////////////////////////////////////////////////////////

static int s_uart_verbose = 1;

#define LOG(...) do { if (s_uart_verbose > 0) SAFE_PRINTF(__VA_ARGS__); } while (false) 

/////////////////////////////////////////////////////////////////////////

 sdmmc_card_t* card = nullptr;

static bool s_air_record = false;

static bool s_shouldRestartRecording = false;

//=============================================================================================
//=============================================================================================
void initialize_status_led()
{
#ifdef STATUS_LED_PIN
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 1ULL << STATUS_LED_PIN;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(STATUS_LED_PIN, STATUS_LED_OFF);
#endif    

#ifdef FLASH_LED_PIN
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << FLASH_LED_PIN;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
#endif    
}

//=============================================================================================
//=============================================================================================
void initialize_rec_button()
{
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1ULL << REC_BUTTON_PIN;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
}

IRAM_ATTR uint64_t micros()
{
    return esp_timer_get_time();
}

IRAM_ATTR uint64_t millis()
{
    return esp_timer_get_time() / 1000ULL;
}

void set_status_led(bool enabled)
{
#ifdef STATUS_LED_PIN
    gpio_set_level(STATUS_LED_PIN, enabled ? STATUS_LED_ON : STATUS_LED_OFF);
#endif    

#ifdef FLASH_LED_PIN
    if ( enabled) 
    {
        gpio_set_pull_mode(FLASH_LED_PIN, GPIO_PULLUP_ONLY);    // D1, needed in 4-line mode only
    }
    else
    {
        gpio_set_pull_mode(FLASH_LED_PIN, GPIO_PULLDOWN_ONLY);    // D1, needed in 4-line mode only
    }
#endif    
}

//=============================================================================================
//=============================================================================================
void update_status_led()
{
 /*
  if ( cameraInitError )
  {
    bool b = (millis() & 0x7f) > 0x40;
    b &= (millis() & 0x7ff) > 0x400;
    digitalWrite( LED_PIN, b ? LOW : HIGH);
    digitalWrite( 4, b ? HIGH : LOW);
    return;
  }

  if ( initError )
  {
    bool b = (millis() & 0x7f) > 0x40;
    digitalWrite( LED_PIN, b ? LOW : HIGH);
    digitalWrite( 4, b ? HIGH : LOW);
    return;
  }
*/
  if (s_air_record)
  {
    bool b = (millis() & 0x7ff) > 0x400;
    set_status_led(b);
  }
  else
  {
#ifdef DVR_SUPPORT    
    set_status_led(true);
#else
    bool b = (millis() & 0x7ff) > 0x400;
    set_status_led(b);
#endif
  }
}

//=============================================================================================
//=============================================================================================
void update_status_led_file_server()
{
    bool b = (millis() & 0x2ff) > 0x180;
    set_status_led(b);
}

//=============================================================================================
//=============================================================================================
bool getButtonState()
{
    return gpio_get_level((gpio_num_t)REC_BUTTON_PIN) == 0;
}

//=============================================================================================
//=============================================================================================
void checkButton()
{
  static uint32_t debounceTime = millis() + 100;
  static bool lastButtonState = false;

  if ( debounceTime > millis() ) return;
  bool buttonState = getButtonState();
  if ( buttonState != lastButtonState )
  {
    debounceTime = millis() + 100;
    lastButtonState = buttonState;

    if ( buttonState )
    {
        if ( s_restart_time == 0 )
        {
            s_air_record = !s_air_record;
        }
        LOG("Button pressed!\n");
    }
    else
    {
        LOG("Button unpressed!\n");
    }
  }
}

//////////////////////////////////////////////////////////////////////////////
static bool s_recv_ground2air_packet = false;

SemaphoreHandle_t s_serial_mux = xSemaphoreCreateBinary();

auto _init_result2 = []() -> bool
{
  xSemaphoreGive(s_serial_mux);
  return true;
}();


//=============================================================================================
//=============================================================================================
void init_failure()
{
    printf("INIT FAILURE!\n");
    
    initialize_status_led();
    while( true )
    {
        //esp_task_wdt_reset();

        bool b = (millis() & 0x7f) > 0x40;
        set_status_led(b);
    }
}

#ifdef DVR_SUPPORT

SemaphoreHandle_t s_sd_fast_buffer_mux = xSemaphoreCreateBinary();
SemaphoreHandle_t s_sd_slow_buffer_mux = xSemaphoreCreateBinary();

auto _init_result = []() -> bool
{
  xSemaphoreGive(s_sd_fast_buffer_mux);
  xSemaphoreGive(s_sd_slow_buffer_mux);
  return true;
}();

////////////////////////////////////////////////////////////////////////////////////

static TaskHandle_t s_sd_write_task = nullptr;
static TaskHandle_t s_sd_enqueue_task = nullptr;
static bool s_sd_initialized = false;
static size_t s_sd_file_size = 0;
static uint32_t s_sd_next_session_id = 0;
static uint32_t s_sd_next_segment_id = 0;


//the fast buffer is RAM and used to transfer data quickly from the camera callback to the slow, SPIRAM buffer. 
//Writing directly to the SPIRAM buffer is too slow in the camera callback and causes lost frames, so I use this RAM buffer and a separate task (sd_enqueue_task) for that.
static constexpr size_t SD_FAST_BUFFER_SIZE = 10000;
Circular_Buffer s_sd_fast_buffer(new uint8_t[SD_FAST_BUFFER_SIZE], SD_FAST_BUFFER_SIZE);

//this slow buffer is used to buffer data that is about to be written to SD. The reason it's this big is because SD card write speed fluctuated a lot and 
// sometimes it pauses for a few hundred ms. So to avoid lost data, I have to buffer it into a big enoigh buffer.
//The data is written to the sd card by the sd_write_task, in chunks of SD_WRITE_BLOCK_SIZE.
static constexpr size_t SD_SLOW_BUFFER_SIZE_PSRAM = 3 * 1024 * 1024;
static constexpr size_t SD_SLOW_BUFFER_SIZE_RAM = 32768;
Circular_Buffer* s_sd_slow_buffer = NULL;

//Cannot write to SD directly from the slow, SPIRAM buffer as that causes the write speed to plummet. So instead I read from the slow buffer into
// this RAM block and write from it directly. This results in several MB/s write speed performance which is good enough.
static constexpr size_t SD_WRITE_BLOCK_SIZE = 2048;


/*
static void shutdown_sd()
{
    if (!s_sd_initialized)
        return;
    LOG("close sd card!\n");
    
    esp_vfs_fat_sdcard_unmount("/sdcard",card);

    s_sd_initialized = false;

    //to turn the LED off
#ifdef BOARD_ESP32CAM    
    gpio_set_pull_mode((gpio_num_t)4, GPIO_PULLDOWN_ONLY);    // D1, needed in 4-line mode only
#endif    
}
*/
void updateSDInfo()
{
    if ( !s_sd_initialized) return;

    /* Get volume information and free clusters of sdcard */
    FATFS *fs;
    DWORD free_clust ;
    auto res = f_getfree("/sdcard/", &free_clust, &fs);
    if (res == 0 ) 
    {
        DWORD free_sect = free_clust * fs->csize;
        DWORD tot_sect = fs->n_fatent * fs->csize;

        SDFreeSpaceGB16 = free_sect / 2 / 1024 / (1024/16);
        SDTotalSpaceGB16 = tot_sect / 2 / 1024 / (1024/16);
	}
}

static bool init_sd()
{
    if (s_sd_initialized)
        return true;

    SDTotalSpaceGB16 = 0;
    SDFreeSpaceGB16 = 0;

#ifdef BOARD_ESP32CAM
    esp_vfs_fat_sdmmc_mount_config_t mount_config;
#ifdef CAMERA_MODEL_ESP_VTX
    mount_config.format_if_mount_failed = true;
#else
    mount_config.format_if_mount_failed = false;
#endif
    mount_config.max_files = 2;
    mount_config.allocation_unit_size = 0;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
    // For setting a specific frequency, use host.max_freq_khz (range 400kHz - 20MHz for SDSPI)
    // Example: for fixed frequency of 10MHz, use host.max_freq_khz = 10000;    
    //host.max_freq_khz = SDMMC_FREQ_PROBING;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    gpio_set_pull_mode((gpio_num_t)14, GPIO_PULLUP_ONLY);   // CLK, needed in 4-line mode only
    gpio_set_pull_mode((gpio_num_t)15, GPIO_PULLUP_ONLY);   // CMD
    gpio_set_pull_mode((gpio_num_t)2, GPIO_PULLUP_ONLY);    // D0
    //gpio_set_pull_mode((gpio_num_t)4, GPIO_PULLDOWN_ONLY);  // D1, needed in 4-line mode only
    //gpio_set_pull_mode((gpio_num_t)12, GPIO_PULLUP_ONLY);   // D2, needed in 4-line mode only
    //gpio_set_pull_mode((gpio_num_t)13, GPIO_PULLUP_ONLY);   // D3, needed in 4-line mode only

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;

    LOG("Mounting SD card...\n");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        LOG("Failed to mount SD card VFAT filesystem. Error: %s\n", esp_err_to_name(ret));
        //to turn the LED off
        gpio_set_pull_mode((gpio_num_t)4, GPIO_PULLDOWN_ONLY);
        return false;
    }
#endif
#ifdef BOARD_XIAOS3SENSE
/*
    esp_vfs_fat_sdmmc_mount_config_t mount_config;
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 2;
    mount_config.allocation_unit_size = 0;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    //host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    //host.max_freq_khz = SDMMC_FREQ_PROBING;
    //host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    //host.max_freq_khz = 26000;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = 9,
        .miso_io_num = 8,
        .sclk_io_num = 7,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092
    };
    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) 
    {
        LOG("Failed to initialize SD SPI bus.");
        return false;
    }
    //host.set_card_clk(host.slot, 10000);

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = GPIO_NUM_21;  //shared with USER LED pin
    slot_config.host_id = (spi_host_device_t)host.slot;

    LOG("Mounting SD card...\n");
    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        LOG("Failed to mount SD card VFAT filesystem. Error: %s\n", esp_err_to_name(ret));
        return false;
    }
*/ 
    esp_vfs_fat_sdmmc_mount_config_t mount_config;
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 2;
    mount_config.allocation_unit_size = 0;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
    // For setting a specific frequency, use host.max_freq_khz (range 400kHz - 20MHz for SDSPI)
    // Example: for fixed frequency of 10MHz, use host.max_freq_khz = 10000;    
    //host.max_freq_khz = SDMMC_FREQ_PROBING;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = GPIO_NUM_7;
    slot_config.cmd = GPIO_NUM_9;
    slot_config.d0 = GPIO_NUM_8;

    // Enable internal pullups on enabled pins. The internal pullups
    // are insufficient however, please make sure 10k external pullups are
    // connected on the bus. This is for debug / example purpose only.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    LOG("Mounting SD card...\n");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        LOG("Failed to mount SD card VFAT filesystem. Error: %s\n", esp_err_to_name(ret));
        return false;
    }
#endif

    LOG("sd card inited!\n");
    s_sd_initialized = true;

#ifdef DVR_SUPPORT
    s_air_record = true;
#endif

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    updateSDInfo();

    //find the latest file number
    char buffer[64];
    for (uint32_t i = 0; i < 100000; i++)
    {
        sprintf(buffer, "/sdcard/v%03lu_000.mpg", (long unsigned int)i);
        FILE* f = fopen(buffer, "rb");
        if (f)
        {
            fclose(f);
            continue;
        }
        s_sd_next_session_id = i;
        s_sd_next_segment_id = 0;
        break;
    }
    return true;
}

static FILE* open_sd_file()
{
    char buffer[64];
    sprintf(buffer, "/sdcard/v%03lu_%03lu.mpg", (long unsigned int)s_sd_next_session_id, (long unsigned int)s_sd_next_segment_id);
    FILE* f = fopen(buffer, "wb");
    if (!f){
        LOG("error to open sdcard session %s!\n",buffer);
        return nullptr;
    }

    LOG("Opening session file '%s'\n", buffer);
    s_sd_file_size = 0;
    s_sd_next_segment_id++;

    return f;
}

//=============================================================================================
//=============================================================================================
//this will write data from the slow queue to file
static void sd_write_proc(void*)
{
    //for fast SD writes, buffer has to be in DMA enabled memory
    uint8_t* block = (uint8_t*)heap_caps_malloc(SD_WRITE_BLOCK_SIZE, MALLOC_CAP_DMA);//new uint8_t[SD_WRITE_BLOCK_SIZE];

    while (true)
    {
        if (!s_air_record)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        FILE* f = open_sd_file();
        if (!f)
        {
            s_air_record = false;
            SDError = true;
            vTaskDelay(1000 / portTICK_PERIOD_MS); 
            continue;
        }

        xSemaphoreTake(s_sd_slow_buffer_mux, portMAX_DELAY);
        s_sd_slow_buffer->clear();
        xSemaphoreGive(s_sd_slow_buffer_mux);

        bool error = false; 
        bool done = false;
        while (!done)
        {
            ulTaskNotifyTake(pdTRUE, 1000 / portTICK_PERIOD_MS); //wait for notification

            while (true) //consume all the buffer
            {
                if (!s_air_record)
                {
                    LOG("Done recording, closing file\n", s_sd_file_size);
                    done = true;
                    break;
                }

                if ( s_shouldRestartRecording ) 
                {
                    s_shouldRestartRecording = false;
                    done = true;
                }

                xSemaphoreTake(s_sd_slow_buffer_mux, portMAX_DELAY);
                bool read = s_sd_slow_buffer->read(block, SD_WRITE_BLOCK_SIZE);
                xSemaphoreGive(s_sd_slow_buffer_mux);
                if (!read)
                {
                    break; //not enough data, wait
                }

                if ( !done )
                {
                    if (fwrite(block, SD_WRITE_BLOCK_SIZE, 1, f) == 0)
                    {
                        LOG("Error while writing! Stopping session\n");
                        done = true;
                        error = true;
                        SDError = true;
                        break;
                    }
                    s_stats.sd_data += SD_WRITE_BLOCK_SIZE;
                    s_sd_file_size += SD_WRITE_BLOCK_SIZE;
                    if (s_sd_file_size > 50 * 1024 * 1024)
                    {
                        LOG("Max file size reached: %d. Restarting session\n", s_sd_file_size);
                        done = true;
                        break;
                    }
                }
            }
        }

        if (!error)
        {
            fflush(f);
            fsync(fileno(f));
            fclose(f);
            updateSDInfo();
        }
        else
        {
            s_air_record = false;
            //shutdown_sd();
        }
    }
}

//=============================================================================================
//=============================================================================================
//this will move data from the fast queue to the slow queue
static void sd_enqueue_proc(void*)
{
    while (true)
    {
        if (!s_air_record)
        {
            //vTaskDelay(1000 / portTICK_PERIOD_MS);
            ulTaskNotifyTake(pdTRUE, 1000 / portTICK_PERIOD_MS); //wait for notification
            continue;
        }

        xSemaphoreTake(s_sd_fast_buffer_mux, portMAX_DELAY);
        s_sd_fast_buffer.clear();
        xSemaphoreGive(s_sd_fast_buffer_mux);

        while (true)
        {
            ulTaskNotifyTake(pdTRUE, 1000 / portTICK_PERIOD_MS); //wait for notification

            xSemaphoreTake(s_sd_fast_buffer_mux, portMAX_DELAY);
            size_t size = s_sd_fast_buffer.size();
            if (size == 0)
            {
                xSemaphoreGive(s_sd_fast_buffer_mux);
                continue; //no data? wait some more
            }

            const void* buffer = s_sd_fast_buffer.start_reading(size);
            xSemaphoreGive(s_sd_fast_buffer_mux);

            xSemaphoreTake(s_sd_slow_buffer_mux, portMAX_DELAY);
            if ( !s_sd_slow_buffer->write(buffer, size) )
            {
                s_stats.sd_drops += size;
            }
            xSemaphoreGive(s_sd_slow_buffer_mux);

            xSemaphoreTake(s_sd_fast_buffer_mux, portMAX_DELAY);
            s_sd_fast_buffer.end_reading(size);
            xSemaphoreGive(s_sd_fast_buffer_mux);

            if (s_sd_write_task)
                xTaskNotifyGive(s_sd_write_task); //notify task

            if (!s_air_record)
                break;
        }
    }
}


IRAM_ATTR static void add_to_sd_fast_buffer(const void* data, size_t size)
{
    xSemaphoreTake(s_sd_fast_buffer_mux, portMAX_DELAY);
    bool ok = s_sd_fast_buffer.write(data, size);
    xSemaphoreGive(s_sd_fast_buffer_mux);
    if (ok)
    {
        if (s_sd_enqueue_task)
            xTaskNotifyGive(s_sd_enqueue_task); //notify task
    }
    else
    {
        s_stats.sd_drops += size;
    }
}

#endif

int16_t s_wlan_incoming_rssi = 0; //this is protected by the s_wlan_incoming_mux

//=============================================================================================
//=============================================================================================
IRAM_ATTR void packet_received_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (type == WIFI_PKT_MGMT)
    {
        //LOG("management packet\n");
        return;
    }
    else if (type == WIFI_PKT_DATA)
    {
        //LOG("data packet\n");
    }
    else if (type == WIFI_PKT_MISC)
    {
        //LOG("misc packet\n");
        return;
    }

    wifi_promiscuous_pkt_t *pkt = reinterpret_cast<wifi_promiscuous_pkt_t*>(buf);

    uint16_t len = pkt->rx_ctrl.sig_len;
    //s_stats.wlan_data_received += len;
    //s_stats.wlan_data_sent += 1;

    if (len <= WLAN_IEEE_HEADER_SIZE)
        return;

    //LOG("Recv %d bytes\n", len);
    //LOG("Channel: %d\n", (int)pkt->rx_ctrl.channel);

    //uint8_t broadcast_mac[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    //LOG("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint8_t *data = pkt->payload;
    if (memcmp(data + 10, WLAN_IEEE_HEADER_GROUND2AIR + 10, 6) != 0)
        return;

    if (len <= WLAN_IEEE_HEADER_SIZE)
    {
        LOG("WLAN receive header error");
        s_stats.wlan_error_count++;
        return;
    }

    data += WLAN_IEEE_HEADER_SIZE;
    len -= WLAN_IEEE_HEADER_SIZE; //skip the 802.11 header

    len -= 4; //the received length has 4 more bytes at the end for some reason.

    int16_t rssi = pkt->rx_ctrl.rssi;

    size_t size = std::min<size_t>(len, WLAN_MAX_PAYLOAD_SIZE);

    xSemaphoreTake(s_wlan_incoming_mux, portMAX_DELAY);
    s_wlan_incoming_rssi = rssi;
    xSemaphoreGive(s_wlan_incoming_mux);

    s_fec_decoder.lock();
    if (!s_fec_decoder.decode_data(data, size, false))
        s_stats.wlan_received_packets_dropped++;
    s_fec_decoder.unlock();

    s_stats.wlan_data_received += len;
}

//=============================================================================================
//=============================================================================================
//process settings not related to camera sensor setup
IRAM_ATTR static void handle_ground2air_config_packetEx1(Ground2Air_Config_Packet& src)
{
    Ground2Air_Config_Packet& dst = s_ground2air_config_packet;
    s_recv_ground2air_packet = true;
    if (dst.wifi_rate != src.wifi_rate)
    {
        LOG("Wifi rate changed from %d to %d\n", (int)dst.wifi_rate, (int)src.wifi_rate);
        ESP_ERROR_CHECK(set_wifi_fixed_rate(src.wifi_rate));
    }
    if (dst.wifi_power != src.wifi_power)
    {
        LOG("Wifi power changed from %d to %d\n", (int)dst.wifi_power, (int)src.wifi_power);
        ESP_ERROR_CHECK(set_wlan_power_dBm(src.wifi_power));
    }
    if (dst.fec_codec_k != src.fec_codec_k || dst.fec_codec_n != src.fec_codec_n || dst.fec_codec_mtu != src.fec_codec_mtu)
    {
        LOG("FEC codec changed from %d/%d/%d to %d/%d/%d\n", (int)dst.fec_codec_k, (int)dst.fec_codec_n, (int)dst.fec_codec_mtu, (int)src.fec_codec_k, (int)src.fec_codec_n, (int)src.fec_codec_mtu);
        {
            init_fec_codec(s_fec_encoder,src.fec_codec_k,src.fec_codec_n,src.fec_codec_mtu,true);
        }
    }
    if (dst.wifi_channel != src.wifi_channel)
    {
        LOG("Wifi channel changed from %d to %d\n", (int)dst.wifi_channel, (int)src.wifi_channel);
        nvs_args_set("channel", src.wifi_channel);
        ESP_ERROR_CHECK(esp_wifi_set_channel((int)src.wifi_channel, WIFI_SECOND_CHAN_NONE));

        s_air_record = false;
        s_restart_time = esp_timer_get_time() + 2000000;
    }

    if (dst.camera.fps_limit != src.camera.fps_limit)
    {
        if (src.camera.fps_limit == 0)
            s_video_target_frame_dt = 0;
        else
            s_video_target_frame_dt = 1000000 / src.camera.fps_limit;
        LOG("Target FPS changed from %d to %d\n", (int)dst.camera.fps_limit, (int)src.camera.fps_limit);
    }

    if ( s_restart_time == 0 )
    {
        if ( dst.air_record_btn != src.air_record_btn )
        {
            if ( ((uint8_t)(dst.air_record_btn + 1)) == src.air_record_btn )
            {
                s_air_record = !s_air_record;
            }
            dst.air_record_btn = src.air_record_btn;
        }
    }

    dst = src;
}

//=============================================================================================
//=============================================================================================
//process settings related to camera sensor setup
IRAM_ATTR void handle_ground2air_config_packetEx2(bool forceCameraSettings)
{
    Ground2Air_Config_Packet& src = s_ground2air_config_packet;
    Ground2Air_Config_Packet& dst = s_ground2air_config_packet2;

    if (forceCameraSettings || (dst.camera.resolution != src.camera.resolution))
    {
        LOG("Camera resolution changed from %d to %d\n", (int)dst.camera.resolution, (int)src.camera.resolution);
        sensor_t* s = esp_camera_sensor_get();

        switch (src.camera.resolution)
        {
            case Resolution::QVGA: s->set_framesize(s, FRAMESIZE_QVGA); break;
            case Resolution::CIF: s->set_framesize(s, FRAMESIZE_CIF); break;
            case Resolution::HVGA: s->set_framesize(s, FRAMESIZE_HVGA); break;
            case Resolution::VGA: s->set_framesize(s, FRAMESIZE_VGA); break;
            case Resolution::SVGA: s->set_framesize(s, FRAMESIZE_SVGA); break;
            case Resolution::XGA: s->set_framesize(s, FRAMESIZE_XGA); break;
            case Resolution::SXGA: s->set_framesize(s, FRAMESIZE_SXGA); break;
            case Resolution::UXGA: s->set_framesize(s, FRAMESIZE_UXGA); break;

            case Resolution::SVGA16:
#ifdef SENSOR_OV5640
                //s->set_res_raw(s, 0, 0, 2623, 1951, 32, 16, 2844, 1968, 800, 600, true, true);  //attempt for 800x600
                //s->set_res_raw(s, 0, 240, 2623, 1711, 32, 16, 2844, 1488, 800, 450, true, true); //attempt for 800x450

                //s->set_pll(s, false, 26, 1, 1, false, 3, true, 4);  - root2x and pre_div are swapped due to incompatible signatures!
                //s->set_pll(s, false, 25, 1, false, 1, 3, true, 4); 

                //waning: LOGxxx should be commented out in ov5640.c otherwise there will be stack overflow in camtask
                s->set_framesize(s, FRAMESIZE_P_HD); //800x450
#else
                //s->set_framesize(s, FRAMESIZE_P_HD);  800x448 13 fps
                s->set_res_raw(s, 1/*OV2640_MODE_SVGA*/,0,0,0, 0, 72, 800, 600-144, 800,600-144,false,false);   //800x456x29.5? fps
                
#endif
            break;

        }
        s_shouldRestartRecording = true;
    }

#define APPLY(n1, n2, type) \
    if (forceCameraSettings || (dst.camera.n1 != src.camera.n1)) \
    { \
        LOG("Camera " #n1 " from %d to %d\n", (int)dst.camera.n1, (int)src.camera.n1); \
        sensor_t* s = esp_camera_sensor_get(); \
        s->set_##n2(s, (type)src.camera.n1); \
    }

    if ( src.camera.quality != 0 )
    {
        APPLY(quality, quality, int);
        s_quality = src.camera.quality;
    }
    else
    {
        if ( forceCameraSettings )
        {
            sensor_t* s = esp_camera_sensor_get(); 
            s->set_quality(s, 20); 
        }
    }

    if ( dst.camera.agc != src.camera.agc)
    {
        //reapply agc gain if agc changed
        forceCameraSettings = true;
    }

    if ( dst.camera.aec != src.camera.aec)
    {
        //reapply aec value if aec changed
        forceCameraSettings = true;
    }

    APPLY(brightness, brightness, int);
    APPLY(contrast, contrast, int);
    APPLY(saturation, saturation, int);
    APPLY(sharpness, sharpness, int);
    APPLY(denoise, denoise, int);
#ifdef SENSOR_OV5640
    //gainceiling for ov5640 is range 0...3ff
    if (forceCameraSettings || (dst.camera.gainceiling != src.camera.gainceiling)) 
    { 
        LOG("Camera gainceiling from %d to %d\n", (int)dst.camera.gainceiling, (int)src.camera.gainceiling); 
        sensor_t* s = esp_camera_sensor_get(); 
        //s->set_gainceiling(s, (gainceiling_t)(2 << src.camera.gainceiling));
        //do not limit gainceiling on OV5640. Contrary to OV2640, it does good images with large gain ceiling, without enormous noise in dark scenes.
        s->set_gainceiling(s, (gainceiling_t)(0x3ff));
    }
#else
    APPLY(gainceiling, gainceiling, gainceiling_t);
#endif
    APPLY(awb, whitebal, int);
    APPLY(awb_gain, awb_gain, int);
    APPLY(wb_mode, wb_mode, int);
    APPLY(agc, gain_ctrl, int);
    APPLY(agc_gain, agc_gain, int);
    APPLY(aec, exposure_ctrl, int);
    APPLY(aec_value, aec_value, int);
    APPLY(aec2, aec2, int);
    APPLY(ae_level, ae_level, int);
    APPLY(hmirror, hmirror, int);
    APPLY(vflip, vflip, int);
    APPLY(special_effect, special_effect, int);
    APPLY(dcw, dcw, int);
    APPLY(bpc, bpc, int);
    APPLY(wpc, wpc, int);
    APPLY(raw_gma, raw_gma, int);
    APPLY(lenc, lenc, int);
#undef APPLY

    dst = src;
}

//===========================================================================================
//===========================================================================================
IRAM_ATTR static void handle_ground2air_config_packet(Ground2Air_Config_Packet& src)
{
    //ahdnle settings not reated to camera sensor setup.
    //camera sensor settings are prcessed in camera_data_available() callback
    handle_ground2air_config_packetEx1(src);
}

//===========================================================================================
//===========================================================================================
IRAM_ATTR static void handle_ground2air_data_packet(Ground2Air_Data_Packet& src)
{
#ifdef UART_MAVLINK
    xSemaphoreTake(s_serial_mux, portMAX_DELAY);

    int s = src.size - sizeof(Ground2Air_Header);
    s_stats.in_telemetry_data += s;        

    size_t freeSize = 0;
    ESP_ERROR_CHECK( uart_get_tx_buffer_free_size(UART_MAVLINK, &freeSize) );

    if ( freeSize >= s )
    {
        uart_write_bytes(UART_MAVLINK, ((uint8_t*)&src) + sizeof(Ground2Air_Header), s);
        //uart_write_bytes(UART_NUM_0, ((uint8_t*)&src) + sizeof(Ground2Air_Header), s);
    }

    xSemaphoreGive(s_serial_mux);
#endif
}

//=============================================================================================
//=============================================================================================
IRAM_ATTR void send_air2ground_video_packet(bool last)
{
    if (last)
        s_stats.video_frames++;
    s_stats.video_data += s_video_frame_data_size;

    uint8_t* packet_data = s_fec_encoder.get_encode_packet_data(true);

    if(!packet_data){
        LOG("no data buf!\n");
        return ;
    }

    Air2Ground_Video_Packet& packet = *(Air2Ground_Video_Packet*)packet_data;
    packet.type = Air2Ground_Header::Type::Video;
    packet.resolution = s_ground2air_config_packet.camera.resolution;
    packet.frame_index = s_video_frame_index;
    packet.part_index = s_video_part_index;
    packet.last_part = last ? 1 : 0;
    packet.size = s_video_frame_data_size + sizeof(Air2Ground_Video_Packet);
    packet.pong = s_ground2air_config_packet.ping;
    packet.version = PACKET_VERSION;
    packet.crc = 0;
    packet.crc = crc8(0, &packet, sizeof(Air2Ground_Video_Packet));
    if (!s_fec_encoder.flush_encode_packet(true))
    {
        LOG("Fec codec busy\n");
        s_stats.wlan_error_count++;
    }
}

//constexpr size_t MAX_TELEMETRY_PAYLOAD_SIZE = AIR2GROUND_MTU - sizeof(Air2Ground_Data_Packet);
//constexpr size_t MAX_TELEMETRY_PAYLOAD_SIZE = 512;
constexpr size_t MAX_TELEMETRY_PAYLOAD_SIZE = 128;

#ifdef UART_MAVLINK
//=============================================================================================
//=============================================================================================
IRAM_ATTR void send_air2ground_data_packet()
{
    uint8_t* packet_data = s_fec_encoder.get_encode_packet_data(true);
    int ds = MAX_TELEMETRY_PAYLOAD_SIZE;

    if(!packet_data){
        LOG("no data buf!\n");
        return ;
    }

    Air2Ground_Data_Packet& packet = *(Air2Ground_Data_Packet*)packet_data;
    packet.type = Air2Ground_Header::Type::Telemetry;
    packet.size = ds + sizeof(Air2Ground_Data_Packet);
    packet.pong = s_ground2air_config_packet.ping;
    packet.version = PACKET_VERSION;
    packet.crc = 0;
    packet.crc = crc8(0, &packet, sizeof(Air2Ground_Data_Packet));

    s_stats.out_telemetry_data += ds;

    //MAX_TELEMETRY_PAYLOAD_SIZE is available in buffer. It was checked before.
    //if uart_read_bytes() can not read specified number of bytes, it returns error.
    //ESP_ERROR_CHECK( uart_read_bytes(UART_MAVLINK, packet_data + sizeof(Air2Ground_Data_Packet), MAX_TELEMETRY_PAYLOAD_SIZE, 0));

    if (!s_fec_encoder.flush_encode_packet(true))
    {
        LOG("Fec codec busy\n");
        s_stats.wlan_error_count++;
    }
}
#endif

//=============================================================================================
//=============================================================================================
IRAM_ATTR void send_air2ground_osd_packet()
{
    uint8_t* packet_data = s_fec_encoder.get_encode_packet_data(true);

    if(!packet_data){
        LOG("no data buf!\n");
        return ;
    }

    Air2Ground_OSD_Packet& packet = *(Air2Ground_OSD_Packet*)packet_data;
    packet.type = Air2Ground_Header::Type::OSD;
    packet.size = sizeof(Air2Ground_OSD_Packet);
    packet.pong = s_ground2air_config_packet.ping;
    packet.version = PACKET_VERSION;
    packet.crc = 0;

    packet.SDDetected = s_sd_initialized ? 1: 0;
    packet.SDSlow = s_stats.sd_drops ? 1: 0;
    packet.SDError = SDError ? 1: 0;
    packet.curr_wifi_rate = (uint8_t)s_wlan_rate;

    packet.wifi_queue = s_max_wlan_outgoing_queue_usage;
    packet.air_record_state = s_air_record ? 1 : 0;

    packet.SDFreeSpaceGB16 = SDFreeSpaceGB16;
    packet.SDTotalSpaceGB16 = SDTotalSpaceGB16;
    packet.curr_quality = s_quality;
#ifdef SENSOR_OV5640    
    packet.isOV5640 = 1;
#else
    packet.isOV5640 = 0;
#endif    

    memcpy( &packet.buffer, g_osd.getBuffer(), OSD_BUFFER_SIZE );
    
    packet.crc = crc8(0, &packet, sizeof(Air2Ground_OSD_Packet));

    if (!s_fec_encoder.flush_encode_packet(true))
    {
        LOG("Fec codec busy\n");
        s_stats.wlan_error_count++;
    }
}

constexpr size_t MAX_VIDEO_DATA_PAYLOAD_SIZE = AIR2GROUND_MTU - sizeof(Air2Ground_Video_Packet);

static const int WifiRateBandwidth[] = 
{
    2*1024*100, // 0 - RATE_B_2M_CCK,
    2*1024*100, // 1 - RATE_B_2M_CCK_S,
    5*1024*100, // 2 - RATE_B_5_5M_CCK,
    5*1024*100, // 3 - RATE_B_5_5M_CCK_S,
    11*1024*100, // 4 - RATE_B_11M_CCK,
    11*1024*100, // 5 - RATE_B_11M_CCK_S,

    6*1024*100, // 6 - RATE_G_6M_ODFM,
    9*1024*100, // 7 - RATE_G_9M_ODFM,
    12*1024*100, // 8 - RATE_G_12M_ODFM,
    18*1024*100, // 9 - RATE_G_18M_ODFM,
    24*1024*100,  // 10 - RATE_G_24M_ODFM,
    36*1024*100,  // 11 - RATE_G_36M_ODFM,
    48*1024*100,  // 12 - RATE_G_48M_ODFM,
    54*1024*100,  // 13 - RATE_G_54M_ODFM,

    6*1024*100, // 14 - RATE_N_6_5M_MCS0,
    7*1024*100, // 15 - RATE_N_7_2M_MCS0_S,
    13*1024*100, // 16 - RATE_N_13M_MCS1,
    14*1024*100, // 17 - RATE_N_14_4M_MCS1_S,
    19*1024*100, // 18 - RATE_N_19_5M_MCS2,
    21*1024*100, // 19 - RATE_N_21_7M_MCS2_S,
    26*1024*100, // 20 - RATE_N_26M_MCS3,
    28*1024*100, // 21 - RATE_N_28_9M_MCS3_S,
    39*1024*100, // 22 - RATE_N_39M_MCS4,
    43*1024*100, // 23 - RATE_N_43_3M_MCS4_S,
    52*1024*100, // 24 - RATE_N_52M_MCS5,
    57*1024*100, // 25 - RATE_N_57_8M_MCS5_S,
    58*1024*100, // 26 - RATE_N_58M_MCS6,
    65*1024*100, // 27 - RATE_N_65M_MCS6_S,
    65*1024*100, // 28 - RATE_N_65M_MCS7,
    72*1024*100 // 29 - RATE_N_72M_MCS7_S,
};


IRAM_ATTR int getBandwidthForRate(WIFI_Rate rate)
{
    return WifiRateBandwidth[(int)rate];
}

//=============================================================================================
//=============================================================================================
IRAM_ATTR void recalculateFrameSizeQualityK(int video_full_frame_size)
{
#ifdef SENSOR_OV5640
    //quality changes kill framerate on ov5640. Limit changes.
    s_quality_counter++;
    if ( s_quality_counter < 30 ) return;
    s_quality_counter = 0;
#endif

    if ( video_full_frame_size > s_max_frame_size )
    {
        s_max_frame_size = video_full_frame_size;
    }
    if ( video_full_frame_size == 0 ) return;
    if (s_ground2air_config_packet.camera.fps_limit == 0) return;

    //K1 - wifi bandwidth
    //data rate available with current wifi rate
    int rateBandwidth = getBandwidthForRate(s_wlan_rate);
    //decrease available data rate using FEC codec parameters
    int FECbandwidth = rateBandwidth * s_ground2air_config_packet.fec_codec_k / s_ground2air_config_packet.fec_codec_n;

    //1.2mb/sec is practical maximum limit which works
    if ( FECbandwidth > 1200*1024 ) FECbandwidth = 1200*1024;
    
    int frameSize = FECbandwidth / s_ground2air_config_packet.camera.fps_limit * 7 / 10;  //assume only  70% of total bandwidth is available in practice
    if ( frameSize < 1 ) frameSize = 1;

    float k = frameSize * 1.0f / video_full_frame_size;
    if ( k > 1.1f ) k = 1.1f;

    s_quality_framesize_K1 = s_quality_framesize_K1 * k;
    if ( s_quality_framesize_K1 < 0.05f ) s_quality_framesize_K1 = 0.05f;
    if ( s_quality_framesize_K1 > 1.0f ) s_quality_framesize_K1 = 1.0f;

    //k2 - max frame size
    int safe_frame_size = 40*1024 * 30 / s_ground2air_config_packet.camera.fps_limit;
    s_quality_framesize_K2 = s_quality_framesize_K2 * safe_frame_size * 1.0f / video_full_frame_size;
    if ( s_quality_framesize_K2 < 0.05f ) s_quality_framesize_K2 = 0.05f;
    if ( s_quality_framesize_K2 > 1.0f ) s_quality_framesize_K2 = 1.0f;

    //K3 - wifi queue
    if ( s_stats.wlan_error_count > 0 )
    {
        s_quality_framesize_K3 -= 0.01f;  
    }

    if ( s_max_wlan_outgoing_queue_usage > 70 )
    {
        s_quality_framesize_K3 -= (s_max_wlan_outgoing_queue_usage - 70) / 30000.0f;
    }
    else
    {
        s_quality_framesize_K3 += ( 70 - s_max_wlan_outgoing_queue_usage ) / 10000.0f;
    }

    if ( s_quality_framesize_K3 < 0) s_quality_framesize_K3 = 0;
    else if ( s_quality_framesize_K3 > 1) s_quality_framesize_K3 = 1;
}

//=============================================================================================
//=============================================================================================
IRAM_ATTR void applyAdaptiveQuality()
{
    if ( s_ground2air_config_packet.camera.quality != 0 ) return;

    int quality1 = (int)(8 + (63-8) * ( 1 - s_quality_framesize_K1 * s_quality_framesize_K2 * s_quality_framesize_K3));

    if (s_quality != quality1)
    {
        s_quality = quality1;
        sensor_t* s = esp_camera_sensor_get(); 
        s->set_quality(s, s_quality); 
    }
}

//=============================================================================================
//=============================================================================================
IRAM_ATTR size_t camera_data_available(void * cam_obj,const uint8_t* data, size_t count, bool last)
{
    size_t stride=((cam_obj_t *)cam_obj)->dma_bytes_per_item;

    if ( getOVFFlagAndReset() )
    {
        s_quality_framesize_K1 = 0.05;
        s_quality_framesize_K2 = 0.05;
        applyAdaptiveQuality();
    }

    s_fec_encoder.lock();
    if (data == nullptr) //start frame
    {
        s_video_frame_started = true;        
    }
    else 
    {
        if (!s_video_skip_frame)
        {

#ifdef BOARD_ESP32CAM
            //ESP32 - sample offset: 2, stride: 4
            const uint8_t* src = (const uint8_t*)data + 2;  // jump to the sample1 of DMA element
#endif
#ifdef BOARD_XIAOS3SENSE
            //ESP32S3 - sample offset: 0, stride: 1
            const uint8_t* src = (const uint8_t*)data;
#endif
            count /= stride;
            if (last) //find the end marker for JPEG. Data after that can be discarded
            {
                const uint8_t* dptr = src + (count - 2) * stride;
                while (dptr > src)
                {
                    if (dptr[0] == 0xFF && dptr[stride] == 0xD9)
                    {
                        count = (dptr - src) / stride + 2; //to include the 0xFFD9
                        if ((count & 0x1FF) == 0)
                            count += 1; 
                        if ((count % 100) == 0)
                            count += 1;
                        break;
                    }
                    dptr -= stride;
                }

            }

            while (count > 0)
            {
                if (s_video_frame_data_size >= MAX_VIDEO_DATA_PAYLOAD_SIZE) //flush prev data?
                {
                    //LOG("Flush: %d %d\n", s_video_frame_index, s_video_frame_data_size);
                    send_air2ground_video_packet(false);
                    s_video_frame_data_size = 0;
                    s_video_part_index++;
                }

                //LOG("Add: %d %d %d %d\n", s_video_frame_index, s_video_part_index, count, s_video_frame_data_size);

                //fill the buffer
                uint8_t* packet_data = s_fec_encoder.get_encode_packet_data(true);
                uint8_t* start_ptr = packet_data + sizeof(Air2Ground_Video_Packet) + s_video_frame_data_size;
                uint8_t* ptr = start_ptr;
                size_t c = std::min(MAX_VIDEO_DATA_PAYLOAD_SIZE - s_video_frame_data_size, count);

                count -= c;
                s_video_frame_data_size += c;
                s_video_full_frame_size += c;


                size_t c8 = c >> 3;
                for (size_t i = c8; i > 0; i--)
                {
                    *ptr++ = *src; src += stride;
                    *ptr++ = *src; src += stride;
                    *ptr++ = *src; src += stride;
                    *ptr++ = *src; src += stride;
                    *ptr++ = *src; src += stride;
                    *ptr++ = *src; src += stride;
                    *ptr++ = *src; src += stride;
                    *ptr++ = *src; src += stride;
                }
                for (size_t i = c - (c8 << 3); i > 0; i--)
                {
                    *ptr++ = *src; src += stride;
                }

#ifdef DVR_SUPPORT
                if (s_air_record)
                {
                    add_to_sd_fast_buffer(start_ptr, c);
                }
#endif
            }
        }

        //////////////////

        if (last && s_video_frame_started)
        {
            s_video_frame_started = false;

            //frame pacing!
            int64_t now = esp_timer_get_time();

            int64_t acquire_dt = now - s_video_last_acquired_tp;
            s_video_last_acquired_tp = now;

            int64_t send_dt = now - s_video_last_sent_tp;
            if (send_dt < s_video_target_frame_dt) //limit fps
            {
                s_video_skip_frame = true;
            }
            else                
            {
                s_video_skip_frame = false;
                s_video_last_sent_tp += std::max(s_video_target_frame_dt, acquire_dt);
            }

/*
            //dynamically decrease fps
            if ( s_wlan_outgoing_queue.size() > s_wlan_outgoing_queue.capacity()  * 60 / 100)
            {
                s_video_skip_frame = true;
            }
*/
            //////////////////

            //LOG("Finish: %d %d\n", s_video_frame_index, s_video_frame_data_size);
            if (s_video_frame_data_size > 0) //left over
                send_air2ground_video_packet(true);

            s_video_frame_data_size = 0;
            if ( !s_video_skip_frame) s_video_frame_index++;
            s_video_part_index = 0;
        }

        if ( last && (s_video_full_frame_size > 0))
        {
            recalculateFrameSizeQualityK(s_video_full_frame_size);
            applyAdaptiveQuality();
            s_video_full_frame_size = 0;

            handle_ground2air_config_packetEx2(false);

            if ( g_osd.isChanged() || (s_osdUpdateCounter == 15))
            {
                send_air2ground_osd_packet();
                s_osdUpdateCounter = 0;
            }
            else
            {
                s_osdUpdateCounter++;
            }
        }
    }

    s_fec_encoder.unlock();
    return count;
}

//=============================================================================================
//=============================================================================================
static void init_camera()
{
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
#ifdef SENSOR_OV5640    
    config.xclk_freq_hz = 26000000;  //real frequency will be 80Mhz/3 = 26,666Mhz
#else
    config.xclk_freq_hz = 12000000;  //real frequency will be 80Mhz/7 = 11,428Mhz and we use clk2x
#endif    
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 8;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.data_available_callback = camera_data_available;

    // camera init
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        LOG("Camera init failed with error 0x%x", err);
        return;
    }
}

//#define SHOW_CPU_USAGE

//=============================================================================================
//=============================================================================================
static void print_cpu_usage()
{
#ifdef SHOW_CPU_USAGE
    TaskStatus_t* pxTaskStatusArray;
    volatile UBaseType_t uxArraySize, x;
    uint32_t ulTotalRunTime, ulStatsAsPercentage;

    // Take a snapshot of the number of tasks in case it changes while this
    // function is executing.
    uxArraySize = uxTaskGetNumberOfTasks();
    //LOG("%u tasks\n", uxArraySize);

    // Allocate a TaskStatus_t structure for each task.  An array could be
    // allocated statically at compile time.
    pxTaskStatusArray = (TaskStatus_t*)heap_caps_malloc(uxArraySize * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);

    if (pxTaskStatusArray != NULL)
    {
        // Generate raw status information about each task.
        uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &ulTotalRunTime);
        //LOG("%u total usage\n", ulTotalRunTime);

        // For percentage calculations.
        ulTotalRunTime /= 100UL;

        // Avoid divide by zero errors.
        if (ulTotalRunTime > 0)
        {
            // For each populated position in the pxTaskStatusArray array,
            // format the raw data as human readable ASCII data
            for (x = 0; x < uxArraySize; x++)
            {
                // What percentage of the total run time has the task used?
                // This will always be rounded down to the nearest integer.
                // ulTotalRunTimeDiv100 has already been divided by 100.
                ulStatsAsPercentage = pxTaskStatusArray[x].ulRunTimeCounter / ulTotalRunTime;

                if (ulStatsAsPercentage > 0UL)
                {
                    LOG("%s\t\t%u\t\t%u%%\r\n", pxTaskStatusArray[x].pcTaskName, pxTaskStatusArray[x].ulRunTimeCounter, ulStatsAsPercentage);
                }
                else
                {
                    // If the percentage is zero here then the task has
                    // consumed less than 1% of the total run time.
                    LOG("%s\t\t%u\t\t<1%%\r\n", pxTaskStatusArray[x].pcTaskName, pxTaskStatusArray[x].ulRunTimeCounter);
                }
            }
        }

        // The array is no longer needed, free the memory it consumes.
        free(pxTaskStatusArray);
    }
#endif
}

//=============================================================================================
//=============================================================================================
extern "C" void app_main()
{
    //esp_task_wdt_init();

#ifdef BOARD_XIAOS3SENSE
    vTaskDelay(10000 / portTICK_PERIOD_MS);  //to see init messages
#endif    

    Ground2Air_Data_Packet& ground2air_data_packet = s_ground2air_data_packet;
    ground2air_data_packet.type = Ground2Air_Header::Type::Telemetry;
    ground2air_data_packet.size = sizeof(ground2air_data_packet);

    Ground2Air_Config_Packet& ground2air_config_packet = s_ground2air_config_packet;
    ground2air_config_packet.type = Ground2Air_Header::Type::Config;
    ground2air_config_packet.size = sizeof(ground2air_config_packet);
    ground2air_config_packet.wifi_rate = WIFI_Rate::RATE_G_24M_ODFM;

    srand(esp_timer_get_time());

    printf("Initializing...\n");

    printf("MEMORY at start: \n");
    heap_caps_print_heap_info(MALLOC_CAP_8BIT);

    initialize_status_led();
    initialize_rec_button();

#ifdef DVR_SUPPORT

    void* psb = heap_caps_malloc(SD_SLOW_BUFFER_SIZE_PSRAM, MALLOC_CAP_SPIRAM);
    if ( !!psb )
    {
        s_sd_slow_buffer = new Circular_Buffer( (uint8_t*)psb, SD_SLOW_BUFFER_SIZE_PSRAM);
    }
    else 
    {
        void* psb = new uint8_t[SD_SLOW_BUFFER_SIZE_RAM];
        if ( psb == NULL)
        {
            printf("SD Slow buffer not allocated\n");
            init_failure( );
        }
        s_sd_slow_buffer = new Circular_Buffer( (uint8_t*)psb, SD_SLOW_BUFFER_SIZE_RAM);
    }

#endif

#ifdef INIT_UART_0
    //init debug uart
    uart_config_t uart_config0 = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_APB
    };  
    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config0) );
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, TXD0_PIN, RXD0_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
#endif

    //reinitialize rec button after configuring uarts
    initialize_rec_button();

    nvs_args_init();

    s_ground2air_config_packet.wifi_channel = (uint16_t)nvs_args_read("channel");
    if(s_ground2air_config_packet.wifi_channel > 13)
    {
        s_ground2air_config_packet.wifi_channel = DEFAULT_WIFI_CHANNEL;
        nvs_args_set("channel", DEFAULT_WIFI_CHANNEL);
    }

    LOG("WIFI channel%d\n", s_ground2air_config_packet.wifi_channel );

    setup_wifi(s_ground2air_config_packet.wifi_rate, s_ground2air_config_packet.wifi_channel, s_ground2air_config_packet.wifi_power, packet_received_cb);

#ifdef DVR_SUPPORT
    init_sd();

    vTaskDelay(100 / portTICK_PERIOD_MS);

    if ( getButtonState() ) 
    {
        LOG("Starting file server...");
        vTaskSuspend(s_wifi_rx_task);
        vTaskSuspend(s_wifi_tx_task);
        setup_wifi_file_server();

        while (true)
        {
            vTaskDelay(1);
            //esp_task_wdt_reset();

            update_status_led_file_server();
        }
    }

    {
        int core = tskNO_AFFINITY;
        BaseType_t res = xTaskCreatePinnedToCore(&sd_write_proc, "SD Write", 4096, nullptr, 1, &s_sd_write_task, core);
        if (res != pdPASS)
            LOG("Failed sd write task: %d\n", res);
    }
    {
        int core = tskNO_AFFINITY;
        BaseType_t res = xTaskCreatePinnedToCore(&sd_enqueue_proc, "SD Enq", 1536, nullptr, 1, &s_sd_enqueue_task, core);
        if (res != pdPASS)
            LOG("Failed sd enqueue task: %d\n", res);
    }
#endif

//initialize UART2 after SD
#ifdef INIT_UART_2

    uart_config_t uart_config2 = {
        .baud_rate = UART2_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_APB
    };  
    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config2) );
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, MAX_TELEMETRY_PAYLOAD_SIZE + 1024, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, TXD2_PIN, RXD2_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

#endif

#ifdef INIT_UART_1

    uart_config_t uart_config1 = {
        .baud_rate = UART1_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_APB
    };  
    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config1) );
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, MAX_TELEMETRY_PAYLOAD_SIZE + 1024, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, TXD1_PIN, RXD1_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

#endif


    init_camera();
    printf("MEMORY Before Loop: \n");
    heap_caps_print_heap_info(MALLOC_CAP_8BIT);

    handle_ground2air_config_packetEx1( s_ground2air_config_packet);
    handle_ground2air_config_packetEx2( true);
    set_ground2air_config_packet_handler(handle_ground2air_config_packet);
    set_ground2air_data_packet_handler(handle_ground2air_data_packet);

    while (true)
    {
        int dt = millis() - s_stats_last_tp;
        if (s_uart_verbose > 0 && (dt >= 1000))
        {
            s_max_wlan_outgoing_queue_usage = getMaxWlanOutgoingQueueUsage();
            s_stats_last_tp = millis();
            LOG("WLAN S: %d, R: %d, E: %d, D: %d, %%: %d || FPS: %d, D: %d || SD D: %d, E: %d || TLM IN: %d OUT: %d || SK1: %d SK2: %d, SK3: %d, Q: %d s: %d\n",
                s_stats.wlan_data_sent, s_stats.wlan_data_received, s_stats.wlan_error_count, s_stats.wlan_received_packets_dropped, s_max_wlan_outgoing_queue_usage,
                (int)(s_stats.video_frames)* 1000 / dt, s_stats.video_data, s_stats.sd_data, s_stats.sd_drops, 
                s_stats.in_telemetry_data, s_stats.out_telemetry_data,
                (int)(s_quality_framesize_K1*100),  (int)(s_quality_framesize_K2*100), (int)(s_quality_framesize_K3*100), 
                s_quality, s_max_frame_size); 
            print_cpu_usage();

            s_max_frame_size = 0;

            s_stats = Stats();
        }

        vTaskDelay(10);
        //esp_task_wdt_reset();

        update_status_led();

        checkButton();

#ifdef UART_MAVLINK
        xSemaphoreTake(s_serial_mux, portMAX_DELAY);

        if ( s_video_frame_data_size == 0 &&  s_video_part_index == 0 )
        {
            while ( true )
            {
                size_t rs = 0;
                ESP_ERROR_CHECK( uart_get_buffered_data_len(UART_MAVLINK, &rs) );

                //LOG("%d\n", rs);

                if (rs >= MAX_TELEMETRY_PAYLOAD_SIZE) //TODO: or rs>0 and >agregation time
                {
                    send_air2ground_data_packet();
                }
                else
                {
                    break;
                }
            }
        }

        xSemaphoreGive(s_serial_mux);
#endif

#ifdef UART_MSP_OSD
    g_msp.loop();
#endif

        if ((s_restart_time!=0) && ( esp_timer_get_time()>s_restart_time))
        {
            esp_restart();
        }

    }

}

/*
Air receive:
1) packet_received_cb()- called by Wifi library when wifi packet is received
  - feeds data to s_fec_decoder.decode_data(). No data type checking at all.

2) s_fec_decoder.decode_data()
 - allocates item in m_decoder.packet_pool
 - concatenates small packets until mtu size (because wifi packets may be broken to smaller parts by wifi layer)
 - enquees packets into m_decoder.packet_queue

3) decoder_task_proc()  
 - retrives item from m_decoder.packet_queue
 - inserts either in m_decoder.block_packets or m_decoder.block_fec_packets
 - inserts into s_wlan_incoming_queue either received or restored packets
 - signals s_wifi_rx_task

4) s_wifi_rx_task
  - parses packets: Ground2Air_Header::Type::Config, Ground2Air_Header::Type::Data
 

Air send:
1) camera_data_available callback from camera library
 - send_air2ground_video_packet() - passes Air2Ground_Video_Packet to s_fec_encode.
 - flush_encode_packet() - concatenates data until mtu size and passes to m_encoder.packet_queue
 
 2) encoder_task_proc()
  - gathers packets in m_encoder.block_packets
  - calls fec_encode(()
  - add_to_wlan_outgoing_queue() - places encoded packets into s_wlan_outgoing_queue

3) wifi_tx_proc
 - reads s_wlan_outgoing_queue
 - calls esp_wifi_80211_tx() 

*/