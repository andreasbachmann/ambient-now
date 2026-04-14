#include <stdio.h>
#include <stdint.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_now.h"
#include "i2c_bus.h"
#include "bme280.h"


// CONSTANTS
static uint8_t bridge_mac[6];
static const char *TAG =                "ESPSENDER";
static const uint64_t SLEEP_TIME =      1 * 30 * 1000000ULL;
static const int I2C_MASTER_FREQ_HZ =   100000;

// PARSE MAC ADR
static void parse_MAC()
{
    sscanf(CONFIG_BRIDGE_MAC_ADR, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &bridge_mac[0], &bridge_mac[1], &bridge_mac[2],
           &bridge_mac[3], &bridge_mac[4], &bridge_mac[5]);
}

// WIFI

static void initialize_wifi()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // TODO: hardcoded channel 8 for now
    ESP_ERROR_CHECK(esp_wifi_set_channel(8, WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG, "wifi initialized for ESPNOW");
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ESPNOW
static void initialize_espnow()
{
    ESP_ERROR_CHECK(esp_now_init());

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, bridge_mac, 6);

    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "added bridge ESP with MAC " MACSTR, MAC2STR(bridge_mac));
}

// I2C
static i2c_bus_handle_t initialize_i2c()
{
    i2c_config_t config = {
        .mode =             I2C_MODE_MASTER,
        .sda_io_num =       CONFIG_SDA_PIN_NUM,
        .sda_pullup_en =    GPIO_PULLUP_ENABLE,
        .scl_io_num =       CONFIG_SCL_PIN_NUM,
        .scl_pullup_en =    GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };

    return (i2c_bus_create(I2C_NUM_0, &config));
}

void app_main(void)
{
    parse_MAC();
    initialize_wifi();
    initialize_espnow();
    
    i2c_bus_handle_t i2c_bus = initialize_i2c();

    esp_sleep_enable_timer_wakeup(SLEEP_TIME);
    esp_deep_sleep_start();
}
