/*
* Copyright (c) 2017, STMicroelectronics - All Rights Reserved
*
* This file : part of VL53L1 Core and : dual licensed,
* either 'STMicroelectronics
* Proprietary license'
* or 'BSD 3-clause "New" or "Revised" License' , at your option.
*
********************************************************************************
*
* 'STMicroelectronics Proprietary license'
*
********************************************************************************
*
* License terms: STMicroelectronics Proprietary in accordance with licensing
* terms at www.st.com/sla0081
*
* STMicroelectronics confidential
* Reproduction and Communication of this document : strictly prohibited unless
* specifically authorized in writing by STMicroelectronics.
*
*
********************************************************************************
*
* Alternatively, VL53L1 Core may be distributed under the terms of
* 'BSD 3-clause "New" or "Revised" License', in which case the following
* provisions apply instead of the ones mentioned above :
*
********************************************************************************
*
* License terms: BSD 3-clause "New" or "Revised" License.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice, this
* list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
* this list of conditions and the following disclaimer in the documentation
* and/or other materials provided with the distribution.
*
* 3. Neither the name of the copyright holder nor the names of its contributors
* may be used to endorse or promote products derived from this software
* without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*
********************************************************************************
*
*/
/*
 * VL53L1X registers and ST's configuration data, for src/vl53l1x/vl53l1x.c.
 *
 * The datasheet (docs/vl53l1x.pdf, DocID031281 Rev 3) publishes only the
 * identity registers (4.2, Table 11) and says the rest of the map is not
 * exposed.
 * Everything else here is data taken from ST's VL53L1X ultra lite driver,
 * STSW-IMG009, version 3.4.0 -- the files VL53L1X_api.h and VL53L1X_api.c,
 * as mirrored at github.com/linkjumper/VL53L1X_ULD_API commit 7d71660a --
 * under the BSD 3-clause licence that ST's notice above grants, the notice
 * kept as its clause 1 asks (see also LICENSE-ST). Only data is copied: the
 * register addresses, the configuration block and the tables. No function
 * of ST's is.
 *
 * Every value cites its line, as "api.h:N" or "api.c:N", in those two
 * files. ULD 3.3.0 (github.com/shizacat/st-vl53l1x-uld-linux commit
 * e161c2c6) has every one of them on the same line with the same value.
 * esp-vl53l1x-tof, which this driver ports, had the same values with no
 * citation; they were checked one by one against 3.4.0 and all match.
 */
#ifndef VL53L1X_REGS_H
#define VL53L1X_REGS_H

#include <stdint.h>

/* 7-bit; the datasheet gives 0x52, the 8-bit write address (DS 4, p.19,
 * Figure 12). */
#define VL53L1X_ADDR_DEFAULT		0x29

/*
 * DS 4.2, Table 11: 0x010F reads 0xEA and 0x0110 reads 0xCC, so the 16-bit
 * read at 0x010F is 0xEACC. (api.h:243 comments "sensor Id must be 0xEEAC";
 * the datasheet is followed, as esp-vl53l1x-tof did.)
 */
#define VL53L1X_MODEL_ID		0xEACC

/* ------------------------------------------------------------ registers -- */

/*
 * A 16-bit index, sent high byte first, and auto-incremented across a
 * multi-byte access (DS 4, Figures 13-16, p.20-21); multi-byte values high
 * byte first too (DS 4.2, Table 12).
 */
#define VL53L1X_I2C_SLAVE_ADDR		0x0001	/* api.h:81 */
#define VL53L1X_VHV_LOOP_BOUND		0x0008	/* api.h:82 */
#define VL53L1X_VHV_START		0x000B	/* api.c:241, a bare 0x0B */
#define VL53L1X_GPIO_HV_MUX_CTRL	0x0030	/* api.h:89 */
#define VL53L1X_GPIO_TIO_HV_STATUS	0x0031	/* api.h:90 */
#define VL53L1X_PHASECAL_TIMEOUT	0x004B	/* api.h:92 */
#define VL53L1X_TIMEOUT_MACROP_A_HI	0x005E	/* api.h:93 */
#define VL53L1X_VCSEL_PERIOD_A		0x0060	/* api.h:94 */
#define VL53L1X_TIMEOUT_MACROP_B_HI	0x0061	/* api.h:96 */
#define VL53L1X_VCSEL_PERIOD_B		0x0063	/* api.h:95 */
#define VL53L1X_VALID_PHASE_HIGH	0x0069	/* api.h:100 */
#define VL53L1X_INTERMEASUREMENT	0x006C	/* api.h:101, 32 bits */
#define VL53L1X_SD_CONFIG_WOI_SD0	0x0078	/* api.h:104 */
#define VL53L1X_SD_CONFIG_PHASE_SD0	0x007A	/* api.h:105 */
#define VL53L1X_INTERRUPT_CLEAR		0x0086	/* api.h:110 */
#define VL53L1X_MODE_START		0x0087	/* api.h:111 */
#define VL53L1X_RANGE_STATUS		0x0089	/* api.h:112 */
#define VL53L1X_OSC_CALIBRATE_VAL	0x00DE	/* api.h:117 */
#define VL53L1X_SYSTEM_STATUS		0x00E5	/* api.h:118 */
#define VL53L1X_MODEL_ID_REG		0x010F	/* api.h:119; DS Table 11 */

