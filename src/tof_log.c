#include <math.h>
#include <stdint.h>

#include "tof_log.h"

/*
 * Decimal text of v, written backwards from end. newlib-nano's printf, the
 * one stm-forge links, has no %lld, so the timestamp cannot go through PRId64.
 */
static const char *tof_format_i64(char *end, int64_t v)
{
	uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;

	*--end = '\0';
	do {
		*--end = (char)('0' + u % 10);
		u /= 10;
	} while (u);
	if (v < 0)
		*--end = '-';
	return end;
}

void tof_log_sample(const struct tof_dev *tof, const struct tof_sample *s)
{
	char t[21];	/* "-9223372036854775808" and its NUL */
	const char *at = tof_format_i64(t + sizeof(t), s->t_us);

	if (isnan(s->temp_c))
		tof_info(tof, "t %s us  %5u mm  %s (%u)  signal %u  ambient %u",
			 at, (unsigned)s->distance_mm,
			 s->valid ? "valid" : "invalid",
			 (unsigned)s->raw_status, (unsigned)s->signal,
			 (unsigned)s->ambient);
	else
		tof_info(tof, "t %s us  %5u mm  %s (%u)  signal %u  ambient %u"
			 "  %5.1f C",
			 at, (unsigned)s->distance_mm,
			 s->valid ? "valid" : "invalid",
			 (unsigned)s->raw_status, (unsigned)s->signal,
			 (unsigned)s->ambient, (double)s->temp_c);
}
