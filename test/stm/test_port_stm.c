/*
 * Host-side check of src/tof_port_stm.c against test/hal_stubs: the bus
 * ops -- I2C, which the VL53L1X uses with its 16-bit register index and the
 * TF-Luna with an 8-bit one, presence probe included; SPI, which no tof
 * chip uses yet; and UART, the TF-Luna's stream and commands. Plus the
 * waits and the default clock.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "tof.h"
#include "tof_priv.h"

static UART_HandleTypeDef uart_handle;
/*
 * In order. UART: O overrun clear, F flush, W send, R receive. SPI: L and H
 * chip select low and high, T transmit, X transmit-receive.
 */
static char calls[64];
static HAL_StatusTypeDef next_status;
static int timeout_after_send;	/* the receive after a send times out */
static int stale_bytes;		/* bytes waiting in the data register */
static int ore_cleared;
static uint8_t sent[40];
static size_t sent_len;
static uint8_t reply[32];
static uint16_t last_size;

static void reset(void)
{
	calls[0] = '\0';
	next_status = HAL_OK;
	timeout_after_send = stale_bytes = ore_cleared = 0;
	sent_len = 0;
}

static void called(char c)
{
	size_t n = strlen(calls);

	assert(n + 1 < sizeof(calls));
	calls[n] = c;
	calls[n + 1] = '\0';
}

int stub_uart_get_flag(UART_HandleTypeDef *h, uint32_t flag)
{
	assert(h == &uart_handle && flag == UART_FLAG_RXNE);
	return stale_bytes > 0;
}

void stub_uart_clear_ore(UART_HandleTypeDef *h)
{
	assert(h == &uart_handle);
	called('O');
	ore_cleared++;
}

void stub_uart_flush(UART_HandleTypeDef *h)
{
	assert(h == &uart_handle);
	called('F');
	stale_bytes--;
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, const uint8_t *data,
				    uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &uart_handle && sent_len + size <= sizeof(sent));
	called('W');
	memcpy(sent + sent_len, data, size);
	sent_len += size;
	if (!timeout_after_send)
		return next_status;
	next_status = HAL_TIMEOUT;	/* for the receive that follows */
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *data,
				   uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &uart_handle);
	called('R');
	last_size = size;
	if (next_status == HAL_OK)
		memcpy(data, reply, size);
	return next_status;
}

/* The SPI device behind a software chip select. */
static SPI_HandleTypeDef spi_handle;
static GPIO_TypeDef cs_port;
static uint8_t spi_out[40];	/* what the last transaction clocked out */
static uint16_t spi_size;
static uint8_t spi_in[40];	/* what the device clocks back */

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
	assert(port == &cs_port && pin == GPIO_PIN_4);
	called(state == GPIO_PIN_RESET ? 'L' : 'H');
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, const uint8_t *data,
				   uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &spi_handle && size <= sizeof(spi_out));
	called('T');
	memcpy(spi_out, data, size);
	spi_size = size;
	return next_status;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h,
					  const uint8_t *tx, uint8_t *rx,
					  uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &spi_handle && size <= sizeof(spi_out));
	called('X');
	memcpy(spi_out, tx, size);
	spi_size = size;
	if (next_status == HAL_OK)
		memcpy(rx, spi_in, size);
	return next_status;
}

/* The I2C device at 0x2A: Mem_Read, Mem_Write or a bare Master_Transmit. */
static I2C_HandleTypeDef i2c_handle;
static char i2c_last;		/* r, w or b */
static uint16_t i2c_addr, i2c_mem, i2c_mem_size;

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t addr,
				   uint16_t mem, uint16_t mem_size,
				   uint8_t *data, uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle);
	i2c_last = 'r';
	i2c_addr = addr;
	i2c_mem = mem;
	i2c_mem_size = mem_size;
	memcpy(data, reply, size);
	return next_status;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t addr,
				    uint16_t mem, uint16_t mem_size,
				    uint8_t *data, uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle && mem_size == I2C_MEMADD_SIZE_8BIT);
	i2c_last = 'w';
	i2c_addr = addr;
	i2c_mem = mem;
	memcpy(sent, data, size);
	sent_len = size;
	return next_status;
}

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *h, uint16_t addr,
					  uint8_t *data, uint16_t size,
					  uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle && size == 1);
	i2c_last = 'b';
	i2c_addr = addr;
	i2c_mem = data[0];
	return next_status;
}

/* HAL_I2C_IsDeviceReady(): what it answers, and how it was asked. */
static HAL_StatusTypeDef ready_status;
static uint16_t ready_addr;
static uint32_t ready_trials;

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t addr,
					uint32_t trials, uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle);
	ready_addr = addr;
	ready_trials = trials;
	return ready_status;
}

