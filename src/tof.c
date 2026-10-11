#include <errno.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define TOF_IRAM	IRAM_ATTR
#else
#define TOF_IRAM
#endif

#include "tof.h"
#include "tof_priv.h"

static bool tof_bound(const struct tof_dev *tof)
{
	return tof && tof->ops && tof->ops->start && tof->ops->stop &&
	       tof->ops->read;
}

int tof_start(struct tof_dev *tof)
{
	if (!tof_bound(tof))
		return -EINVAL;
	return tof->ops->start(tof);
}

int tof_stop(struct tof_dev *tof)
{
	if (!tof_bound(tof))
		return -EINVAL;
	return tof->ops->stop(tof);
}

int tof_data_ready(struct tof_dev *tof, bool *ready)
{
	bool r;
	int ret;

	if (!tof_bound(tof) || !ready)
		return -EINVAL;
	if (!tof->ops->data_ready)
		return -EOPNOTSUPP;

	ret = tof->ops->data_ready(tof, &r);
	if (ret)
		return ret;
	*ready = r;
	return 0;
}

int tof_read(struct tof_dev *tof, struct tof_sample *s)
{
	struct tof_sample tmp = { 0 };
	int64_t t;
	int ret;

	if (!tof_bound(tof) || !s)
		return -EINVAL;

	/* Stamp before the bus transfer: that is closer to the sample. */
	t = tof_time_us();
	ret = tof->ops->read(tof, &tmp);
	if (ret)
		return ret;
	tmp.t_us = t;
	tmp.valid = tmp.status == TOF_VALID;
	*s = tmp;
	return 0;
}

int tof_read_blocking(struct tof_dev *tof, struct tof_sample *s,
		      uint32_t timeout_ms)
{
	int64_t start;
	int ret;

	if (!tof_bound(tof) || !s)
		return -EINVAL;

	start = tof_time_us();
	for (;;) {
		ret = tof_read(tof, s);
		if (ret != -EAGAIN)
			return ret;
		if (tof_elapsed_ms(start) >= timeout_ms)
			return -ETIMEDOUT;
		tof_yield();
	}
}

int tof_set_period_ms(struct tof_dev *tof, uint32_t ms)
{
	if (!tof_bound(tof))
		return -EINVAL;
	if (!tof->ops->set_period_ms)
		return -EOPNOTSUPP;
	return tof->ops->set_period_ms(tof, ms);
}

int tof_set_budget_ms(struct tof_dev *tof, uint32_t ms)
{
	if (!tof_bound(tof))
		return -EINVAL;
	if (!tof->ops->set_budget_ms)
		return -EOPNOTSUPP;
	return tof->ops->set_budget_ms(tof, ms);
}

/* A count, not a flag: a flag cleared by the thread could lose an edge
 * that landed between its look and its clear. In IRAM on ESP-IDF, for an
 * ISR that runs with flash off; it calls nothing. */
TOF_IRAM void tof_irq(struct tof_dev *tof)
{
	if (tof)
		tof->irq_count++;
}

bool tof_irq_pending(struct tof_dev *tof)
{
	uint32_t now;

	if (!tof)
		return false;
	now = tof->irq_count;
	if (now == tof->irq_seen)
		return false;
	tof->irq_seen = now;
	return true;
}

uint32_t tof_elapsed_ms(int64_t start)
{
	int64_t us = tof_time_us() - start;

	if (us <= 0)
		return 0;
	return us / 1000 > UINT32_MAX ? UINT32_MAX : (uint32_t)(us / 1000);
}
