#include <stdio.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MQTT_CONNECTED_BIT  BIT2
#define MQTT_FAIL_BIT       BIT3

// GLOBALS
static const char *TAG = "ESPBRIDGE";

// WIFI
static EventGroupHandle_t       wifi_event_group;
static int retries =            0;
static const int MAX_RETRIES =  5;

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
            ESP_LOGE(TAG, "ran out of retries after %d connection attempts", MAX_RETRIES);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        retries = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void initialize_wifi()
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
}

static void wait_for_wifi()
{
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to SSID: %s", CONFIG_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "failed to connect to SSID: %s", CONFIG_WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "unexpected event");
    }
}

// MQTT
static EventGroupHandle_t mqtt_event_group;
static esp_mqtt_client_handle_t mqtt_client    = NULL;

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == MQTT_EVENT_CONNECTED) {
        xEventGroupSetBits(mqtt_event_group, MQTT_CONNECTED_BIT);
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        xEventGroupSetBits(mqtt_event_group, MQTT_FAIL_BIT);
    }
}


static void initialize_mqtt()
{
    mqtt_event_group = xEventGroupCreate();

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = CONFIG_MQTT_BROKER_ADR
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
}

static void wait_for_mqtt()
{
    EventBits_t bits = xEventGroupWaitBits(mqtt_event_group,
                                           MQTT_CONNECTED_BIT | MQTT_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    
    if (bits & MQTT_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to MQTT broker: %s", CONFIG_MQTT_BROKER_ADR);
    } else if (bits & MQTT_FAIL_BIT) {
        ESP_LOGE(TAG, "failed to connect to MQTT broker: %s", CONFIG_MQTT_BROKER_ADR);
    } else {
        ESP_LOGE(TAG, "unexpected event");
    }
}


void app_main(void)
{
    initialize_wifi();
    wait_for_wifi();

    initialize_mqtt();
    wait_for_mqtt();
}