static void test_i2c(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	struct tof_bus bus;
	uint8_t rx[2] = { 0 };

	reset();
	assert(tof_i2c_attach(&bus, &i2c_handle, 0x2A, 0, 10) == 0);
	assert(bus.ops->kind == TOF_BUS_I2C);

	/* The 7-bit address goes to the HAL shifted. */
	memcpy(reply, "\x12\x34", 2);
	assert(bus.ops->transfer(&bus, 0x0F, NULL, 0, rx, 2) == 0);
	assert(i2c_last == 'r' && i2c_addr == 0x54 && i2c_mem == 0x0F);
	assert(i2c_mem_size == I2C_MEMADD_SIZE_8BIT);
	assert(rx[0] == 0x12 && rx[1] == 0x34);
	assert(bus.ops->transfer(&bus, 0x20, args, 2, NULL, 0) == 0);
	assert(i2c_last == 'w' && i2c_mem == 0x20 && sent_len == 2 &&
	       sent[1] == 0x22);

	/* A 16-bit index: cmd high, tx[0] low, as the HAL's 16-bit memory
	 * address, which it sends high byte first. */
	memcpy(reply, "\xEA\xCC", 2);
	assert(bus.ops->transfer(&bus, 0x01, args, 1, rx, 2) == 0);
	assert(i2c_last == 'r' && i2c_mem == 0x0111 &&
	       i2c_mem_size == I2C_MEMADD_SIZE_16BIT);
	assert(rx[0] == 0xEA && rx[1] == 0xCC);
	assert(bus.ops->transfer(&bus, 0x06, NULL, 0, NULL, 0) == 0);
	assert(i2c_last == 'b' && i2c_mem == 0x06);

	/* More than one tx byte before a read is not an I2C transaction;
	 * failures mapped. */
	assert(bus.ops->transfer(&bus, 0x20, args, 2, rx, 1) == -EINVAL);
	next_status = HAL_ERROR;
	assert(bus.ops->transfer(&bus, 0x0F, NULL, 0, rx, 1) == -EIO);
	next_status = HAL_OK;

	/* No receive() on I2C. */
	assert(tof_bus_receive(&bus, rx, 1, false) == -EOPNOTSUPP);
	tof_bus_detach(&bus);
}

static void test_spi(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	uint8_t many[TOF_BUS_MAX_LEN] = { 0 };
	struct tof_bus bus;
	uint8_t rx[3] = { 0 };

	/* Deselected at attach, so the first transfer starts on a fall. */
	reset();
	assert(tof_spi_attach(&bus, &spi_handle, &cs_port, GPIO_PIN_4,
				   100) == 0);
	assert(bus.ops->kind == TOF_BUS_SPI);
	assert(strcmp(calls, "H") == 0);

	/* A read: CS low, cmd then zeros full duplex, CS high. */
	reset();
	memcpy(spi_in, "\xFF\x12\x34\x56", 4);
	memset(spi_out, 0xAA, sizeof(spi_out));
	assert(bus.ops->transfer(&bus, 0x8D, NULL, 0, rx, 3) == 0);
	assert(strcmp(calls, "LXH") == 0 && spi_size == 4);
	assert(spi_out[0] == 0x8D && spi_out[1] == 0 && spi_out[2] == 0 &&
	       spi_out[3] == 0);
	assert(rx[0] == 0x12 && rx[1] == 0x34 && rx[2] == 0x56);

	/* A write: transmit only. */
	reset();
	assert(bus.ops->transfer(&bus, 0x0D, args, 2, NULL, 0) == 0);
	assert(strcmp(calls, "LTH") == 0 && spi_size == 3);
	assert(spi_out[0] == 0x0D && spi_out[1] == 0x11 && spi_out[2] == 0x22);

	/* CS goes high again on a failure; rx is left alone. */
	reset();
	rx[0] = 0x99;
	next_status = HAL_TIMEOUT;
	assert(bus.ops->transfer(&bus, 0x8D, NULL, 0, rx, 1) == -ETIMEDOUT);
	assert(strcmp(calls, "LXH") == 0 && rx[0] == 0x99);

	/* Too long, tx and rx together: refused before CS moves. */
	reset();
	assert(bus.ops->transfer(&bus, 0x0D, many, sizeof(many), rx, 1) ==
	       -EINVAL);
	assert(calls[0] == '\0');

	/* No receive() on SPI. */
	assert(tof_bus_receive(&bus, rx, 1, false) == -EOPNOTSUPP);
	tof_bus_detach(&bus);
}

