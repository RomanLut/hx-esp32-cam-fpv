#include "temperature_sensor.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"

#ifdef BOARD_XIAOS3SENSE

static const char* TAG = "temp_sensor";
static temperature_sensor_handle_t temp_handle = NULL;

//=====================================================================
//=====================================================================
bool temperature_sensor_available()
{
    return true;
}

//=====================================================================
//=====================================================================
void temperature_sensor_init(void)
{
    ESP_LOGI(TAG, "Initializing temperature sensor");
    
    temperature_sensor_config_t temp_sensor = {
        .range_min = 50,
        .range_max = 125,
        .clk_src = TEMPERATURE_SENSOR_CLK_SRC_DEFAULT
    };
    
    esp_err_t ret = temperature_sensor_install(&temp_sensor, &temp_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install temperature sensor");
        return;
    }
    
    ret = temperature_sensor_enable(temp_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable temperature sensor");
        return;
    }
}

//=====================================================================
//=====================================================================
void temperature_sensor_read(float *temperature)
{
    if (temp_handle == NULL) {
        ESP_LOGE(TAG, "Temperature sensor not initialized");
        return;
    }
    
    esp_err_t ret = temperature_sensor_get_celsius(temp_handle, temperature);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read temperature");
        return;
    }
    
    ESP_LOGI(TAG, "Temperature: %.1f °C", *temperature);
}

#endif

#ifdef BOARD_ESP32CAM

//=====================================================================
//=====================================================================
bool temperature_sensor_available()
{
    return false;
}

//=====================================================================
//=====================================================================
void temperature_sensor_init(void)
{
}

//=====================================================================
//=====================================================================
void temperature_sensor_read(float *temperature)
{
    *temperature = 0;
}

#endif
