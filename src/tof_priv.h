/*
 * Shared by the chip drivers inside this library. Not public API.
 *
 * Two layers sit under the chips, the way regmap sits under a kernel driver:
 *
 *   - The bus kind is chosen at run time. A chip holds a struct tof_bus and
 *     reaches it only through bus->ops, so a driver can sit on I2C, SPI or
 *     a UART. Every category library offers all three kinds, used or not:
 *     the VL53L1X is on I2C, the TF-Luna on I2C or a UART, nothing on SPI.
 *   - The platform is chosen at link time. tof_port_esp.c and
 *     tof_port_stm.c each define the ops tables, the attach functions, the
 *     clock and the waits below for their platform; the other one compiles
 *     to nothing.
 *
 * ESP_PLATFORM, which ESP-IDF defines for every component, and the HAL's own
 * module switches are defined or not, never 0 or 1, so they are the one place
 * #ifdef is right; this library's CONFIG_TOF_* options are always tested
 * with #if.
 */
#ifndef TOF_PRIV_H
#define TOF_PRIV_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most bytes after cmd: tx on I2C and UART, tx and rx together on SPI. */
#define TOF_BUS_MAX_LEN		31

struct tof_bus;

enum tof_bus_kind {
	TOF_BUS_I2C,
	TOF_BUS_SPI,
	TOF_BUS_UART,
};

struct tof_bus_ops {
	enum tof_bus_kind kind;
	/*
	 * One transaction: cmd, then tx_len bytes of tx, then rx_len bytes
	 * into rx. cmd is the register address, with any SPI read bit already
	 * set by the chip, and the first byte of a datagram on a UART.
	 *
	 *   I2C:  rx_len ? cmd, repeated start, read : cmd and tx, one write;
	 *         a bare cmd is a one-byte write. A read takes at most one tx
	 *         byte, sent after cmd before the repeated start: that is a
	 *         16-bit register index, cmd its high byte and tx[0] its low
	 *         one, as the VL53L1X takes it. More tx before a read is
	 *         -EINVAL.
	 *   SPI:  CS low, cmd, tx, rx, CS high -- high again even when the
	 *         transfer failed.
	 *   UART: drop stale input, send cmd and tx, then wait for rx_len
	 *         bytes; fewer within the timeout is -ETIMEDOUT.
	 *
	 * A write with a 16-bit index needs nothing new: cmd is its high byte
	 * and tx starts with its low one. On SPI more than TOF_BUS_MAX_LEN
	 * bytes after cmd is -EINVAL; on I2C and UART only tx is bounded so,
	 * and rx on the HAL by its 16-bit count. 0 or a negative errno.
	 */
	int (*transfer)(struct tof_bus *bus, uint8_t cmd, const uint8_t *tx,
			size_t tx_len, uint8_t *rx, size_t rx_len);
	/*
	 * UART only; NULL on I2C and SPI. Wait for rx_len bytes with nothing
	 * sent, for a part that streams on its own instead of answering a
	 * command. drop_stale first drops whatever is already waiting, to
	 * start on fresh data. Fewer than rx_len within the timeout is
	 * -ETIMEDOUT; rx is then undefined.
	 */
	int (*receive)(struct tof_bus *bus, uint8_t *rx, size_t rx_len,
		       bool drop_stale);
	/* Undo the attach. The bus itself is the app's and is left alone. */
	void (*detach)(struct tof_bus *bus);
};

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"

/* The I2C argument of every I2C attach on this platform. */
typedef i2c_master_bus_handle_t tof_i2c_bus;

/*
 * One device on one bus, as its driver holds it. TOF_HAVE_BUS says the
 * platform has at least one bus kind, so the struct exists.
 */
#define TOF_HAVE_BUS
struct tof_bus {
	const struct tof_bus_ops *ops;
	uint32_t timeout_ms;
	union {
		i2c_master_dev_handle_t i2c;
		spi_device_handle_t spi;
		uart_port_t uart;
	};
};

/*
 * esp_err_t -> negative errno, at the bus boundary and nowhere else.
 * ESP_ERR_NOT_FOUND is -ENXIO (nothing at that address), not -ENODEV
 * (something answered but it is the wrong chip).
 */
