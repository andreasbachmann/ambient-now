#include <stdio.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"

static const char *TAG = "ESPBRIDGE";

static void initialize_wifi()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "default event loop created");
}

void app_main(void)
{
    initialize_wifi();
}
