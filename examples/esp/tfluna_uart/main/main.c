/*
 * Read a TF-Luna on a UART and print the distance. Leave its pin 5 open (or
 * at 3.3 V) for serial mode; its TXD goes to RX_GPIO, its RXD to TX_GPIO.
 */
#include <stdio.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tfluna.h"
#include "tof.h"

#define PORT        UART_NUM_1
#define TX_GPIO     17
#define RX_GPIO     16

static const char *TAG = "tfluna-uart";

void app_main(void)
{
    /* The module's own 115200 8N1 (user manual 6.2). */
    uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    tfluna_handle_t lidar;
    struct tof_dev *tof;

    ESP_ERROR_CHECK(uart_driver_install(PORT, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(PORT, TX_GPIO, RX_GPIO, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(tfluna_init_uart(PORT, NULL, &lidar));
    ESP_ERROR_CHECK(tfluna_set_rate(lidar, 50));    /* not saved */
    tof = tfluna_tof(lidar);

    while (1) {
        struct tof_sample s;
        int err = tof_read(tof, &s);

        if (err) {
            ESP_LOGW(TAG, "no reading (%d)", err);
            continue;
        }
        /* Always check validity - a number comes back either way. */
        if (s.valid)
            ESP_LOGI(TAG, "%5u mm   amp %5u   %.1f C", s.distance_mm,
                     s.signal, s.temp_c);
        else
            ESP_LOGW(TAG, "%5u mm   amp %5u: not reliable", s.distance_mm,
                     s.signal);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
