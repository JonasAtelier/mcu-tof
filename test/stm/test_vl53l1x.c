/*
 * Host-side check of the vl53l1x driver on the STM32 HAL: ../vl53l1x_cases.h
 * is the fake chip and the shared cases; this file feeds it from
 * HAL_I2C_Mem_Write/Read(), and checks what only the HAL side has -- the
 * shifted address, the 16-bit memory address on reads, XSHUT as a port and
 * pin, and the data-ready line through an EXTI callback.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "tof.h"
#include "vl53l1x.h"

#define RET(err)	(err)

static I2C_HandleTypeDef hi2c;
#define I2C_BUS		(&hi2c)

static GPIO_TypeDef xshut_port;

static int64_t now_us(void)
{
	return (int64_t)stub_tick * 1000;
}

static void cfg_xshut(vl53l1x_config_t *cfg)
{
	cfg->xshut_port = NULL;
}

/* The HAL adds nothing to its handle: nothing to remove. */
static int devices_live(void)
{
	return 0;
}

#include "../vl53l1x_cases.h"

/* ------------------------------------------------- the HAL side of it --- */

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t addr,
					uint32_t trials, uint32_t timeout)
{
	assert(h == &hi2c && trials >= 1 && timeout > 0);
	return chip_answers((uint8_t)(addr >> 1)) ? HAL_OK : HAL_ERROR;
}

/* A write: the high index byte as the HAL's 8-bit memory address, then the
 * low one and the data. */
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t addr,
				    uint16_t mem, uint16_t mem_size,
				    uint8_t *data, uint16_t size,
				    uint32_t timeout)
{
	uint8_t b[1 + 32];

	assert(h == &hi2c && mem_size == I2C_MEMADD_SIZE_8BIT &&
	       size < sizeof b && timeout > 0);
	if (fails())
		return HAL_TIMEOUT;
	b[0] = (uint8_t)mem;
	memcpy(b + 1, data, size);
	fake_write((uint8_t)(addr >> 1), b, 1 + (size_t)size);
	return HAL_OK;
}

/* A read: the whole index as a 16-bit memory address. */
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t addr,
				   uint16_t mem, uint16_t mem_size,
				   uint8_t *data, uint16_t size,
				   uint32_t timeout)
{
	assert(h == &hi2c && mem_size == I2C_MEMADD_SIZE_16BIT &&
	       timeout > 0);
	if (fails())
		return HAL_TIMEOUT;
	fake_read((uint8_t)(addr >> 1), mem, data, size);
	return HAL_OK;
}

static int xshut_level = -1;
static int64_t xshut_low_at, xshut_high_at;

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
	assert(port == &xshut_port && pin == GPIO_PIN_1);
	xshut_level = state == GPIO_PIN_SET;
	if (xshut_level) {
		xshut_high_at = now_us();
		chip_power_on(1200);	/* DS 2.6.1: boot 1.2 ms max */
	} else {
		xshut_low_at = now_us();
		chip.powered = false;
	}
}

/* ----------------------------------------------------------- STM cases --- */

static void test_xshut(void)
{
	vl53l1x_config_t cfg = { .xshut_port = &xshut_port,
				 .xshut_pin = GPIO_PIN_1 };
	vl53l1x_handle_t h;

	fakes_reset();
	h = open_dev(&cfg);
	assert(xshut_level == 1 && xshut_high_at - xshut_low_at >= 2000);
	assert(chip.boot_at_us <= xshut_high_at + 2000);
	vl53l1x_del(h);
}

/*
 * GPIO1 as an app wires it: the EXTI callback only calls tof_irq(); the
 * main loop sees tof_irq_pending() and reads over the bus.
 */
static struct tof_dev *exti_dev;

static void exti_callback(void)
{
	tof_irq(exti_dev);
}

static void test_exti_hook(void)
{
	vl53l1x_handle_t h;
	struct tof_sample s;

	fakes_reset();
	h = open_dev(NULL);
	exti_dev = vl53l1x_tof(h);
	assert(tof_start(exti_dev) == 0);
	assert(!tof_irq_pending(exti_dev));
	chip.waiting = true;
	chip_result(0x09, 321, 0, 0);
	exti_callback();
	assert(tof_irq_pending(exti_dev));
	assert(!tof_irq_pending(exti_dev));	/* once per batch */
	assert(tof_read(exti_dev, &s) == 0 && s.distance_mm == 321);
	vl53l1x_del(h);
}

int main(void)
{
	run_vl53l1x_cases();
	test_xshut();
	test_exti_hook();
	printf("all vl53l1x checks passed (STM32 HAL)\n");
	return 0;
}
