/*
 * agg/intids.h - the ids (1..) of 64-bit integer keys, in order of first sight (agg/intids.c)
 */
#ifndef SIFT_AGG_INTIDS_H
#define SIFT_AGG_INTIDS_H

#include "core/hash.h"

typedef struct IntIdEnt
{
	int64		key;
	uint32		id;				/* 0: free */
} IntIdEnt;

#define INTIDS_PAGE_BITS	8
#define INTIDS_PAGE			(1 << INTIDS_PAGE_BITS)

/*
 * Open addressing of (key, id), one probe a cache line. While the keys lie in a range that is small, or at most four times their number,
 * the ids are pages of INTIDS_PAGE entries indexed by key - lo instead (pg, a page made when a key first falls in it): a lookup is two
 * loads, in the order of the keys, and the range a scan does not touch costs nothing. The table follows its keys: a range that grows
 * sparse goes back to the hash, one that has become dense is made pages when the hash grows.
 */
typedef struct IntIds
{
	MemoryContext cxt;
	uint32		n;				/* keys held */
	uint32		free_span;		/* a range of this many keys is paged whatever the keys held (a caller that knows the scan is large raises it) */
	IntIdEnt   *tab;			/* hash mode (pg NULL) */
	uint32		nslot;
	int64		kmin;			/* hash mode: the range of the keys held */
	int64		kmax;
	uint32	  **pg;			/* paged mode: the page of key lo + i * INTIDS_PAGE, or NULL */
	uint32		npg;
	int64		lo;				/* a multiple of INTIDS_PAGE */
} IntIds;

extern void intids_init(IntIds *x);
extern void intids_free(IntIds *x);
extern uint32 intids_slow(IntIds *x, int64 k, uint32 next);

static inline uint32
intids_hash(int64 k)
{
	uint64		x = sift_fib64((uint64) k);

	return (uint32) (sift_fold64(x, x >> 29) >> 32);
}

/* the id of k; a key not held takes `next` (the caller's count of ids + 1), and the caller sees that by id == next */
static inline uint32
intids_id(IntIds *x, int64 k, uint32 next)
{
	if (likely(x->pg != NULL))
	{
		uint64		off = (uint64) k - (uint64) x->lo;

		if (likely(off < (uint64) x->npg << INTIDS_PAGE_BITS))
		{
			uint32	   *p = x->pg[off >> INTIDS_PAGE_BITS];

			if (likely(p != NULL))
			{
				uint32	   *e = &p[off & (INTIDS_PAGE - 1)];

				if (likely(*e != 0))
					return *e;
				*e = next;
				x->n++;
				return next;
			}
		}
	}
	return intids_slow(x, k, next);
}

/* ask for the memory a lookup of k will read, a few keys early */
static inline void
intids_prefetch(const IntIds *x, int64 k)
{
	if (x->pg != NULL)
	{
		uint64		off = (uint64) k - (uint64) x->lo;

		if (off < (uint64) x->npg << INTIDS_PAGE_BITS && x->pg[off >> INTIDS_PAGE_BITS] != NULL)
			__builtin_prefetch(&x->pg[off >> INTIDS_PAGE_BITS][off & (INTIDS_PAGE - 1)]);
	}
	else if (x->nslot > 4096)
		__builtin_prefetch(&x->tab[intids_hash(k) & (x->nslot - 1)]);
}

#endif							/* SIFT_AGG_INTIDS_H */
