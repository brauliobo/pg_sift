/*
 * agg/par/morsel.c - the guided work queue of agg/par/morsel.h.
 */
#include "postgres.h"

#include "agg/par/morsel.h"
#include "port/pg_bitutils.h"

Size
sift_morsel_size(uint32 n_parts)
{
	return add_size(offsetof(SiftMorselQueue, part_start), mul_size(sizeof(uint32), (Size) n_parts + 1));
}

void
sift_morsel_init(SiftMorselQueue *q, uint32 n_parts, const uint32 *granules, uint32 n_participants)
{
	uint64		total = 0;

	Assert(n_participants >= 1);
	for (uint32 p = 0; p < n_parts; p++)
	{
		q->part_start[p] = (uint32) total;
		total += granules[p];
		if (total > UINT32_MAX)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("too many granules in one parallel scan"),
							errdetail("A parallel scan holds at most %u granules.", UINT32_MAX), errhint("Scan fewer parts at once.")));
	}
	q->part_start[n_parts] = (uint32) total;
	q->total = (uint32) total;
	q->n_parts = n_parts;
	q->n_participants = n_participants;
	q->n_lanes = Min(n_participants, SIFT_MORSEL_LANES);
	pg_atomic_init_u64(&q->topn_theta_bits, 0);
	for (uint32 l = 0; l < q->n_lanes; l++)
	{
		pg_atomic_init_u32(&q->lanes[l].cursor, (uint32) (total * l / q->n_lanes));
		q->lanes[l].end = (uint32) (total * (l + 1) / q->n_lanes);
	}
}

uint32
sift_morsel_run(uint32 left, uint32 n_participants)
{
	return pg_prevpower2_32(Max(1, Min(left / (4 * n_participants), SIFT_MORSEL_MAX)));
}

/* The part of the flat granule g: the last part that starts at or before it (the parts with no granule start where the next does) */
static uint32
part_of(const SiftMorselQueue *q, uint32 g)
{
	uint32		lo = 0,
				hi = q->n_parts;

	while (hi - lo > 1)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (q->part_start[mid] <= g)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

/* A run from the lane: guided by the granules the lane has left */
static bool
lane_claim(SiftMorselQueue *q, SiftMorselLane *lane, SiftMorsel *m)
{
	uint32		cur = pg_atomic_read_u32(&lane->cursor);

	while (cur < lane->end)
	{
		const uint32 part = part_of(q, cur);
		const uint32 run = Min(sift_morsel_run(lane->end - cur, q->n_participants), q->part_start[part + 1] - cur);

		if (pg_atomic_compare_exchange_u32(&lane->cursor, &cur, cur + run))
		{
			m->part = part;
			m->first = cur - q->part_start[part];
			m->n = run;
			return true;
		}
	}
	return false;
}

/* The lane with the most granules left, NULL when every lane is consumed */
static SiftMorselLane *
fullest_lane(SiftMorselQueue *q)
{
	SiftMorselLane *best = NULL;
	uint32		best_left = 0;

	for (uint32 l = 0; l < q->n_lanes; l++)
	{
		const uint32 cur = pg_atomic_read_u32(&q->lanes[l].cursor);
		const uint32 left = cur < q->lanes[l].end ? q->lanes[l].end - cur : 0;

		if (left > best_left)
		{
			best = &q->lanes[l];
			best_left = left;
		}
	}
	return best;
}

bool
sift_morsel_claim(SiftMorselQueue *q, uint32 participant, SiftMorsel *m)
{
	SiftMorselLane *lane = &q->lanes[participant % q->n_lanes];

	while (lane)
	{
		if (lane_claim(q, lane, m))
			return true;
		lane = fullest_lane(q);
	}
	return false;
}

uint64
sift_morsel_theta_load(SiftMorselQueue *q)
{
	return pg_atomic_read_u64(&q->topn_theta_bits);
}

void
sift_morsel_theta_raise(SiftMorselQueue *q, uint64 bits)
{
	uint64		cur = pg_atomic_read_u64(&q->topn_theta_bits);

	while (cur < bits && !pg_atomic_compare_exchange_u64(&q->topn_theta_bits, &cur, bits))
		;
}

void
sift_morsel_stop(SiftMorselQueue *q)
{
	for (uint32 l = 0; l < q->n_lanes; l++)
		pg_atomic_write_u32(&q->lanes[l].cursor, q->lanes[l].end);
}
