/*
 * Host-side check of src/tof_port_esp.c against test/idf_stubs: the bus
 * ops -- I2C, which the VL53L1X uses with its 16-bit register index and the
 * TF-Luna with an 8-bit one, presence probe included; SPI, which no tof
 * chip uses yet; and UART, the TF-Luna's stream and commands. Plus the
 * waits: tof_sleep_ms() never short at a 100 Hz tick.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "tof.h"
#include "tof_priv.h"

#define UART_PORT 1


static esp_err_t flush_err;
static bool installed;
static char calls[16];		/* F flush, W write, R read */
static uint8_t out[40];		/* what the last write sent */
static size_t out_len;
static uint8_t in[64];		/* what is waiting to be read */
static size_t in_len;
static uint8_t reply[32];	/* what the device sends after a write */
static size_t reply_len;
static TickType_t waited;	/* ticks_to_wait of the last read */
static int write_short, read_error;

static void reset(void)
{
	installed = true;
	calls[0] = '\0';
	out_len = in_len = reply_len = 0;
	write_short = read_error = 0;
	flush_err = ESP_OK;
}

static void called(char c)
{
	size_t n = strlen(calls);

	assert(n + 1 < sizeof(calls));
	calls[n] = c;
	calls[n + 1] = '\0';
}

bool uart_is_driver_installed(uart_port_t port)
{
	return installed && port == UART_PORT;
}

esp_err_t uart_flush_input(uart_port_t port)
{
	assert(port == UART_PORT);
	called('F');
	if (flush_err != ESP_OK)
		return flush_err;
	in_len = 0;
	return ESP_OK;
}

int uart_write_bytes(uart_port_t port, const void *src, size_t size)
{
	assert(port == UART_PORT && size <= sizeof(out));
	called('W');
	if (write_short)
		return 0;
	memcpy(out, src, size);
	out_len = size;
	/* The device answers after the write. */
	memcpy(in + in_len, reply, reply_len);
	in_len += reply_len;
	return (int)size;
}

int uart_read_bytes(uart_port_t port, void *buf, uint32_t length,
		    TickType_t ticks_to_wait)
{
	size_t n = length < in_len ? length : in_len;

	assert(port == UART_PORT);
	called('R');
	waited = ticks_to_wait;
	if (read_error)
		return -1;
	memcpy(buf, in, n);
	memmove(in, in + n, in_len - n);
	in_len -= n;
	return (int)n;
}

/* The SPI device: what was added, the last transaction, the next answer. */
static spi_device_interface_config_t spi_cfg;
static int spi_added;
static esp_err_t spi_err;
static uint8_t spi_out[40];	/* what the last transaction clocked out */
static size_t spi_bits;
static bool spi_inline;		/* the last transaction used tx_data/rx_data */
static uint8_t spi_in[40];	/* what the device clocks back */

esp_err_t spi_bus_add_device(spi_host_device_t host,
			     const spi_device_interface_config_t *config,
			     spi_device_handle_t *out_handle)
{
	assert(host == SPI2_HOST);
	spi_cfg = *config;
	spi_added++;
	*out_handle = (spi_device_handle_t)(void *)&spi_cfg;
	return spi_err;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t device)
{
	assert(device == (spi_device_handle_t)(void *)&spi_cfg);
	spi_added--;
	return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t device,
				      spi_transaction_t *transfer)
{
	size_t n = transfer->length / 8;

	assert(device == (spi_device_handle_t)(void *)&spi_cfg);
	assert(transfer->length % 8 == 0 && n <= sizeof(spi_out));
	assert(transfer->rxlength == 0 && "rx as long as tx");
	if (spi_err != ESP_OK)
		return spi_err;
	spi_inline = transfer->flags != 0;
	if (spi_inline) {
		/* Up to four bytes in the transaction itself, both ways. */
		assert(transfer->flags ==
		       (SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA) && n <= 4);
		memcpy(spi_out, transfer->tx_data, n);
		memcpy(transfer->rx_data, spi_in, n);
	} else {
		memcpy(spi_out, transfer->tx_buffer, n);
		memcpy(transfer->rx_buffer, spi_in, n);
	}
	spi_bits = transfer->length;
	return ESP_OK;
}

