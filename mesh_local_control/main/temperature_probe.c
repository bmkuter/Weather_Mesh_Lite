#include "temperature_probe.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define SHT45_I2C_ADDR 0x44

static const char *TAG = "temperature_probe";

// Updated CRC8 implementation from Adafruit_SHT4x.cpp
static uint8_t crc8(const uint8_t *data, int len)
{
    const uint8_t POLYNOMIAL = 0x31;
    uint8_t crc = 0xFF;
    for (int j = len; j; --j) {
        crc ^= *data++;
        for (int i = 8; i; --i) {
            crc = (crc & 0x80) ? (crc << 1) ^ POLYNOMIAL : (crc << 1);
        }
    }
    return crc;
}

// Cached sensor measurement values and timestamp
static float cached_temperature = 0.0f;
static float cached_humidity = 0.0f;
static TickType_t last_measurement_ticks = 0;

// Internal function to update sensor measurements if needed
static void update_sensor_measurement(void)
{
    TickType_t now = xTaskGetTickCount();
    if(now - last_measurement_ticks < pdMS_TO_TICKS(100))
        return; // use cached values

    uint8_t cmd = 0xFD; // No-heater high precision command
    esp_err_t err;

    // I2C write command
    i2c_cmd_handle_t i2c_cmd = i2c_cmd_link_create();
    i2c_master_start(i2c_cmd);
    i2c_master_write_byte(i2c_cmd, (SHT45_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(i2c_cmd, cmd, true);
    i2c_master_stop(i2c_cmd);
    err = i2c_master_cmd_begin(I2C_NUM_0, i2c_cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(i2c_cmd);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed");
        return;
    }

    // Wait for measurement (10 ms for high precision)
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t readbuffer[6] = {0};
    i2c_cmd = i2c_cmd_link_create();
    i2c_master_start(i2c_cmd);
    i2c_master_write_byte(i2c_cmd, (SHT45_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(i2c_cmd, readbuffer, 6, I2C_MASTER_LAST_NACK);
    i2c_master_stop(i2c_cmd);
    err = i2c_master_cmd_begin(I2C_NUM_0, i2c_cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(i2c_cmd);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed");
        return;
    }

    // Verify CRC for temperature (first two bytes) and humidity (bytes 3-4)
    if(crc8(readbuffer, 2) != readbuffer[2] ||
       crc8(readbuffer + 3, 2) != readbuffer[5])
    {
        ESP_LOGE(TAG, "CRC mismatch");
        return;
    }

    uint16_t temp_ticks = ((uint16_t)readbuffer[0] << 8) | readbuffer[1];
    uint16_t hum_ticks  = ((uint16_t)readbuffer[3] << 8) | readbuffer[4];

    cached_temperature = -45 + 175 * ((float)temp_ticks / 65535);
    cached_humidity = -6 + 125 * ((float)hum_ticks / 65535);
    if(cached_humidity < 0.0f) cached_humidity = 0.0f;
    if(cached_humidity > 100.0f) cached_humidity = 100.0f;

    last_measurement_ticks = now;
}

void temperature_probe_init()
{
    ;
}

float temperature_probe_read_temperature()
{
    update_sensor_measurement();
    return cached_temperature;
}

float temperature_probe_read_humidity()
{
    update_sensor_measurement();
    return cached_humidity;
}

void temperature_task(void *arg)
{
    while (true) {
        float temp = temperature_probe_read_temperature();
        float hum = temperature_probe_read_humidity();
        ESP_LOGI(TAG, "Temperature: %.2f C, Humidity: %.2f %%", temp, hum);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}