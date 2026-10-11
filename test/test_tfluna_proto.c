/*
 * Host-side check of src/tfluna/tfluna_proto.h against the TF-Luna user
 * manual's own examples (REV 01/04/2024; printed pages): the checksum,
 * frame lengths, the command builder, the validity rules and both
 * temperature encodings. No bus, no platform.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tfluna_proto.h"

static void test_checksum(void)
{
	/* Appendix II item 8, p.25: "Disable checksum comparison
	 * [5A 05 08 00 67]" -- the one example with a real checksum. */
	static const uint8_t disable[5] = { 0x5A, 0x05, 0x08, 0x00, 0x67 };
	uint8_t frame[9] = { 0x59, 0x59, 0x2C, 0x01, 0xE8, 0x03, 0x00, 0x09 };

	assert(tfluna_sum(disable, 4) == 0x67);
	assert(tfluna_frame_valid(disable, sizeof disable));

	/* A data frame: the low byte of the sum, carries dropped. */
	frame[8] = tfluna_sum(frame, 8);
	assert(frame[8] == (uint8_t)(0x59 + 0x59 + 0x2C + 0x01 + 0xE8 + 0x03 +
				     0x00 + 0x09));
	assert(tfluna_frame_valid(frame, 9));
	frame[3] ^= 0x01;
	assert(!tfluna_frame_valid(frame, 9));
}

static void test_frame_len(void)
{
	static const uint8_t data[2] = { 0x59, 0x59 };
	static const uint8_t half[2] = { 0x59, 0x5A };
	static const uint8_t version[2] = { 0x5A, 0x07 };
	static const uint8_t too_short[2] = { 0x5A, 0x03 };
	static const uint8_t too_long[2] = { 0x5A, TFLUNA_REPLY_MAX + 1 };
	static const uint8_t junk[2] = { 0x00, 0x59 };

	assert(tfluna_frame_len(data) == 9);
	assert(tfluna_frame_len(half) == 0);
	assert(tfluna_frame_len(version) == 7);
	assert(tfluna_frame_len(too_short) == 0);
	assert(tfluna_frame_len(too_long) == 0);
	assert(tfluna_frame_len(junk) == 0);
	assert(tfluna_header(0x59) && tfluna_header(0x5A));
	assert(!tfluna_header(0x58) && !tfluna_header(0x00));
}

/*
 * The manual's commands, byte for byte up to the checksum, which its
 * examples mostly leave 00 because the module ignores it by default (6.2,
 * p.9); the builder fills it in.
 */
static void test_commands(void)
{
	static const uint8_t hz10[2] = { 0x0A, 0x00 };
	static const uint8_t hz250[2] = { 0xFA, 0x00 };
	static const uint8_t off = 0x00;
	uint8_t out[16];

	/* Appendix II item 3, p.23: 10 Hz [5A 06 03 0A 00 00]. */
	assert(tfluna_command(out, TFLUNA_ID_SAMPLE_FREQ, hz10, 2) == 6);
	assert(!memcmp(out, "\x5A\x06\x03\x0A\x00", 5));
	assert(out[5] == (uint8_t)(0x5A + 0x06 + 0x03 + 0x0A));
	/* 250 Hz [5A 06 03 FA 00 00]. */
	assert(tfluna_command(out, TFLUNA_ID_SAMPLE_FREQ, hz250, 2) == 6);
	assert(!memcmp(out, "\x5A\x06\x03\xFA\x00", 5));
	/* Item 4, p.23: trigger [5A 04 04 00]. */
	assert(tfluna_command(out, TFLUNA_ID_SAMPLE_TRIG, NULL, 0) == 4);
	assert(!memcmp(out, "\x5A\x04\x04", 3) && out[3] == 0x62);
	/* Item 11, p.26: save [5A 04 11 00]. */
	assert(tfluna_command(out, TFLUNA_ID_SAVE, NULL, 0) == 4);
	assert(!memcmp(out, "\x5A\x04\x11", 3));
	/* Item 1, p.22: version [5A 04 01 00]. */
	assert(tfluna_command(out, TFLUNA_ID_GET_VERSION, NULL, 0) == 4);
	assert(!memcmp(out, "\x5A\x04\x01", 3));
	/* Item 7, p.24: disable output [5A 05 07 00 00]. */
	assert(tfluna_command(out, TFLUNA_ID_OUTPUT_EN, &off, 1) == 5);
	assert(!memcmp(out, "\x5A\x05\x07\x00", 4));
	/* And the one with a checksum the manual gives: item 8, p.25. */
	assert(tfluna_command(out, 0x08, &off, 1) == 5);
	assert(!memcmp(out, "\x5A\x05\x08\x00\x67", 5));
}

/* 6.4, p.10; Appendix I, p.20. */
static void test_status(void)
{
	assert(tfluna_status(0) == TOF_WEAK_SIGNAL);
	assert(tfluna_status(99) == TOF_WEAK_SIGNAL);
	assert(tfluna_status(100) == TOF_VALID);
	assert(tfluna_status(30000) == TOF_VALID);
	assert(tfluna_status(32768) == TOF_VALID);	/* "above 32768" */
	assert(tfluna_status(32769) == TOF_OVEREXPOSED);
	assert(tfluna_status(65534) == TOF_OVEREXPOSED);
	assert(tfluna_status(65535) == TOF_OVEREXPOSED);
}

static void test_temperature(void)
{
	/* UART: Temp / 8 - 256 (6.4, p.10). 0x0900 = 2304: 32 degC. */
	assert(tfluna_uart_temp_c(0x0900) == 32.0f);
	assert(tfluna_uart_temp_c(2048) == 0.0f);
	assert(tfluna_uart_temp_c(2044) == -0.5f);
	/* I2C: 0.01 degC (Appendix III, p.32). */
	assert(fabsf(tfluna_i2c_temp_c(3250) - 32.5f) < 1e-4f);
	assert(fabsf(tfluna_i2c_temp_c(0xFF9C) + 1.0f) < 1e-4f);
	assert(tfluna_le16((const uint8_t *)"\x34\x12") == 0x1234);
}

int main(void)
{
	test_checksum();
	test_frame_len();
	test_commands();
	test_status();
	test_temperature();
	printf("test_tfluna_proto: all passed\n");
	return 0;
}
