/*
 * col/f2/f2_part_idx.h - the parts of one partition by generation, and sets of generations: what a pass over the parts that asks for many of them by generation builds once (one hash of the
 * parts, one probe for each ask) instead of walking the list for each.
 */
#ifndef SIFT_F2_PART_IDX_H
#define SIFT_F2_PART_IDX_H

#include "postgres.h"

#include "port/pg_bitutils.h"

#include "core/f2/manifest.h"
#include "core/hash.h"

#define SIFT_PART_IDX_MIN_PARTS	16

typedef struct SiftPartGenIndex
{
	const SiftPartDesc *parts;
	int			nparts;
	uint32	   *slots;			/* the place in parts plus one, 0: empty; NULL for at most SIFT_PART_IDX_MIN_PARTS parts, which are walked */
	uint32		mask;
} SiftPartGenIndex;

static inline void
sift_part_gen_idx_init(SiftPartGenIndex *ix, const SiftPartDesc *parts, int nparts)
{
	*ix = (SiftPartGenIndex) {.parts = parts,.nparts = nparts};
	if (nparts <= SIFT_PART_IDX_MIN_PARTS)
		return;
	ix->mask = pg_nextpower2_32(2 * (uint32) nparts) - 1;
	ix->slots = palloc0((ix->mask + 1) * sizeof(uint32));
	for (int i = 0; i < nparts; i++)
	{
		uint32		h = (uint32) (sift_fmix64(parts[i].part_gen) & ix->mask);

		while (ix->slots[h] != 0)
			h = (h + 1) & ix->mask;
		ix->slots[h] = i + 1;
	}
}

static inline const SiftPartDesc *
sift_part_gen_idx_find(const SiftPartGenIndex *ix, uint64 part_gen)
{
	if (ix->slots == NULL)
	{
		for (int i = 0; i < ix->nparts; i++)
			if (ix->parts[i].part_gen == part_gen)
				return &ix->parts[i];
		return NULL;
	}
	for (uint32 h = (uint32) (sift_fmix64(part_gen) & ix->mask); ix->slots[h] != 0; h = (h + 1) & ix->mask)
		if (ix->parts[ix->slots[h] - 1].part_gen == part_gen)
			return &ix->parts[ix->slots[h] - 1];
	return NULL;
}

static inline void
sift_part_gen_idx_free(SiftPartGenIndex *ix)
{
	if (ix->slots != NULL)
		pfree(ix->slots);
	ix->slots = NULL;
}

/* A set of 64 bit numbers (generations) that only grows; the table is a power of two long and at most half full */
typedef struct SiftU64Set
{
	uint64	   *keys;
	uint32		mask;
	uint32		count;
	bool		has_zero;		/* 0 marks an empty slot, so the number 0 is kept apart */
} SiftU64Set;

static inline void
sift_u64_set_init(SiftU64Set *s, uint32 expected)
{
	s->mask = pg_nextpower2_32(Max(2 * expected, 16)) - 1;
	s->keys = palloc0((s->mask + 1) * sizeof(uint64));
	s->count = 0;
	s->has_zero = false;
}

static inline bool
sift_u64_set_has(const SiftU64Set *s, uint64 v)
{
	if (v == 0)
		return s->has_zero;
	for (uint32 h = (uint32) (sift_fmix64(v) & s->mask); s->keys[h] != 0; h = (h + 1) & s->mask)
		if (s->keys[h] == v)
			return true;
	return false;
}

static inline void
sift_u64_set_add(SiftU64Set *s, uint64 v)
{
	uint32		h;

	if (sift_u64_set_has(s, v))
		return;
	s->count++;
	if (v == 0)
	{
		s->has_zero = true;
		return;
	}
	if ((uint64) s->count * 2 > (uint64) s->mask + 1)
	{
		uint64	   *old = s->keys;
		uint32		n = s->mask + 1;

		s->mask = n * 2 - 1;
		s->keys = palloc0((s->mask + 1) * sizeof(uint64));
		for (uint32 i = 0; i < n; i++)
			if (old[i] != 0)
			{
				for (h = (uint32) (sift_fmix64(old[i]) & s->mask); s->keys[h] != 0; h = (h + 1) & s->mask)
					;
				s->keys[h] = old[i];
			}
		pfree(old);
	}
	for (h = (uint32) (sift_fmix64(v) & s->mask); s->keys[h] != 0; h = (h + 1) & s->mask)
		;
	s->keys[h] = v;
}

static inline void
sift_u64_set_free(SiftU64Set *s)
{
	pfree(s->keys);
	s->keys = NULL;
}

#endif							/* SIFT_F2_PART_IDX_H */
