/*
 * A fake VL53L1X at register level, and this library's cases against it,
 * shared by esp/test_vl53l1x.c and stm/test_vl53l1x.c. The fake models what
 * the driver relies on: the 16-bit register index, high byte first, and
 * auto-increment (datasheet DocID031281 Rev 3, section 4, Figures 13-16;
 * 4.2, Table 12); the identity at 0x010F (Table 11); and, from ST's ultra
 * lite driver 3.4.0 (cited as api.c/api.h:line), the booted state at 0x00E5
 * (api.c:526), ranging started and stopped at 0x0087 (api.c:279, 287), a
 * measurement waiting in GPIO_TIO_HV_STATUS bit 0 at the polarity
 * GPIO_HV_MUX_CTRL bit 4 sets (api.c:264-307), cleared through 0x0086
 * (api.c:249), the result block from 0x0089 (api.c:632-640), and the address
 * at 0x0001 (api.c:217-223). XSHUT low holds it off the bus; high starts its
 * boot.
 *
 * Each platform file feeds it from its own bus calls -- fails() first, to
 * fail the transfer a case asks for, then fake_write() or fake_read() --
 * and provides:
 *
 *   RET(err)        what the handle API returns for a negative errno
 *   I2C_BUS         the bus handle vl53l1x_init() takes
 *   now_us()        the platform's clock behind tof_time_us()
 *   cfg_xshut(cfg)  wire XSHUT into a config
 *   devices_live()  bus devices still attached (0 on the HAL)
 *
 * The expected values are written out here from ST's driver, not taken from
 * src/vl53l1x/vl53l1x_regs.h, so a wrong value there fails a case.
 */

#include "vl53l1x_regs.h"

extern char stub_log_last[256];

/* ------------------------------------------------------- the fake chip --- */

struct fake_vl {
	bool powered;			/* XSHUT high, or not wired */
	uint8_t addr;
	uint8_t reg[0x10000];
	int64_t boot_at_us;		/* booted from then on; -1 never */
	int64_t measure_us;		/* one measurement; 0 never ends */
	bool ranging;
	int64_t ready_at_us;		/* when the next one is waiting */
	bool waiting;			/* one is waiting, not cleared */
	int mux_override;		/* GPIO_HV_MUX_CTRL as read, or -1 */
	int clears, starts;
};

static struct fake_vl chip;
static int xfers, fail_at = -1;		/* fail the xfers'th transfer */
static int allocs;			/* calloc() minus free() */

/* Every write, in order: the index and the value of each byte. */
struct write_rec {
	uint16_t idx;
	uint8_t val;
};
static struct write_rec writes[512];
static int nwrites;

static void chip_power_on(int64_t boot_after_us)
{
	memset(chip.reg, 0, sizeof chip.reg);
	chip.addr = VL53L1X_ADDR_DEFAULT;
	chip.reg[0x010F] = 0xEA;	/* DS Table 11 */
	chip.reg[0x0110] = 0xCC;
	chip.reg[0x00DE] = 0xF5;	/* oscillator calibration: */
	chip.reg[0x00DF] = 0xF4;	/* 0x01F4 = 500 after the mask */
	chip.powered = true;
	chip.boot_at_us = now_us() + boot_after_us;
	chip.ranging = chip.waiting = false;
	chip.clears = chip.starts = 0;
}

static void fakes_reset(void)
{
	memset(&chip, 0, sizeof chip);
	chip.measure_us = 20000;
	chip.mux_override = -1;
	chip_power_on(0);
	xfers = 0;
	fail_at = -1;
	nwrites = 0;
}

/* Time passes only in the driver's waits; catch up the measurement. */
static void chip_tick(void)
{
	if (chip.ranging && !chip.waiting && chip.measure_us &&
	    now_us() >= chip.ready_at_us)
		chip.waiting = true;
}

static bool fails(void)
{
	return xfers++ == fail_at;
}

static bool chip_answers(uint8_t addr)
{
	return chip.powered && addr == chip.addr;
}

static uint8_t mux(void)
{
	return chip.mux_override >= 0 ? (uint8_t)chip.mux_override :
					chip.reg[0x0030];
}

