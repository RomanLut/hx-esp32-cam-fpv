// Copyright 2010-2025 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "ll_cam.h"
#include "cam_hal.h"
#include "esp_private/periph_ctrl.h"
#include "driver/parlio_rx.h"
#include "driver/parlio_types.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_private/gpio.h"
#include "hal/clk_gate_ll.h"

#define CAM_V_SYNC_IDX 0
#define MIN_FRAME_ALLOC_SIZE 160

//parlio buffer
#define CONFIG_CAMERA_PAYLOAD_BUFFER_SIZE 16384

// Forward declaration for helper function
static bool ll_cam_calc_rgb_dma_sizes(cam_obj_t *cam);

static const char *TAG = "c5 ll_cam";


volatile int pk = 0;
extern volatile int pk2;

// PARLIO partial receive callback - handles JPEG frame detection and buffering
static bool IRAM_ATTR on_partial_receive_callback(parlio_rx_unit_handle_t rx_unit,
                                                            const parlio_rx_event_data_t *edata,
                                                            void *user_ctx) {

    cam_obj_t *cam_obj = (cam_obj_t *)user_ctx;
    const uint8_t *data = edata->data;
    uint32_t received_bytes = edata->recv_bytes;
    BaseType_t HPTaskAwoken = pdFALSE;

    pk++;

    //send event to cam_task
    cam_event_t event = { .kind = CAM_PARLIO_DATA, .data = data, .length = (uint16_t)received_bytes };
    ll_cam_send_event(cam_obj, &event, &HPTaskAwoken);

    return (HPTaskAwoken == pdTRUE);
}

bool IRAM_ATTR ll_cam_stop(cam_obj_t *cam)
{
/*
    if (cam->rx_unit) {
        // Stop PARLIO RX unit
        parlio_rx_unit_disable(cam->rx_unit);
    }
*/
    return true;
}

bool ll_cam_start(cam_obj_t *cam, int frame_pos)
{
    //capture is continuous
    return true;
}

bool ll_cam_start_continuous(cam_obj_t *cam, const camera_config_t *config)
{
    esp_err_t ret = ESP_OK;

    // Configure PARLIO RX unit
    // ESP32-C5 does NOT support valid signals with 8 data lines, so we use software delimiter

    parlio_rx_unit_config_t rx_config = {
        .trans_queue_depth = 8,  //NOTE: does not matter in the infinite transation mode
        .max_recv_size = 0xffff, //CONFIG_CAMERA_PAYLOAD_BUFFER_SIZE,  // Maximum receive size
        .data_width = 8,  // 8 data lines (D0-D7)
        .clk_src = PARLIO_CLK_SRC_EXTERNAL,  // Use external PCLK from camera
        .ext_clk_freq_hz = 120 * 1000 * 1000,  // Expected external clock frequency (high estimate)
        .clk_in_gpio_num = config->pin_pclk,  // PCLK input pin
        .clk_out_gpio_num = -1,  // No clock output
        .valid_gpio_num = -1,  // ESP32-C5 doesn't support valid signal with 8 data lines
        .flags = {
            //NOTE: free_clk does not matter in the infinite transation mode
            .free_clk = 1,       // PCLK is a free-running clock
            .allow_pd = 1,       // Allow power down
        }
    };

    // Copy data GPIO pins
    rx_config.data_gpio_nums[0] = config->pin_d0;
    rx_config.data_gpio_nums[1] = config->pin_d1;
    rx_config.data_gpio_nums[2] = config->pin_d2;
    rx_config.data_gpio_nums[3] = config->pin_d3;
    rx_config.data_gpio_nums[4] = config->pin_d4;
    rx_config.data_gpio_nums[5] = config->pin_d5;
    rx_config.data_gpio_nums[6] = config->pin_d6;
    rx_config.data_gpio_nums[7] = config->pin_d7;

    // Create PARLIO RX unit
    ret = parlio_new_rx_unit(&rx_config, &cam->rx_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create PARLIO RX unit: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create software delimiter (required for ESP32-C5 since no valid signal support)
    parlio_rx_soft_delimiter_config_t delim_config = {
        .sample_edge = PARLIO_SAMPLE_EDGE_NEG,   //BUG in SDK: actually sets rising edge
        .eof_data_len = cam->payload_size,
        .timeout_ticks = 0,  // No timeout
    };

    ret = parlio_new_rx_soft_delimiter(&delim_config, &cam->delimiter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create software delimiter: %s", esp_err_to_name(ret));
        return ret;
    }

    // Allocate payload buffer for PARLIO (internal DMA buffer)
    cam->payload_buffer = heap_caps_malloc(cam->payload_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!cam->payload_buffer) {
        ESP_LOGE(TAG, "Failed to allocate payload buffer");
        return ESP_ERR_NO_MEM;
    }

    // Register RX callbacks
    parlio_rx_event_callbacks_t cbs = {
        .on_partial_receive = on_partial_receive_callback,
    };

    ret = parlio_rx_unit_register_event_callbacks(cam->rx_unit, &cbs, cam);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register RX callbacks: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "PARLIO RX configured: 8-bit data, software delimiter, payload_size=%d", cam->payload_size);


    if (!cam->rx_unit || !cam->payload_buffer) {
        ESP_LOGE(TAG, "Invalid context or payload buffer");
        return false;
    }

    // Enable PARLIO RX unit
    ret = parlio_rx_unit_enable(cam->rx_unit, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable PARLIO RX unit: %s", esp_err_to_name(ret));
        return false;
    }

    // Start software delimiter if in JPEG mode
    if (cam->jpeg_mode && cam->delimiter) {
        ret = parlio_rx_soft_delimiter_start_stop(cam->rx_unit, cam->delimiter, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start software delimiter: %s", esp_err_to_name(ret));
            return false;
        }
    }

    // Start receiving data into payload buffer
    parlio_receive_config_t recv_cfg = {
        .delimiter = cam->delimiter,
        .flags = {
            .partial_rx_en = 1,  // Enable partial receive for continuous data streaming
        }
    };

    // Use PARLIO's internal payload buffer (not camera's DMA buffer)
    ret = parlio_rx_unit_receive(cam->rx_unit,
                                cam->payload_buffer,
                                cam->payload_size,
                                &recv_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start PARLIO RX receive: %s", esp_err_to_name(ret));
        return false;
    }

    return true;
}

