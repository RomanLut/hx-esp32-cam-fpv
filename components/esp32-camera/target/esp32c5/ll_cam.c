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
#include "freertos/FreeRTOS.h"
#include "ll_cam.h"
#include "cam_hal.h"
#include "esp_private/periph_ctrl.h"
#include "driver/parlio_rx.h"
#include "driver/parlio_types.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"
// #include "soc/rdma_struct.h"
// #include "soc/rdma_periph.h"
// #include "soc/rdma_reg.h"
#include "hal/clk_gate_ll.h"

#define CAM_V_SYNC_IDX 0

// Forward declaration for helper function
static bool ll_cam_calc_rgb_dma_sizes(cam_obj_t *cam);

static const char *TAG = "c5 ll_cam";

// ESP32C5 camera context using PARLIO RX
typedef struct {
    parlio_rx_unit_handle_t rx_unit;           /*!< PARLIO RX unit handle */
    parlio_rx_delimiter_handle_t delimiter;    /*!< Software delimiter for JPEG parsing */
    uint8_t *recv_buf;                         /*!< Receive buffer */
    size_t buf_size;                           /*!< Buffer size */
    size_t received_bytes;                     /*!< Bytes received in current frame */
    bool frame_started;                        /*!< Whether we've found SOI marker */
    QueueHandle_t event_queue;                 /*!< Event queue for camera events */
    TaskHandle_t task_handle;                  /*!< Camera task handle */
    intr_handle_t intr_handle;                 /*!< Interrupt handle */
    bool jpeg_mode;                            /*!< JPEG mode flag */
    uint16_t width;                            /*!< Image width */
    uint16_t height;                           /*!< Image height */
    uint8_t in_bytes_per_pixel;                /*!< Input bytes per pixel */
    uint8_t fb_bytes_per_pixel;                 /*!< Frame buffer bytes per pixel */
} esp32c5_cam_context_t;

// Global camera context
static esp32c5_cam_context_t *s_cam_ctx = NULL;

// JPEG frame parsing function
static bool IRAM_ATTR ll_cam_check_jpeg_frame(const uint8_t *data, size_t len, bool *frame_started, bool *frame_complete) {
    *frame_complete = false;

    for (size_t i = 0; i < len - 1; i++) {
        // Look for SOI marker (0xFF 0xD8)
        if (data[i] == 0xFF && data[i + 1] == 0xD8) {
            *frame_started = true;
        }
        // Look for EOI marker (0xFF 0xD9)
        else if (*frame_started && data[i] == 0xFF && data[i + 1] == 0xD9) {
            *frame_complete = true;
            break;
        }
    }

    return *frame_started;
}

// PARLIO RX callbacks
static bool CAMERA_ISR_IRAM_ATTR ll_cam_parlio_rx_callback(parlio_rx_unit_handle_t rx_unit,
                                                          const parlio_rx_event_data_t *edata,
                                                          void *user_ctx) {
    cam_obj_t *cam = (cam_obj_t *)user_ctx;
    BaseType_t HPTaskAwoken = pdFALSE;

    if (s_cam_ctx->jpeg_mode) {
        // JPEG mode: parse received data for complete frames
        if (edata->recv_bytes > 1) {
            bool frame_complete = false;
            s_cam_ctx->frame_started = ll_cam_check_jpeg_frame(edata->data,
                                                             edata->recv_bytes,
                                                             &s_cam_ctx->frame_started,
                                                             &frame_complete);

            // Only send frame complete event when we've found the EOI marker
            if (frame_complete) {
                s_cam_ctx->received_bytes += edata->recv_bytes;
                ll_cam_send_event(cam, CAM_IN_SUC_EOF_EVENT, &HPTaskAwoken);
                // Reset frame state for next frame
                s_cam_ctx->frame_started = false;
                s_cam_ctx->received_bytes = 0;
            } else {
                // Continue receiving more data
                s_cam_ctx->received_bytes += edata->recv_bytes;
            }
        }
    } else {
        // RGB/YUV mode: send event on every receive completion
        if (edata->recv_bytes > 0) {
            ll_cam_send_event(cam, CAM_IN_SUC_EOF_EVENT, &HPTaskAwoken);
        }
    }

    return (HPTaskAwoken == pdTRUE);
}

