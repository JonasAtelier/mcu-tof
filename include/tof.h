/*
 * tof -- a small Linux-IIO-style time-of-flight distance subsystem for
 * ESP-IDF and the STM32 HAL.
 *
 * One struct tof_dev per sensor. It holds what every ranging sensor shares:
 * start and stop, "is a measurement waiting?", and the measurement itself,
 * as a struct tof_sample in millimetres with the chip's verdict on it. Code
 * above this header sees one shape of sample whichever chip produced it.
 *
 * Each chip keeps its own handle API; the handle holds a struct tof_dev,
 * and <chip>_tof() hands it out. The handle's init fills it and its del
 * ends it: there is no probe or remove of its own.
 *
 * Every int-returning function returns 0 or a negative errno.
 */
#ifndef TOF_H
#define TOF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the chip thinks of a measurement, in one scale for every chip. Only
 * TOF_VALID makes distance_mm worth using; raw_status keeps the chip's own
 * code for the rest.
 */
enum tof_status {
	TOF_VALID = 0,
	TOF_WEAK_SIGNAL,	/* too little light back: far, dark or angled */
	TOF_OVEREXPOSED,	/* too much light: sun or a mirror */
	TOF_NOISY,		/* the spread of the estimate is too wide */
	TOF_OUT_OF_RANGE,
	TOF_WRAPAROUND,		/* a shiny target past the range reads short */
	TOF_HW_FAIL,
	TOF_OTHER,		/* a code this scale has no name for */
};

struct tof_sample {
	uint16_t distance_mm;
	/*
	 * How much light came back, and how much was there anyway, in the
	 * chip's own unit: kcps on the VL53L1X, the dimensionless Amp on the
	 * TF-Luna (whose ambient is 0). Saturates at 65535.
	 */
	uint16_t signal;
	uint16_t ambient;
	float temp_c;			/* NAN on a chip without a sensor */
	enum tof_status status;
	uint8_t raw_status;		/* the chip's own code */
	bool valid;			/* status == TOF_VALID */
	int64_t t_us;			/* tof_time_us() at read */
};

struct tof_dev;

struct tof_ops {
	int (*start)(struct tof_dev *tof);
	int (*stop)(struct tof_dev *tof);
	/* optional: is a measurement waiting, without taking it */
	int (*data_ready)(struct tof_dev *tof, bool *ready);
	/*
	 * Take the waiting measurement; -EAGAIN if a chip that is polled has
	 * none yet. Fills everything but t_us and valid.
	 */
	int (*read)(struct tof_dev *tof, struct tof_sample *s);
	/* optional: time between measurements, and time spent on each */
	int (*set_period_ms)(struct tof_dev *tof, uint32_t ms);
	int (*set_budget_ms)(struct tof_dev *tof, uint32_t ms);
};

struct tof_dev {
	const char *name;		/* "vl53l1x"; log prefix */
	const struct tof_ops *ops;
	void *priv;			/* the chip's handle */

	/* tof_irq() and tof_irq_pending(); see below */
	volatile uint32_t irq_count;
	uint32_t irq_seen;
};

int tof_start(struct tof_dev *tof);
int tof_stop(struct tof_dev *tof);

/* -EOPNOTSUPP on a chip that cannot tell: a streaming one, for instance. */
int tof_data_ready(struct tof_dev *tof, bool *ready);

/*
 * Timestamp, then the chip's read. -EAGAIN when a polled chip has nothing
 * waiting; a streaming chip waits for its next frame instead, for as long
 * as its bus timeout. *s is written only on success.
 */
int tof_read(struct tof_dev *tof, struct tof_sample *s);

/*
 * tof_read() until it has a measurement or timeout_ms has passed on
 * tof_time_us(): a deadline on the clock, not a count of polls, so it waits
 * as long on a 100 Hz tick as on a 1 kHz one. -ETIMEDOUT when it ran out.
 */
int tof_read_blocking(struct tof_dev *tof, struct tof_sample *s,
		      uint32_t timeout_ms);

/* -EOPNOTSUPP on a chip without; the chip's own limits otherwise. */
int tof_set_period_ms(struct tof_dev *tof, uint32_t ms);
int tof_set_budget_ms(struct tof_dev *tof, uint32_t ms);

/*
 * The chip's data-ready line, on either platform: the VL53L1X's GPIO1 or the
 * TF-Luna's pin 6. Optional; polling works without it. Call tof_irq() from
 * the app's GPIO ISR or EXTI callback: it only counts, and never touches the
 * bus. In thread context, tof_irq_pending() is true once for every batch of
 * counts since it last looked; then call tof_read() there. One caller of
 * tof_irq_pending() per device.
 *
 * On ESP-IDF tof_irq() is in IRAM and touches only the struct tof_dev, in
 * RAM, so it is safe in an ISR registered with ESP_INTR_FLAG_IRAM, which
 * runs while flash is off; the dev must not be const or in flash.
 */
void tof_irq(struct tof_dev *tof);
bool tof_irq_pending(struct tof_dev *tof);

/*
 * The clock tof_read() stamps samples with, in microseconds. On ESP-IDF it
 * is esp_timer_get_time(). On the STM32 HAL the default is weak and only
 * HAL_GetTick() * 1000: millisecond steps that wrap after 49.7 days. Define
 * your own tof_time_us() for real microseconds.
 */
int64_t tof_time_us(void);

#ifdef __cplusplus
}
#endif

#endif /* TOF_H */