static uint8_t read_byte(uint16_t idx)
{
	bool booted = chip.boot_at_us >= 0 && now_us() >= chip.boot_at_us;
	uint8_t active = !(mux() & 0x10);

	chip_tick();
	switch (idx) {
	case 0x00E5:
		return booted ? 0x01 : 0x00;
	case 0x0030:
		return mux();
	case 0x0031:
		/* bit 0 at the active level while one is waiting */
		return (uint8_t)((chip.reg[0x0031] & ~1) |
				 (chip.waiting ? active : !active));
	default:
		return chip.reg[idx];
	}
}

static void fake_read(uint8_t addr, uint16_t idx, uint8_t *rx, size_t n)
{
	assert(chip_answers(addr));
	for (size_t i = 0; i < n; i++)
		rx[i] = read_byte((uint16_t)(idx + i));
}

static void write_byte(uint16_t idx, uint8_t v)
{
	assert(nwrites < (int)(sizeof writes / sizeof *writes));
	writes[nwrites++] = (struct write_rec){ idx, v };
	chip.reg[idx] = v;
	chip_tick();
	if (idx == 0x0087) {
		chip.ranging = v == 0x40;
		if (chip.ranging) {
			chip.starts++;
			chip.ready_at_us = now_us() + chip.measure_us;
		}
	} else if (idx == 0x0086 && v == 0x01) {
		chip.clears++;
		chip.waiting = false;
		chip.ready_at_us = now_us() + chip.measure_us;
	}
}

/* data: the index, high byte first, then the bytes from there on. */
static void fake_write(uint8_t addr, const uint8_t *data, size_t len)
{
	uint16_t idx;
	bool readdress = false;
	uint8_t next = 0;

	assert(chip_answers(addr) && len >= 3);
	idx = (uint16_t)(data[0] << 8 | data[1]);
	for (size_t i = 2; i < len; i++, idx++) {
		write_byte(idx, data[i]);
		if (idx == 0x0001) {
			readdress = true;
			next = data[i] & 0x7F;
		}
	}
	/* The new address answers from the next transfer on. */
	if (readdress)
		chip.addr = next;
}

/* What was written to idx most recently; -1 if never. */
static int last_write(uint16_t idx)
{
	for (int i = nwrites - 1; i >= 0; i--)
		if (writes[i].idx == idx)
			return writes[i].val;
	return -1;
}

static int last_write16(uint16_t idx)
{
	int hi = last_write(idx), lo = last_write((uint16_t)(idx + 1));

	return hi < 0 || lo < 0 ? -1 : hi << 8 | lo;
}

/* The index of the first write to idx with value v at or after from. */
static int find_write(int from, uint16_t idx, uint8_t v)
{
	for (int i = from; i < nwrites; i++)
		if (writes[i].idx == idx && writes[i].val == v)
			return i;
	return -1;
}

/* A measurement in the result block (api.c:632-640). */
static void chip_result(uint8_t raw_status, uint16_t mm, uint16_t signal,
			uint16_t ambient)
{
	chip.reg[0x0089] = raw_status;
	chip.reg[0x0096] = (uint8_t)(mm >> 8);
	chip.reg[0x0097] = (uint8_t)mm;
	chip.reg[0x0098] = (uint8_t)(signal >> 8);
	chip.reg[0x0099] = (uint8_t)signal;
	chip.reg[0x0090] = (uint8_t)(ambient >> 8);
	chip.reg[0x0091] = (uint8_t)ambient;
}

/* calloc() and free() as the driver sees them: counted. */
void *__real_calloc(size_t n, size_t size);
void __real_free(void *p);
static bool calloc_fails;

void *__wrap_calloc(size_t n, size_t size)
{
	void *p;

	if (calloc_fails)
		return NULL;
	p = __real_calloc(n, size);
	if (p)
		allocs++;
	return p;
}

void __wrap_free(void *p)
{
	if (p)
		allocs--;
	__real_free(p);
}

static vl53l1x_handle_t open_dev(vl53l1x_config_t *cfg)
{
	vl53l1x_handle_t h = NULL;

	assert(vl53l1x_init(I2C_BUS, cfg, &h) == RET(0) && h);
	return h;
}

/* ------------------------------------------------------------- cases --- */

/*
 * Init, step by step: the identity, ST's 91-byte configuration written a
 * byte at a time from 0x2D, long mode, a 50 ms budget, a 50 ms period, and
 * the throwaway measurement that settles the VHV.
 */
