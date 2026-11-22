/*
 * agg/group/rollup.h - GROUPING SETS, ROLLUP and CUBE in one pass over the rows: PostgreSQL sorts and groups the input once for each chain of sets; here every
 * set is fed from one scan of the batches, and the aggregates are those of agg/group/ht.h's table.
 *
 * Aggregates: count(*), and for each of up to SIFT_ROLLUP_MAX_VALS int4 or int8 value columns count, sum (128 bits, exact), min and max with the NULLs
 * ignored, and optionally count(DISTINCT) of the first value column. Keys: up to SIFT_ROLLUP_MAX_KEYS int4 or int8 columns, a NULL key a group of its own.
 *
 * Two ways, chosen by the aggregates. When every aggregate can be merged from the groups of a finer set (all but count(DISTINCT)), only the leaf sets are
 * fed from the rows (a set no other set strictly contains: the one set of ROLLUP(a, b, c) and CUBE(a, b, c)), each into a table of its own, and
 * sift_rollup_finish builds every other set from the groups of the smallest set that contains it: the grand total is one accumulator and no table. Else
 * (or on request) every set is fed from the rows, with the hash of each key column of the batch made once and combined for each set, so that a string
 * or a number is not hashed again for each set. The groups are the same either way; the second costs one upsert per row for each set.
 *
 * The same set listed twice (GROUPING SETS ((a), (a))) is one table, emitted twice. The grand total () has a group even when there is no row; no other set has.
 * An emitted group has the key columns of its set (the others NULL, and in omitted the bits of the columns that are not in the set, which is what GROUPING()
 * reports), count(*), and the aggregates of each value column. The groups of a table come in no order. The tables are in cxt and are held to max_bytes by
 * tiers (agg/group/spill.h): a set grows its table in memory, and when the sets hold SIFT_SPILL_SOFT_PERCENT of max_bytes the biggest one writes its groups to
 * temporary files by the hash kept in each slot, and keeps a small table for the rows that follow; its groups come back, final, one partition at a time, when the
 * sets are built from it and when they are emitted. A scan of any number of groups gives the same groups as one that fits. What has no way to be smaller is an error
 * 54000: the distinct values of one group that outgrow max_bytes.
 */
#ifndef SIFT_AGG_GROUP_ROLLUP_H
#define SIFT_AGG_GROUP_ROLLUP_H

#include "agg/group/ht.h"

#define SIFT_ROLLUP_MAX_KEYS	16
#define SIFT_ROLLUP_MAX_VALS	4
#define SIFT_ROLLUP_MAX_SETS	256

typedef struct SiftRollupSpec
{
	uint8		n_keys;
	uint8		n_vals;
	uint16		n_sets;
	uint64		set_masks[SIFT_ROLLUP_MAX_SETS];	/* bit k: key column k is grouped in the set; the sets in the order they are emitted */
	bool		count_distinct;	/* count(DISTINCT) of value column 0 */
	bool		force_prehash;	/* feed every set from the rows even when the leaves would do */
} SiftRollupSpec;

typedef struct SiftRollupBatch
{
	uint16		n;
	const SiftVec *keys[SIFT_ROLLUP_MAX_KEYS];
	const SiftVec *vals[SIFT_ROLLUP_MAX_VALS];
	const uint64 *mask;			/* the rows taken, NULL = all n */
} SiftRollupBatch;

typedef struct SiftRollupVal
{
	int64		nvals;			/* the rows with a value */
	int64		pad;
	SiftI128	sum;
	int64		min;
	int64		max;
} SiftRollupVal;

typedef struct SiftRollupGroup
{
	uint16		set;			/* the index in set_masks */
	uint64		omitted;		/* bit k: key column k is not in the set */
	int64		key[SIFT_ROLLUP_MAX_KEYS];
	uint64		key_nulls;		/* bit k: key column k is NULL in the output: omitted, or a NULL key */
	int64		rows;
	uint64		distinct;		/* count(DISTINCT) of value column 0 */
	SiftRollupVal val[SIFT_ROLLUP_MAX_VALS];
} SiftRollupGroup;

typedef struct SiftRollupTree SiftRollupTree;
typedef void (*SiftRollupEmit) (void *arg, const SiftRollupGroup *g);

extern SiftRollupTree *sift_rollup_create(MemoryContext cxt, const SiftRollupSpec *spec, uint64 max_bytes);
extern void sift_rollup_feed(SiftRollupTree *t, const SiftRollupBatch *b);
/* After the last batch: the sets that were not fed from the rows are built from the ones that were */
extern void sift_rollup_finish(SiftRollupTree *t);
extern void sift_rollup_emit(SiftRollupTree *t, SiftRollupEmit emit, void *arg);
/* The sets fed from the rows, for EXPLAIN: n_nodes distinct sets, n_leaves of them fed, post_rollup when the others are built from them */
extern void sift_rollup_shape(const SiftRollupTree *t, uint16 *n_nodes, uint16 *n_leaves, bool *post_rollup);
/* The sets that went to the files and the bytes they wrote, for EXPLAIN */
extern void sift_rollup_spill_stats(const SiftRollupTree *t, uint16 *nodes_spilled, uint64 *bytes_spilled, uint32 *depth);
extern void sift_rollup_free(SiftRollupTree *t);

#endif							/* SIFT_AGG_GROUP_ROLLUP_H */
