/*
 * agg/sorts_test.c - test-only SQL function of the external sorts of the aggregation, compiled only with SIFT_TESTING and registered by the pg_sift_test extension:
 *   sift._test_sorts(kind, n, mem_kb)   puts n distinct pairs (each of a fifth of them twice) in an order that no run holds in order into the sort of kind 'pair' (agg/dist/psort.c, count(DISTINCT)) or
 *                                       'rows' (agg/rows_psort.h), with mem_kb of memory for it, and reads them back: they must come out in order, each once. Returns the pairs read.
 *                                       Kind 'du64g': the set of (group, value) pairs of agg/v6/v6_distinct_u64.h with mem_kb of budget is offered n pairs, a hundredth of them, after 160,000 in
 *                                       64 groups, in a group far from the others, until it says it is full, and counted per group: the counts must add up to the pairs it took, which are returned.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "agg/dist_int.h"
#include "agg/rows_psort.h"
#include "agg/v6/v6_distinct_u64.h"

PG_FUNCTION_INFO_V1(sift_test_sorts);

#define TEST_GROUPS		1000

/* a bijection of the 32 bit integers: the value of pair i, so that every pair of the n is distinct and the n are in no order */
static inline uint32
value_of(uint64 i)
{
	return (uint32) (i * UINT64CONST(2654435761));
}

static inline uint32
group_of(uint64 i)
{
	return (uint32) (i % TEST_GROUPS);
}

static uint64
pair_sort(uint64 n, uint64 mem_bytes)
{
	PairSort   *s = psort_begin(CurrentMemoryContext, mem_bytes);
	uint64		x,
				prev = 0,
				got = 0;

	for (uint64 i = 0; i < n; i++)
		psort_put(s, (uint64) group_of(i) << 32 | value_of(i));
	for (uint64 i = 0; i < n; i += 5)
		psort_put(s, (uint64) group_of(i) << 32 | value_of(i));
	psort_finish(s);
	psort_rewind(s);
	while (psort_next(s, &x))
	{
		if (got > 0 && x <= prev)
			elog(ERROR, "the pairs of a count(DISTINCT) sort came out of order at %llu", (unsigned long long) got);
		prev = x;
		got++;
	}
	psort_end(s);
	return got;
}

static uint64
rows_sort(uint64 n, uint64 mem_bytes)
{
	RowsPSort  *s = rows_psort_begin(CurrentMemoryContext, mem_bytes);
	RowsPair	p,
				prev = {0};
	uint64		got = 0;

	for (uint64 i = 0; i < n; i++)
		rows_psort_put(s, group_of(i), (int64) value_of(i), 1);
	for (uint64 i = 0; i < n; i += 5)
		rows_psort_put(s, group_of(i), (int64) value_of(i), 2);
	rows_psort_finish(s);
	while (rows_psort_next(s, &p))
	{
		if (got > 0 && !(p.g > prev.g || (p.g == prev.g && p.v > prev.v)))
			elog(ERROR, "the pairs of a distinct sort of rows came out of order at %llu", (unsigned long long) got);
		prev = p;
		got++;
	}
	rows_psort_end(s);
	return got;
}

#define DU_BATCH		512
#define DU_GID_STRIDE	4096
#define DU_DENSE_PAIRS	160000		/* pairs in the first 64 groups before the far ones: the table of the set is full of them and has gone to buckets */

static void
du_count(void *arg, uint32 gid, uint64 count)
{
	*(uint64 *) arg += count;
}

static uint64
du64g_sparse(uint64 n, uint64 mem_bytes)
{
	SiftDu64G  *d = sift_du64g_create(CurrentMemoryContext, mem_bytes);
	uint32		gids[DU_BATCH];
	uint64		vals[DU_BATCH];
	uint64		took = 0,
				counted = 0;

	for (uint64 base = 0; base < n; base += DU_BATCH)
	{
		const uint32 m = (uint32) Min((uint64) DU_BATCH, n - base);
		uint32		done;

		for (uint32 j = 0; j < m; j++)
		{
			const uint64 i = base + j;

			gids[j] = i >= DU_DENSE_PAIRS && i % 100 == 0 ? (uint32) ((i / 100 + 1) * DU_GID_STRIDE) : (uint32) (i % 64);
			vals[j] = i;
		}
		done = sift_du64g_add_batch(d, gids, vals, m);
		took += done;
		if (done < m)
			break;
	}
	sift_du64g_groups(d, du_count, &counted);
	sift_du64g_free(d);
	if (counted != took)
		elog(ERROR, "the groups of the set of distinct pairs hold %llu pairs, it took %llu", (unsigned long long) counted, (unsigned long long) took);
	return took;
}

Datum
sift_test_sorts(PG_FUNCTION_ARGS)
{
	char	   *kind = text_to_cstring(PG_GETARG_TEXT_PP(0));
	uint64		n = (uint64) PG_GETARG_INT64(1);
	const uint64 mem = (uint64) PG_GETARG_INT32(2) * 1024;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift test sorts", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	uint64		got;

	if (strcmp(kind, "pair") == 0)
		got = pair_sort(n, mem);
	else if (strcmp(kind, "rows") == 0)
		got = rows_sort(n, mem);
	else if (strcmp(kind, "du64g") == 0)
		got = n = du64g_sparse(n, mem);
	else
		elog(ERROR, "sift._test_sorts: unknown sort \"%s\"", kind);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	if (got != n)
		elog(ERROR, "the sort of kind \"%s\" gave %llu pairs, expected %llu", kind, (unsigned long long) got, (unsigned long long) n);
	PG_RETURN_INT64((int64) got);
}

#endif							/* SIFT_TESTING */
