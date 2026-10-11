/*
 * The core's cases -- start, stop, data-ready, read and its timestamp,
 * read_blocking's deadline, the optional ops, the data-ready hook and the
 * log -- shared by esp/test_core.c and stm/test_core.c. Each includes this
 * after providing:
 *
 *   LOG_PREFIX             what the platform's log puts before "tof: "
 *   stub_log_last          the last log line
 *   now_us()               the platform's clock behind tof_time_us()
 *   set_now_us(us)         set it
 *
 * Time moves only when the core waits -- tof_yield() -- by a tick on
 * ESP-IDF's stub and by HAL_Delay(1) on the HAL's. What the chips' ops do
 * lives in their drivers, so test_vl53l1x.c and test_tfluna.c check that.
 */

static int fake_calls;
static int fake_ret;			/* fail the next op with this */
static int fake_not_ready;		/* reads that say -EAGAIN first */
static int64_t fake_ready_at_us = -1;	/* or ready from this time on */
static uint32_t fake_ms;
static int64_t fake_read_at;		/* the clock when read ran */

static int fake_start(struct tof_dev *t)
{
	(void)t;
	fake_calls++;
	return fake_ret;
}

static int fake_data_ready(struct tof_dev *t, bool *ready)
{
	(void)t;
	fake_calls++;
	*ready = true;
	return fake_ret;
}

static int fake_read(struct tof_dev *t, struct tof_sample *s)
{
	(void)t;
	fake_calls++;
	fake_read_at = now_us();
	if (fake_ret) {
		s->distance_mm = 999;	/* scribbled; the caller must not see */
		return fake_ret;
	}
	if (fake_not_ready > 0) {
		fake_not_ready--;
		return -EAGAIN;
	}
	if (fake_ready_at_us >= 0 && now_us() < fake_ready_at_us)
		return -EAGAIN;
	s->distance_mm = 1234;
	s->signal = 50;
	s->ambient = 7;
	s->temp_c = NAN;
	s->raw_status = 9;
	s->status = TOF_VALID;
	s->t_us = -1;		/* the core stamps it, whatever the op says */
	s->valid = false;	/* and decides this */
	return 0;
}

static int fake_set_ms(struct tof_dev *t, uint32_t ms)
{
	(void)t;
	fake_calls++;
	fake_ms = ms;
	return fake_ret;
}

static const struct tof_ops fake_ops = {
	.start = fake_start,
	.stop = fake_start,
	.data_ready = fake_data_ready,
	.read = fake_read,
	.set_period_ms = fake_set_ms,
	.set_budget_ms = fake_set_ms,
};

static const struct tof_ops fake_ops_bare = {
	.start = fake_start,
	.stop = fake_start,
	.read = fake_read,
};

static struct tof_dev fake_dev(void)
{
	struct tof_dev t = {
		.name = "fake",
		.ops = &fake_ops,
	};

	fake_calls = 0;
	fake_ret = 0;
	fake_not_ready = 0;
	fake_ready_at_us = -1;
	set_now_us(1000000);
	return t;
}

static void test_ops(void)
{
	struct tof_dev t = fake_dev();
	bool ready = false;

	assert(tof_start(&t) == 0 && tof_stop(&t) == 0 && fake_calls == 2);
	assert(tof_data_ready(&t, &ready) == 0 && ready);
	assert(tof_set_period_ms(&t, 70) == 0 && fake_ms == 70);
	assert(tof_set_budget_ms(&t, 33) == 0 && fake_ms == 33);

	/* A failure is the op's; ready is left alone. */
	fake_ret = -EIO;
	ready = false;
	assert(tof_data_ready(&t, &ready) == -EIO && !ready);
	assert(tof_start(&t) == -EIO);

	/* Optional ops missing: said so, nothing called. */
	t.ops = &fake_ops_bare;
	fake_ret = 0;
	fake_calls = 0;
	assert(tof_data_ready(&t, &ready) == -EOPNOTSUPP);
	assert(tof_set_period_ms(&t, 1) == -EOPNOTSUPP);
	assert(tof_set_budget_ms(&t, 1) == -EOPNOTSUPP);
	assert(fake_calls == 0);
}

static void test_read(void)
{
	struct tof_dev t = fake_dev();
	struct tof_sample s = { 0 };

	/* Stamped by the core before the op, valid from the status. */
	set_now_us(5000000);
	assert(tof_read(&t, &s) == 0);
	assert(s.distance_mm == 1234 && s.signal == 50 && s.ambient == 7);
	assert(s.t_us == 5000000 && s.valid && s.raw_status == 9);
	assert(isnan(s.temp_c));

	/* Nothing waiting: -EAGAIN, and *s untouched. */
	s.distance_mm = 1;
	fake_not_ready = 1;
	assert(tof_read(&t, &s) == -EAGAIN && s.distance_mm == 1);
	fake_ret = -ETIMEDOUT;
	assert(tof_read(&t, &s) == -ETIMEDOUT && s.distance_mm == 1);
}

