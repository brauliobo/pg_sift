/*
 * agg/v6/v6_distinct_u64_test.c - test-only SQL function of the set of distinct (group, value) pairs of agg/v6/v6_distinct_u64.h, compiled only with SIFT_TESTING and registered by
 * the pg_sift_test extension: sift._test_sorts('du64g', n, mem_kb) offers the set, with mem_kb of budget, n pairs, a hundredth of them, after 160,000 in 64 groups, in a group far
 * from the others, until it says it is full, and counts them per group: the counts must add up to the pairs it took, which are returned.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "agg/v6/v6_distinct_u64.h"

PG_FUNCTION_INFO_V1(sift_test_sorts);

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
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift test sorts", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	uint64		got;

	if (strcmp(kind, "du64g") != 0)
		elog(ERROR, "sift._test_sorts: unknown sort \"%s\"", kind);
	got = n = du64g_sparse(n, mem);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	PG_RETURN_INT64((int64) got);
}

#endif							/* SIFT_TESTING */
