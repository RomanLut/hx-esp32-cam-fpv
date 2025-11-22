#include "mock_camera.h"
#include <esp_timer.h>
#include "esp_camera.h"
#include <cstring>
#include "main.h"

typedef struct {
    size_t dma_bytes_per_item;
} mock_cam_obj_t;

extern size_t camera_data_available(void * cam_obj, const uint8_t* data, size_t count, bool last);

static int64_t last_sent_time = 0;
static size_t current_offset = 0;
static bool frame_started = false;

static int mock_noop(sensor_t*, int) { return 0; }
static int mock_noop_pll(sensor_t*, int, int, int, int, int, int, int, int) { return 0; }
static int mock_noop_xclk(sensor_t*, int, int) { return 0; }
static int mock_noop_res_raw(sensor_t*, int, int, int, int, int, int, int, int, int, int, bool, bool) { return 0; }
static int mock_noop_gc(sensor_t*, gainceiling_t) { return 0; }

static sensor_t mock_sensor;
static bool initialized = false;

void init_mock_camera()
{
    last_sent_time = esp_timer_get_time();
    current_offset = 0;
    frame_started = false;
}

void get_mock_camera_frame(const uint8_t** out_buf, size_t* out_len)
{
    extern const unsigned char jpg_start[] asm("_binary_nosignal_jpg_start");
    extern const unsigned char jpg_end[]   asm("_binary_nosignal_jpg_end");
    const size_t jpg_size = (jpg_end - jpg_start);

    *out_buf = jpg_start;
    *out_len = jpg_size;
}

void mock_camera_process()
{
    
    int64_t current_time = esp_timer_get_time();
    int64_t time_diff = current_time - last_sent_time;

    if (time_diff >= 1000)  //1ms
    {
        last_sent_time = current_time;

        const uint8_t* jpg_data = nullptr;
        size_t jpg_size = 0;
        get_mock_camera_frame(&jpg_data, &jpg_size);

        mock_cam_obj_t mock_cam_obj;
        mock_cam_obj.dma_bytes_per_item = 1;

        // Send start of frame if not started
        if (!frame_started)
        {
            camera_data_available(&mock_cam_obj, nullptr, 0, false);
            frame_started = true;
        }

        // Send next 1024 bytes or remaining data
        size_t remaining = jpg_size - current_offset;
        size_t data_to_send = (remaining < 1024) ? remaining : 1024;
        bool is_last = (current_offset + data_to_send >= jpg_size);

        camera_data_available(&mock_cam_obj, jpg_data + current_offset, data_to_send, is_last);

        current_offset += data_to_send;

        // Reset for next frame if this was the last block
        if (is_last)
        {
            current_offset = 0;
            frame_started = false;
        }
    }
    
}

sensor_t* mock_camera_sensor_get()
{
    if (!initialized) {
        memset(&mock_sensor, 0, sizeof(sensor_t));
        mock_sensor.set_framesize = reinterpret_cast< int (*)(sensor_t*, framesize_t) >(mock_noop);
        mock_sensor.set_quality = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_brightness = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_contrast = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_saturation = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_sharpness = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_denoise = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_gainceiling = reinterpret_cast< int (*)(sensor_t*, gainceiling_t) >(mock_noop_gc);
        mock_sensor.set_whitebal = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_awb_gain = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_wb_mode = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_gain_ctrl = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_agc_gain = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_exposure_ctrl = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_aec_value = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_aec2 = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_ae_level = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_hmirror = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_vflip = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_special_effect = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_dcw = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_bpc = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_wpc = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_raw_gma = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_lenc = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_res_raw = mock_noop_res_raw;
        mock_sensor.set_pll = reinterpret_cast< int (*)(sensor_t*, int, int, int, int, int, int, int, int) >(mock_noop_pll);
        mock_sensor.set_colorbar = reinterpret_cast< int (*)(sensor_t*, int) >(mock_noop);
        mock_sensor.set_xclk = reinterpret_cast< int (*)(sensor_t*, int, int) >(mock_noop_xclk);
        initialized = true;
    }
    return &mock_sensor;
}
