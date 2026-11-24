/*
 * agg/v6/par/distinct_int.h - what distinct.c (phase 1, the seal, the answer) and distinct_p2.c (phase 2) share: the state of a thread and the records of the partitions.
 */
#ifndef SIFT_AGG_V6_PAR_DISTINCT_INT_H
#define SIFT_AGG_V6_PAR_DISTINCT_INT_H

#include "postgres.h"

#include "agg/v6/par/distinct.h"
#include "agg/v6/par/keytab.h"
#include "core/par/radix.h"

/* The keys a thread has numbered, with the entries of its phase 2 for its partitions: its answer, in blocks of its state arena */
typedef struct DistOut
{
	struct DistOut *next;
	uint32		n;
	uint64		ent[FLEXIBLE_ARRAY_MEMBER];
} DistOut;

/*
 * The payload of the entry of a key is its rows, its sums, and a last word: in the table of the thread, the number the key has in the table of its partition (phase 2 sets it before the values of the
 * partition are read); in the table of a partition, the distinct values of the key.
 */
typedef struct DistWorker
{
	SiftKeyTab	keys;
	SiftKeyTab	vals;			/* the texts the thread counts */
	SiftRadixOut raw;			/* the values: DistRec with a key, else the word of the value (the number of a text) */
	SiftRadixOut agg;			/* the rows and sums of each key: its number, its rows, its sums */
	DistOut    *out;
	DistOut    *out_tail;
	uint64		cnt;			/* with no key: the distinct values of the partitions it took */
} DistWorker;

struct SiftParDistinct
{
	SiftParDistinctSpec sp;
	SiftParDistinctHooks hooks;
	SiftParArenas *ar;
	int			nw;
	bool		grouped;
	bool		direct;			/* the keys are a range of words (the key tables of the threads are direct) */
	SiftKeyShape ks;
	SiftKeyShape vs;
	DistWorker *w;
};

typedef struct DistRec
{
	uint64		v;
	uint32		kid;
	uint32		pad;
} DistRec;

/* The words of the payload of a key: its rows, its sums, the text minimums and maximums (3 words each), and a last word (below) */
static inline uint32
dist_pay_mm(const SiftParDistinct *d, int m)
{
	return 1 + d->sp.nsum + 3 * m;
}

static inline uint32
dist_pay_last(const SiftParDistinct *d)
{
	return dist_pay_mm(d, d->sp.nmm);
}

/*
 * A text minimum or maximum in the payload of a key: the place of the bytes, their length plus one (0: no row had a text), and the bytes the thread owns there (0: the bytes are another's). The
 * order is that of the bytes. A thread copies the best into its arena when a better one comes, in a block it reuses while the best fit it; the answer of a partition points to those blocks.
 */
static inline bool
dist_mm_better(const uint64 *s, bool max, const uint8 *p, uint32 len)
{
	const uint32 n = s[1] == 0 ? 0 : Min(len, (uint32) (s[1] - 1));
	int			c;

	if (s[1] == 0)
		return true;
	c = n > 0 ? memcmp(p, (const uint8 *) (uintptr_t) s[0], n) : 0;
	if (c == 0)
		c = len < s[1] - 1 ? -1 : len > s[1] - 1;
	return max ? c > 0 : c < 0;
}

/* The text p into the state s if it is better; own: the arena the copy is made in (NULL: s points at p, which stays); false when the arena has no room */
static inline bool
dist_mm_offer(uint64 *s, bool max, const uint8 *p, uint32 len, SiftArena *own)
{
	if (!dist_mm_better(s, max, p, len))
		return true;
	if (own != NULL)
	{
		if (len > s[2] || s[2] == 0)
		{
			const uint64 cap = Max(len, Max(32, 2 * s[2]));
			uint8	   *copy = sift_arena_alloc(own, cap, 1);

			if (copy == NULL)
				return false;
			s[0] = (uint64) (uintptr_t) copy;
			s[2] = cap;
		}
		if (len > 0)
			memcpy((void *) (uintptr_t) s[0], p, len);
	}
	else
	{
		s[0] = (uint64) (uintptr_t) p;
		s[2] = 0;
	}
	s[1] = (uint64) len + 1;
	return true;
}

static inline bool
dist_fail(SiftErr *err, const char *msg)
{
	sift_err_fill(err, SIFT_ERR_WORK_MEM, 0, 0, msg);
	return false;
}

/* Phase 2 of partition `part` on thread t */
extern bool sift_dist_phase2(SiftParDistinct *d, int t, uint32 part, SiftArena *work);

#endif							/* SIFT_AGG_V6_PAR_DISTINCT_INT_H */