/* The I2C device: what was added, the last write, the next answer. */
static i2c_device_config_t i2c_cfg;
static int i2c_added;
static esp_err_t i2c_err;
static uint8_t i2c_out[40];
static size_t i2c_out_len, i2c_rx_len;

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *config,
				    i2c_master_dev_handle_t *out_handle)
{
	(void)bus;
	i2c_cfg = *config;
	i2c_added++;
	*out_handle = (i2c_master_dev_handle_t)(void *)&i2c_cfg;
	return i2c_err;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
	assert(device == (i2c_master_dev_handle_t)(void *)&i2c_cfg);
	i2c_added--;
	return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
			      const uint8_t *data, size_t length,
			      int timeout_ms)
{
	(void)device;
	(void)timeout_ms;
	memcpy(i2c_out, data, length);
	i2c_out_len = length;
	i2c_rx_len = 0;
	return i2c_err;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
				      const uint8_t *tx, size_t tx_length,
				      uint8_t *rx, size_t rx_length,
				      int timeout_ms)
{
	(void)device;
	(void)timeout_ms;
	memcpy(i2c_out, tx, tx_length);
	i2c_out_len = tx_length;
	i2c_rx_len = rx_length;
	memcpy(rx, reply, rx_length);
	return i2c_err;
}

/* What i2c_master_probe() answers, and where it was asked. */
static esp_err_t probe_err;
static uint16_t probe_addr;
static int probe_timeout;

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int xfer_timeout_ms)
{
	(void)bus;
	probe_addr = address;
	probe_timeout = xfer_timeout_ms;
	return probe_err;
}

static void test_i2c(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	struct tof_bus bus = { 0 };
	uint8_t rx[2] = { 0 };

	/* A refused add leaves no ops: nothing to detach. */
	i2c_err = ESP_ERR_NO_MEM;
	assert(tof_i2c_attach(&bus, NULL, 0x2A, 0, 10) == -ENOMEM);
	assert(!bus.ops);
	i2c_added = 0;
	i2c_err = ESP_OK;

	assert(tof_i2c_attach(&bus, NULL, 0x2A, 0, 10) == 0);
	assert(bus.ops->kind == TOF_BUS_I2C && i2c_added == 1);
	assert(i2c_cfg.device_address == 0x2A &&
	       i2c_cfg.scl_speed_hz == 400000);

	reset();
	memcpy(reply, "\x12\x34", 2);
	assert(bus.ops->transfer(&bus, 0x0F, NULL, 0, rx, 2) == 0);
	assert(i2c_out_len == 1 && i2c_out[0] == 0x0F && i2c_rx_len == 2);
	assert(rx[0] == 0x12 && rx[1] == 0x34);
	assert(bus.ops->transfer(&bus, 0x20, args, 2, NULL, 0) == 0);
	assert(i2c_out_len == 3 && i2c_out[0] == 0x20 && i2c_out[2] == 0x22);
	/* A bare cmd: one byte written. */
	assert(bus.ops->transfer(&bus, 0x06, NULL, 0, NULL, 0) == 0);
	assert(i2c_out_len == 1 && i2c_out[0] == 0x06 && i2c_rx_len == 0);

	/* A 16-bit index: cmd its high byte and tx[0] its low one, both
	 * written before the repeated start (VL53L1X datasheet, Figure 14). */
	memcpy(reply, "\xEA\xCC", 2);
	assert(bus.ops->transfer(&bus, 0x01, args, 1, rx, 2) == 0);
	assert(i2c_out_len == 2 && i2c_out[0] == 0x01 && i2c_out[1] == 0x11);
	assert(i2c_rx_len == 2 && rx[0] == 0xEA && rx[1] == 0xCC);
	/* More than one tx byte before a read is not an I2C transaction. */
	assert(bus.ops->transfer(&bus, 0x20, args, 2, rx, 1) == -EINVAL);

	/* No receive() on I2C. */
	assert(tof_bus_receive(&bus, rx, 1, false) == -EOPNOTSUPP);

	tof_bus_detach(&bus);
	assert(i2c_added == 0);
}

