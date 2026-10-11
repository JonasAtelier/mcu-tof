/*
 * VL53L1X: distance at 20 Hz, read when GPIO1 says a measurement is in.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the
 * I2C bus are the app's -- here hi2c1 as CubeMX generates it
 * (MX_I2C1_Init()), at up to 400 kHz, with pull-ups on SDA and SCL; XSHUT on
 * PA1, a push-pull output; GPIO1 on PB0, an EXTI input with a pull-up (it
 * is open drain). Needs HAL_I2C_MODULE_ENABLED and TOF_VL53L1X = y.
 */
#include CMSIS_device_header

#include "tof.h"
#include "tof_log.h"
#include "vl53l1x.h"

extern I2C_HandleTypeDef hi2c1;		/* from the app's MX_I2C1_Init() */

static struct tof_dev *tof;

/* GPIO1's EXTI: only a count; the bus is read in the main loop. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
	if (pin == GPIO_PIN_0)
		tof_irq(tof);
}

void distance_task(void)
{
	vl53l1x_config_t cfg = {
		.mode = VL53L1X_SHORT,	/* 1.3 m, steadier in daylight */
		.budget_ms = 33,
		.period_ms = 50,
		.xshut_port = GPIOA,
		.xshut_pin = GPIO_PIN_1,
	};
	vl53l1x_handle_t h;

	/* -ENXIO: nothing at 0x29. -ENODEV: it never booted, or is not a
	 * VL53L1X. */
	if (vl53l1x_init(&hi2c1, &cfg, &h))
		return;
	tof = vl53l1x_tof(h);
	tof_start(tof);

	for (;;) {
		struct tof_sample s;

		if (!tof_irq_pending(tof))
			continue;		/* or sleep: __WFI() */
		if (tof_read(tof, &s))
			continue;
		tof_log_sample(tof, &s);	/* s.valid before trusting it */
	}
}
