/*
 * A fake TF-Luna on its UART and on I2C, and this library's cases against
 * it, shared by esp/test_tfluna.c and stm/test_tfluna.c. Page numbers are
 * the user manual's printed ones (REV 01/04/2024).
 *
 * The UART side is a line: bytes already waiting (pending), which a flush
 * drops, and bytes still to come (future), which arrive once the waiting
 * ones are read. A command gets its reply (Appendix II, p.22-26) appended
 * to what is waiting, after any data frames a case puts "between"; a
 * trigger gets a data frame (item 4, p.23). The I2C side is the register
 * table of Appendix III (p.32-33).
 *
 * Each platform file feeds the UART from its own calls -- fake_flush(),
 * fake_send(), fake_take() -- and I2C from fake_reg_write/read(), and
 * provides:
 *
 *   RET(err)        what the handle API returns for a negative errno
 *   UART_BUS        the UART tfluna_init_uart() takes
 *   I2C_BUS         the bus handle tfluna_init_i2c() takes
 *   now_us()        the platform's clock
 */

#include "tfluna_proto.h"

extern char stub_log_last[256];

/* ------------------------------------------------------ the fake UART --- */

static uint8_t pending[2048], future[2048], between[256];
static size_t pending_len, future_len, between_len;
static bool answers;			/* the module replies to commands */
static uint8_t sent[64];		/* the last command */
static size_t sent_len;
static int sends;
static uint16_t rate;			/* what SAMPLE_FREQ set */
static int rate_reply_delta;		/* reply with a different rate */
static int status_reply;		/* SAVE's status byte */
static int output_on;
static uint8_t trig_frame[9];

static void put(uint8_t *q, size_t *len, const uint8_t *b, size_t n)
{
	assert(*len + n <= sizeof pending);
	memcpy(q + *len, b, n);
	*len += n;
}

/* A data frame, 9-byte/cm or 9-byte/mm (Appendix I, p.20). */
static void data_frame(uint8_t f[9], uint16_t dist, uint16_t amp,
		       uint16_t temp)
{
	f[0] = 0x59;
	f[1] = 0x59;
	f[2] = (uint8_t)dist;
	f[3] = (uint8_t)(dist >> 8);
	f[4] = (uint8_t)amp;
	f[5] = (uint8_t)(amp >> 8);
	f[6] = (uint8_t)temp;
	f[7] = (uint8_t)(temp >> 8);
	f[8] = tfluna_sum(f, 8);
}

static void future_frame(uint16_t dist, uint16_t amp, uint16_t temp)
{
	uint8_t f[9];

	data_frame(f, dist, amp, temp);
	put(future, &future_len, f, 9);
}

static void reply(const uint8_t *payload, uint8_t id, size_t n)
{
	uint8_t r[16];
	size_t len = 4 + n;

	r[0] = 0x5A;
	r[1] = (uint8_t)len;
	r[2] = id;
	memcpy(r + 3, payload, n);
	r[len - 1] = tfluna_sum(r, len - 1);
	put(pending, &pending_len, between, between_len);
	put(pending, &pending_len, r, len);
}

/* What a flush drops: everything waiting, or one byte of it. Inline, as
 * each platform uses only one of them. */
static inline void fake_flush(void)
{
	pending_len = 0;
}

static inline void fake_drop_byte(void)
{
	assert(pending_len);
	pending_len--;
	memmove(pending, pending + 1, pending_len);
}

