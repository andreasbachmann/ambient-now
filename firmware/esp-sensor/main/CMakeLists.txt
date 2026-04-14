idf_component_register(SRCS "esp-sensor.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_netif esp_wifi)