static bool CAMERA_ISR_IRAM_ATTR ll_cam_parlio_timeout_callback(parlio_rx_unit_handle_t rx_unit,
                                                               const parlio_rx_event_data_t *edata,
                                                               void *user_ctx) {
    cam_obj_t *cam = (cam_obj_t *)user_ctx;
    BaseType_t HPTaskAwoken = pdFALSE;

    if (s_cam_ctx->jpeg_mode) {
        // For JPEG, timeout might indicate end of frame in some cases
        if (s_cam_ctx->received_bytes > 0) {
            ESP_LOGW(TAG, "JPEG frame timeout, received %u bytes", s_cam_ctx->received_bytes);
            s_cam_ctx->frame_started = false;
            s_cam_ctx->received_bytes = 0;
        }
    } else {
        // Handle timeout (could be end of frame in some cases)
        ll_cam_send_event(cam, CAM_IN_SUC_EOF_EVENT, &HPTaskAwoken);
    }

    return (HPTaskAwoken == pdTRUE);
}

bool IRAM_ATTR ll_cam_stop(cam_obj_t *cam)
{
    if (s_cam_ctx && s_cam_ctx->rx_unit) {
        // Stop PARLIO RX unit
        parlio_rx_unit_disable(s_cam_ctx->rx_unit);
    }

    return true;
}

bool ll_cam_start(cam_obj_t *cam, int frame_pos)
{
    esp_err_t ret = ESP_OK;

    if (!s_cam_ctx || !s_cam_ctx->rx_unit) {
        return false;
    }

    // Enable PARLIO RX unit
    ret = parlio_rx_unit_enable(s_cam_ctx->rx_unit, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable PARLIO RX unit: %s", esp_err_to_name(ret));
        return false;
    }

    // Start receiving data
    parlio_receive_config_t recv_cfg = {
        .delimiter = s_cam_ctx->delimiter,
        .flags = {
            .partial_rx_en = 1,  // Enable partial receive for continuous data
        }
    };

    // Use camera's DMA buffer for receiving data
    uint8_t *dma_buffer = (uint8_t *)cam->dma_buffer;
    size_t dma_size = cam->dma_buffer_size;

    ret = parlio_rx_unit_receive(s_cam_ctx->rx_unit,
                                dma_buffer,
                                dma_size,
                                &recv_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start PARLIO RX receive: %s", esp_err_to_name(ret));
        return false;
    }

    return true;
}

esp_err_t ll_cam_deinit(cam_obj_t *cam)
{
    if (s_cam_ctx) {
        // Disable and delete PARLIO RX unit
        if (s_cam_ctx->rx_unit) {
            parlio_rx_unit_disable(s_cam_ctx->rx_unit);
            parlio_del_rx_unit(s_cam_ctx->rx_unit);
            s_cam_ctx->rx_unit = NULL;
        }

        // Delete delimiter
        if (s_cam_ctx->delimiter) {
            parlio_del_rx_delimiter(s_cam_ctx->delimiter);
            s_cam_ctx->delimiter = NULL;
        }

        // Free receive buffer
        if (s_cam_ctx->recv_buf) {
            free(s_cam_ctx->recv_buf);
            s_cam_ctx->recv_buf = NULL;
        }

        // Free context
        free(s_cam_ctx);
        s_cam_ctx = NULL;
    }

    return ESP_OK;
}

// DMA initialization function (not needed for PARLIO)
static esp_err_t ll_cam_dma_init(cam_obj_t *cam)
{
    // PARLIO handles DMA internally, no separate initialization needed
    return ESP_OK;
}

