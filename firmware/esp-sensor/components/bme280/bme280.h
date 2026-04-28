#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

typedef struct {
    float temperature;
    float pressure;
    float humidity;
} ambient_t;

esp_err_t init_bme280(i2c_master_dev_handle_t dev);
esp_err_t read_bme280(ambient_t *reading);