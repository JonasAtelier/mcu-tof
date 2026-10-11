/*
 * Host-side check of the tfluna driver on the STM32 HAL: ../tfluna_cases.h
 * is the fake module and the shared cases; this file feeds it from
 * HAL_UART_Transmit/Receive() and the data-register flush, and from
 * HAL_I2C_Mem_Write/Read() at the shifted address, and checks the pin 6
 * line through an EXTI callback.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "tfluna.h"
#include "tof.h"

#define RET(err)	(err)

static UART_HandleTypeDef huart;
#define UART_BUS	(&huart)
static I2C_HandleTypeDef hi2c;
#define I2C_BUS		(&hi2c)

#include "../tfluna_cases.h"

/* ------------------------------------------------- the HAL side of it --- */

int stub_uart_get_flag(UART_HandleTypeDef *h, uint32_t flag)
{
	assert(h == &huart && flag == UART_FLAG_RXNE);
	return pending_len > 0;
}

void stub_uart_clear_ore(UART_HandleTypeDef *h)
{
	assert(h == &huart);
}

/* One byte out of the data register. */
void stub_uart_flush(UART_HandleTypeDef *h)
{
	assert(h == &huart);
	fake_drop_byte();
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h,
				    const uint8_t *data, uint16_t size,
				    uint32_t timeout)
{
	assert(h == &huart && timeout > 0);
	fake_send(data, size);
	return HAL_OK;
}

/* Short is HAL_TIMEOUT, as the blocking receive has it. */
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *data,
				   uint16_t size, uint32_t timeout)
{
	assert(h == &huart && timeout > 0);
	return fake_take(data, size) == size ? HAL_OK : HAL_TIMEOUT;
}

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t addr,
					uint32_t trials, uint32_t timeout)
{
	assert(h == &hi2c && trials >= 1 && timeout > 0);
	return i2c_present && addr == i2c_addr << 1 ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t addr,
				    uint16_t mem, uint16_t mem_size,
				    uint8_t *data, uint16_t size,
				    uint32_t timeout)
{
	uint8_t b[1 + 32];

	assert(h == &hi2c && mem_size == I2C_MEMADD_SIZE_8BIT &&
	       size < sizeof b && timeout > 0);
	if (i2c_fails())
		return HAL_TIMEOUT;
	b[0] = (uint8_t)mem;
	memcpy(b + 1, data, size);
	fake_reg_write((uint8_t)(addr >> 1), b, 1 + (size_t)size);
	return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t addr,
				   uint16_t mem, uint16_t mem_size,
				   uint8_t *data, uint16_t size,
				   uint32_t timeout)
{
	assert(h == &hi2c && mem_size == I2C_MEMADD_SIZE_8BIT &&
	       timeout > 0);
	if (i2c_fails())
		return HAL_TIMEOUT;
	fake_reg_read((uint8_t)(addr >> 1), (uint8_t)mem, data, size);
	return HAL_OK;
}

/* ----------------------------------------------------------- STM cases --- */

/*
 * Pin 6 as an app wires it in I2C mode: high on new data (6.3, p.10). The
 * EXTI callback only calls tof_irq(); the main loop reads once per edge.
 */
static struct tof_dev *exti_dev;

static void exti_callback(void)
{
	tof_irq(exti_dev);
}

static void test_exti_hook(void)
{
	tfluna_handle_t h;
	struct tof_sample s;

	fakes_reset();
	h = open_i2c(NULL);
	exti_dev = tfluna_tof(h);
	assert(!tof_irq_pending(exti_dev));
	regs[0x00] = 0x64;		/* 100 cm */
	regs[0x02] = 0xE8;		/* Amp 1000 */
	regs[0x03] = 0x03;
	exti_callback();
	assert(tof_irq_pending(exti_dev));
	assert(tof_read(exti_dev, &s) == 0 && s.distance_mm == 1000);
	assert(!tof_irq_pending(exti_dev));
	tfluna_del(h);
}

int main(void)
{
	run_tfluna_cases();
	test_exti_hook();
	printf("all tfluna checks passed (STM32 HAL)\n");
	return 0;
}
