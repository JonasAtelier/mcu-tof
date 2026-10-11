/*
 * Kernel dev_err()-style logging for ToF drivers and their users.
 *
 * ESP-IDF: everything goes out under the one ESP-IDF tag "tof", prefixed
 * with the device's name, so one esp_log_level_set("tof", ...) controls
 * every chip and a line still says which chip it came from. There is
 * deliberately no level machinery of our own; tof_dbg() is also subject
 * to CONFIG_LOG_MAXIMUM_LEVEL, and CONFIG_TOF_DEBUG is not used.
 *
 * STM32 HAL: every line goes out through printf() as "tof: <name>: ...",
 * so a line still says which chip it came from. Where printf() ends up is
 * the app's business: with newlib, retarget _write() to a UART. There is
 * deliberately no level machinery; tof_dbg() alone is compiled out
 * unless CONFIG_TOF_DEBUG is 1 (stm-forge: TOF_DEBUG = y).
 */
#ifndef TOF_LOG_H
#define TOF_LOG_H

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <stdio.h>
#endif

#include "tof.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#define TOF_LOG_TAG	"tof"

#define tof_err(tof, fmt, ...) \
	ESP_LOGE(TOF_LOG_TAG, "%s: " fmt, (tof)->name, ##__VA_ARGS__)
#define tof_warn(tof, fmt, ...) \
	ESP_LOGW(TOF_LOG_TAG, "%s: " fmt, (tof)->name, ##__VA_ARGS__)
#define tof_info(tof, fmt, ...) \
	ESP_LOGI(TOF_LOG_TAG, "%s: " fmt, (tof)->name, ##__VA_ARGS__)
#define tof_dbg(tof, fmt, ...) \
	ESP_LOGD(TOF_LOG_TAG, "%s: " fmt, (tof)->name, ##__VA_ARGS__)

#else /* STM32 HAL */
#define tof_err(tof, fmt, ...) \
	printf("tof: %s: " fmt "\n", (tof)->name, ##__VA_ARGS__)
#define tof_warn(tof, fmt, ...) \
	printf("tof: %s: " fmt "\n", (tof)->name, ##__VA_ARGS__)
#define tof_info(tof, fmt, ...) \
	printf("tof: %s: " fmt "\n", (tof)->name, ##__VA_ARGS__)

#if CONFIG_TOF_DEBUG
#define tof_dbg(tof, fmt, ...) \
	printf("tof: %s: " fmt "\n", (tof)->name, ##__VA_ARGS__)
#else
/* Still type-checked, so a debug line cannot rot while it is off. */
#define tof_dbg(tof, fmt, ...) do { \
	if (0) \
		printf("tof: %s: " fmt "\n", (tof)->name, \
		       ##__VA_ARGS__); \
} while (0)
#endif
#endif /* ESP_PLATFORM */

/* One info line: timestamp, distance, status, signal, ambient and, where
 * the chip has one, temperature. */
void tof_log_sample(const struct tof_dev *tof, const struct tof_sample *s);

#ifdef __cplusplus
}
#endif

#endif /* TOF_LOG_H */
