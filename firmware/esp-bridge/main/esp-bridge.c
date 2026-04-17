#include <stdio.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "esp_now.h"
#include "mqtt_client.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MQTT_CONNECTED_BIT  BIT2
#define MQTT_FAIL_BIT       BIT3

// GLOBALS
static const char *TAG =                         "ESPBRIDGE";
static const uint64_t SLEEP_TIME =               1 * 30 * 1000000ULL;
static EventGroupHandle_t mqtt_event_group;
static esp_mqtt_client_handle_t mqtt_client =    NULL;
static EventGroupHandle_t wifi_event_group;
static int retries =                             0;
static const int MAX_RETRIES =                   5;

// PRE-DEFINED
static void send_data_to_broker(esp_mqtt_client_handle_t mqtt_client, float temperature,
                                float humidity, float pressure);

// WIFI
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "trying to connect to wifi");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retries < MAX_RETRIES) {
            ESP_LOGW(TAG, "trying to reconnect to wifi (retry %d)", (retries + 1));
            esp_wifi_connect();
            retries++;
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "failed to connect after %d connection attempts", MAX_RETRIES);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "connected to SSID: %s", CONFIG_WIFI_SSID);
        retries = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool initialize_wifi()
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_LOGI(TAG, "wifi initialized");

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip)); 
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid =     CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi started, waiting for connection");

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    return ((bits & WIFI_CONNECTED_BIT));
}

// ESPNOW
typedef struct {
    float temperature;
    float humidity;
    float pressure;
} ambient_t;

static void esp_now_receive_callback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    uint8_t *mac_addr = recv_info->src_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "receive callback arg error");
        return;
    }

    if (len < sizeof(ambient_t)) {
        ESP_LOGE(TAG, "received packet too small: %d bytes", len);
        return;
    }

    ambient_t reading = {0};
    memcpy(&reading, data, sizeof(ambient_t));
    send_data_to_broker(mqtt_client, reading.temperature, reading.humidity, reading.pressure);
}

static void initialize_esp_now()
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_receive_callback));
}


// MQTT
static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch(event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected to MQTT broker: %s", CONFIG_MQTT_BROKER_ADDR);
            xEventGroupSetBits(mqtt_event_group, MQTT_CONNECTED_BIT);
            break;
        
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected from MQTT broker: %s", CONFIG_MQTT_BROKER_ADDR);
            xEventGroupClearBits(mqtt_event_group, MQTT_CONNECTED_BIT);
            break;
        
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "message published successfully");
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred");
            xEventGroupSetBits(mqtt_event_group, MQTT_FAIL_BIT);
            break;
        
        default:
            break;
    }
}


static bool initialize_mqtt()
{
    mqtt_event_group = xEventGroupCreate();

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = CONFIG_MQTT_BROKER_ADDR
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_config);

    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "failed to initialize mqtt client");
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler,
                                                   NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));

    EventBits_t bits = xEventGroupWaitBits(mqtt_event_group,
                                           MQTT_CONNECTED_BIT | MQTT_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    return (bits & MQTT_CONNECTED_BIT);
}

static void send_data_to_broker(esp_mqtt_client_handle_t mqtt_client, float temperature,
                                float humidity, float pressure)
{
    char body[128];
    snprintf(body, sizeof(body),
             "{\"temperature\": %.1f, \"humidity\": %.1f, \"pressure\": %.1f}",
             temperature, humidity, pressure);
    
    esp_mqtt_client_publish(mqtt_client, "sensors/ambient", body, 0, 1, 0);
}

void app_main(void)
{   
    if (!initialize_wifi()) {
        ESP_LOGE(TAG, "wifi connection failed, going to sleep...");
        esp_sleep_enable_timer_wakeup(SLEEP_TIME);
        esp_deep_sleep_start();
    }

    initialize_esp_now();

    if (!initialize_mqtt()) {
        ESP_LOGE(TAG, "MQTT connection failed, going to sleep...");
        esp_sleep_enable_timer_wakeup(SLEEP_TIME);
        esp_deep_sleep_start();
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
