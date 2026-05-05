#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT  BIT0
#define MQTT_CONNECTED_BIT  BIT0

static const char *TAG = "ESPBRIDGE";

// WIFI
static EventGroupHandle_t s_wifi_event_group;
static volatile bool s_wifi_connected = false;
static volatile int s_backoff_seconds = 1;
static const int BACKOFF_MAX_SECONDS = 30;
static volatile TickType_t s_disconnect_started_tick = 0;
static const int LONG_OUTAGE_REBOOT_SECONDS = 600;
static TaskHandle_t s_reconnect_task = NULL;

// MQTT
static EventGroupHandle_t s_mqtt_event_group;
static esp_mqtt_client_handle_t mqtt_client = NULL;

// ESP-NOW
typedef struct {
    float temperature;
    float humidity;
    float pressure;
} ambient_t;
static QueueHandle_t ambient_queue;

// PRE-DEFINED
static void send_data_to_broker(float temperature, float humidity, float pressure);

// WIFI
static void wifi_reconnect_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGW(TAG, "disconnected, waiting %ds before retry", s_backoff_seconds);
        vTaskDelay(pdMS_TO_TICKS(s_backoff_seconds * 1000));

        if (s_disconnect_started_tick != 0) {
            TickType_t elapsed = xTaskGetTickCount() - s_disconnect_started_tick;
            int elapsed_seconds = elapsed * portTICK_PERIOD_MS / 1000;
            if (elapsed_seconds > LONG_OUTAGE_REBOOT_SECONDS) {
                ESP_LOGE(TAG, "wifi down for %ds, rebooting", elapsed_seconds);
                esp_restart();
            }
        }

        s_backoff_seconds *= 2;
        if (s_backoff_seconds > BACKOFF_MAX_SECONDS) {
            s_backoff_seconds = BACKOFF_MAX_SECONDS;
        }

        ESP_LOGI(TAG, "retrying connect");
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "trying to connect to wifi");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_connected) {
            s_disconnect_started_tick = xTaskGetTickCount();
        }
        s_wifi_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_reconnect_task) {
            xTaskNotifyGive(s_reconnect_task);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "connected to SSID: %s", CONFIG_WIFI_SSID);
        s_wifi_connected = true;
        s_backoff_seconds = 1;
        s_disconnect_started_tick = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void initialize_wifi(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_LOGI(TAG, "wifi initialized");

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid =     CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    xTaskCreatePinnedToCore(wifi_reconnect_task, "wifi_reconnect", 4096,
                            NULL, 4, &s_reconnect_task, 0);

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi started, waiting for first connection");
    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdFALSE,
                        portMAX_DELAY);
}

// ESPNOW
static void send_data_task(void *parameters)
{
    while (1) {
        ambient_t reading = {0};
        if (xQueueReceive(ambient_queue, &reading, portMAX_DELAY) == pdTRUE) {
            send_data_to_broker(reading.temperature, reading.humidity, reading.pressure);
        }
    }
}

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
    xQueueSend(ambient_queue, &reading, 0);
}

static void initialize_esp_now(void)
{
    ambient_queue = xQueueCreate(10, sizeof(ambient_t));
    xTaskCreate(send_data_task, "send_data", 2048, NULL, 1, NULL);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_receive_callback));
}

// MQTT
static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected to MQTT broker: %s", CONFIG_MQTT_BROKER_ADDR);
            xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected from MQTT broker (auto-reconnecting)");
            xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "message published successfully");
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error (client will retry)");
            break;

        default:
            break;
    }
}

static void initialize_mqtt(void)
{
    s_mqtt_event_group = xEventGroupCreate();

    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = CONFIG_MQTT_BROKER_ADDR
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_config);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "failed to initialize mqtt client");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler,
                                                   NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));

    ESP_LOGI(TAG, "mqtt started, waiting for first connection");
    xEventGroupWaitBits(s_mqtt_event_group,
                        MQTT_CONNECTED_BIT,
                        pdFALSE,
                        pdFALSE,
                        portMAX_DELAY);
}

static void send_data_to_broker(float temperature, float humidity, float pressure)
{
    char body[128];
    snprintf(body, sizeof(body),
             "{\"temperature\": %.1f, \"humidity\": %.1f, \"pressure\": %.1f}",
             temperature, humidity, pressure);

    esp_mqtt_client_publish(mqtt_client, "sensors/ambient", body, 0, 1, 0);
}

void app_main(void)
{
    initialize_wifi();
    initialize_mqtt();
    initialize_esp_now();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}