static inline int tof_errno(esp_err_t err)
{
	switch (err) {
	case ESP_OK:
		return 0;
	case ESP_ERR_INVALID_ARG:
		return -EINVAL;
	case ESP_ERR_TIMEOUT:
		return -ETIMEDOUT;
	case ESP_ERR_NOT_FOUND:
		return -ENXIO;
	case ESP_ERR_INVALID_SIZE:
		return -EMSGSIZE;
	case ESP_ERR_NO_MEM:
		return -ENOMEM;
	case ESP_ERR_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	default:
		return -EIO;
	}
}

/*
 * negative errno -> esp_err_t, for the chips' handle APIs, which return
 * esp_err_t on ESP-IDF as the original driver did. The codes the original
 * returned come back as they were: -ENODEV and -ENXIO as
 * ESP_ERR_NOT_FOUND. The bus layer's own mapping is lossy in this
 * direction: an IDF code with no errno of its own came up as -EIO and goes
 * back as ESP_FAIL.
 */
static inline esp_err_t tof_esp_err(int err)
{
	switch (err) {
	case 0:
		return ESP_OK;
	case -EINVAL:
		return ESP_ERR_INVALID_ARG;
	case -ENOMEM:
		return ESP_ERR_NO_MEM;
	case -ETIMEDOUT:
		return ESP_ERR_TIMEOUT;
	case -EOPNOTSUPP:
		return ESP_ERR_NOT_SUPPORTED;
	case -ENODEV:
	case -ENXIO:
		return ESP_ERR_NOT_FOUND;
	case -EMSGSIZE:
		return ESP_ERR_INVALID_SIZE;
	default:
		return ESP_FAIL;
	}
}

#else /* STM32 HAL */
/*
 * Undefined, the include below fails with only
 * `#include expects "FILENAME" or <FILENAME>`. Every library source that
 * needs the HAL includes this header before any chip header, so one check
 * here names the cause on every build route. The HAL module checks sit in
 * its #else: without the family header no module looks enabled, and each of
 * them would blame a module instead.
 */
#ifndef CMSIS_device_header
#error "tof: CMSIS_device_header is not defined; define it as the STM32 family header, \"stm32f4xx.h\", \"stm32f7xx.h\" or \"stm32g0xx.h\" (CMake: set(MCU_DEVICE_HEADER stm32f4xx.h))"
#else
#include CMSIS_device_header

#if !defined(HAL_I2C_MODULE_ENABLED) && CONFIG_TOF_VL53L1X
#error "TOF_VL53L1X needs the HAL I2C module: #define HAL_I2C_MODULE_ENABLED in the app's stm32<family>xx_hal_conf.h, with stm32<family>xx_hal_i2c.c in the build"
#endif
#if !defined(HAL_I2C_MODULE_ENABLED) && !defined(HAL_UART_MODULE_ENABLED) && \
    CONFIG_TOF_TFLUNA
#error "TOF_TFLUNA needs the HAL I2C module or the UART module: #define HAL_I2C_MODULE_ENABLED or HAL_UART_MODULE_ENABLED in the app's stm32<family>xx_hal_conf.h, with its stm32<family>xx_hal_<i2c|uart>.c in the build"
#endif
#endif /* CMSIS_device_header */

#ifdef HAL_I2C_MODULE_ENABLED
typedef I2C_HandleTypeDef *tof_i2c_bus;
#endif

#if defined(HAL_I2C_MODULE_ENABLED) || defined(HAL_SPI_MODULE_ENABLED) || \
    defined(HAL_UART_MODULE_ENABLED)
#define TOF_HAVE_BUS
struct tof_bus {
	const struct tof_bus_ops *ops;
	uint32_t timeout_ms;
	union {
#ifdef HAL_I2C_MODULE_ENABLED
		struct {
			I2C_HandleTypeDef *handle;
			uint8_t address;	/* 7-bit */
		} i2c;
#endif
#ifdef HAL_SPI_MODULE_ENABLED
		struct {
			SPI_HandleTypeDef *handle;
			GPIO_TypeDef *cs_port;	/* app's output, idle high */
			uint16_t cs_pin;
		} spi;
#endif
#ifdef HAL_UART_MODULE_ENABLED
		UART_HandleTypeDef *uart;
#endif
	};
};
#endif

/*
 * HAL_StatusTypeDef -> negative errno, at the bus boundary and nowhere else.
 * HAL_BUSY -- the bus or the handle in use, or a handle never initialised --
 * is -EAGAIN. A NACK in the middle of a transfer is HAL_ERROR like any other
 * fault, so -EIO.
 */