static void test_init_sequence(void)
{
	vl53l1x_handle_t h;
	int first, i, start, clear, stop, vhv;

	fakes_reset();
	allocs = 0;
	h = open_dev(NULL);
	assert(allocs == 1);

	/* The configuration: one byte per write, 0x2D to 0x87 in order, the
	 * values of api.c:107-199. A few spelled out: */
	first = find_write(0, 0x002D, 0x00);
	assert(first >= 0 && first + 91 <= nwrites);
	for (i = 0; i < 91; i++) {
		assert(writes[first + i].idx == 0x2D + i);
		assert(writes[first + i].val == vl53l1x_default_config[i]);
	}
	assert(writes[first + 0x30 - 0x2D].val == 0x01);	/* api.c:111 */
	assert(writes[first + 0x46 - 0x2D].val == 0x20);	/* api.c:133 */
	assert(writes[first + 0x5E - 0x2D].val == 0x01);	/* api.c:157 */
	assert(writes[first + 0x5F - 0x2D].val == 0xCC);	/* api.c:158 */
	assert(writes[first + 0x64 - 0x2D].val == 0x01);	/* api.c:163 */
	assert(writes[first + 0x65 - 0x2D].val == 0x68);	/* api.c:164 */
	assert(writes[first + 0x6E - 0x2D].val == 0x0F);	/* api.c:173 */
	assert(writes[first + 0x6F - 0x2D].val == 0x89);	/* api.c:174 */
	assert(writes[first + 0x7F - 0x2D].val == 0xC7);	/* api.c:190 */
	assert(writes[first + 0x87 - 0x2D].val == 0x00);	/* api.c:198 */
	assert(sizeof vl53l1x_default_config == 91);

	/* Long mode (api.c:469-475). */
	assert(last_write(0x004B) == 0x0A && last_write(0x0060) == 0x0F);
	assert(last_write(0x0063) == 0x0D && last_write(0x0069) == 0xB8);
	assert(last_write16(0x0078) == 0x0F0D);
	assert(last_write16(0x007A) == 0x0E0E);
	/* 50 ms in long mode (api.c:379-383). */
	assert(last_write16(0x005E) == 0x00AD);
	assert(last_write16(0x0061) == 0x00C6);
	/* The period: 500 (the calibration, masked) * 50 ms * 1.075 = 26875
	 * (api.c:505-507), as 32 bits, high byte first. */
	assert(last_write16(0x006C) == 0x0000 && last_write16(0x006E) == 26875);

	/* The VHV settling after them: start, a measurement, clear, stop,
	 * then 0x09 to 0x0008 and 0x00 to 0x000B (api.c:233-241). */
	start = find_write(first + 91, 0x0087, 0x40);
	clear = find_write(start, 0x0086, 0x01);
	stop = find_write(clear, 0x0087, 0x00);
	vhv = find_write(stop, 0x0008, 0x09);
	assert(start > 0 && clear > start && stop > clear && vhv > stop);
	assert(find_write(vhv, 0x000B, 0x00) == vhv + 1);
	assert(vhv + 2 == nwrites);
	assert(!chip.ranging && chip.starts == 1);

	assert(vl53l1x_del(h) == RET(0) && allocs == 0);
	assert(devices_live() == 0);
}

