/*
 * Host-side check of the tfluna driver on ESP-IDF: ../tfluna_cases.h is the
 * fake module and the shared cases; this file feeds it from the uart and
 * i2c_master calls, and checks what only ESP-IDF has -- the I2C device
 * added and removed, its clock, and the esp_err_t codes.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tfluna.h"
#include "tof.h"
#include "tof_priv.h"

#define RET(err)	tof_esp_err(err)

#define UART_BUS	1
static int bus_tag;
#define I2C_BUS		((i2c_master_bus_handle_t)(void *)&bus_tag)

#include "../tfluna_cases.h"

/* ------------------------------------------------- the IDF side of it --- */

bool uart_is_driver_installed(uart_port_t port)
{
	return port == UART_BUS;
}

esp_err_t uart_flush_input(uart_port_t port)
{
	assert(port == UART_BUS);
	fake_flush();
	return ESP_OK;
}

int uart_write_bytes(uart_port_t port, const void *src, size_t size)
{
	assert(port == UART_BUS);
	fake_send(src, size);
	return (int)size;
}

/* A short count is the wait running out, as uart_read_bytes() has it. */
int uart_read_bytes(uart_port_t port, void *buf, uint32_t length,
		    TickType_t ticks_to_wait)
{
	assert(port == UART_BUS && ticks_to_wait > 0);
	return (int)fake_take(buf, length);
}

struct i2c_master_dev_t {
	bool live;
	uint8_t addr;
	uint32_t scl_hz;
};
static struct i2c_master_dev_t i2c_dev;
static int i2c_devices;

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int timeout_ms)
{
	assert(bus == I2C_BUS && timeout_ms > 0);
	return i2c_present && address == i2c_addr ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *config,
				    i2c_master_dev_handle_t *out)
{
	assert(bus == I2C_BUS && !i2c_dev.live);
	i2c_dev = (struct i2c_master_dev_t){
		.live = true,
		.addr = (uint8_t)config->device_address,
		.scl_hz = config->scl_speed_hz,
	};
	i2c_devices++;
	*out = &i2c_dev;
	return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
	assert(dev->live);
	dev->live = false;
	i2c_devices--;
	return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
			      const uint8_t *data, size_t len, int timeout_ms)
{
	assert(dev->live && timeout_ms > 0);
	if (i2c_fails())
		return ESP_ERR_TIMEOUT;
	fake_reg_write(dev->addr, data, len);
	return ESP_OK;
}

/* The TF-Luna's index is one byte (Appendix III). */
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
				      const uint8_t *tx, size_t tx_len,
				      uint8_t *rx, size_t rx_len,
				      int timeout_ms)
{
	assert(dev->live && tx_len == 1 && timeout_ms > 0);
	if (i2c_fails())
		return ESP_ERR_TIMEOUT;
	fake_reg_read(dev->addr, tx[0], rx, rx_len);
	return ESP_OK;
}

/* ----------------------------------------------------------- ESP cases --- */

static void test_esp_side(void)
{
	tfluna_handle_t h = NULL;

	/* 400 kHz unless asked for less; removed at del, and after a
	 * failed init. */
	fakes_reset();
	h = open_i2c(NULL);
	assert(i2c_devices == 1 && i2c_dev.scl_hz == 400000);
	tfluna_del(h);
	h = open_i2c(&(tfluna_config_t){ .scl_speed_hz = 100000 });
	assert(i2c_dev.scl_hz == 100000);
	tfluna_del(h);
	memcpy(regs + 0x3C, "XXXX", 4);
	assert(tfluna_init_i2c(I2C_BUS, NULL, &h) == ESP_ERR_NOT_FOUND);
	assert(i2c_devices == 0);

	/* A UART with no driver installed is the app's mistake. */
	assert(tfluna_init_uart(2, NULL, &h) == ESP_ERR_INVALID_ARG);
	/* Bytes but no valid frame comes back as ESP_FAIL. */
	fakes_reset();
	answers = false;
	future_len = 150;
	memset(future, 0x11, future_len);
	assert(tfluna_init_uart(UART_BUS, NULL, &h) == ESP_FAIL);
}

int main(void)
{
	run_tfluna_cases();
	assert(i2c_devices == 0);
	test_esp_side();
	printf("all tfluna checks passed (ESP-IDF)\n");
	return 0;
}