static void fake_send(const uint8_t *b, size_t n)
{
	assert(n <= sizeof sent);
	memcpy(sent, b, n);
	sent_len = n;
	sends++;
	/* Commands start 0x5A, Len counts them whole (6.2, p.8-9). */
	assert(n >= 4 && b[0] == 0x5A && b[1] == n);
	assert(b[n - 1] == tfluna_sum(b, n - 1));
	if (!answers)
		return;
	switch (b[2]) {
	case 0x01: {
		/* Version 9.50.112 is sent as 112, 50, 9 (item 1, p.22). */
		static const uint8_t v[3] = { 112, 50, 9 };

		reply(v, 0x01, 3);
		break;
	}
	case 0x03: {
		uint8_t r[2];

		rate = (uint16_t)(b[3] | b[4] << 8);
		r[0] = (uint8_t)(rate + rate_reply_delta);
		r[1] = (uint8_t)((rate + rate_reply_delta) >> 8);
		reply(r, 0x03, 2);
		break;
	}
	case 0x04:
		put(pending, &pending_len, trig_frame, 9);
		break;
	case 0x07:
		output_on = b[3];
		reply(&b[3], 0x07, 1);
		break;
	case 0x11: {
		uint8_t st = (uint8_t)status_reply;

		reply(&st, 0x11, 1);
		break;
	}
	default:
		assert(!"a command the driver should not send");
	}
}

/* Up to n bytes, waiting ones first, then the ones still to come. */
static size_t fake_take(uint8_t *b, size_t n)
{
	size_t got = 0;

	while (got < n) {
		size_t k;

		if (!pending_len) {
			if (!future_len)
				break;
			memcpy(pending, future, future_len);
			pending_len = future_len;
			future_len = 0;
		}
		k = n - got < pending_len ? n - got : pending_len;
		memcpy(b + got, pending, k);
		memmove(pending, pending + k, pending_len - k);
		pending_len -= k;
		got += k;
	}
	return got;
}

/* ------------------------------------------------------- the fake I2C --- */

static uint8_t regs[0x40];
static bool i2c_present;
static uint8_t i2c_addr;
static int reg_written[0x40];		/* the last value written, or -1 */
static int i2c_xfers, i2c_fail_at;

static void fake_reg_write(uint8_t addr, const uint8_t *data, size_t len)
{
	assert(i2c_present && addr == i2c_addr && len >= 2);
	for (size_t i = 1; i < len; i++) {
		assert(data[0] + i - 1 < sizeof regs);
		regs[data[0] + i - 1] = data[i];
		reg_written[data[0] + i - 1] = data[i];
	}
}

static void fake_reg_read(uint8_t addr, uint8_t reg, uint8_t *rx, size_t n)
{
	assert(i2c_present && addr == i2c_addr && reg + n <= sizeof regs);
	memcpy(rx, regs + reg, n);
}

static bool i2c_fails(void)
{
	return i2c_xfers++ == i2c_fail_at;
}

/* ------------------------------------------------------------ helpers --- */

static int allocs;
static bool calloc_fails;
void *__real_calloc(size_t n, size_t size);
void __real_free(void *p);

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

static void fakes_reset(void)
{
	pending_len = future_len = between_len = 0;
	answers = true;
	sent_len = 0;
	sends = 0;
	rate = 100;
	rate_reply_delta = 0;
	status_reply = 0;
	output_on = 1;
	data_frame(trig_frame, 77, 500, 2048);

	memset(regs, 0, sizeof regs);
	memcpy(regs + 0x3C, "LUNA", 4);
	regs[0x0A] = 112;		/* revision, minor, major */
	regs[0x0B] = 50;
	regs[0x0C] = 9;
	regs[0x26] = 0x64;		/* FPS 100, the initial value */
	for (size_t i = 0; i < sizeof regs; i++)
		reg_written[i] = -1;
	i2c_present = true;
	i2c_addr = 0x10;
	i2c_xfers = 0;
	i2c_fail_at = -1;
	allocs = 0;
}

static tfluna_handle_t open_uart(const tfluna_config_t *cfg)
{
	tfluna_handle_t h = NULL;

	assert(tfluna_init_uart(UART_BUS, cfg, &h) == RET(0) && h);
	return h;
}

static tfluna_handle_t open_i2c(const tfluna_config_t *cfg)
{
	tfluna_handle_t h = NULL;

	assert(tfluna_init_i2c(I2C_BUS, cfg, &h) == RET(0) && h);
	return h;
}

/* --------------------------------------------------------- UART cases --- */

