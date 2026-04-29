#include <stdio.h>
#include <stdint.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_now.h"
#include "driver/i2c_master.h"
#include "bme280.h"
#include "freertos/semphr.h"

#define BME280_ADDR         0x76
#define BME280_SCL_SPEED    100000
#define DEFAULT_CHANNEL     1

// CONSTANTS
static uint8_t bridge_mac[6];
static const char *TAG =                    "ESPSENSOR";
static const uint64_t SLEEP_TIME =          2 * 60 * 1000000ULL;
static bool last_send_success =             false;

static SemaphoreHandle_t send_semaphore =   NULL;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t bme280_dev;



// PARSE MAC ADR
static void parse_MAC(void)
{
    sscanf(CONFIG_BRIDGE_MAC_ADR, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &bridge_mac[0], &bridge_mac[1], &bridge_mac[2],
           &bridge_mac[3], &bridge_mac[4], &bridge_mac[5]);
}

// NVS CHANNEL INFORMATION
static uint8_t load_channel(void)
{
    nvs_handle_t handle;
    uint8_t channel = DEFAULT_CHANNEL;

    if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "channel", &channel);
        nvs_close(handle);
    }
    return channel;
}

static void save_channel(uint8_t channel)
{
    nvs_handle_t handle;
    if(nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "channel", channel);
        nvs_commit(handle);
        nvs_close(handle);
    }
}


// WIFI
static void initialize_wifi(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi initialized for ESPNOW");
    vTaskDelay(pdMS_TO_TICKS(100));
}


// ESPNOW
static void esp_now_callback(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    last_send_success = (status == ESP_NOW_SEND_SUCCESS);
    xSemaphoreGive(send_semaphore);
}

static void initialize_espnow(void)
{
    ESP_ERROR_CHECK(esp_now_init());

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, bridge_mac, 6);

    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_LOGI(TAG, "added bridge ESP with MAC " MACSTR, MAC2STR(bridge_mac));
    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_callback));
}

static bool send_on_channel(uint8_t channel, ambient_t *reading)
{
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_now_send(bridge_mac, (uint8_t*)reading, sizeof(ambient_t));
    xSemaphoreTake(send_semaphore, portMAX_DELAY);
    return last_send_success;
}

static void send_reading(ambient_t *reading)
{
    send_semaphore = xSemaphoreCreateBinary();
    uint8_t known_channel = load_channel();

    if (send_on_channel(known_channel, reading)) {
        ESP_LOGI(TAG, "sent on channel %d", known_channel);
        return;
    }

    ESP_LOGW(TAG, "send failed, scanning...");
    for (uint8_t ch = 1; ch <= 13; ch++) {
        if (ch == known_channel) continue;
        if (send_on_channel(ch, reading)) {
            ESP_LOGI(TAG, "found bridge on channel %d, saving", ch);
            save_channel(ch);
            return;
        }
    }
    ESP_LOGE(TAG, "bridge not found on any channel");
}


// I2C
static void init_i2c(void)
{
    i2c_master_bus_config_t i2c_bus_cfg = {
        .clk_source                     = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt              = 7,
        .i2c_port                       = I2C_NUM_0,
        .sda_io_num                     = CONFIG_SDA_PIN_NUM,
        .scl_io_num                     = CONFIG_SCL_PIN_NUM,
        .flags.enable_internal_pullup   = true
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));
}

// BME280
static void add_bme_dev(void)
{
    i2c_device_config_t bme280_dev_cfg = {
        .dev_addr_length        = I2C_ADDR_BIT_LEN_7,
        .device_address         = BME280_ADDR,
        .scl_speed_hz           = BME280_SCL_SPEED
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &bme280_dev_cfg, &bme280_dev));
}


void app_main(void)
{
    parse_MAC();
    initialize_wifi();
    initialize_espnow();
    
    init_i2c();
    add_bme_dev();

    esp_err_t ret = init_bme280(bme280_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init failed");
        return;
    }

    ambient_t reading = {0};
    ret = read_bme280(&reading);

    if (ret == ESP_OK) {
        send_reading(&reading);
    }

    esp_sleep_enable_timer_wakeup(SLEEP_TIME);
    esp_deep_sleep_start();
}