/* Short mode and every budget, from ST's tables, and what is refused. */
static void test_modes_and_budgets(void)
{
	static const struct {
		uint16_t ms, a, b;
	} shorts[] = {
		{ 15, 0x001D, 0x0027 }, { 20, 0x0051, 0x006E },
		{ 33, 0x00D6, 0x006E }, { 50, 0x01AE, 0x01E8 },
		{ 100, 0x02E1, 0x0388 }, { 200, 0x03E1, 0x0496 },
		{ 500, 0x0591, 0x05C1 },	/* api.c:319-359 */
	}, longs[] = {
		{ 20, 0x001E, 0x0022 }, { 33, 0x0060, 0x006E },
		{ 50, 0x00AD, 0x00C6 }, { 100, 0x01CC, 0x01EA },
		{ 200, 0x02D9, 0x02F8 }, { 500, 0x048F, 0x04A4 },
		/* api.c:367-401 */
	};
	vl53l1x_config_t cfg = { 0 };
	vl53l1x_handle_t h = NULL;

	for (size_t i = 0; i < sizeof shorts / sizeof *shorts; i++) {
		fakes_reset();
		cfg = (vl53l1x_config_t){ .mode = VL53L1X_SHORT,
					  .budget_ms = shorts[i].ms };
		cfg_xshut(&cfg);
		h = open_dev(&cfg);
		/* Short mode (api.c:461-467). */
		assert(last_write(0x004B) == 0x14 &&
		       last_write(0x0060) == 0x07);
		assert(last_write(0x0063) == 0x05 &&
		       last_write(0x0069) == 0x38);
		assert(last_write16(0x0078) == 0x0705);
		assert(last_write16(0x007A) == 0x0606);
		assert(last_write16(0x005E) == shorts[i].a);
		assert(last_write16(0x0061) == shorts[i].b);
		vl53l1x_del(h);
	}
	for (size_t i = 0; i < sizeof longs / sizeof *longs; i++) {
		fakes_reset();
		cfg = (vl53l1x_config_t){ .mode = VL53L1X_LONG,
					  .budget_ms = longs[i].ms };
		cfg_xshut(&cfg);
		h = open_dev(&cfg);
		assert(last_write16(0x005E) == longs[i].a);
		assert(last_write16(0x0061) == longs[i].b);
		vl53l1x_del(h);
	}

	/* 15 ms is short mode's only; 42 is in no table; a period shorter
	 * than the budget; a mode that does not exist. All refused, and
	 * nothing left allocated. */
	allocs = 0;
	fakes_reset();
	cfg = (vl53l1x_config_t){ .mode = VL53L1X_LONG, .budget_ms = 15 };
	cfg_xshut(&cfg);
	assert(vl53l1x_init(I2C_BUS, &cfg, &h) == RET(-EINVAL));
	cfg.budget_ms = 42;
	assert(vl53l1x_init(I2C_BUS, &cfg, &h) == RET(-EINVAL));
	cfg.budget_ms = 100;
	cfg.period_ms = 99;
	assert(vl53l1x_init(I2C_BUS, &cfg, &h) == RET(-EINVAL));
	cfg.period_ms = 0;
	cfg.mode = (vl53l1x_mode_t)3;
	assert(vl53l1x_init(I2C_BUS, &cfg, &h) == RET(-EINVAL));
	assert(allocs == 0 && devices_live() == 0);
}

/*
 * The waits are on the clock (the bug this port fixed: esp-vl53l1x-tof's
 * waits were counts of 2 ms polls with no wait between them at a 100 Hz
 * tick). A chip that boots 5 ms late is waited for; one that never boots is
 * given up on only after 20 ms have really passed, as -ENODEV.
 */
static void test_boot_wait(void)
{
	vl53l1x_handle_t h = NULL;
	int64_t t0;

	fakes_reset();
	chip_power_on(5000);
	h = open_dev(NULL);
	vl53l1x_del(h);

	fakes_reset();
	chip.boot_at_us = -1;
	allocs = 0;
	t0 = now_us();
	assert(vl53l1x_init(I2C_BUS, NULL, &h) == RET(-ENODEV));
	assert(now_us() - t0 >= 20000);
	assert(allocs == 0 && devices_live() == 0);
}

/*
 * Nothing at 0x29 is -ENXIO, decided before anything is allocated; a bus
 * error after that is returned as itself, not as "not found"; a wrong model
 * ID is -ENODEV. Nothing leaks.
 */
static void test_init_errors(void)
{
	vl53l1x_handle_t h = NULL;

	fakes_reset();
	allocs = 0;
	chip.powered = false;
	assert(vl53l1x_init(I2C_BUS, NULL, &h) == RET(-ENXIO));
	assert(allocs == 0 && xfers == 0);

	/* The first read of the boot state fails: a timeout, as such. */
	fakes_reset();
	fail_at = 0;
	assert(vl53l1x_init(I2C_BUS, NULL, &h) == RET(-ETIMEDOUT));
	assert(allocs == 0 && devices_live() == 0);
	/* So does one deep in the configuration. */
	fakes_reset();
	fail_at = 50;
	assert(vl53l1x_init(I2C_BUS, NULL, &h) == RET(-ETIMEDOUT));
	assert(allocs == 0);

	/* api.h:243 comments 0xEEAC; the datasheet says 0xEACC. */
	fakes_reset();
	chip.reg[0x010F] = 0xEE;
	chip.reg[0x0110] = 0xAC;
	assert(vl53l1x_init(I2C_BUS, NULL, &h) == RET(-ENODEV));
	assert(strstr(stub_log_last, "model ID 0xEEAC, not 0xEACC"));
	assert(allocs == 0 && devices_live() == 0);

	fakes_reset();
	calloc_fails = true;
	assert(vl53l1x_init(I2C_BUS, NULL, &h) == RET(-ENOMEM));
	calloc_fails = false;
	assert(devices_live() == 0);

	assert(vl53l1x_init(NULL, NULL, &h) == RET(-EINVAL));
	assert(vl53l1x_init(I2C_BUS, NULL, NULL) == RET(-EINVAL));
	assert(!h);
}