esp_err_t ll_cam_deinit(cam_obj_t *cam)
{
    /*
    // Disable and delete PARLIO RX unit
    if (cam->rx_unit) {
        parlio_rx_unit_disable(cam->rx_unit);
        parlio_del_rx_unit(cam->rx_unit);
        cam->rx_unit = NULL;
    }

    // Delete delimiter
    if (cam->delimiter) {
        parlio_del_rx_delimiter(cam->delimiter);
        cam->delimiter = NULL;
    }

    // Free payload buffer
    if (cam->payload_buffer) {
        free(cam->payload_buffer);
        cam->payload_buffer = NULL;
    }
*/
    return ESP_OK;
}


esp_err_t ll_cam_config(cam_obj_t *cam, const camera_config_t *config)
{
    esp_err_t ret = ESP_OK;

    // Store camera parameters
    cam->jpeg_mode = (config->pixel_format == PIXFORMAT_JPEG);
    cam->payload_size = CONFIG_CAMERA_PAYLOAD_BUFFER_SIZE;

    cam->jpeg_state.capture = false;
    cam->jpeg_state.dma_buffer_index = 0;
    cam->jpeg_state.index = 0;  
    cam->jpeg_state.frame_length = 0;
    cam->jpeg_state.last_byte = 0;

    return ESP_OK;
}

void ll_cam_vsync_intr_enable(cam_obj_t *cam, bool en)
{
    // VSYNC interrupt is not directly handled by PARLIO, but can be implemented via GPIO
    // For now, this is a placeholder since VSYNC can be handled in the main application
    ESP_LOGI(TAG, "VSYNC interrupt %s", en ? "enabled" : "disabled");
}

esp_err_t ll_cam_set_pin(cam_obj_t *cam, const camera_config_t *config)
{
    // PARLIO RX configures the pins internally, but we need to configure XCLK and VSYNC manually
    // since they are not part of PARLIO RX configuration

/*
 LEDC configures pin itself
    // XCLK (camera input clock) - configure as GPIO output if specified
    if (config->pin_xclk >= 0) 
    {
        gpio_config_t io_conf = { 0 };
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << config->pin_xclk;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
    }
*/
/*
    // VSYNC pin - configure as GPIO input (used for frame synchronization in application code)
    //VSYNC is not used in current implementation
    if (config->pin_vsync >= 0) {
        gpio_config_t vsync_conf = {
            .pin_bit_mask = (1ULL << config->pin_vsync),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&vsync_conf);
    }

*/
    // Note: PCLK, HREF, and data pins (D0-D7) are configured automatically by PARLIO RX

    return ESP_OK;
}

esp_err_t ll_cam_init_isr(cam_obj_t *cam)
{
    // PARLIO RX handles interrupts internally through callbacks
    // No additional ISR setup needed as interrupts are handled by PARLIO driver
    return ESP_OK;
}

void ll_cam_do_vsync(cam_obj_t *cam)
{
}

uint8_t ll_cam_get_dma_align(cam_obj_t *cam)
{
    // For PARLIO, DMA alignment is 16 bytes (default)
    return 16;
}

bool ll_cam_dma_sizes(cam_obj_t *cam)
{
    cam->dma_bytes_per_item = 1;
    return 1;
}

size_t IRAM_ATTR ll_cam_memcpy(cam_obj_t *cam, uint8_t *out, const uint8_t *in, size_t len)
{
    // YUV422 to YUV420 conversion
    if (cam->in_bytes_per_pixel == 2 && cam->fb_bytes_per_pixel == 1) {
        size_t end = len / 8;
        for (size_t i = 0; i < end; ++i) {
            out[0] = in[0];
            out[1] = in[2];
            out[2] = in[4];
            out[3] = in[6];
            out += 4;
            in += 8;
        }
        return len / 2;
    }

    // Standard memcpy
    memcpy(out, in, len);
    return len;
}

esp_err_t ll_cam_set_sample_mode(cam_obj_t *cam, pixformat_t pix_format, uint32_t xclk_freq_hz, uint16_t sensor_pid)
{
    // Set pixel format handling (similar to ESP32S3)
    if (pix_format == PIXFORMAT_GRAYSCALE) {
        // Determine bytes per pixel based on sensor
        // Implementation similar to ESP32S3
        cam->fb_bytes_per_pixel = 1;
    } else if (pix_format == PIXFORMAT_YUV422 || pix_format == PIXFORMAT_RGB565) {
        cam->in_bytes_per_pixel = 2;
        cam->fb_bytes_per_pixel = 2;
    } else if (pix_format == PIXFORMAT_JPEG) {
        cam->in_bytes_per_pixel = 1;
        cam->fb_bytes_per_pixel = 1;
    } else {
        ESP_LOGE(TAG, "Requested format is not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

// Helper function for DMA calculation (similar to ESP32S3)
static bool ll_cam_calc_rgb_dma_sizes(cam_obj_t *cam) {
    return 1;
}

static void ll_cam_dma_reset(cam_obj_t *cam)
{
}
