/*
 * core/f2/unit_queue.c - the queue of units of a parallel scan (core/f2/unit_queue.h).
 */
#include "postgres.h"

#include "core/f2/unit_queue.h"

void
sift_unit_queue_init(SiftUnitQueue *q, uint64 nunits)
{
	memset(q, 0, sizeof(SiftUnitQueue));
	q->nunits = nunits;
	pg_atomic_init_u64(&q->next, 0);
}

void
sift_unit_queue_reset(SiftUnitQueue *q)
{
	pg_atomic_write_u64(&q->next, 0);
}

bool
sift_unit_queue_next(SiftUnitQueue *q, uint64 *first, uint32 *count)
{
	uint64		cur = pg_atomic_read_u64(&q->next);

	while (cur < q->nunits)
	{
		uint64		left = q->nunits - cur;
		uint64		take = Max(1, Min(left / SIFT_UNIT_QUEUE_SHARE, SIFT_UNIT_QUEUE_MAX_CLAIM));

		if (pg_atomic_compare_exchange_u64(&q->next, &cur, cur + take))
		{
			*first = cur;
			*count = (uint32) take;
			return true;
		}
	}
	return false;
}
