/*
 * agg/v6/par/group.h - the grouped aggregation of SiftAggV6 for W threads that share nothing but their results, as an operator of core/par/morsel.h: in pure C, with no palloc, no ereport and no
 * PostgreSQL call. Every byte a thread takes comes from the arenas that the leader made before the threads started (SiftParArenas), and what a thread cannot do it says in a SiftErr that the leader
 * raises after the barrier.
 *
 * Few groups (the planner expects up to SIFT_PAR_GROUP_LOW): each thread folds its rows into a table of its own, and the seal merges the W tables on the leader. Many groups: phase 1, each thread
 * scatters its rows (the key words and the arguments, and the bytes of the text keys) by the top bits of the hash of the key into the partitions of core/par/radix.h. Phase 2, every thread takes whole
 * partitions from the shared cursor, folds the W streams of a partition into a table that fits the L2 cache (no other thread holds a key of it: no lock) and offers its groups to the K best that
 * the thread keeps, or (no K) to a stream of its own that the leader hands to the consumer, which gets each group once. A thread whose table of a few groups outgrows them goes on in the second regime, its groups handed to the partitions as partial
 * aggregates. The table is the compact group table of the backend (agg/v6/group/cmp.h): the same slots, the same hashes, in the arena of a thread; its layout of a key is agg/v6/par/group_key.h.
 *
 * The key is nk columns, each a word of the caller (a NULL is a word of its own: the table never sees one) or a text (the length SIFT_STR_NULL is the NULL one), in the order of the statement: a
 * column that the caller computes (a derived key) is one more word or one more text. The aggregates are the count of the rows and nsum sums of 64-bit arguments, which the rows carry. The order of the
 * K best is the count or a sum (or none), then the keys in their order, a word as unsigned and a text as bytes: a total order, so that the best are the same for any W.
 *
 * A batch is folded whole or not at all: when a thread's arena has no room for it, it says SIFT_ERR_WORK_MEM and the run hands what is folded to the consumer as partial aggregates (the count and
 * the sums of a row group) and the items that are not to the serial path (SiftParGroupHooks.overflow).
 */
#ifndef SIFT_AGG_V6_PAR_GROUP_H
#define SIFT_AGG_V6_PAR_GROUP_H

#include "postgres.h"

#include "core/par/morsel.h"

#define SIFT_PAR_GROUP_MAX_KEYS	5
#define SIFT_PAR_GROUP_MAX_SUM	6
#define SIFT_PAR_GROUP_LOW		4096
#define SIFT_PAR_GROUP_TOPK_MAX	1024
#define SIFT_PAR_GROUP_BY_KEY	UINT8_MAX
#define SIFT_STR_NULL			UINT32_MAX

typedef struct SiftParGroupSpec
{
	uint8		nk;				/* the key columns */
	uint8		strs;			/* bit j: key j is a text, else a word */
	uint8		nsum;			/* the sums of 64-bit arguments, after the count */
	bool		key1_u32;		/* two words, and the second has no bit above 31 (a text is one): the count takes its upper half (16 or 32 byte slots) */
	uint16		topk;			/* 0: every group goes to the consumer; else the K best are kept */
	uint8		order;			/* 0: by count, 1 + i: by sum i (as int64), SIFT_PAR_GROUP_BY_KEY: by the keys alone */
	bool		desc;
} SiftParGroupSpec;

static inline bool
sift_par_group_is_str(const SiftParGroupSpec *sp, int j)
{
	return (sp->strs >> j) & 1;
}

typedef struct SiftStrRef
{
	const uint8 *p;
	uint32		len;
} SiftStrRef;

/* A group, or a partial aggregate of one: key[j] of a word key j, str[j] of a text key j */
typedef struct SiftParGroupRow
{
	uint64		key[SIFT_PAR_GROUP_MAX_KEYS];
	SiftStrRef	str[SIFT_PAR_GROUP_MAX_KEYS];
	uint64		count;
	uint64		sum[SIFT_PAR_GROUP_MAX_SUM];
} SiftParGroupRow;

/* The rows of a unit of the scan: the words of key j in key[j][0..n) or its texts in str[j][0..n), and the argument m in arg[m][0..n) */
typedef struct SiftParGroupBatch
{
	uint32		n;
	const uint64 *key[SIFT_PAR_GROUP_MAX_KEYS];
	const SiftStrRef *str[SIFT_PAR_GROUP_MAX_KEYS];
	const uint64 *arg[SIFT_PAR_GROUP_MAX_SUM];
} SiftParGroupBatch;

typedef struct SiftParGroupHooks
{
	/* Thread `worker`: the rows of the items [lo, hi), in memory of scratch that is valid until the next call; false, with err filled, when it cannot (the items are none of them folded) */
	bool		(*fill) (void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err);
	/* A group, or a partial aggregate of one, that thread `worker` held, on the thread of the run once the others are done (the row is the callee's for the call only); the consumer adds what it gets to what it has */
	void		(*emit) (void *arg, int worker, const SiftParGroupRow *row);
	/* NULL or, after the partial aggregates of everything folded, the items that are still to be done */
	void		(*overflow) (void *arg, const SiftParRange *left, uint32 nleft);
	void	   *arg;
} SiftParGroupHooks;

typedef struct SiftParGroup SiftParGroup;

/* Whether the spec is one a run can have: its columns, sums and K, and a slot of its table that is at most 8 words */
extern bool sift_par_group_spec_holds(const SiftParGroupSpec *spec);

/*
 * The state of the run: its tables and streams are in the state arenas of ar, its K best in the work arenas. high_card: every row goes to the partitions (the planner expects more than
 * SIFT_PAR_GROUP_LOW groups). rows_bound: the rows the run folds at most (a count that takes half a slot holds below 2^31). NULL, with err set, for a spec that does not hold or an arena that is
 * too small for the first tables.
 */
extern SiftParGroup *sift_par_group_create(SiftParArenas *ar, const SiftParGroupSpec *spec, bool high_card, uint64 rows_bound, const SiftParGroupHooks *hooks, SiftErr *err);

/* The operator: sift_par_run(cfg, &sift_par_group_ops, g, ar, rel) */
extern const SiftParOps sift_par_group_ops;

#endif							/* SIFT_AGG_V6_PAR_GROUP_H */
