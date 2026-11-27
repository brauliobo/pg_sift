/*
 * core/u64set.h - a set of 64 bit keys: open addressing, no removal, at most half full. It starts on the stack of the caller with room for the keys the caller expects and doubles
 * (in the memory context it was made in) when a key would fill it past half. The membership tests of the passes that compare two lists of ids (segments, parts, generations, tids)
 * are one probe each instead of a scan of the other list.
 */
#ifndef SIFT_CORE_U64SET_H
#define SIFT_CORE_U64SET_H

#include "postgres.h"

#include "utils/memutils.h"

#include "core/host.h"

#define SIFT_U64SET_STACK	64
/* the most slots one palloc holds, and so the most keys: half of them */
#define SIFT_U64SET_MAX_SLOTS	((uint64) 1 << 26)
#define SIFT_U64SET_MAX_KEYS	(SIFT_U64SET_MAX_SLOTS / 2)

typedef struct SiftU64Set
{
	uint64	   *key;
	uint64	   *used;			/* a bit for each slot: a key may be any value, 0 included */
	uint32		mask;
	uint32		n;
	MemoryContext cxt;
	uint64		stack_key[2 * SIFT_U64SET_STACK];
	uint64		stack_used[2 * SIFT_U64SET_STACK / 64];
} SiftU64Set;

StaticAssertDecl(SIFT_U64SET_MAX_SLOTS * sizeof(uint64) <= MaxAllocSize, "a u64set table is one palloc");

static inline uint32
sift_u64set_slot(const SiftU64Set *s, uint64 k)
{
	return (uint32) ((k * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & s->mask;
}

static inline void
sift_u64set_alloc(SiftU64Set *s, uint64 cap)
{
	SIFT_INVARIANT(cap <= SIFT_U64SET_MAX_SLOTS, "sift: a set of ids holds at most " UINT64_FORMAT " keys, which its callers bound", SIFT_U64SET_MAX_KEYS);
	s->mask = (uint32) (cap - 1);
	if (cap == 2 * SIFT_U64SET_STACK)
	{
		s->key = s->stack_key;
		s->used = s->stack_used;
	}
	else
	{
		/* check-memory: allow a table of twice the keys one pass holds, which the callers bound (ids of one directory or partition, a share of work_mem) */
		s->key = MemoryContextAlloc(s->cxt, cap * sizeof(uint64));
		s->used = MemoryContextAlloc(s->cxt, cap / 64 * sizeof(uint64));
	}
	memset(s->used, 0, cap / 64 * sizeof(uint64));
}

/* Room for n keys before the first growth */
static inline void
sift_u64set_init(SiftU64Set *s, uint32 n)
{
	uint64		cap = 2 * SIFT_U64SET_STACK;

	while (cap < 2 * (uint64) n)
		cap *= 2;
	s->n = 0;
	s->cxt = CurrentMemoryContext;
	sift_u64set_alloc(s, cap);
}

static inline uint32
sift_u64set_probe(const SiftU64Set *s, uint64 k, bool *found)
{
	uint32		p = sift_u64set_slot(s, k);

	for (; (s->used[p / 64] >> (p % 64)) & 1; p = (p + 1) & s->mask)
		if (s->key[p] == k)
		{
			*found = true;
			return p;
		}
	*found = false;
	return p;
}

static inline void
sift_u64set_put(SiftU64Set *s, uint32 p, uint64 k)
{
	s->used[p / 64] |= UINT64CONST(1) << (p % 64);
	s->key[p] = k;
	s->n++;
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

static inline void
sift_u64set_grow(SiftU64Set *s)
{
	SiftU64Set	old = *s;
	uint64		cap = (uint64) s->mask + 1;
	bool		found;

	if (old.key == s->stack_key)
	{
		old.key = old.stack_key;
		old.used = old.stack_used;
	}
	s->n = 0;
	sift_u64set_alloc(s, cap * 2);
	for (uint64 p = 0; p < cap; p++)
		if ((old.used[p / 64] >> (p % 64)) & 1)
			sift_u64set_put(s, sift_u64set_probe(s, old.key[p], &found), old.key[p]);
	sift_u64set_free(&old);
}

/* true when k was not in the set */
static inline bool
sift_u64set_add(SiftU64Set *s, uint64 k)
{
	bool		found;
	uint32		p = sift_u64set_probe(s, k, &found);

	if (found)
		return false;
	if (2 * ((uint64) s->n + 1) > (uint64) s->mask + 1)
	{
		sift_u64set_grow(s);
		p = sift_u64set_probe(s, k, &found);
	}
	sift_u64set_put(s, p, k);
	return true;
}

static inline bool
sift_u64set_has(const SiftU64Set *s, uint64 k)
{
	bool		found;

	(void) sift_u64set_probe(s, k, &found);
	return found;
}

#endif
