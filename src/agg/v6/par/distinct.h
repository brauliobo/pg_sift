/*
 * agg/v6/par/distinct.h - COUNT(DISTINCT) for W threads, as an operator of core/par/morsel.h, in pure C (arenas of the leader, no server call; see agg/v6/par/group.h for the rules of the threads).
 *
 * An integer column, with or without groups: phase 1 scatters the values (the pairs of a group's number and a value, for a grouped one) into the partitions of core/par/radix.h by the hash of the
 * value (of the pair: a group that holds most rows spreads over all of them, where the hash of the group would give one thread all of it). Phase 2: each thread takes whole partitions from the shared
 * cursor and deduplicates them in a table that fits the L2 cache; a new value adds to the count of the thread, a new pair to the count of its group in the thread's own array. The counts are added when
 * the run is done. A dictionary column is a bitset a thread sets for the codes it saw: the threads OR them (sift_par_distinct_bits_or), each a range of words. A text column is the aggregation of
 * agg/v6/par/group.h with no sum: its groups are the distinct strings.
 */
#ifndef SIFT_AGG_V6_PAR_DISTINCT_H
#define SIFT_AGG_V6_PAR_DISTINCT_H

#include "postgres.h"

#include "core/par/morsel.h"

/* The values of a unit of the scan, and with groups the group of each (below ngroups) */
typedef struct SiftParDistinctBatch
{
	uint32		n;
	const uint64 *val;
	const uint32 *gid;
} SiftParDistinctBatch;

typedef struct SiftParDistinctHooks
{
	/* Thread `worker`: the values of the items [lo, hi), in memory of scratch that is valid until the next call; false, with err filled, when it cannot (the items are none of them folded) */
	bool		(*fill) (void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParDistinctBatch *out, SiftErr *err);
	/* After an overflow: every value that was folded, once or more, for the exact set of the server (which deduplicates); gid is 0 for the one set */
	void		(*value) (void *arg, uint32 gid, uint64 value);
	/* After the values: the items that are still to be done */
	void		(*overflow) (void *arg, const SiftParRange *left, uint32 nleft);
	void	   *arg;
} SiftParDistinctHooks;

typedef struct SiftParDistinct SiftParDistinct;

/* ngroups 0: the values of one set; else the groups are the numbers 0 .. ngroups-1 (below UINT32_MAX). NULL, with err set, when an arena is too small for the first tables */
extern SiftParDistinct *sift_par_distinct_create(SiftParArenas *ar, uint32 ngroups, const SiftParDistinctHooks *hooks, SiftErr *err);

/* The operator: sift_par_run(cfg, &sift_par_distinct_ops, d, ar, rel) */
extern const SiftParOps sift_par_distinct_ops;

/* After the run: the distinct values of the one set; out[g], the distinct values of group g, for every group */
extern uint64 sift_par_distinct_count(const SiftParDistinct *d);
extern void sift_par_distinct_groups(const SiftParDistinct *d, uint64 *out);

/* The OR of the nbits bitsets (nwords words each) over the words of thread t of nthreads, into out (when not NULL); returns the bits set in them */
extern uint64 sift_par_distinct_bits_or(const uint64 *const *bits, int nbits, uint32 nwords, int t, int nthreads, uint64 *out);

#endif							/* SIFT_AGG_V6_PAR_DISTINCT_H */