/* -------------------------------------------------------------- values -- */

#define VL53L1X_START_RANGING		0x40	/* api.c:279 */
#define VL53L1X_STOP_RANGING		0x00	/* api.c:287 */
#define VL53L1X_CLEAR_INTERRUPT		0x01	/* api.c:249 */
#define VL53L1X_VHV_TWO_BOUNDS		0x09	/* api.c:240 */
#define VL53L1X_VHV_FROM_LAST		0x00	/* api.c:241 */

/*
 * GPIO_HV_MUX_CTRL bit 4 is the interrupt polarity, and a measurement is
 * waiting when GPIO_TIO_HV_STATUS bit 0 reads !(bit 4) (api.c:270-271,
 * 301).
 */
#define VL53L1X_MUX_POLARITY		0x10
#define VL53L1X_TIO_READY		0x01

/* The firmware's booted state, 1 booted and 0 not (api.h:237, api.c:526). */
#define VL53L1X_BOOTED			0x01

/* The oscillator calibration's significant bits (api.c:505). */
#define VL53L1X_OSC_CALIBRATE_MASK	0x03FF

/*
 * The inter-measurement period, in the chip's oscillator ticks, is
 * calibration * ms * 1.075, worked in double and cut to 32 bits (api.c:507).
 */
#define VL53L1X_PERIOD_FACTOR		1.075

/*
 * The result block, read in one go from RANGE_STATUS (api.c:632-640):
 * byte 0 the raw status, low five bits; 7-8 the ambient rate and 15-16 the
 * signal rate, both times 8 for kcps (api.c:637, 639; api.h:263, 273); 13-14
 * the distance in mm (api.c:640). High byte first.
 */
#define VL53L1X_RESULT_LEN		17
#define VL53L1X_RESULT_STATUS_MASK	0x1F	/* api.c:633 */
#define VL53L1X_RESULT_AMBIENT		7
#define VL53L1X_RESULT_DISTANCE		13
#define VL53L1X_RESULT_SIGNAL		15
#define VL53L1X_RATE_TO_KCPS		8

/*
 * ST's renumbering of the raw status, status_rtn
 * (api.c:201-204): raw 9 is 0, valid; 255 is no code of ST's. Raw values
 * past the table also read 255 (api.c:620-622).
 */
#define VL53L1X_STATUS_NONE		255
static const uint8_t vl53l1x_status_map[24] = {
	255, 255, 255, 5, 2, 4, 1, 7, 3, 0,
	255, 255, 9, 13, 255, 255, 255, 255, 10, 6,
	255, 255, 11, 12,
};

/*
 * ST's boot configuration, written to 0x2D-0x87 one byte at a time before
 * the chip will range: VL53L1X_DEFAULT_CONFIGURATION (api.c:107-199,
 * written by the loop at api.c:230-232). The notes are ST's, from the same
 * lines; ST marks every byte not noted "not user-modifiable".
 */
