/* Read a VL53L1X and print the distance. Wire SDA/SCL to the pins below. */
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vl53l1x.h"

#define SDA_GPIO    21
#define SCL_GPIO    22

static const char *TAG = "vl53l1x-example";

void app_main(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SDA_GPIO,
        .scl_io_num = SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    vl53l1x_config_t cfg = {
        .mode       = VL53L1X_LONG,   /* up to ~4 m indoors */
        .budget_ms  = 50,             /* steady without being slow */
        .xshut_gpio = GPIO_NUM_NC,    /* breakouts pull XSHUT high already */
    };
    i2c_master_bus_handle_t bus;
    vl53l1x_handle_t sensor;

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_ERROR_CHECK(vl53l1x_init(bus, &cfg, &sensor));
    ESP_ERROR_CHECK(vl53l1x_start(sensor));

    while (1) {
        vl53l1x_result_t r;
        esp_err_t err;

        err = vl53l1x_read_blocking(sensor, &r, 200);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "no reading: %s", esp_err_to_name(err));
            continue;
        }

        /* Always check the status - a number is returned either way. */
        if (r.status == VL53L1X_VALID)
            ESP_LOGI(TAG, "%4u mm   signal %5u kcps   ambient %5u kcps",
                     r.distance_mm, r.signal_kcps, r.ambient_kcps);
        else
            ESP_LOGW(TAG, "%4u mm   %s", r.distance_mm,
                     vl53l1x_status_str(r.status));

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
