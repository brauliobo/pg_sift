/*
 * test/unit/table/f2/lock/ovf_find.c - a stripe whose entries are mostly in chunks (S120-12: hundreds of sessions holding thousands of rows) finds an entry by the tags of its chunks: a find
 * compares the key of about one entry, not of every entry of the chunks before it, and a remove that moves the last entry of the head chunk keeps the tag of its new place.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"

static uint64 key_compares;

#define memcmp(a, b, n) (key_compares++, __builtin_memcmp((a), (b), (n)))
#include "table/f2/lock/ovf.c"
#undef memcmp

enum { M = 2048, SI = 5 };

char	   *sift_test_dsm_fail = NULL;
sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;

void *
dsa_get_address(dsa_area *a, dsa_pointer p)
{
	return (void *) p;
}

void
dsa_free(dsa_area *a, dsa_pointer p)
{
	free((void *) p);
}

static SiftRowLockKey
key_n(uint64 n)
{
	return (SiftRowLockKey) {.locator = {.spcOid = 1663,.dbOid = 5,.relNumber = 16384},.tid = n << 16 | 1};
}

static void
add(SiftRowLockTable *t, uint64 n)
{
	SiftRowLockKey k = key_n(n);

	if (!sift_rowlock_has_room(t, SI))
		spare = (dsa_pointer) aligned_alloc(PG_CACHE_LINE_SIZE, sizeof(SiftRowLockChunk));
	sift_rowlock_add(t, SI, &k)->nholders = 1;
}

static double
compares_per_find(SiftRowLockTable *t, uint64 from, uint64 to)
{
	key_compares = 0;
	for (uint64 n = from; n < to; n += from == 0 ? 1 : 3)
	{
		SiftRowLockKey k = key_n(n);
		SiftRowLockEntry *e = sift_rowlock_find(t, SI, &k);

		SIFT_CHECKF(e != NULL && e->key.tid == k.tid, "row %llu is found", (unsigned long long) n);
	}
	return (double) key_compares / (to - from);
}

int
main(void)
{
	SiftRowLockTable *t = calloc(1, sizeof(SiftRowLockTable));
	double		per;

	area = (dsa_area *) t;
	owner = t;
	t->hdr[SI].overflow = InvalidDsaPointer;
	for (uint64 n = 0; n < M; n++)
		add(t, n);
	SIFT_CHECKF(sift_rowlock_count(t, SI).in_chunks == M - SIFT_ROWLOCK_BUCKETS, "%d entries are in chunks", M - SIFT_ROWLOCK_BUCKETS);
	per = compares_per_find(t, 0, M);
	SIFT_CHECKF(per <= 1 + M / 128.0, "a find among %d entries compares %.1f keys, not a scan of the chunks", M, per);
	for (uint64 n = 0; n < M; n += 3)
	{
		SiftRowLockKey k = key_n(n);

		sift_rowlock_remove(t, SI, sift_rowlock_find(t, SI, &k));
	}
	sift_rowlock_free_emptied();
	for (uint64 n = 0; n < M; n++)
	{
		SiftRowLockKey k = key_n(n);

		SIFT_CHECKF((sift_rowlock_find(t, SI, &k) == NULL) == (n % 3 == 0), "row %llu is found after the removes unless it was removed", (unsigned long long) n);
	}
	per = compares_per_find(t, 1, 3);
	SIFT_CHECKF(per <= 1 + M / 128.0, "a find after the removes compares %.1f keys", per);
	return sift_check_summary();
}
