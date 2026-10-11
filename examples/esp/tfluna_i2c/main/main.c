/*
 * Read a TF-Luna on I2C in trigger mode. Tie its pin 5 to ground before
 * power-up for I2C mode; SDA is its pin 2, SCL its pin 3.
 */
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tfluna.h"

#define SDA_GPIO    21
#define SCL_GPIO    22

static const char *TAG = "tfluna-i2c";

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
    i2c_master_bus_handle_t bus;
    tfluna_handle_t lidar;

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_ERROR_CHECK(tfluna_init_i2c(bus, NULL, &lidar));   /* 0x10 */

    /* Trigger mode: one measurement per trigger, so a read never lands
     * on an update (user manual 6.3). */
    ESP_ERROR_CHECK(tfluna_set_rate(lidar, 0));

    while (1) {
        tfluna_result_t r;

        ESP_ERROR_CHECK(tfluna_trigger(lidar));
        /* Pin 6 rises when the result is in (6.3); with it unwired, a
         * margin -- the manual gives no figure for one measurement. */
        vTaskDelay(pdMS_TO_TICKS(20));
        if (tfluna_read(lidar, &r) != ESP_OK)
            continue;
        if (tfluna_valid(&r))
            ESP_LOGI(TAG, "%5u mm   amp %5u   %.2f C", r.distance_mm, r.amp,
                     r.temp_c);
        else
            ESP_LOGW(TAG, "amp %u: not reliable", r.amp);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}
