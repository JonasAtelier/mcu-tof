/*
 * vl53l1x — ESP-IDF driver for the ST VL53L1X time-of-flight sensor.
 *
 * It fires an invisible 940 nm laser and times the photons back, so unlike an
 * ultrasonic or IR-reflectance sensor it reports true distance in millimetres
 * and does not care what colour the target is. Up to 4 m in the dark, roughly
 * 1.3 m in bright sun.
 *
 * A warning about provenance: ST does not publish this chip's register map.
 * The datasheet in docs/ says so in as many words (section 4.2 — "full
 * register details are not exposed"), and gives only the three ID registers.
 * Everything else here — the register addresses, the 91-byte boot
 * configuration, the distance-mode and timing-budget magic numbers — is taken
 * from ST's own ultra lite driver, STSW-IMG009, which is the vendor's
 * published reference implementation. Treat those constants as fixed. Do not
 * invent register writes for this chip.
 */
/*
 * In mcu-tof: the same calls on the STM32 HAL, where they return 0 or a
 * negative errno and XSHUT is a port and pin the app has made an output;
 * and vl53l1x_tof(), the handle as a struct tof_dev. Every value the driver
 * writes now cites ST's ultra lite driver, version 3.4.0, by line, in
 * src/vl53l1x/vl53l1x_regs.h. Waits are on the clock, so they hold at any
 * FreeRTOS tick rate; a bus error during init comes back as itself, and
 * ESP_ERR_NOT_FOUND means nothing answered, the chip never booted, or its
 * model ID is wrong.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#else
#include CMSIS_device_header
#endif

#include "tof.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VL53L1X_ADDR    0x29    /* 7-bit; the datasheet quotes 0x52, 8-bit */

typedef struct vl53l1x_t *vl53l1x_handle_t;

/* How far the sensor is asked to look. Short reaches only 1.3 m but is far
 * more resistant to ambient light — outdoors, or under bright lighting, it is
 * usually the better reading even within long's range. */
typedef enum {
    VL53L1X_SHORT = 1,      /* up to ~1.3 m */
    VL53L1X_LONG  = 2,      /* up to ~4 m, the power-on choice here */
} vl53l1x_mode_t;

/* Result quality, as ST's driver reports it. Anything other than VALID means
 * distance_mm is not trustworthy — check it, do not just read the number. */
typedef enum {
    VL53L1X_VALID       = 0,
    VL53L1X_SIGMA_FAIL  = 1,  /* too noisy to trust */
    VL53L1X_SIGNAL_FAIL = 2,  /* too little light back: far, dark or angled */
    VL53L1X_OUT_OF_BOUNDS = 4,
    VL53L1X_HARDWARE_FAIL = 5,
    VL53L1X_WRAPAROUND  = 7,  /* very shiny target beyond range, reads short */
} vl53l1x_status_t;

typedef struct {
    uint16_t distance_mm;
    uint8_t  status;        /* a vl53l1x_status_t, or another ST code */
    uint16_t signal_kcps;   /* light coming back; more is a firmer reading */
    uint16_t ambient_kcps;  /* background light; high means a hostile scene */
} vl53l1x_result_t;

#ifdef ESP_PLATFORM
typedef struct {
    uint32_t       scl_speed_hz; /* 0 means 400000, the chip's maximum */
    vl53l1x_mode_t mode;         /* 0 selects VL53L1X_LONG */

    /* How long the sensor integrates for, in ms. Longer sees further and
     * reads steadier. Must be one of 15 (short mode only), 20, 33, 50, 100,
     * 200 or 500 — the chip has a calibration table, not a formula, so
     * anything else is rejected. 0 selects 50. */
    uint16_t       budget_ms;

    /* Gap between the *starts* of measurements, in ms. Must be at least
     * budget_ms; 0 selects budget_ms, which means measure back to back. */
    uint16_t       period_ms;

    /* XSHUT, active low. Pulled high on most breakouts, so GPIO_NUM_NC is
     * fine — but with several sensors on one bus you need it, one pin each,
     * to bring them up one at a time and re-address them. */
    gpio_num_t     xshut_gpio;
} vl53l1x_config_t;
#else
typedef struct {
    vl53l1x_mode_t mode;         /* 0 selects VL53L1X_LONG */
    uint16_t       budget_ms;    /* as on ESP-IDF; 0 selects 50 */
    uint16_t       period_ms;    /* as on ESP-IDF; 0 selects budget_ms */

    /* XSHUT, active low, as an output the app has already set up; NULL
     * xshut_port if it is not wired. The I2C clock is the handle's. */
    GPIO_TypeDef  *xshut_port;
    uint16_t       xshut_pin;
} vl53l1x_config_t;
#endif

