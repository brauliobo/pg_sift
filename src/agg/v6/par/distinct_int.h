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

static inline uint32
dist_pay_last(const SiftParDistinct *d)
{
	return 1 + d->sp.nsum;
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