/*
 * A measurement: waiting per GPIO_TIO_HV_STATUS at the configured
 * polarity, read in one block, renumbered by ST's table, the rates x8, and
 * the interrupt cleared after.
 */
static void test_read(void)
{
	vl53l1x_handle_t h;
	vl53l1x_result_t r = { 0 };
	bool ready = true;

	fakes_reset();
	h = open_dev(NULL);
	assert(vl53l1x_start(h) == RET(0) && chip.ranging);
	assert(vl53l1x_data_ready(h, &ready) == RET(0) && !ready);

	chip.waiting = true;
	chip_result(0x09, 1234, 0x0100, 0x0020);
	assert(vl53l1x_data_ready(h, &ready) == RET(0) && ready);
	assert(vl53l1x_read(h, &r) == RET(0));
	assert(r.distance_mm == 1234 && r.status == VL53L1X_VALID);
	assert(r.signal_kcps == 2048 && r.ambient_kcps == 256);
	assert(!chip.waiting && last_write(0x0086) == 0x01);

	/* Only the low five bits are the status (api.c:620, 633). */
	chip_result(0xE9, 10, 0, 0);
	assert(vl53l1x_read(h, &r) == RET(0) && r.status == 0);

	/* ST's renumbering, every entry (api.c:201-204). */
	{
		static const uint8_t want[24] = {
			255, 255, 255, 5, 2, 4, 1, 7, 3, 0, 255, 255,
			9, 13, 255, 255, 255, 255, 10, 6, 255, 255, 11, 12,
		};

		for (uint8_t raw = 0; raw < 32; raw++) {
			chip_result(raw, 10, 0, 0);
			assert(vl53l1x_read(h, &r) == RET(0));
			assert(r.status == (raw < 24 ? want[raw] : 255));
		}
	}

	/* x8 into 16 bits wraps above 8191, as ST's driver does. */
	chip_result(0x09, 10, 0x2001, 0xFFFF);
	assert(vl53l1x_read(h, &r) == RET(0));
	assert(r.signal_kcps == 8 && r.ambient_kcps == 0xFFF8);

	assert(vl53l1x_stop(h) == RET(0) && !chip.ranging);
	assert(!strcmp(vl53l1x_status_str(0), "valid"));
	assert(!strcmp(vl53l1x_status_str(7),
		       "wraparound - target beyond range"));
	assert(!strcmp(vl53l1x_status_str(255), "no measurement"));
	assert(!strcmp(vl53l1x_status_str(13),
		       "not valid (a code ST does not name)"));
	vl53l1x_del(h);
}

/* Polarity active low: GPIO_HV_MUX_CTRL bit 4 set; waiting reads 0. */
static void test_polarity(void)
{
	vl53l1x_handle_t h;
	bool ready;

	fakes_reset();
	chip.mux_override = 0x11;
	h = open_dev(NULL);
	assert(vl53l1x_start(h) == RET(0));
	chip.reg[0x0031] = 0x00;
	assert(vl53l1x_data_ready(h, &ready) == RET(0) && !ready);
	chip.waiting = true;
	assert(vl53l1x_data_ready(h, &ready) == RET(0) && ready);
	assert((read_byte(0x0031) & 1) == 0);
	vl53l1x_del(h);
}

/* read_blocking: a deadline in ms on the clock, not a count of polls. */
static void test_read_blocking(void)
{
	vl53l1x_handle_t h;
	vl53l1x_result_t r;
	int64_t t0;

	fakes_reset();
	chip.measure_us = 30000;
	h = open_dev(NULL);
	assert(vl53l1x_start(h) == RET(0));
	chip_result(0x09, 500, 0, 0);
	t0 = now_us();
	assert(vl53l1x_read_blocking(h, &r, 50) == RET(0));
	assert(r.distance_mm == 500 && now_us() - t0 >= 30000);

	/* The next is 30 ms away again; 20 ms is not enough, and the miss
	 * comes only once 20 ms have passed. */
	t0 = now_us();
	assert(vl53l1x_read_blocking(h, &r, 20) == RET(-ETIMEDOUT));
	assert(now_us() - t0 >= 20000);
	vl53l1x_del(h);
}