esp_err_t ll_cam_config(cam_obj_t *cam, const camera_config_t *config)
{
    esp_err_t ret = ESP_OK;

    // Allocate camera context if not already done
    if (!s_cam_ctx) {
        s_cam_ctx = (esp32c5_cam_context_t *)heap_caps_calloc(1, sizeof(esp32c5_cam_context_t), MALLOC_CAP_DEFAULT);
        if (!s_cam_ctx) {
            ESP_LOGE(TAG, "Failed to allocate camera context");
            return ESP_ERR_NO_MEM;
        }
    }

    // Store camera parameters
    s_cam_ctx->jpeg_mode = (config->pixel_format == PIXFORMAT_JPEG);
    s_cam_ctx->width = config->frame_size;  // This needs to be converted from frame_size enum
    s_cam_ctx->height = config->frame_size; // This needs to be converted from frame_size enum
    s_cam_ctx->in_bytes_per_pixel = 1;      // Default, will be updated later
    s_cam_ctx->fb_bytes_per_pixel = 1;      // Default, will be updated later

    // Configure PARLIO RX unit
    parlio_rx_unit_config_t rx_config = {
        .trans_queue_depth = 8,
        .max_recv_size = cam->dma_buffer_size,
        .data_width = 8,  // 8 data lines (D0-D7)
        .clk_src = PARLIO_CLK_SRC_EXTERNAL,  // Use external PCLK
        .ext_clk_freq_hz = 0,  // 0 for external clock (PCLK from camera)
        .exp_clk_freq_hz = config->xclk_freq_hz,  // Expected sample frequency
        .data_gpio_nums = {
            config->pin_d0, config->pin_d1, config->pin_d2, config->pin_d3,
            config->pin_d4, config->pin_d5, config->pin_d6, config->pin_d7,
            -1, -1, -1, -1, -1, -1, -1, -1  // Pad to 16
        },
        .valid_gpio_num = config->pin_href,  // HREF as valid signal
        .flags = {
            .free_clk = 1,       // PCLK is a free-running clock
            .allow_pd = 1,       // Allow power down
        }
    };

    // Create PARLIO RX unit
    ret = parlio_new_rx_unit(&rx_config, &s_cam_ctx->rx_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create PARLIO RX unit: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create level delimiter (for now, using the same config for both modes)
    parlio_rx_level_delimiter_config_t delim_config = {
        .valid_sig_line_id = 8,  // Use one of the extra lines for valid signal (arbitrary choice)
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,  // Sample on rising edge
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = s_cam_ctx->jpeg_mode ? 100000 : cam->dma_half_buffer_size,  // Large for JPEG, specific for RGB
        .timeout_ticks = 1000,  // Timeout in APB clock ticks
        .flags = {
            .active_low_en = 0,  // Active high HREF
        }
    };

    ret = parlio_new_rx_level_delimiter(&delim_config, &s_cam_ctx->delimiter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create level delimiter: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register RX callbacks
    parlio_rx_event_callbacks_t cbs = {
        .on_receive_done = ll_cam_parlio_rx_callback,
        .on_timeout = ll_cam_parlio_timeout_callback,
    };

    ret = parlio_rx_unit_register_event_callbacks(s_cam_ctx->rx_unit, &cbs, cam);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register RX callbacks: %s", esp_err_to_name(ret));
        return ret;
    }

    // Store buffer info for reference (PARLIO uses camera's DMA buffer directly)
    s_cam_ctx->buf_size = cam->dma_buffer_size;

    cam->vsync_pin = config->pin_vsync;
    cam->vsync_invert = 0;

    ret = ll_cam_dma_init(cam);
    if (ret != ESP_OK) {
        return ret;
    }

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

    // XCLK (camera input clock) - configure as GPIO output if specified
    if (config->pin_xclk >= 0) {
        gpio_config_t xclk_conf = {
            .pin_bit_mask = (1ULL << config->pin_xclk),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&xclk_conf);
        gpio_set_level(config->pin_xclk, 0);  // Start with low level
    }

    // VSYNC pin - configure as GPIO input (used for frame synchronization in application code)
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
    // Placeholder for VSYNC signaling on ESP32C5
    // gpio_matrix_in is not available; use GPIO directly if needed
    esp_rom_delay_us(10);
}

uint8_t ll_cam_get_dma_align(cam_obj_t *cam)
{
    // For PARLIO, DMA alignment is 16 bytes (default)
    return 16;
}

bool ll_cam_dma_sizes(cam_obj_t *cam)
{
    cam->dma_bytes_per_item = 1;

    if (cam->jpeg_mode) {
        if (cam->psram_mode) {
            cam->dma_buffer_size = cam->recv_size;
            cam->dma_half_buffer_size = 1024;
            cam->dma_half_buffer_cnt = cam->dma_buffer_size / cam->dma_half_buffer_size;
            cam->dma_node_buffer_size = cam->dma_half_buffer_size;
        } else {
            cam->dma_half_buffer_cnt = 16;
            cam->dma_buffer_size = cam->dma_half_buffer_cnt * 1024;
            cam->dma_half_buffer_size = cam->dma_buffer_size / cam->dma_half_buffer_cnt;
            cam->dma_node_buffer_size = cam->dma_half_buffer_size;
        }
    } else {
        // RGB/YUV modes calculation (similar to ESP32S3)
        return ll_cam_calc_rgb_dma_sizes(cam);
    }
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
    size_t node_max = LCD_CAM_DMA_NODE_BUFFER_MAX_SIZE / cam->dma_bytes_per_item;
    size_t line_width = cam->width * cam->in_bytes_per_pixel;
    size_t node_size = node_max;
    size_t nodes_per_line = 1;
    size_t lines_per_node = 1;

    // Calculate DMA Node Size
    if(line_width >= node_max){
        // One or more nodes per line
        for(size_t i = node_max; i > 0; i=i-1){
            if ((line_width % i) == 0) {
                node_size = i;
                nodes_per_line = line_width / node_size;
                break;
            }
        }
    } else {
        // One or more lines per node
        for(size_t i = node_max; i > 0; i=i-1){
            if ((i % line_width) == 0) {
                node_size = i;
                lines_per_node = node_size / line_width;
                while((cam->height % lines_per_node) != 0){
                    lines_per_node = lines_per_node - 1;
                    node_size = lines_per_node * line_width;
                }
                break;
            }
        }
    }

    cam->dma_node_buffer_size = node_size * cam->dma_bytes_per_item;

    size_t dma_half_buffer_max = CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX / 2 / cam->dma_bytes_per_item;
    if (line_width > dma_half_buffer_max) {
        ESP_LOGE(TAG, "Resolution too high");
        return 0;
    }

    // Calculate minimum and maximum EOF sizes
    size_t dma_half_buffer_min = node_size * nodes_per_line;
    size_t dma_half_buffer = (dma_half_buffer_max / dma_half_buffer_min) * dma_half_buffer_min;

    // Adjust for height constraints
    size_t lines_per_half_buffer = dma_half_buffer / line_width;
    while((cam->height % lines_per_half_buffer) != 0){
        dma_half_buffer = dma_half_buffer - dma_half_buffer_min;
        lines_per_half_buffer = dma_half_buffer / line_width;
    }

    // Calculate DMA buffer size
    size_t dma_buffer_max = 2 * dma_half_buffer_max;
    if (cam->psram_mode) {
        dma_buffer_max = cam->recv_size / cam->dma_bytes_per_item;
    }
    size_t dma_buffer_size = dma_buffer_max;
    if (!cam->psram_mode) {
        dma_buffer_size =(dma_buffer_max / dma_half_buffer) * dma_half_buffer;
    }

    cam->dma_buffer_size = dma_buffer_size * cam->dma_bytes_per_item;
    cam->dma_half_buffer_size = dma_half_buffer * cam->dma_bytes_per_item;
    cam->dma_half_buffer_cnt = cam->dma_buffer_size / cam->dma_half_buffer_size;

    return 1;
}

static void ll_cam_dma_reset(cam_obj_t *cam)
{
    // PARLIO handles DMA internally, no separate reset needed
}