#define VL53L1X_CONFIG_FIRST		0x2D
static const uint8_t vl53l1x_default_config[] = {
	0x00,	/* 0x2d: bits 2 and 5 set for fast plus mode (1 MHz I2C) */
	0x00,	/* 0x2e: bit 0, I2C pulled up at 1.8 V (0) or AVDD (1) */
	0x00,	/* 0x2f: bit 0, GPIO pulled up at 1.8 V (0) or AVDD (1) */
	0x01,	/* 0x30: bit 4 interrupt polarity; bits 3:0 must be 0x1 */
	0x02,	/* 0x31: bit 1, interrupt depending on the polarity */
	0x00, 0x02, 0x08, 0x00, 0x08, 0x10, 0x01, 0x01,		/* 0x32 */
	0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x0f, 0x00,		/* 0x3a */
	0x00, 0x00, 0x00, 0x00,					/* 0x42 */
	0x20,	/* 0x46: interrupt on new sample ready */
	0x0b, 0x00, 0x00, 0x02, 0x0a, 0x21, 0x00, 0x00,		/* 0x47 */
	0x05, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00,		/* 0x4f */
	0x38, 0xff, 0x01, 0x00, 0x08, 0x00, 0x00, 0x01,		/* 0x57 */
	0xcc, 0x0f, 0x01, 0xf1, 0x0d,				/* 0x5f */
	0x01,	/* 0x64: sigma threshold MSB, mm in 14.2: 0x0168 = 90 mm */
	0x68,	/* 0x65: sigma threshold LSB */
	0x00,	/* 0x66: min count rate MSB, MCPS in 9.7 */
	0x80,	/* 0x67: min count rate LSB */
	0x08, 0xb8, 0x00, 0x00,					/* 0x68 */
	0x00,	/* 0x6c: inter-measurement period MSB, 32 bits */
	0x00, 0x0f, 0x89,					/* 0x6d */
	0x00, 0x00,						/* 0x70 */
	0x00,	/* 0x72: distance threshold high MSB, mm */
	0x00,
	0x00,	/* 0x74: distance threshold low MSB, mm */
	0x00,
	0x00, 0x01, 0x0f, 0x0d, 0x0e, 0x0e, 0x00, 0x00, 0x02,	/* 0x76 */
	0xc7,	/* 0x7f: ROI centre */
	0xff,	/* 0x80: ROI width and height */
	0x9b, 0x00, 0x00, 0x00, 0x01,				/* 0x81 */
	0x00,	/* 0x86: clear interrupt */
	0x00,	/* 0x87: start ranging; 0x40 would start at once */
};
#define VL53L1X_CONFIG_LAST		0x87

/* ---------------------------------------------------- distance modes -- */

enum { VL53L1X_MODE_SHORT = 1, VL53L1X_MODE_LONG = 2 };	/* api.c:461, 469 */

/*
 * What each mode writes (api.c:461-475): PHASECAL_TIMEOUT, VCSEL_PERIOD_A,
 * VCSEL_PERIOD_B and VALID_PHASE_HIGH as bytes, SD_CONFIG_WOI_SD0 and
 * SD_CONFIG_PHASE_SD0 as 16-bit words. PHASECAL_TIMEOUT also tells the
 * modes apart when read back (api.c:487-496).
 */
struct vl53l1x_mode_tune {
	uint8_t phasecal, vcsel_a, vcsel_b, valid_phase_high;
	uint16_t woi, phase;
};

static const struct vl53l1x_mode_tune vl53l1x_mode_tune[] = {
	[VL53L1X_MODE_SHORT] = { 0x14, 0x07, 0x05, 0x38, 0x0705, 0x0606 },
	[VL53L1X_MODE_LONG] = { 0x0A, 0x0F, 0x0D, 0xB8, 0x0F0D, 0x0E0E },
};

/* ---------------------------------------------------- timing budgets -- */

/*
 * TIMEOUT_MACROP_A_HI and _B_HI, as 16-bit words, per budget and mode
 * (api.c:309-409). There is no formula: a budget not in the table is
 * refused, and 15 ms exists only in short mode.
 */
struct vl53l1x_budget {
	uint16_t ms;
	uint16_t macrop_a;
	uint16_t macrop_b;
};

static const struct vl53l1x_budget vl53l1x_budget_short[] = {
	{  15, 0x001D, 0x0027 },	/* api.c:319-323 */
	{  20, 0x0051, 0x006E },	/* api.c:325-329 */
	{  33, 0x00D6, 0x006E },	/* api.c:331-335 */
	{  50, 0x01AE, 0x01E8 },	/* api.c:337-341 */
	{ 100, 0x02E1, 0x0388 },	/* api.c:343-347 */
	{ 200, 0x03E1, 0x0496 },	/* api.c:349-353 */
	{ 500, 0x0591, 0x05C1 },	/* api.c:355-359 */
};

static const struct vl53l1x_budget vl53l1x_budget_long[] = {
	{  20, 0x001E, 0x0022 },	/* api.c:367-371 */
	{  33, 0x0060, 0x006E },	/* api.c:373-377 */
	{  50, 0x00AD, 0x00C6 },	/* api.c:379-383 */
	{ 100, 0x01CC, 0x01EA },	/* api.c:385-389 */
	{ 200, 0x02D9, 0x02F8 },	/* api.c:391-395 */
	{ 500, 0x048F, 0x04A4 },	/* api.c:397-401 */
};

#endif /* VL53L1X_REGS_H */
