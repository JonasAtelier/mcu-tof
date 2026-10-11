/*
 * tfluna -- Benewake TF-Luna single-point LiDAR module, as a tof chip
 * driver, on its UART or on I2C.
 *
 * A time-of-flight module, 0.2 to 8 m (user manual 3.2, Table 1), that
 * measures 500 times a second and puts out the average, at 100 Hz by
 * default (6.5). Which interface it speaks is wired, not commanded: pin 5
 * to ground at power-up for I2C, open or 3.3 V for the UART (6.1, Table 6).
 * Pick the init that matches.
 *
 * The manual in docs/ is Benewake's "TF-Luna User Manual", REV 01/04/2024.
 * Every byte, register and scale in src/tfluna/tfluna_proto.h cites its
 * section and printed page. Nothing here has been run against a physical
 * module.
 *
 * On the UART the module streams 9-byte frames on its own; this driver
 * reads that stream, checks every frame's checksum, and resyncs on the
 * 0x59 0x59 header. On I2C it reads the measurement registers.
 *
 * Every call returns esp_err_t on ESP-IDF and 0 or a negative errno on the
 * STM32 HAL. Nothing is saved to the module's flash unless tfluna_save() is
 * called.
 */
#ifndef TFLUNA_H
#define TFLUNA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#else
#include CMSIS_device_header
#endif

#include "tof.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tfluna_t *tfluna_handle_t;

typedef struct {
    uint16_t distance_mm;   /* the module's cm x 10, or its mm */
    uint16_t amp;           /* signal strength; see tfluna_valid() */
    float    temp_c;        /* the module's chip temperature */
} tfluna_result_t;

/* All zero, or a NULL pointer, selects the defaults. */
typedef struct {
    /* I2C: 0 means 0x10, the module's default; otherwise 0x08-0x77. */
    uint8_t  addr;
    /* I2C on ESP-IDF: 0 means 400000, the module's most (6.3). On the HAL
     * the clock is the handle's. */
    uint32_t scl_speed_hz;
    /* UART: the longest wait for a frame or a reply. 0 means 100 ms, ten
     * frames at the default 100 Hz; raise it for a slow rate or for
     * trigger mode. */
    uint32_t timeout_ms;
    /* UART: the module was set to the 9-byte/mm format (0x06) and saved;
     * frames are then read as mm instead of cm. */
    bool     millimetres;
} tfluna_config_t;

#ifdef ESP_PLATFORM
/*
 * port's driver is the app's, already installed at the module's 115200 8N1
 * (6.2). Asks the module for its version: ESP_ERR_NOT_FOUND if nothing comes
 * back, or valid frames do but no answer; ESP_FAIL if bytes arrive but no
 * frame's checksum matches -- the baud rate, most likely.
 */
esp_err_t tfluna_init_uart(uart_port_t port, const tfluna_config_t *cfg,
                           tfluna_handle_t *out);

/* bus is an already-initialised I2C master bus. ESP_ERR_NOT_FOUND if
 * nothing answers at the address, or what does is not signed "LUNA". */
esp_err_t tfluna_init_i2c(i2c_master_bus_handle_t bus,
                          const tfluna_config_t *cfg, tfluna_handle_t *out);

/* The next measurement: on the UART the next frame after fresh data
 * starts, on I2C the registers as they are now. */
esp_err_t tfluna_read(tfluna_handle_t dev, tfluna_result_t *out);

/* Output rate in Hz, 1-250; 0 is trigger mode, one measurement per
 * tfluna_trigger(). Not saved. */
esp_err_t tfluna_set_rate(tfluna_handle_t dev, uint16_t hz);

/* One measurement, in trigger mode; read it with tfluna_read(). */
esp_err_t tfluna_trigger(tfluna_handle_t dev);

/* Write the current settings to the module's flash (Appendix II item 11;
 * Appendix III 0x20). */
esp_err_t tfluna_save(tfluna_handle_t dev);

esp_err_t tfluna_del(tfluna_handle_t dev);
#else
#ifdef HAL_UART_MODULE_ENABLED
/* handle is HAL_UART_Init()ed by the app at 115200 8N1. -ENXIO if nothing
 * comes back, -EBADMSG if no checksum matches, -ENODEV if valid frames come
 * but no answer to the version request. */
int tfluna_init_uart(UART_HandleTypeDef *uart, const tfluna_config_t *cfg,
                     tfluna_handle_t *out);
#endif
#ifdef HAL_I2C_MODULE_ENABLED
/* bus is HAL_I2C_Init()ed by the app, at 400 kHz or less. -ENXIO if nothing
 * answers at the address, -ENODEV if it is not signed "LUNA". */
int tfluna_init_i2c(I2C_HandleTypeDef *bus, const tfluna_config_t *cfg,
                    tfluna_handle_t *out);
#endif

int tfluna_read(tfluna_handle_t dev, tfluna_result_t *out);
int tfluna_set_rate(tfluna_handle_t dev, uint16_t hz);
int tfluna_trigger(tfluna_handle_t dev);
int tfluna_save(tfluna_handle_t dev);
int tfluna_del(tfluna_handle_t dev);
#endif

/* Is the distance worth using? Amp at least 100, and neither 0xFFFF nor
 * above 32768 (6.4). */
bool tfluna_valid(const tfluna_result_t *r);

/*
 * The handle as a struct tof_dev; NULL for a NULL handle. Valid until
 * tfluna_del(). tof_start() and tof_stop() switch the output (UART) or the
 * LiDAR (I2C) on and off; the module starts on. There is no data-ready
 * query: on the UART tof_read() waits for the next frame, on I2C it reads
 * the registers -- wire pin 6 to tof_irq() to read each one once.
 */
struct tof_dev *tfluna_tof(tfluna_handle_t dev);

#ifdef __cplusplus
}
#endif

#endif /* TFLUNA_H */
