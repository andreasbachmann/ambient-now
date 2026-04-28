#include "bme280.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// registers
#define BME280_REG_ID           0xD0
#define BME280_REG_CAL_T        0x88
#define BME280_REG_CAL_H1       0xA1
#define BME280_REG_CAL_H2       0xE1
#define BME280_REG_CTRL_HUM     0xF2
#define BME280_REG_CTRL_MEAS    0xF4
#define BME280_REG_STATUS       0xF3
#define BME280_REG_PRESS_MSB    0xF7

#define BME280_ID               0x60
#define WAIT_TIMEOUT_MS         50

// temperature calibration data
static uint16_t dig_T1;
static int16_t  dig_T2; 
static int16_t  dig_T3;

// pressure calibration data
static uint16_t dig_P1;
static int16_t  dig_P2;
static int16_t  dig_P3;
static int16_t  dig_P4;
static int16_t  dig_P5;
static int16_t  dig_P6;
static int16_t  dig_P7;
static int16_t  dig_P8;
static int16_t  dig_P9;

// humidity calibration data
static uint8_t  dig_H1;
static int16_t  dig_H2;
static uint8_t  dig_H3;
static int16_t  dig_H4;
static int16_t  dig_H5;
static int8_t   dig_H6;

static int32_t t_fine;

static const char *TAG = "BME280_Driver";
static i2c_master_dev_handle_t bme280_dev;

static esp_err_t read_register(uint8_t reg_addr, uint8_t *data)
{
    esp_err_t ret = i2c_master_transmit_receive(bme280_dev, &reg_addr, 
                                                sizeof(reg_addr), data,
                                                sizeof(*data), WAIT_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to read register 0x%02X: %s",
                 reg_addr, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t burst_read_register(uint8_t reg_addr, uint8_t *buf, uint8_t len)
{
    esp_err_t ret = i2c_master_transmit_receive(bme280_dev, &reg_addr, sizeof(reg_addr),
                                                buf, len, WAIT_TIMEOUT_MS);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to read registers starting at 0x%02X: %s",
                 reg_addr, esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t write_register(uint8_t reg_addr, uint8_t value)
{
    uint8_t buf[2] = {reg_addr, value};
    esp_err_t ret = i2c_master_transmit(bme280_dev, buf, sizeof(buf), WAIT_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to write to register 0x%02X: %s", 
                 reg_addr, esp_err_to_name(ret));
    }
    return ret;
}

// compensation formulas for BME280, copied from its datasheet
static int32_t compensate_temperature(int32_t adc_T)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) * ((int32_t)dig_T3)) >> 14;
    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8;
    return T;
}

static uint32_t compensate_pressure(int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + (((int64_t)dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) + ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;
    if (var1 == 0) { return 0; } // avoid exception caused by division by zero
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);
    return (uint32_t)p;
}