static inline int tof_hal_errno(HAL_StatusTypeDef status)
{
	switch (status) {
	case HAL_OK:
		return 0;
	case HAL_TIMEOUT:
		return -ETIMEDOUT;
	case HAL_BUSY:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

#endif /* STM32 HAL */

/*
 * The chips' handle APIs return esp_err_t on ESP-IDF, as the original
 * driver did, and 0 or a negative errno on the HAL. The drivers work in
 * errno and convert once, where each public handle function returns.
 */
#ifdef ESP_PLATFORM
typedef esp_err_t tof_handle_ret;
#define TOF_HANDLE_RET(err)	tof_esp_err(err)
#else
typedef int tof_handle_ret;
#define TOF_HANDLE_RET(err)	(err)
#endif

#ifdef TOF_HAVE_BUS
/* bus->ops->receive(), or -EOPNOTSUPP on a kind that has none. */
static inline int tof_bus_receive(struct tof_bus *bus, uint8_t *rx,
				  size_t rx_len, bool drop_stale)
{
	if (!bus->ops->receive)
		return -EOPNOTSUPP;
	return bus->ops->receive(bus, rx, rx_len, drop_stale);
}

static inline void tof_bus_detach(struct tof_bus *bus)
{
	bus->ops->detach(bus);
}
#endif

#if defined(ESP_PLATFORM) || defined(HAL_I2C_MODULE_ENABLED)
/*
 * Is anything at this 7-bit address? -ENXIO when nothing ACKs -- nothing at
 * that address -- not -ENODEV, which means something answered but it is the
 * wrong chip. A NACK in the middle of a transfer is a plain -EIO on both
 * platforms, which is why a driver decides presence here, never by its
 * first transfer.
 */
int tof_i2c_probe(tof_i2c_bus handle, uint8_t address, uint32_t timeout_ms);

/*
 * Bind bus to the I2C device at address. On ESP-IDF the driver adds the
 * device at scl_hz, 0 for 400 kHz; on the HAL the clock is the handle's and
 * scl_hz is ignored. Undo with tof_bus_detach(). Every attach leaves
 * bus->ops NULL when it fails, so a bus with ops is one with something to
 * detach.
 */
int tof_i2c_attach(struct tof_bus *bus, tof_i2c_bus handle, uint8_t address,
		   uint32_t scl_hz, uint32_t timeout_ms);
#endif

#ifdef ESP_PLATFORM
/*
 * Add the SPI device on host's cs_pin, in SPI mode 0-3 at clock_hz. The
 * host is the app's, already spi_bus_initialize()d.
 */
int tof_spi_attach(struct tof_bus *bus, spi_host_device_t host, int cs_pin,
		   uint32_t clock_hz, uint8_t mode);

/* port's driver is the app's, already installed and configured. */
int tof_uart_attach(struct tof_bus *bus, uart_port_t port,
		    uint32_t timeout_ms);
#else
#ifdef HAL_SPI_MODULE_ENABLED
/*
 * Bind bus to the SPI device behind cs_port/cs_pin, and deselect it, so the
 * first transfer starts on a falling edge. Mode, clock and frame are the
 * handle's; the chip select is an output the app has set up.
 */
int tof_spi_attach(struct tof_bus *bus, SPI_HandleTypeDef *handle,
		   GPIO_TypeDef *cs_port, uint16_t cs_pin,
		   uint32_t timeout_ms);
#endif
#ifdef HAL_UART_MODULE_ENABLED
/* handle is the app's, already HAL_UART_Init()ed. */
int tof_uart_attach(struct tof_bus *bus, UART_HandleTypeDef *handle,
		    uint32_t timeout_ms);
#endif
#endif

/* Block for at least ms milliseconds: long, never short. */
void tof_sleep_ms(uint32_t ms);

/*
 * Between two polls: on ESP-IDF a yield of one tick, not a timed wait; on
 * the HAL, which has no scheduler, one millisecond.
 */
void tof_yield(void);

/*
 * Whole milliseconds on tof_time_us() since start, a value it returned. A
 * deadline is "tof_elapsed_ms(start) >= timeout_ms": on the clock, so a
 * wait lasts as long whatever the tick rate and however long a poll takes.
 */
uint32_t tof_elapsed_ms(int64_t start);

#endif /* TOF_PRIV_H */
