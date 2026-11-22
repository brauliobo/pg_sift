/*
 * core/u64set.h - a set of 64 bit keys that is sized once for the keys it will hold: open addressing, no removal. The membership tests of the passes that compare two lists of ids
 * (segments, parts, generations) are one probe each instead of a scan of the other list.
 */
#ifndef SIFT_CORE_U64SET_H
#define SIFT_CORE_U64SET_H

#include "postgres.h"

#define SIFT_U64SET_STACK	64

typedef struct SiftU64Set
{
	uint64	   *key;
	uint64	   *used;			/* a bit for each slot: a key may be any value, 0 included */
	uint32		mask;
	uint64		stack_key[2 * SIFT_U64SET_STACK];
	uint64		stack_used[2 * SIFT_U64SET_STACK / 64];
} SiftU64Set;

static inline uint32
sift_u64set_slot(const SiftU64Set *s, uint64 k)
{
	return (uint32) ((k * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & s->mask;
}

/* Room for n keys: a table of at least twice as many slots, on the stack of the caller up to SIFT_U64SET_STACK keys */
static inline void
sift_u64set_init(SiftU64Set *s, uint32 n)
{
	uint32		cap = 2 * SIFT_U64SET_STACK;

	while (cap < 2 * n)
		cap *= 2;
	s->mask = cap - 1;
	if (cap == 2 * SIFT_U64SET_STACK)
	{
		s->key = s->stack_key;
		s->used = s->stack_used;
	}
	else
	{
		/* check-memory: allow a table of twice the keys one pass compares, which the ids of one directory or one partition bound */
		s->key = palloc(cap * sizeof(uint64));
		s->used = palloc(cap / 64 * sizeof(uint64));
	}
	memset(s->used, 0, cap / 64 * sizeof(uint64));
}

/* true when k was not in the set */
static inline bool
sift_u64set_add(SiftU64Set *s, uint64 k)
{
	uint32		p = sift_u64set_slot(s, k);

	for (; (s->used[p / 64] >> (p % 64)) & 1; p = (p + 1) & s->mask)
		if (s->key[p] == k)
			return false;
	s->used[p / 64] |= UINT64CONST(1) << (p % 64);
	s->key[p] = k;
	return true;
}

static inline bool
sift_u64set_has(const SiftU64Set *s, uint64 k)
{
	for (uint32 p = sift_u64set_slot(s, k); (s->used[p / 64] >> (p % 64)) & 1; p = (p + 1) & s->mask)
		if (s->key[p] == k)
			return true;
	return false;
}

static inline void
sift_u64set_free(SiftU64Set *s)
{
	if (s->key != s->stack_key)
	{
		pfree(s->key);
		pfree(s->used);
	}
}

#endif