/*
 * The deadline is on the clock: a measurement that comes 30 ms late is
 * caught by a 50 ms wait and missed by a 20 ms one, and the miss is reported
 * only once 20 ms have really passed -- however many polls that took.
 */
static void test_read_blocking(void)
{
	struct tof_dev t = fake_dev();
	struct tof_sample s = { 0 };
	int64_t start;

	fake_ready_at_us = now_us() + 30000;
	assert(tof_read_blocking(&t, &s, 50) == 0 && s.distance_mm == 1234);
	assert(fake_read_at >= fake_ready_at_us);
	assert(s.t_us >= fake_ready_at_us);

	start = now_us();
	fake_ready_at_us = start + 30000;
	s.distance_mm = 1;
	assert(tof_read_blocking(&t, &s, 20) == -ETIMEDOUT);
	assert(now_us() - start >= 20000 && s.distance_mm == 1);

	/* Ready at once: no wait at all; a 0 ms wait still looks once. */
	start = now_us();
	fake_ready_at_us = -1;
	assert(tof_read_blocking(&t, &s, 0) == 0 && now_us() == start);
	fake_not_ready = 1;
	assert(tof_read_blocking(&t, &s, 0) == -ETIMEDOUT);

	/* Any error but -EAGAIN ends the wait. */
	fake_ret = -EIO;
	start = now_us();
	assert(tof_read_blocking(&t, &s, 1000) == -EIO && now_us() == start);
}

/* The data-ready hook: counted from the ISR side, seen once per batch,
 * never lost between a look and a clear. */
static void test_irq_hook(void)
{
	struct tof_dev t = fake_dev();

	assert(!tof_irq_pending(&t));
	tof_irq(&t);
	tof_irq(&t);
	assert(tof_irq_pending(&t));
	assert(!tof_irq_pending(&t));
	tof_irq(&t);
	assert(tof_irq_pending(&t));

	/* The counter wraps and still reads as pending. */
	t.irq_count = UINT32_MAX;
	t.irq_seen = UINT32_MAX;
	tof_irq(&t);
	assert(t.irq_count == 0 && tof_irq_pending(&t));

	tof_irq(NULL);
	assert(!tof_irq_pending(NULL));
}

static void test_unbound(void)
{
	struct tof_dev t = fake_dev();
	struct tof_ops partial = fake_ops;
	struct tof_sample s;
	bool b;

	/* Nothing to call is the caller's mistake, and nothing is called. */
	assert(tof_start(NULL) == -EINVAL);
	assert(tof_read(&t, NULL) == -EINVAL);
	assert(tof_data_ready(&t, NULL) == -EINVAL);
	assert(tof_read_blocking(NULL, &s, 10) == -EINVAL);
	t.ops = NULL;
	assert(tof_stop(&t) == -EINVAL);
	assert(tof_read(&t, &s) == -EINVAL);
	/* An ops table without start, stop or read is refused. */
	partial.read = NULL;
	t.ops = &partial;
	assert(tof_data_ready(&t, &b) == -EINVAL);
	assert(tof_set_period_ms(&t, 5) == -EINVAL);
	assert(fake_calls == 0);
}

static void test_log(void)
{
	struct tof_dev t = fake_dev();
	struct tof_sample s = {
		.distance_mm = 1234, .signal = 50, .ambient = 7,
		.temp_c = NAN, .raw_status = 0, .valid = true,
		.t_us = 1500000,
	};

	tof_err(&t, "no answer (%d)", -6);
	assert(!strcmp(stub_log_last,
		       LOG_PREFIX("E") "tof: fake: no answer (-6)"));

	tof_log_sample(&t, &s);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "tof: fake: t 1500000 us   1234 mm  valid (0)  "
		       "signal 50  ambient 7"));
	s.temp_c = 32.5f;
	s.valid = false;
	s.raw_status = 4;
	s.t_us = -20;
	tof_log_sample(&t, &s);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "tof: fake: t -20 us   1234 mm  invalid (4)  "
		       "signal 50  ambient 7   32.5 C"));
}

static void run_core_cases(void)
{
	test_ops();
	test_read();
	test_read_blocking();
	test_irq_hook();
	test_unbound();
	test_log();
}
