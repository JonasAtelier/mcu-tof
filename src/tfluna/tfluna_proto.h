/*
 * TF-Luna serial protocol and I2C registers: frame bytes, command IDs,
 * scales and the checksum. Every value cites Benewake's "TF-Luna User
 * Manual", REV 01/04/2024, in docs/tfluna_um_2024.pdf, by section and
 * printed page (the PDF's own page is four more). Pure, so it compiles and
 * is asserted on the host.
 *
 * Pin 5 picks the interface at power-up: to ground for I2C, open or 3.3 V
 * for the UART (6.1, Table 6, p.8). Nothing on either interface switches
 * it.
 */
#ifndef TFLUNA_PROTO_H
#define TFLUNA_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tof.h"

/* ---------------------------------------------------------------- UART -- */

/* 6.2, p.8: 8 data bits, 1 stop bit, no parity, 115200 bps by default. */
#define TFLUNA_BAUD_DEFAULT	115200

/*
 * The data frame the module streams, in the 9-byte/cm format it starts in
 * and in 9-byte/mm (Appendix I items 1 and 3, p.20): 0x59 0x59, Dist, Amp,
 * Temp, each low byte first, then the checksum.
 */
#define TFLUNA_DATA_HEADER	0x59
#define TFLUNA_DATA_LEN		9
#define TFLUNA_DATA_DIST	2
#define TFLUNA_DATA_AMP		4
#define TFLUNA_DATA_TEMP	6

/*
 * A command, and the module's reply to one: 0x5A, Len -- the whole frame,
 * head to checksum, 4 to 255 bytes -- ID, payload, checksum (6.2, p.8-9).
 */
#define TFLUNA_CMD_HEADER	0x5A
#define TFLUNA_CMD_MIN_LEN	4
#define TFLUNA_CMD_LEN		1	/* offset of Len */
#define TFLUNA_CMD_ID		2	/* offset of ID */
#define TFLUNA_CMD_PAYLOAD	3	/* offset of the payload */

/*
 * The longest reply this driver waits for is GET_VERSION's, 7 bytes
 * (Appendix II item 1, p.22). A 0x5A whose Len says more is not taken for
 * a frame start, so a stray 0x5A in a data frame costs at most this many
 * bytes of look-ahead.
 */
#define TFLUNA_REPLY_MAX	16

/* Command IDs, Appendix II (p.22-26). */
#define TFLUNA_ID_GET_VERSION	0x01	/* item 1, p.22 */
#define TFLUNA_ID_SAMPLE_FREQ	0x03	/* item 3, p.22-23 */
#define TFLUNA_ID_SAMPLE_TRIG	0x04	/* item 4, p.23 */
#define TFLUNA_ID_OUTPUT_EN	0x07	/* item 7, p.24 */
#define TFLUNA_ID_SAVE		0x11	/* item 11, p.26 */

/*
 * Output frequency: 0 is trigger mode (6.6, p.11); otherwise at most 250 Hz,
 * and the module runs at 500/n Hz, n from 2 to 500 (6.5, p.10-11).
 */
#define TFLUNA_RATE_MAX_HZ	250

/* ----------------------------------------------------------------- I2C -- */

/* 6.3, p.9: 0x10 by default, 0x08 to 0x77, up to 400 kHz. */
#define TFLUNA_ADDR_DEFAULT	0x10
#define TFLUNA_ADDR_MIN		0x08
#define TFLUNA_ADDR_MAX		0x77
#define TFLUNA_SCL_MAX_HZ	400000

/* Appendix III, p.32-33. Two-byte values low byte first. */
#define TFLUNA_REG_DIST		0x00	/* cm */
#define TFLUNA_REG_AMP		0x02
#define TFLUNA_REG_TEMP		0x04	/* 0.01 degC */
#define TFLUNA_REG_VERSION	0x0A	/* revision, minor, major */
#define TFLUNA_REG_SAVE		0x20	/* write 0x01 to save */
#define TFLUNA_REG_MODE		0x23	/* 0x00 continuous, 0x01 trigger */
#define TFLUNA_REG_TRIG_ONE_SHOT 0x24	/* write 0x01, trigger mode only */
#define TFLUNA_REG_ENABLE	0x25	/* 0x00 off, 0x01 on */
#define TFLUNA_REG_FPS		0x26	/* Hz, two bytes */
#define TFLUNA_REG_SIGNATURE	0x3C	/* 'L' 'U' 'N' 'A' */