/*
 * Init asks for the version (5A 04 01, checksum 5F) and finds the reply
 * among the data frames streaming past.
 */
static void test_uart_init(void)
{
	tfluna_handle_t h;

	fakes_reset();
	data_frame(between, 100, 1000, 2048);
	data_frame(between + 9, 101, 1000, 2048);
	between_len = 18;
	h = open_uart(NULL);
	assert(sent_len == 4 && !memcmp(sent, "\x5A\x04\x01\x5F", 4));
	assert(strstr(stub_log_last, "UART, firmware 9.50.112"));
	assert(allocs == 1);
	assert(tfluna_del(h) == RET(0) && allocs == 0);
}

/*
 * Silence is -ENXIO; bytes but never a valid checksum, -EBADMSG -- the
 * wrong baud rate; valid frames but no answer, -ENODEV. Nothing allocated.
 */
static void test_uart_init_errors(void)
{
	tfluna_handle_t h = NULL;
	uint8_t junk[200];

	fakes_reset();
	answers = false;
	assert(tfluna_init_uart(UART_BUS, NULL, &h) == RET(-ENXIO));

	fakes_reset();
	answers = false;
	for (size_t i = 0; i < sizeof junk; i++)
		junk[i] = (uint8_t)(i % 0x40);	/* never 0x59 or 0x5A */
	put(future, &future_len, junk, sizeof junk);
	assert(tfluna_init_uart(UART_BUS, NULL, &h) == RET(-EBADMSG));
	assert(strstr(stub_log_last, "check the baud rate"));

	fakes_reset();
	answers = false;
	for (int i = 0; i < 20; i++)
		future_frame(100, 1000, 2048);
	assert(tfluna_init_uart(UART_BUS, NULL, &h) == RET(-ENODEV));
	assert(strstr(stub_log_last, "no answer to the version request"));

	fakes_reset();
	calloc_fails = true;
	assert(tfluna_init_uart(UART_BUS, NULL, &h) == RET(-ENOMEM));
	calloc_fails = false;
	assert(tfluna_init_uart(UART_BUS, NULL, NULL) == RET(-EINVAL));
	assert(allocs == 0 && !h);
}

/*
 * A read drops what was waiting and takes the next frame whose checksum
 * matches: past garbage, a stray header byte, a frame with a bad checksum
 * and a command reply.
 */
static void test_uart_read(void)
{
	static const uint8_t reply_frame[5] = { 0x5A, 0x05, 0x07, 0x01, 0x67 };
	tfluna_handle_t h;
	tfluna_result_t r;
	uint8_t f[9];

	fakes_reset();
	h = open_uart(NULL);

	/* Stale: dropped. */
	data_frame(f, 111, 1000, 2048);
	put(pending, &pending_len, f, 9);
	/* Garbage, a stray 0x59 right before a real header, a bad frame, a
	 * reply, then the one. */
	put(future, &future_len, (const uint8_t *)"\x00\x13\x59", 3);
	data_frame(f, 222, 1000, 2048);
	f[8] ^= 0xFF;
	put(future, &future_len, f, 9);
	put(future, &future_len, reply_frame, 5);
	future_frame(300, 1500, 0x0900);
	assert(tfluna_read(h, &r) == RET(0));
	/* 300 cm, Amp 1500, 0x0900 / 8 - 256 = 32 degC (6.4, p.10). */
	assert(r.distance_mm == 3000 && r.amp == 1500 && r.temp_c == 32.0f);
	assert(tfluna_valid(&r));

	/* "59 59 59 ...": the first 0x59 is stray, the frame starts at the
	 * second, and no byte of it is lost. */
	put(future, &future_len, (const uint8_t *)"\x59", 1);
	future_frame(400, 99, 2048);
	assert(tfluna_read(h, &r) == RET(0) && r.distance_mm == 4000);
	assert(!tfluna_valid(&r));		/* Amp under 100 */

	/* Nothing comes: the bus's timeout. */
	assert(tfluna_read(h, &r) == RET(-ETIMEDOUT));
	tfluna_del(h);

	/* 9-byte/mm, as configured (Appendix I item 3, p.20). */
	fakes_reset();
	h = open_uart(&(tfluna_config_t){ .millimetres = true });
	future_frame(1234, 1000, 2048);
	assert(tfluna_read(h, &r) == RET(0) && r.distance_mm == 1234);
	tfluna_del(h);
}

