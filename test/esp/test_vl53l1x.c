/*
 * Host-side check of the vl53l1x driver on ESP-IDF: ../vl53l1x_cases.h is
 * the fake chip and the shared cases; this file feeds it from the
 * i2c_master calls, and checks what only ESP-IDF has -- the device added to
 * the app's bus and removed again, its clock, XSHUT through the gpio
 * driver, and the esp_err_t codes callers of esp-vl53l1x-tof saw.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tof.h"
#include "tof_priv.h"
#include "vl53l1x.h"

#define RET(err)	tof_esp_err(err)

static int bus_tag;
#define I2C_BUS		((i2c_master_bus_handle_t)(void *)&bus_tag)

#define PIN_XSHUT	4

static int64_t now_us(void)
{
	return stub_time_us;
}

static void cfg_xshut(vl53l1x_config_t *cfg)
{
	cfg->xshut_gpio = GPIO_NUM_NC;
}

static int devices_live(void);

#include "../vl53l1x_cases.h"

/* ------------------------------------------------- the IDF side of it --- */

/* The devices added: each remembers its address and clock. */
struct i2c_master_dev_t {
	bool live;
	uint8_t addr;
	uint32_t scl_hz;
};
static struct i2c_master_dev_t i2c_devs[4];
static int i2c_devices;

static int devices_live(void)
{
	return i2c_devices;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int timeout_ms)
{
	assert(bus == I2C_BUS && timeout_ms > 0);
	return chip_answers((uint8_t)address) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *config,
				    i2c_master_dev_handle_t *out)
{
	assert(bus == I2C_BUS &&
	       config->dev_addr_length == I2C_ADDR_BIT_LEN_7);
	for (int i = 0; i < 4; i++) {
		if (!i2c_devs[i].live) {
			i2c_devs[i] = (struct i2c_master_dev_t){
				.live = true,
				.addr = (uint8_t)config->device_address,
				.scl_hz = config->scl_speed_hz,
			};
			i2c_devices++;
			*out = &i2c_devs[i];
			return ESP_OK;
		}
	}
	abort();
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
	if (fails())
		return ESP_ERR_TIMEOUT;
	fake_write(dev->addr, data, len);
	return ESP_OK;
}

/* Every read sends the two index bytes, high first, then reads. */
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
				      const uint8_t *tx, size_t tx_len,
				      uint8_t *rx, size_t rx_len,
				      int timeout_ms)
{
	assert(dev->live && tx_len == 2 && timeout_ms > 0);
	if (fails())
		return ESP_ERR_TIMEOUT;
	fake_read(dev->addr, (uint16_t)(tx[0] << 8 | tx[1]), rx, rx_len);
	return ESP_OK;
}

/* XSHUT: when each level was set. Low holds the chip off; high boots it. */
static int xshut_level = -1;
static int64_t xshut_low_at, xshut_high_at;
static gpio_config_t gpio_cfg;

esp_err_t gpio_config(const gpio_config_t *config)
{
	gpio_cfg = *config;
	return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
	assert(gpio == PIN_XSHUT && (gpio_cfg.pin_bit_mask >> PIN_XSHUT) & 1);
	xshut_level = (int)level;
	if (level) {
		xshut_high_at = now_us();
		chip_power_on(1200);	/* DS 2.6.1: boot 1.2 ms max */
	} else {
		xshut_low_at = now_us();
		chip.powered = false;
	}
	return ESP_OK;
}

/* ----------------------------------------------------------- ESP cases --- */

/* The device at 0x29 and scl_speed_hz (0: 400 kHz), removed at del. */
static void test_i2c_device(void)
{
	vl53l1x_config_t cfg = { .scl_speed_hz = 100000,
				 .xshut_gpio = GPIO_NUM_NC };
	vl53l1x_handle_t h;

	fakes_reset();
	h = open_dev(NULL);
	assert(i2c_devices == 1 && i2c_devs[0].addr == 0x29 &&
	       i2c_devs[0].scl_hz == 400000);
	assert(vl53l1x_del(h) == ESP_OK && i2c_devices == 0);

	fakes_reset();
	h = open_dev(&cfg);
	assert(i2c_devs[0].scl_hz == 100000);
	/* A move of address keeps the clock. */
	assert(vl53l1x_set_address(h, 0x31) == ESP_OK);
	assert(i2c_devices == 1);
	for (int i = 0; i < 4; i++)
		if (i2c_devs[i].live)
			assert(i2c_devs[i].addr == 0x31 &&
			       i2c_devs[i].scl_hz == 100000);
	vl53l1x_del(h);
}

/*
 * XSHUT low, then high, each held at least 2 ms, before the first look at
 * the chip -- at a 100 Hz tick, where esp-vl53l1x-tof's two 2 ms waits were
 * none: its chip was still in reset, or still booting, when it looked.
 */
static void test_xshut(void)
{
	vl53l1x_config_t cfg = { .xshut_gpio = PIN_XSHUT };
	vl53l1x_handle_t h;

	fakes_reset();
	h = open_dev(&cfg);
	assert(gpio_cfg.mode == GPIO_MODE_OUTPUT && xshut_level == 1);
	assert(xshut_high_at - xshut_low_at >= 2000);
	assert(chip.boot_at_us <= xshut_high_at + 2000);
	vl53l1x_del(h);
}

/* The codes esp-vl53l1x-tof's callers saw. */
static void test_esp_errors(void)
{
	vl53l1x_handle_t h = NULL;

	fakes_reset();
	chip.powered = false;
	assert(vl53l1x_init(I2C_BUS, NULL, &h) == ESP_ERR_NOT_FOUND);
	fakes_reset();
	chip.boot_at_us = -1;
	assert(vl53l1x_init(I2C_BUS, NULL, &h) == ESP_ERR_NOT_FOUND);
	fakes_reset();
	chip.reg[0x0110] = 0x00;
	assert(vl53l1x_init(I2C_BUS, NULL, &h) == ESP_ERR_NOT_FOUND);
	assert(vl53l1x_init(NULL, NULL, &h) == ESP_ERR_INVALID_ARG);
	assert(vl53l1x_start(NULL) == ESP_ERR_INVALID_ARG);
	assert(i2c_devices == 0 && !h);
}

int main(void)
{
	run_vl53l1x_cases();
	assert(i2c_devices == 0);
	test_i2c_device();
	test_xshut();
	test_esp_errors();
	printf("all vl53l1x checks passed (ESP-IDF)\n");
	return 0;
}