#ifdef ESP_PLATFORM
/* Attach to an already-initialised I2C master bus, boot the sensor and load
 * ST's configuration. cfg may be NULL for defaults. Takes about 50 ms — it
 * runs one throwaway measurement, as ST's driver does, to calibrate. Fails
 * with ESP_ERR_NOT_FOUND if the chip does not answer or is not a VL53L1X. */
esp_err_t vl53l1x_init(i2c_master_bus_handle_t bus, const vl53l1x_config_t *cfg,
                       vl53l1x_handle_t *out);

/* Begin measuring, repeatedly, in the background. Nothing is read until you
 * call it; the sensor keeps going until vl53l1x_stop(). */
esp_err_t vl53l1x_start(vl53l1x_handle_t dev);
esp_err_t vl53l1x_stop(vl53l1x_handle_t dev);

/* Is there a fresh measurement waiting? */
esp_err_t vl53l1x_data_ready(vl53l1x_handle_t dev, bool *ready);

/* Take the waiting measurement and let the sensor start the next one. Call it
 * only when vl53l1x_data_ready() said yes, or use vl53l1x_read_blocking(). */
esp_err_t vl53l1x_read(vl53l1x_handle_t dev, vl53l1x_result_t *out);

/* Wait for a measurement, then take it. Returns ESP_ERR_TIMEOUT if none
 * arrives in time — allow at least budget_ms plus a little. */
esp_err_t vl53l1x_read_blocking(vl53l1x_handle_t dev, vl53l1x_result_t *out,
                                uint32_t timeout_ms);

/* Move this sensor to a different I2C address so several can share one bus.
 * Needs xshut_gpio: bring the sensors up one at a time, re-addressing each
 * before releasing the next. The new address lives in RAM — redo it on every
 * boot. */
esp_err_t vl53l1x_set_address(vl53l1x_handle_t dev, uint8_t addr);

/* A short description of a result's status, for logging. */
const char *vl53l1x_status_str(uint8_t status);

esp_err_t vl53l1x_del(vl53l1x_handle_t dev);
#else
/* bus is HAL_I2C_Init()ed by the app, at 400 kHz or less. -ENXIO if
 * nothing answers at 0x29, -ENODEV if the chip never boots or is not a
 * VL53L1X. */
#ifdef HAL_I2C_MODULE_ENABLED
int vl53l1x_init(I2C_HandleTypeDef *bus, const vl53l1x_config_t *cfg,
                 vl53l1x_handle_t *out);
#endif

int vl53l1x_start(vl53l1x_handle_t dev);
int vl53l1x_stop(vl53l1x_handle_t dev);
int vl53l1x_data_ready(vl53l1x_handle_t dev, bool *ready);
int vl53l1x_read(vl53l1x_handle_t dev, vl53l1x_result_t *out);
int vl53l1x_read_blocking(vl53l1x_handle_t dev, vl53l1x_result_t *out,
                          uint32_t timeout_ms);
int vl53l1x_set_address(vl53l1x_handle_t dev, uint8_t addr);
const char *vl53l1x_status_str(uint8_t status);
int vl53l1x_del(vl53l1x_handle_t dev);
#endif

/* The handle as a struct tof_dev; NULL for a NULL handle. Valid until
 * vl53l1x_del(). Its samples carry the signal and ambient rates in kcps,
 * stopped at 65535 where vl53l1x_read() lets them wrap. */
struct tof_dev *vl53l1x_tof(vl53l1x_handle_t dev);

#ifdef __cplusplus
}
#endif