#define TFLUNA_SAVE_NOW		0x01
#define TFLUNA_MODE_CONTINUOUS	0x00
#define TFLUNA_MODE_TRIGGER	0x01
#define TFLUNA_TRIGGER_ONCE	0x01

/* The measurement registers in one read: DIST, AMP, TEMP. */
#define TFLUNA_REG_MEASURE_LEN	6

/* -------------------------------------------------------- validity -- */

/*
 * 6.4, p.10, and Appendix I, p.20: Dist is unreliable when Amp is under 100,
 * or 0xFFFF, overexposed; above 32768 the module has seen ambient light
 * overexposure, facing the sun for one. Below amp_threshold * 10, 100 by
 * default, the module puts out its dummy distance, 0 by default, instead
 * (6.8, p.12): that is the under-100 case.
 */
#define TFLUNA_AMP_MIN		100
#define TFLUNA_AMP_AMBIENT	32768
#define TFLUNA_AMP_SATURATED	0xFFFF

/* ------------------------------------------------------------ helpers -- */

static inline uint16_t tfluna_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/*
 * The checksum: the low 8 bits of the sum of every byte before it (6.2,
 * p.9). Upward frames always carry a valid one (Appendix II item 8, p.25);
 * the module checks the downward ones only once told to.
 */
static inline uint8_t tfluna_sum(const uint8_t *p, size_t n)
{
	uint8_t sum = 0;

	for (size_t i = 0; i < n; i++)
		sum = (uint8_t)(sum + p[i]);
	return sum;
}

/* Either header's first byte: a frame can start here. */
static inline bool tfluna_header(uint8_t b)
{
	return b == TFLUNA_DATA_HEADER || b == TFLUNA_CMD_HEADER;
}

/*
 * How long the frame starting at w is, from its first two bytes: 9 for a
 * data frame, Len for a reply, 0 if no frame starts here.
 */
static inline size_t tfluna_frame_len(const uint8_t *w)
{
	if (w[0] == TFLUNA_DATA_HEADER && w[1] == TFLUNA_DATA_HEADER)
		return TFLUNA_DATA_LEN;
	if (w[0] == TFLUNA_CMD_HEADER && w[1] >= TFLUNA_CMD_MIN_LEN &&
	    w[1] <= TFLUNA_REPLY_MAX)
		return w[1];
	return 0;
}

static inline bool tfluna_frame_valid(const uint8_t *frame, size_t len)
{
	return tfluna_sum(frame, len - 1) == frame[len - 1];
}

/*
 * A command frame for id with n bytes of payload into out, which holds at
 * least n + 4; its length. The checksum is filled in even though the module
 * ignores it until told otherwise (6.2, p.9).
 */
static inline size_t tfluna_command(uint8_t *out, uint8_t id,
				    const uint8_t *payload, size_t n)
{
	size_t len = TFLUNA_CMD_MIN_LEN + n;

	out[0] = TFLUNA_CMD_HEADER;
	out[TFLUNA_CMD_LEN] = (uint8_t)len;
	out[TFLUNA_CMD_ID] = id;
	for (size_t i = 0; i < n; i++)
		out[TFLUNA_CMD_PAYLOAD + i] = payload[i];
	out[len - 1] = tfluna_sum(out, len - 1);
	return len;
}

/* What Amp says of the distance; see the validity rules above. */
static inline enum tof_status tfluna_status(uint16_t amp)
{
	if (amp == TFLUNA_AMP_SATURATED || amp > TFLUNA_AMP_AMBIENT)
		return TOF_OVEREXPOSED;
	if (amp < TFLUNA_AMP_MIN)
		return TOF_WEAK_SIGNAL;
	return TOF_VALID;
}

/* The UART frame's Temp: Temp / 8 - 256 degC (6.4, p.10; Appendix I, p.20). */
static inline float tfluna_uart_temp_c(uint16_t raw)
{
	return (float)raw / 8.0f - 256.0f;
}

/*
 * The I2C TEMP register, in 0.01 degC (Appendix III, p.32): a different
 * encoding from the UART frame's. Read as signed; the table does not say,
 * and below 0 degC an unsigned reading would be absurd.
 */
static inline float tfluna_i2c_temp_c(uint16_t raw)
{
	return (float)(int16_t)raw / 100.0f;
}

#endif /* TFLUNA_PROTO_H */