static void test_spi(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	uint8_t many[TOF_BUS_MAX_LEN] = { 0 };
	struct tof_bus bus = { 0 };
	uint8_t rx[3] = { 0 };

	/* A refused add leaves no ops: nothing to detach. */
	spi_err = ESP_ERR_INVALID_ARG;
	assert(tof_spi_attach(&bus, SPI2_HOST, 5, 1000000, 3) == -EINVAL);
	assert(!bus.ops && spi_added == 1);
	spi_added = 0;
	spi_err = ESP_OK;

	assert(tof_spi_attach(&bus, SPI2_HOST, 5, 4000000, 3) == 0);
	assert(bus.ops->kind == TOF_BUS_SPI && spi_added == 1);
	assert(spi_cfg.mode == 3 && spi_cfg.clock_speed_hz == 4000000 &&
	       spi_cfg.spics_io_num == 5);

	/* A read: cmd, then zeros, and what came back after cmd. */
	memcpy(spi_in, "\xFF\x12\x34\x56", 4);
	memset(spi_out, 0xAA, sizeof(spi_out));
	assert(bus.ops->transfer(&bus, 0x8D, NULL, 0, rx, 3) == 0);
	assert(spi_bits == 4 * 8);
	assert(spi_out[0] == 0x8D && spi_out[1] == 0 && spi_out[2] == 0 &&
	       spi_out[3] == 0);
	assert(rx[0] == 0x12 && rx[1] == 0x34 && rx[2] == 0x56);

	/* A write: cmd and tx, nothing kept. */
	assert(bus.ops->transfer(&bus, 0x0D, args, 2, NULL, 0) == 0);
	assert(spi_bits == 3 * 8);
	assert(spi_out[0] == 0x0D && spi_out[1] == 0x11 && spi_out[2] == 0x22);

	/* Too long, counting tx and rx together; a failure is mapped. */
	assert(bus.ops->transfer(&bus, 0x0D, many, sizeof(many), rx, 1) ==
	       -EINVAL);
	rx[0] = 0x99;
	spi_err = ESP_ERR_TIMEOUT;
	assert(bus.ops->transfer(&bus, 0x8D, NULL, 0, rx, 1) == -ETIMEDOUT);
	assert(rx[0] == 0x99 && "rx untouched on failure");
	spi_err = ESP_OK;

	/* No receive() on SPI. */
	assert(tof_bus_receive(&bus, rx, 1, false) == -EOPNOTSUPP);
	tof_bus_detach(&bus);
	assert(spi_added == 0);
}

static void test_uart(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	uint8_t many[TOF_BUS_MAX_LEN + 1] = { 0 };
	struct tof_bus bus;
	uint8_t rx[2] = { 0 };

	/* The driver is the app's to install; without it, refuse. */
	reset();
	installed = false;
	assert(tof_uart_attach(&bus, UART_PORT, 100) == -EINVAL);
	installed = true;
	assert(tof_uart_attach(&bus, UART_PORT, 100) == 0);
	assert(bus.ops->kind == TOF_BUS_UART);

	/* A stale byte is dropped, then cmd and args go out, then the reply. */
	in[0] = 0xEE;
	in_len = 1;
	memcpy(reply, "\x12\x34", 2);
	reply_len = 2;
	assert(bus.ops->transfer(&bus, 0x13, args, 2, rx, 2) == 0);
	assert(strcmp(calls, "FWR") == 0);
	assert(out_len == 3 && out[0] == 0x13 && out[1] == 0x11 &&
	       out[2] == 0x22);
	assert(rx[0] == 0x12 && rx[1] == 0x34);
	/* 100 ms at the stub's 100 Hz: 10 ticks, plus one for the first. */
	assert(waited == 11);

	/* A command with no reply does not read. */
	reset();
	assert(bus.ops->transfer(&bus, 0x80, NULL, 0, NULL, 0) == 0);
	assert(strcmp(calls, "FW") == 0);

	/* A reply that stops short is a timeout. */
	reset();
	reply[0] = 0x12;
	reply_len = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -ETIMEDOUT);

	/* Driver failures. */
	reset();
	read_error = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EIO);
	reset();
	write_short = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EIO);
	assert(strcmp(calls, "FW") == 0);
	reset();
	flush_err = ESP_ERR_INVALID_ARG;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EINVAL);
	assert(strcmp(calls, "F") == 0);
	reset();
	assert(bus.ops->transfer(&bus, 0x13, many, sizeof(many), NULL, 0) ==
	       -EINVAL);
	assert(calls[0] == '\0');

	tof_bus_detach(&bus);
}