static uint32_t compensate_humidity(int32_t adc_H)
{
    int32_t v_x1_u32r;
    v_x1_u32r = (t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)dig_H4) << 20) - (((int32_t)dig_H5) * v_x1_u32r)) + ((int32_t)16384)) >> 15) * (((((((v_x1_u32r * ((int32_t)dig_H6)) >> 10) * (((v_x1_u32r * ((int32_t)dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152)) * ((int32_t)dig_H2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) * ((int32_t)dig_H1)) >> 4));
    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);
    return (uint32_t)(v_x1_u32r >> 12);
}

static esp_err_t calibrate_bme280(void)
{
    // 24 bytes temperature + pressure calibration data
    // temperature: 6 bytes from 0x88 onwards
    // pressure: 18 bytes from 0x8E onwards
    uint8_t buf_tp[24];
    uint8_t buf_h[7];
    esp_err_t ret = burst_read_register(BME280_REG_CAL_T, buf_tp, sizeof(buf_tp));
    if (ret == ESP_OK) {
        // build compensation data
        dig_T1 = (uint16_t)(buf_tp[0]  | (buf_tp[1] << 8));
        dig_T2 = (int16_t) (buf_tp[2]  | (buf_tp[3] << 8));
        dig_T3 = (int16_t) (buf_tp[4]  | (buf_tp[5] << 8));

        dig_P1 = (uint16_t)(buf_tp[6]  | (buf_tp[7] << 8));
        dig_P2 = (int16_t) (buf_tp[8]  | (buf_tp[9] << 8));
        dig_P3 = (int16_t) (buf_tp[10] | (buf_tp[11] << 8));
        dig_P4 = (int16_t) (buf_tp[12] | (buf_tp[13] << 8));
        dig_P5 = (int16_t) (buf_tp[14] | (buf_tp[15] << 8));
        dig_P6 = (int16_t) (buf_tp[16] | (buf_tp[17] << 8));
        dig_P7 = (int16_t) (buf_tp[18] | (buf_tp[19] << 8));
        dig_P8 = (int16_t) (buf_tp[20] | (buf_tp[21] << 8));
        dig_P9 = (int16_t) (buf_tp[22] | (buf_tp[23] << 8));

        // humidity calibration data
        // read 1 byte at 0xA1
        ret = read_register(BME280_REG_CAL_H1, &dig_H1);
    }

    if (ret == ESP_OK) {
        // read 7 byte from 0xE1 onwards
        ret = burst_read_register(BME280_REG_CAL_H2, buf_h, sizeof(buf_h));
    }

    if (ret == ESP_OK) {
        // build compensation data
        dig_H2 = (int16_t)(buf_h[0] | (buf_h[1] << 8));
        dig_H3 = (uint8_t)(buf_h[2]);
        dig_H4 = (int16_t)((buf_h[3] << 4) | (buf_h[4] & 0x0F));
        dig_H5 = (int16_t)((buf_h[4] >> 4) | (buf_h[5] << 4));
        dig_H6 = (int8_t) (buf_h[6]);
    }
    return ret; 
}

esp_err_t init_bme280(i2c_master_dev_handle_t dev)
{
    bme280_dev = dev;
    uint8_t id = 0;
    ambient_t reading = {0};

    esp_err_t ret = read_register(BME280_REG_ID, &id);

    if (ret == ESP_OK) {
        if (id != BME280_ID) {
            ESP_LOGE(TAG, "wrong chip ID: got 0x%02X expected 0x%02X", id, BME280_ID);
        } else {
            ESP_LOGI(TAG, "BME280 found, chip ID: 0x%02X", id);
        }
    }

    if (ret == ESP_OK) {
        ret = calibrate_bme280();
    }
    
    if (ret == ESP_OK) {
        // read once to populate t_fine
        ret = read_bme280(&reading);
    }
    
    return ret;
}

static esp_err_t wait_for_status(void)
{
    uint8_t status = 0;
    esp_err_t ret;
    uint16_t retries = WAIT_TIMEOUT_MS;

    do {
        ret = read_register(BME280_REG_STATUS, &status);
        if (ret != ESP_OK) return ret;
        if (!(status & 0x08)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (--retries > 0);
    
    ESP_LOGE(TAG, "measurement timeout");
    return ESP_ERR_TIMEOUT;
}

esp_err_t read_bme280(ambient_t *reading)
{   
    uint8_t buf[8];

    // request measurements
    // humidity x1
    esp_err_t ret = write_register(BME280_REG_CTRL_HUM, 0x01);
    if (ret == ESP_OK) {
        // temperature and pressure x1, forced mode
        ret = write_register(BME280_REG_CTRL_MEAS, 0x25);
    }
    
    if (ret == ESP_OK) {
        // wait for status bit to clear
        ret = wait_for_status();
    }
    
    if (ret == ESP_OK) {
        // read 8 bytes from measurement storage registers starting at 0xF7
        ret = burst_read_register(BME280_REG_PRESS_MSB, buf, sizeof(buf));
    }
    
    if (ret == ESP_OK) {
        // build raw measurements
        int32_t adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
        int32_t adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);
        int32_t adc_H = ((int32_t)buf[6] << 8)  | buf[7];

        // compensate measurements
        uint32_t raw_P = compensate_pressure(adc_P);
        int32_t raw_T = compensate_temperature(adc_T);
        uint32_t raw_H = compensate_humidity(adc_H);

        // store properly converted data in output struct
        reading->pressure =     (raw_P / 256.0f) / 100.0f;
        reading->temperature =  raw_T / 100.0f;
        reading->humidity =     raw_H / 1024.0f;
    }
    return ret;
}