/*
 * Rate, trigger, save -- each command byte for byte, each reply checked.
 * After a trigger the frame it asked for is kept, not dropped as stale.
 */
static void test_uart_commands(void)
{
	tfluna_handle_t h;
	tfluna_result_t r;

	fakes_reset();
	h = open_uart(NULL);

	assert(tfluna_set_rate(h, 10) == RET(0) && rate == 10);
	assert(sent_len == 6 && !memcmp(sent, "\x5A\x06\x03\x0A\x00\x6D", 6));
	assert(tfluna_set_rate(h, 251) == RET(-EINVAL));
	rate_reply_delta = 1;
	assert(tfluna_set_rate(h, 250) == RET(-EIO));
	rate_reply_delta = 0;

	/* Trigger mode, then one measurement, read even though it came
	 * before the read did. */
	assert(tfluna_set_rate(h, 0) == RET(0) && rate == 0);
	assert(tfluna_trigger(h) == RET(0));
	assert(sent_len == 4 && !memcmp(sent, "\x5A\x04\x04\x62", 4));
	assert(tfluna_read(h, &r) == RET(0) && r.distance_mm == 770);
	/* The next read is fresh again: nothing new, a timeout. */
	put(pending, &pending_len, trig_frame, 9);
	assert(tfluna_read(h, &r) == RET(-ETIMEDOUT));

	assert(tfluna_save(h) == RET(0));
	assert(sent_len == 4 && !memcmp(sent, "\x5A\x04\x11\x6F", 4));
	status_reply = 1;
	assert(tfluna_save(h) == RET(-EIO));

	/* No reply at all. */
	answers = false;
	assert(tfluna_save(h) == RET(-ETIMEDOUT));
	tfluna_del(h);
}

/* The handle as a struct tof_dev, on the UART. */
static void test_uart_tof(void)
{
	tfluna_handle_t h;
	struct tof_dev *t;
	struct tof_sample s;
	bool ready;

	fakes_reset();
	h = open_uart(NULL);
	t = tfluna_tof(h);
	assert(t && !strcmp(t->name, "tfluna"));

	assert(tof_stop(t) == 0 && output_on == 0);
	assert(!memcmp(sent, "\x5A\x05\x07\x00\x66", 5));
	assert(tof_start(t) == 0 && output_on == 1);

	future_frame(250, 2000, 0x0900);
	assert(tof_read(t, &s) == 0);
	assert(s.distance_mm == 2500 && s.signal == 2000 && s.ambient == 0);
	assert(s.temp_c == 32.0f && s.valid && s.status == TOF_VALID);
	assert(s.raw_status == 0);

	future_frame(0, 65535, 2048);
	assert(tof_read(t, &s) == 0 && !s.valid);
	assert(s.status == TOF_OVEREXPOSED);
	future_frame(0, 32769, 2048);
	assert(tof_read(t, &s) == 0 && s.status == TOF_OVEREXPOSED);
	future_frame(0, 50, 2048);
	assert(tof_read(t, &s) == 0 && s.status == TOF_WEAK_SIGNAL);

	assert(tof_data_ready(t, &ready) == -EOPNOTSUPP);
	assert(tof_set_period_ms(t, 10) == -EOPNOTSUPP);
	assert(tof_read(t, &s) == -ETIMEDOUT);
	tfluna_del(h);
	assert(tfluna_tof(NULL) == NULL);
}

/* ---------------------------------------------------------- I2C cases --- */

