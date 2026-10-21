/*
 * agg/osa_int.h - ordered-set aggregates: the values kept exactly (agg/osa.c)
 */
#ifndef SIFT_AGG_OSA_INT_H
#define SIFT_AGG_OSA_INT_H

#include "agg/agg_fwd_int.h"

/*
 * The values of an ordered-set aggregate, kept exactly (the multiset PostgreSQL sorts): a histogram of the distinct (group,
 * value) pairs while it fits work_mem, and every value in a tuplesort (work_mem, spilling to disk as the normal plan's sort)
 * once it does not. A value is its order key (float8 in the order of float8_cmp: -0 = 0, NaN last), a group the id of its
 * sorted values (in order of appearance; acc->i of the aggregate that owns the values, which the group renumbering carries).
 */
#define OSA_BLOCK	64

typedef struct OsaSlot
{
	uint64		key;
	uint32		sid1;			/* sorted-values id + 1, 0: free */
	uint32		cnt;
} OsaSlot;

typedef struct Osa
{
	OsaSlot    *slot;			/* histogram; NULL once spilled */
	uint64		cap;
	uint64		n;
	uint64		maxn;
	uint32		nsid;
	uint64		next;			/* read position in the compacted histogram */
	Tuplesortstate *sort;
	TupleTableSlot *put;
	TupleTableSlot *get;
} Osa;

typedef struct OsaRun
{
	uint32		sid;
	uint64		key;
	uint64		cnt;
} OsaRun;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* osa.c */
Osa *osa_new(AggExec *ex);
void osa_add_n(AggExec *ex, Osa *o, uint32 sid, uint64 key, uint64 cnt);
void osa_update(AggExec *ex, int ai, const uint64 *ok, int nsel);
void osa_sort(Osa *o);
bool osa_next(Osa *o, OsaRun *r);
void osa_finish_all(AggExec *ex);

#pragma GCC visibility pop

#endif