static void test_uart(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	static const struct {
		HAL_StatusTypeDef status;
		int expected;
	} map[] = {
		{ HAL_OK,	0 },
		{ HAL_ERROR,	-EIO },
		{ HAL_BUSY,	-EAGAIN },
		{ HAL_TIMEOUT,	-ETIMEDOUT },
	};
	uint8_t rx[2] = { 0 }, many[TOF_BUS_MAX_LEN + 1] = { 0 };
	struct tof_bus bus;

	assert(tof_uart_attach(&bus, &uart_handle, 100) == 0);
	assert(bus.ops->kind == TOF_BUS_UART);

	/* Stale input dropped first: overrun cleared, two bytes read out. */
	reset();
	stale_bytes = 2;
	memcpy(reply, "\x12\x34", 2);
	assert(bus.ops->transfer(&bus, 0x13, args, 2, rx, 2) == 0);
	assert(strcmp(calls, "OFFWR") == 0);
	assert(stale_bytes == 0 && ore_cleared == 1);
	assert(sent_len == 3 && sent[0] == 0x13 && sent[1] == 0x11 &&
	       sent[2] == 0x22);
	assert(last_size == 2 && rx[0] == 0x12 && rx[1] == 0x34);

	/* No reply wanted: no receive. A failed send: no receive either. */
	reset();
	assert(bus.ops->transfer(&bus, 0x80, NULL, 0, NULL, 0) == 0);
	assert(strcmp(calls, "OW") == 0);
	reset();
	next_status = HAL_BUSY;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EAGAIN);
	assert(strcmp(calls, "OW") == 0);

	/* A reply that stops short: the send passes, the receive times out. */
	reset();
	timeout_after_send = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -ETIMEDOUT);
	assert(strcmp(calls, "OWR") == 0);

	/* HAL status to errno, through a send and a receive. */
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		reset();
		next_status = map[i].status;
		assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 1) ==
		       map[i].expected);
	}

	/* Too much to send is refused before the line is touched. */
	reset();
	assert(bus.ops->transfer(&bus, 0x13, many, sizeof(many), NULL, 0) ==
	       -EINVAL);
	assert(calls[0] == '\0');

	/* receive(): the bytes, nothing sent, nothing dropped first ... */
	reset();
	stale_bytes = 1;
	memcpy(reply, "\x56\x78", 2);
	assert(tof_bus_receive(&bus, rx, 2, false) == 0);
	assert(strcmp(calls, "R") == 0 && last_size == 2);
	assert(rx[0] == 0x56 && rx[1] == 0x78 && stale_bytes == 1);
	/* ... unless asked: then the overrun and the stale byte go first. */
	reset();
	stale_bytes = 1;
	assert(tof_bus_receive(&bus, rx, 2, true) == 0);
	assert(strcmp(calls, "OFR") == 0 && stale_bytes == 0);
	reset();
	next_status = HAL_TIMEOUT;
	assert(tof_bus_receive(&bus, rx, 2, false) == -ETIMEDOUT);
	assert(tof_bus_receive(&bus, rx, 0, false) == -EINVAL);

	tof_bus_detach(&bus);
}

/*
 * The waits: HAL_Delay() as asked, which already adds its tick; a yield is
 * a millisecond; and the default clock is HAL_GetTick() in microseconds.
 */
static void test_waits(void)
{
	stub_tick = 100;
	tof_sleep_ms(2);
	assert(stub_tick == 103);
	tof_yield();
	assert(stub_tick == 105);
	assert(tof_time_us() == 105000);
}

static void test_probe(void)
{
	/* No ACK is HAL_ERROR there, -ENXIO here; the address goes shifted. */
	ready_status = HAL_ERROR;
	assert(tof_i2c_probe(&i2c_handle, 0x2A, 100) == -ENXIO);
	assert(ready_addr == 0x54 && ready_trials == 1);
	ready_status = HAL_BUSY;
	assert(tof_i2c_probe(&i2c_handle, 0x2A, 100) == -EAGAIN);
	ready_status = HAL_TIMEOUT;
	assert(tof_i2c_probe(&i2c_handle, 0x2A, 100) == -ETIMEDOUT);
	ready_status = HAL_OK;
	assert(tof_i2c_probe(&i2c_handle, 0x2A, 100) == 0);
}

int main(void)
{
	test_i2c();
	test_spi();
	test_uart();
	test_probe();
	test_waits();
	printf("test_port_stm: all passed\n");
	return 0;
}