static void test_i2c(void)
{
	tfluna_handle_t h = NULL;
	tfluna_result_t r;
	struct tof_sample s;

	fakes_reset();
	h = open_i2c(NULL);
	assert(strstr(stub_log_last, "I2C at 0x10, firmware 9.50.112"));

	/* DIST 300 cm, AMP 1500, TEMP 3250 = 32.50 degC (Appendix III). */
	regs[0x00] = 0x2C;
	regs[0x01] = 0x01;
	regs[0x02] = 0xDC;
	regs[0x03] = 0x05;
	regs[0x04] = 0xB2;
	regs[0x05] = 0x0C;
	assert(tfluna_read(h, &r) == RET(0));
	assert(r.distance_mm == 3000 && r.amp == 1500);
	assert(fabsf(r.temp_c - 32.5f) < 1e-4f);

	/* Trigger mode is MODE 1; a rate is MODE 0 and FPS, low byte first. */
	assert(tfluna_set_rate(h, 0) == RET(0) && reg_written[0x23] == 1);
	assert(tfluna_trigger(h) == RET(0) && reg_written[0x24] == 1);
	assert(tfluna_set_rate(h, 250) == RET(0));
	assert(reg_written[0x23] == 0 && reg_written[0x26] == 250 &&
	       reg_written[0x27] == 0);
	assert(tfluna_set_rate(h, 251) == RET(-EINVAL));
	assert(tfluna_save(h) == RET(0) && reg_written[0x20] == 1);

	assert(tof_stop(tfluna_tof(h)) == 0 && reg_written[0x25] == 0);
	assert(tof_start(tfluna_tof(h)) == 0 && reg_written[0x25] == 1);
	assert(tof_read(tfluna_tof(h), &s) == 0 && s.distance_mm == 3000);
	assert(s.valid && fabsf(s.temp_c - 32.5f) < 1e-4f);

	/* A failed transfer is the bus's error. */
	i2c_fail_at = i2c_xfers;
	assert(tfluna_read(h, &r) == RET(-ETIMEDOUT));
	assert(tfluna_del(h) == RET(0) && allocs == 0);
}

static void test_i2c_init(void)
{
	tfluna_handle_t h = NULL;

	/* Another address, as strapped by I2C_SLAVE_ADDR. */
	fakes_reset();
	i2c_addr = 0x22;
	h = open_i2c(&(tfluna_config_t){ .addr = 0x22 });
	tfluna_del(h);

	/* Out of the manual's range (6.3, p.9), or too fast: refused. */
	h = NULL;
	assert(tfluna_init_i2c(I2C_BUS, &(tfluna_config_t){ .addr = 0x07 },
			       &h) == RET(-EINVAL));
	assert(tfluna_init_i2c(I2C_BUS, &(tfluna_config_t){ .addr = 0x78 },
			       &h) == RET(-EINVAL));
	assert(tfluna_init_i2c(I2C_BUS,
			       &(tfluna_config_t){ .scl_speed_hz = 1000000 },
			       &h) == RET(-EINVAL));

	/* Nobody there: -ENXIO, nothing allocated. */
	fakes_reset();
	i2c_present = false;
	assert(tfluna_init_i2c(I2C_BUS, NULL, &h) == RET(-ENXIO));
	assert(allocs == 0);

	/* Somebody, but not signed LUNA: -ENODEV, freed. */
	fakes_reset();
	memcpy(regs + 0x3C, "LUN?", 4);
	assert(tfluna_init_i2c(I2C_BUS, NULL, &h) == RET(-ENODEV));
	assert(strstr(stub_log_last, "not \"LUNA\""));
	assert(allocs == 0 && !h);

	assert(tfluna_init_i2c(NULL, NULL, &h) == RET(-EINVAL));
	assert(tfluna_read(NULL, NULL) == RET(-EINVAL));
	assert(tfluna_del(NULL) == RET(-EINVAL));
}

static void run_tfluna_cases(void)
{
	test_uart_init();
	test_uart_init_errors();
	test_uart_read();
	test_uart_commands();
	test_uart_tof();
	test_i2c();
	test_i2c_init();
}
