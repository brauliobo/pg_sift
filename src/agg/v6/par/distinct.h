/*
 * agg/v6/par/distinct.h - COUNT(DISTINCT) for W threads, as an operator of core/par/morsel.h, in pure C (arenas of the leader, no server call; see agg/v6/par/group.h for the rules of the threads).
 *
 * The statement is the groups of a key (words of integers and at most one text, or none: one set), the rows of each group, the sums of some integer columns of its rows, and the number of the distinct
 * values of one column of it, an integer or a text. Phase 1: each thread numbers the keys it sees (agg/v6/par/keytab.h: the bytes of a text are copied, the granule goes away) and adds the rows and
 * sums of a row to its key; the values (the bytes of a text, numbered the same way) go to the partitions of core/par/radix.h as records of the number of the key and the value, by the hash of the
 * key, so that a key is in one partition and its count is whole. The seal puts the rows and sums of each key of each thread into the partition of its key. Phase 2: each thread takes whole partitions
 * from the shared cursor; the keys of a partition are numbered again across the threads (bytes compared, never the hash alone: a text of one thread is the same as a text of another when the bytes
 * are, whatever the codes of their dictionaries), the values deduplicated in a table that fits the L2 cache, and each key goes with its rows, sums and distinct values to the answer of the thread.
 * With no key the one set is spread over the partitions by the hash of the value, and the rows and sums are those of the threads, added.
 */
#ifndef SIFT_AGG_V6_PAR_DISTINCT_H
#define SIFT_AGG_V6_PAR_DISTINCT_H

#include "postgres.h"

#include "agg/v6/par/group.h"
#include "core/par/morsel.h"

typedef struct SiftParDistinctSpec
{
	uint8		kw;				/* the words of the key (0 with no text: no key) */
	bool		kstr;			/* and a text key */
	uint8		nsum;			/* the sums of 64-bit arguments, of the rows of the group */
	bool		vstr;			/* the counted value is a text (else a 64-bit word) */
} SiftParDistinctSpec;

/* The rows of a unit of the scan: the key words of column j in key[j][0..n), the text keys in kstr[0..n), the sum arguments m in sum[m][0..n), and the counted values */
typedef struct SiftParDistinctBatch
{
	uint32		n;
	const uint64 *key[SIFT_PAR_GROUP_MAX_KEYS];
	const SiftStrRef *kstr;
	const uint64 *sum[SIFT_PAR_GROUP_MAX_SUM];
	const uint64 *val;			/* a word value: vnull[i] says it is NULL (vnull may be NULL: none is) */
	const uint8 *vnull;
	const SiftStrRef *vstr;		/* a text value: the length SIFT_STR_NULL is a NULL */
} SiftParDistinctBatch;

typedef struct SiftParDistinctHooks
{
	/* Thread `worker`: the rows of the items [lo, hi), in memory of scratch that is valid until the next call; false, with err filled, when it cannot (the run is then not an answer) */
	bool		(*fill) (void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParDistinctBatch *out, SiftErr *err);
	/* After SIFT_ERR_WORK_MEM: the run is no answer, and the items that are not done are for the serial path */
	void		(*overflow) (void *arg, const SiftParRange *left, uint32 nleft);
	void	   *arg;
} SiftParDistinctHooks;

typedef struct SiftParDistinct SiftParDistinct;

/* NULL, with err set, when an arena is too small for the first tables */
extern SiftParDistinct *sift_par_distinct_create(SiftParArenas *ar, const SiftParDistinctSpec *spec, const SiftParDistinctHooks *hooks, SiftErr *err);

/* The operator: sift_par_run(cfg, &sift_par_distinct_ops, d, ar, rel) */
extern const SiftParOps sift_par_distinct_ops;

/* A group of the answer: its key, its rows (a row with a NULL value is one), the sums of them, and the distinct values of its rows (NULL not counted) */
typedef struct SiftParDistinctRow
{
	uint64		key[SIFT_PAR_GROUP_MAX_KEYS];
	const uint8 *str;
	uint32		len;
	uint64		rows;
	uint64		sum[SIFT_PAR_GROUP_MAX_SUM];
	uint64		distinct;
} SiftParDistinctRow;

/* After a run that is an answer: every group once (with no key, one group, also with no rows) */
extern void sift_par_distinct_each(const SiftParDistinct *d, void (*fn) (void *arg, const SiftParDistinctRow *row), void *arg);

/* The OR of the nbits bitsets (nwords words each) over the words of thread t of nthreads, into out (when not NULL); returns the bits set in them */
extern uint64 sift_par_distinct_bits_or(const uint64 *const *bits, int nbits, uint32 nwords, int t, int nthreads, uint64 *out);

#endif							/* SIFT_AGG_V6_PAR_DISTINCT_H */
