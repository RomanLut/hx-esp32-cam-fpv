#include "nvs_args.h"
#include "freertos/FreeRTOS.h" 
#include "freertos/semphr.h"   

nvs_handle_t nvs_handler;
static SemaphoreHandle_t s_nvs_mux = NULL; 

//=============================================================================================
//=============================================================================================
esp_err_t nvs_args_init()
{
    printf("Init NVS...\n");

    s_nvs_mux = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    return nvs_open("storage", NVS_READWRITE, &nvs_handler);
}

//=============================================================================================
//=============================================================================================
uint32_t nvs_args_read(const char *key, uint32_t defaultValue)
{
    uint32_t result = defaultValue;
    xSemaphoreTake(s_nvs_mux, portMAX_DELAY);
    nvs_get_u32(nvs_handler,key,&result);
    xSemaphoreGive(s_nvs_mux);
    return result;
}

//=============================================================================================
//=============================================================================================
esp_err_t nvs_args_set(const char *key,uint32_t value)
{
    esp_err_t ret = 0;
    xSemaphoreTake(s_nvs_mux, portMAX_DELAY);
    ret = nvs_set_u32(nvs_handler,key,value);
    ret |= nvs_commit(nvs_handler);
    xSemaphoreGive(s_nvs_mux);
    return ret;
}