/*
 * core/i64set.h - a set of int64 in an open-addressing table of at most half full slots: a probe is a multiply, a shift and (about) one load.
 * `empty` marks a free slot, a value that is not a member (no byte table beside the slots, one cache line per probe). Portable (core/host.h).
 */
#ifndef SIFT_I64SET_H
#define SIFT_I64SET_H

#include "core/host.h"
#include "core/cmp.h"

typedef struct SiftI64Set
{
	int64_t    *slot;			/* mask + 1 slots, to pfree */
	uint64_t	mask;
	uint32_t	shift;			/* 64 - log2(mask + 1) */
	int64_t		empty;
} SiftI64Set;

static inline uint64_t
sift_i64set_slot(const SiftI64Set *s, int64_t k)
{
	return ((uint64_t) k * UINT64_C(0x9E3779B97F4A7C15)) >> s->shift;
}

/* the members vals[0..n) in any order, duplicates allowed */
static inline void
sift_i64set_build(SiftI64Set *s, const int64_t *vals, uint64_t n)
{
	int64_t		lo = INT64_MAX,
				hi = INT64_MIN;
	uint32_t	bits = 4;
	uint64_t	size;

	for (uint64_t i = 0; i < n; i++)
	{
		lo = vals[i] < lo ? vals[i] : lo;
		hi = vals[i] > hi ? vals[i] : hi;
	}
	if (lo > INT64_MIN)
		s->empty = lo - 1;
	else if (hi < INT64_MAX)
		s->empty = hi + 1;
	else
	{
		int64_t    *sorted = palloc(n * sizeof(int64_t));

		memcpy(sorted, vals, n * sizeof(int64_t));
		qsort(sorted, n, sizeof(int64_t), sift_cmp_i64);
		s->empty = INT64_MIN;
		for (uint64_t i = 0; i < n; i++)
			s->empty += sorted[i] == s->empty;
		pfree(sorted);
	}
	while ((UINT64_C(1) << bits) < n * 2)
		bits++;
	size = UINT64_C(1) << bits;
	s->shift = 64 - bits;
	s->mask = size - 1;
	s->slot = palloc(size * sizeof(int64_t));
	for (uint64_t i = 0; i < size; i++)
		s->slot[i] = s->empty;
	for (uint64_t i = 0; i < n; i++)
	{
		uint64_t	p = sift_i64set_slot(s, vals[i]);

		while (s->slot[p] != s->empty && s->slot[p] != vals[i])
			p = (p + 1) & s->mask;
		s->slot[p] = vals[i];
	}
}

static inline bool
sift_i64set_has(const SiftI64Set *s, int64_t k)
{
	for (uint64_t p = sift_i64set_slot(s, k); s->slot[p] != s->empty; p = (p + 1) & s->mask)
		if (s->slot[p] == k)
			return true;
	return false;
}

#endif