/*
 * A stream, nothing sent: receive() reads what comes, dropping what was
 * already waiting first only when asked to.
 */
static void test_uart_receive(void)
{
	uint8_t rx[4] = { 0 };
	struct tof_bus bus;

	reset();
	assert(tof_uart_attach(&bus, UART_PORT, 20) == 0);

	/* Fresh data: the stale byte goes, then the read -- which, with
	 * nothing new on the line, comes up short: a timeout. */
	in[0] = 0xEE;
	in_len = 1;
	assert(tof_bus_receive(&bus, rx, 1, true) == -ETIMEDOUT);
	assert(strcmp(calls, "FR") == 0 && in_len == 0);
	/* Not dropped: what waits is read. */
	reset();
	memcpy(in, "\x59\x59\x01", 3);
	in_len = 3;
	assert(tof_bus_receive(&bus, rx, 3, false) == 0);
	assert(strcmp(calls, "R") == 0 && rx[2] == 0x01);
	/* 20 ms at 100 Hz: 2 ticks rounded up, plus one. */
	assert(waited == 3);
	assert(tof_bus_receive(&bus, rx, 1, false) == -ETIMEDOUT);

	/* Driver failures; nothing to read is refused first. */
	reset();
	read_error = 1;
	assert(tof_bus_receive(&bus, rx, 2, false) == -EIO);
	reset();
	flush_err = ESP_ERR_INVALID_ARG;
	assert(tof_bus_receive(&bus, rx, 2, true) == -EINVAL);
	assert(strcmp(calls, "F") == 0);
	reset();
	assert(tof_bus_receive(&bus, rx, 0, false) == -EINVAL);
	assert(calls[0] == '\0');

	tof_bus_detach(&bus);
}

/*
 * tof_sleep_ms() waits long, never short: 2 ms is no tick at all through
 * pdMS_TO_TICKS() at IDF's default 100 Hz, which is how esp-vl53l1x-tof's
 * waits came to nothing. Here it is two ticks, 20 ms.
 */
static void test_sleep(void)
{
	int64_t t0 = esp_timer_get_time();

	assert(pdMS_TO_TICKS(2) == 0);
	tof_sleep_ms(2);
	assert(esp_timer_get_time() - t0 >= 2000);
	assert(esp_timer_get_time() - t0 == 20000);
	t0 = esp_timer_get_time();
	tof_yield();
	assert(esp_timer_get_time() - t0 == 10000);
	assert(tof_time_us() == esp_timer_get_time());
}

static void test_probe(void)
{
	/* Nothing ACKs: -ENXIO, which the handle API returns as NOT_FOUND. */
	probe_err = ESP_ERR_NOT_FOUND;
	assert(tof_i2c_probe(NULL, 0x2A, 100) == -ENXIO);
	assert(probe_addr == 0x2A && probe_timeout == 100);
	assert(tof_esp_err(-ENXIO) == ESP_ERR_NOT_FOUND);
	probe_err = ESP_ERR_TIMEOUT;
	assert(tof_i2c_probe(NULL, 0x2A, 100) == -ETIMEDOUT);
	probe_err = ESP_OK;
	assert(tof_i2c_probe(NULL, 0x2A, 100) == 0);
}

int main(void)
{
	test_i2c();
	test_spi();
	test_uart();
	test_uart_receive();
	test_probe();
	test_sleep();
	printf("test_port_esp: all passed\n");
	return 0;
}
