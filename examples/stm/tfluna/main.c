/*
 * TF-Luna on a UART: the stream at its default 100 Hz, one sample logged in
 * ten.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the
 * UART are the app's -- here huart2 as CubeMX generates it
 * (MX_USART2_UART_Init()) at 115200 8N1, the module's own (user manual
 * 6.2), its TXD to the MCU's RX. Pin 5 open or at 3.3 V for serial mode.
 * Needs HAL_UART_MODULE_ENABLED and TOF_TFLUNA = y.
 *
 * The blocking HAL receive only listens while it runs, so bytes that come
 * between two reads are lost; the driver resyncs on the 0x59 0x59 header
 * and the checksum, so a read costs at most a frame more.
 */
#include CMSIS_device_header

#include "tfluna.h"
#include "tof.h"
#include "tof_log.h"

extern UART_HandleTypeDef huart2;	/* from MX_USART2_UART_Init() */

void lidar_task(void)
{
	tfluna_handle_t h;
	struct tof_dev *tof;
	unsigned int n = 0;

	/* NULL config: 100 ms per frame or reply, cm frames. -ENXIO: silence;
	 * -EBADMSG: no checksum matches, the baud rate. */
	if (tfluna_init_uart(&huart2, NULL, &h))
		return;
	tof = tfluna_tof(h);

	for (;;) {
		struct tof_sample s;

		if (tof_read(tof, &s))
			continue;
		if (++n % 10 == 0)
			tof_log_sample(tof, &s);
	}
}
