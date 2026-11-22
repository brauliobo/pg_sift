/*
 * table/v1/keyidx.h - an insert-only index from a 64 bit key to the position of its entry in an array
 */
#ifndef SIFT_TABLE_V1_KEYIDX_H
#define SIFT_TABLE_V1_KEYIDX_H

#include "postgres.h"

#include "utils/memutils.h"

/* An insert-only open-addressed index from a 64 bit key to the position of its entry in an array: one probe for what the loops over the segments of a directory looked up in a scan of the array */
typedef struct KeyIdx
{
	uint64	   *key;
	int		   *pos;			/* -1 for a free slot */
	uint32		mask;
	uint32		n;
} KeyIdx;

static inline uint32
keyidx_slot(const KeyIdx *x, uint64 k)
{
	return (uint32) ((k * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & x->mask;
}

static inline int
keyidx_find(const KeyIdx *x, uint64 k)
{
	if (x->n == 0)
		return -1;
	for (uint32 p = keyidx_slot(x, k); x->pos[p] >= 0; p = (p + 1) & x->mask)
		if (x->key[p] == k)
			return x->pos[p];
	return -1;
}

static inline void
keyidx_clear(KeyIdx *x)
{
	if (x->n)
		memset(x->pos, 0xFF, (x->mask + 1) * sizeof(int));
	x->n = 0;
}


static inline void
keyidx_put(MemoryContext cxt, KeyIdx *x, uint64 k, int pos)
{
	if (2 * (x->n + 1) > x->mask + 1)
	{
		KeyIdx		old = *x;
		uint32		cap = x->n ? 2 * (x->mask + 1) : 64;

		/* check-memory: allow an index over the segments of the table, which the directory bounds */
		x->key = MemoryContextAlloc(cxt, cap * sizeof(uint64));
		x->pos = MemoryContextAlloc(cxt, cap * sizeof(int));
		x->mask = cap - 1;
		memset(x->pos, 0xFF, cap * sizeof(int));
		x->n = 0;
		for (uint32 p = 0; old.n && p <= old.mask; p++)
			if (old.pos[p] >= 0)
				keyidx_put(cxt, x, old.key[p], old.pos[p]);
		if (old.n)
		{
			pfree(old.key);
			pfree(old.pos);
		}
	}
	uint32		p = keyidx_slot(x, k);

	while (x->pos[p] >= 0)
		p = (p + 1) & x->mask;
	x->key[p] = k;
	x->pos[p] = pos;
	x->n++;
}


static inline void
keyidx_free(KeyIdx *x)
{
	if (x->n)
	{
		pfree(x->key);
		pfree(x->pos);
	}
	*x = (KeyIdx) {0};
}

#endif