/* The new address takes from the next transfer on; refusals touch
 * nothing. */
static void test_set_address(void)
{
	vl53l1x_handle_t h;
	bool ready;
	int before;

	fakes_reset();
	h = open_dev(NULL);
	before = nwrites;
	assert(vl53l1x_set_address(h, 0x29) == RET(0) && nwrites == before);
	assert(vl53l1x_set_address(h, 0x80) == RET(-EINVAL));
	assert(vl53l1x_set_address(h, 0x30) == RET(0));
	assert(chip.addr == 0x30 && last_write(0x0001) == 0x30);
	assert(vl53l1x_data_ready(h, &ready) == RET(0));
	assert(devices_live() <= 1);

	/* A failed write leaves both where they were. */
	fail_at = xfers;
	assert(vl53l1x_set_address(h, 0x31) == RET(-ETIMEDOUT));
	assert(chip.addr == 0x30 && devices_live() <= 1);
	assert(vl53l1x_data_ready(h, &ready) == RET(0));
	assert(vl53l1x_del(h) == RET(0) && devices_live() == 0);

	assert(vl53l1x_set_address(NULL, 0x30) == RET(-EINVAL));
	assert(vl53l1x_del(NULL) == RET(-EINVAL));
}

/* The handle as a struct tof_dev. */
static void test_tof_view(void)
{
	vl53l1x_handle_t h;
	struct tof_dev *t;
	struct tof_sample s;

	assert(vl53l1x_tof(NULL) == NULL);
	fakes_reset();
	h = open_dev(NULL);
	t = vl53l1x_tof(h);
	assert(t && !strcmp(t->name, "vl53l1x"));

	assert(tof_start(t) == 0 && chip.ranging);
	assert(tof_read(t, &s) == -EAGAIN);
	chip.waiting = true;
	chip_result(0x09, 812, 0x2001, 0x0010);
	assert(tof_read(t, &s) == 0);
	assert(s.distance_mm == 812 && s.valid && s.status == TOF_VALID);
	assert(s.raw_status == 0 && isnan(s.temp_c));
	/* Saturated here, where the handle API wraps. */
	assert(s.signal == 65535 && s.ambient == 128);
	assert(!chip.waiting);

	/* ST's codes in the category's scale. */
	{
		static const struct {
			uint8_t raw;
			enum tof_status want;
		} map[] = {
			{ 9, TOF_VALID }, { 6, TOF_NOISY },
			{ 4, TOF_WEAK_SIGNAL }, { 5, TOF_OUT_OF_RANGE },
			{ 3, TOF_HW_FAIL }, { 7, TOF_WRAPAROUND },
			{ 8, TOF_OTHER }, { 19, TOF_OTHER }, { 0, TOF_OTHER },
		};

		for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
			chip.waiting = true;
			chip_result(map[i].raw, 1, 0, 0);
			assert(tof_read(t, &s) == 0);
			assert(s.status == map[i].want);
			assert(s.valid == (map[i].want == TOF_VALID));
		}
	}

	/* Budgets from the table for the mode, the period at least that. */
	assert(tof_set_budget_ms(t, 33) == 0);
	assert(last_write16(0x005E) == 0x0060);
	assert(tof_set_budget_ms(t, 15) == -EINVAL);
	assert(tof_set_budget_ms(t, 100) == -EINVAL);	/* period is 50 */
	assert(tof_set_period_ms(t, 32) == -EINVAL);
	assert(tof_set_period_ms(t, 100) == 0);
	assert(last_write16(0x006E) == (int)(500 * 100 * 1.075));
	assert(tof_set_budget_ms(t, 100) == 0);
	assert(last_write16(0x005E) == 0x01CC);
	assert(tof_set_period_ms(t, 70000) == -EINVAL);

	/* read_blocking through the core. */
	chip.waiting = false;
	chip.measure_us = 30000;
	chip.ready_at_us = now_us() + 30000;
	assert(tof_read_blocking(t, &s, 100) == 0);

	assert(tof_stop(t) == 0 && !chip.ranging);
	vl53l1x_del(h);
}

static void run_vl53l1x_cases(void)
{
	test_init_sequence();
	test_modes_and_budgets();
	test_boot_wait();
	test_init_errors();
	test_read();
	test_polarity();
	test_read_blocking();
	test_set_address();
	test_tof_view();
}
