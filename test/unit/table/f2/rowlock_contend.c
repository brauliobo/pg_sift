/*
 * test/unit/table/f2/rowlock_contend.c - the stripes of the row locks of a sift_table (table/f2/lock/ovf.c) and the registry of the keys of UNIQUE sift_col indexes (col/f2/uniq/shm.h) do not share a
 * cache line between two stripes: the header of a stripe (lock, counts, overflow, tags) is one line, writers of disjoint stripes scale across threads, the tags find an entry among 64 by one line,
 * and a key that another entry shares the tag with is told apart by its bytes.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

char       *sift_test_dsm_fail = NULL;

#include "table/f2/lock/ovf.c"
#include "col/f2/uniq/shm.c"

enum { THREADS = 16, ROUNDS = 40000, KEYS = 48, LEGACY_STRIPES = 128 };

StaticAssertDecl(sizeof(SiftRowLockStripeHdr) == PG_CACHE_LINE_SIZE, "the header of a stripe is one cache line");
StaticAssertDecl(offsetof(SiftRowLockTable, hdr) % PG_CACHE_LINE_SIZE == 0, "the headers start on a line");
StaticAssertDecl(offsetof(SiftRowLockTable, area_lock) % PG_CACHE_LINE_SIZE == 0, "the lock of the area has a line of its own");
StaticAssertDecl(offsetof(SiftRowLockTable, area_handle) % PG_CACHE_LINE_SIZE == 0, "the handle of the area has a line of its own");
StaticAssertDecl(sizeof(((SiftRowLockStripeHdr *) 0)->tags) == SIFT_ROWLOCK_BUCKETS, "a tag for each entry of the table of a stripe");
StaticAssertDecl(SIFT_ROWLOCK_PARTITIONS == 256, "256 stripes");

typedef struct LegacyTable
{
	uint16		stripe_n[LEGACY_STRIPES];
	dsa_pointer overflow[LEGACY_STRIPES];
} LegacyTable;

static SiftRowLockTable *tbl;
static LegacyTable *legacy;

static SiftRowLockKey
key_n(uint64 n)
{
	SiftRowLockKey k = {.locator = {.spcOid = 1663,.dbOid = 5,.relNumber = 16384},.tid = n << 16 | 1};

	return k;
}

static void
fill(int si, int n)
{
	for (int i = 0; i < n; i++)
	{
		SiftRowLockKey k = key_n((uint64) si * 1000 + i);
		dsa_pointer spare = InvalidDsaPointer;
		SiftRowLockEntry *e = sift_rowlock_add(tbl, si, &k, &spare);

		e->nholders = 1;
	}
}

static void
drain(int si, int n)
{
	for (int i = 0; i < n; i++)
	{
		SiftRowLockKey k = key_n((uint64) si * 1000 + i);

		sift_rowlock_remove(tbl, si, sift_rowlock_find(tbl, si, &k));
	}
}

static void
stripe_worker(void *arg pg_attribute_unused(), int w)
{
	int			si = w * (SIFT_ROWLOCK_PARTITIONS / THREADS);

	for (int r = 0; r < ROUNDS / KEYS; r++)
	{
		fill(si, KEYS);
		for (int i = 0; i < KEYS; i++)
		{
			SiftRowLockKey k = key_n((uint64) si * 1000 + i);

			if (sift_rowlock_find(tbl, si, &k) == NULL)
				abort();
		}
		drain(si, KEYS);
	}
}

static void
legacy_worker(void *arg pg_attribute_unused(), int w)
{
	volatile LegacyTable *t = legacy;
	int			si = w * (LEGACY_STRIPES / THREADS);

	for (int r = 0; r < ROUNDS * 100; r++)
	{
		t->stripe_n[si] = (uint16) (t->stripe_n[si] + 1);
		if (t->overflow[si] != InvalidDsaPointer)
			abort();
		t->stripe_n[si] = (uint16) (t->stripe_n[si] - 1);
	}
}

static double
timed(int threads, void (*fn) (void *, int))
{
	double		best = 1e30;

	for (int rep = 0; rep < 5; rep++)
	{
		double		t0 = now_s();

		unit_run_workers(threads, fn, NULL);
		best = Min(best, now_s() - t0);
	}
	return best;
}

static double
speedup(void (*fn) (void *, int))
{
	double		one = timed(1, fn),
				many = timed(THREADS, fn);

	return THREADS * one / many;
}

static void
test_tags_find_among_a_full_table(void)
{
	SiftRowLockKey k;
	SiftRowLockEntry *e;
	uint64		twin = 0;

	fill(3, SIFT_ROWLOCK_BUCKETS);
	SIFT_CHECKF(pg_atomic_read_u32(&tbl->hdr[3].total_n) == SIFT_ROWLOCK_BUCKETS && tbl->hdr[3].table_n == SIFT_ROWLOCK_BUCKETS,
				"a full table counts %d entries", SIFT_ROWLOCK_BUCKETS);
	for (int i = 0; i < SIFT_ROWLOCK_BUCKETS; i++)
	{
		k = key_n(3000 + i);
		e = sift_rowlock_find(tbl, 3, &k);
		SIFT_CHECKF(e != NULL && memcmp(&e->key, &k, sizeof(k)) == 0, "entry %d is found by its key", i);
	}
	k = key_n(999999);
	SIFT_CHECKF(sift_rowlock_find(tbl, 3, &k) == NULL, "a key that is not there is not found");
	for (uint64 n = 1000000; twin == 0 && n < 1100000; n++)
	{
		k = key_n(n);
		for (int i = 0; i < SIFT_ROWLOCK_BUCKETS; i++)
			if (tbl->hdr[3].tags[i] == sift_rowlock_tag(&k))
				twin = n;
	}
	SIFT_CHECKF(twin != 0, "a key whose tag an entry of the stripe has was found");
	k = key_n(twin);
	SIFT_CHECKF(sift_rowlock_find(tbl, 3, &k) == NULL, "a key that shares the tag of an entry is told apart by its bytes");
	drain(3, SIFT_ROWLOCK_BUCKETS);
	SIFT_CHECKF(pg_atomic_read_u32(&tbl->hdr[3].total_n) == 0 && tbl->hdr[3].table_n == 0, "a drained stripe counts none");
}

static void
test_remove_moves_the_tag_with_the_entry(void)
{
	SiftRowLockKey first = key_n(5000),
				last = key_n(5009);

	fill(5, 10);
	sift_rowlock_remove(tbl, 5, sift_rowlock_find(tbl, 5, &first));
	SIFT_CHECKF(sift_rowlock_find(tbl, 5, &first) == NULL, "a removed key is gone");
	SIFT_CHECKF(sift_rowlock_find(tbl, 5, &last) != NULL, "the entry that took its place is found by its own tag");
	SIFT_CHECKF(pg_atomic_read_u32(&tbl->hdr[5].total_n) == 9, "the count follows the remove");
	for (int i = 1; i < 10; i++)
	{
		SiftRowLockKey k = key_n(5000 + i);

		sift_rowlock_remove(tbl, 5, sift_rowlock_find(tbl, 5, &k));
	}
	SIFT_CHECKF(pg_atomic_read_u32(&tbl->hdr[5].total_n) == 0, "the stripe is empty again");
}

static void
test_reuse_of_a_slot_takes_the_tag_of_its_new_key(void)
{
	SiftRowLockKey old = key_n(7000),
				fresh = key_n(7777);
	SiftRowLockEntry *e;

	fill(7, 4);
	e = sift_rowlock_find(tbl, 7, &old);
	sift_rowlock_reset(tbl, 7, e, &fresh);
	SIFT_CHECKF(sift_rowlock_find(tbl, 7, &old) == NULL, "the old key of a reused slot is gone");
	SIFT_CHECKF(sift_rowlock_find(tbl, 7, &fresh) == e, "the new key of a reused slot is found at it");
	sift_rowlock_remove(tbl, 7, e);
	for (int i = 1; i < 4; i++)
	{
		SiftRowLockKey k = key_n(7000 + i);

		sift_rowlock_remove(tbl, 7, sift_rowlock_find(tbl, 7, &k));
	}
}

static void
test_uniq_buckets_of_a_stripe_are_a_run_of_whole_lines(void)
{
	int			per_stripe[SIFT_UNIQ_SHM_STRIPES] = {0};
	bool		seen[SIFT_UNIQ_SHM_BUCKETS] = {false};
	uint32		h = 12345;

	StaticAssertStmt(SIFT_UNIQ_SHM_STRIPES == 256, "256 stripes of the registry");
	for (uint32 i = 0; i < SIFT_UNIQ_SHM_BUCKETS; i++)
	{
		uint32		b = bucket_of(i);

		SIFT_CHECKF(b < SIFT_UNIQ_SHM_BUCKETS && !seen[b], "the 65536 low hashes take 65536 different buckets (hash %u)", i);
		seen[b % SIFT_UNIQ_SHM_BUCKETS] = true;
		per_stripe[b / BUCKETS_PER_STRIPE]++;
	}
	for (int s = 0; s < SIFT_UNIQ_SHM_STRIPES; s++)
		SIFT_CHECKF(per_stripe[s] == BUCKETS_PER_STRIPE, "stripe %d has its %d buckets", s, BUCKETS_PER_STRIPE);
	for (int i = 0; i < 100000; i++)
	{
		h = h * 2654435761u + 40503u;
		SIFT_CHECKF(bucket_of(h) / BUCKETS_PER_STRIPE == (h & (SIFT_UNIQ_SHM_STRIPES - 1)), "the bucket of hash %u is in the run of its stripe", h);
	}
}

static void
test_disjoint_stripes_scale(void)
{
	double		legacy_up = speedup(legacy_worker),
				up = speedup(stripe_worker);
	bool		sanitized = false;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	sanitized = true;
#endif
	printf("legacy packed counters: %.1fx on %d threads; striped headers: %.1fx\n", legacy_up, THREADS, up);
	SIFT_CHECKF(sanitized || up >= 3.0, "disjoint stripes scale at least 3x on %d threads, not %.1fx", THREADS, up);
}

int
main(void)
{
	tbl = calloc(1, sizeof(SiftRowLockTable) + PG_CACHE_LINE_SIZE);
	tbl = (SiftRowLockTable *) TYPEALIGN(PG_CACHE_LINE_SIZE, tbl);
	legacy = calloc(1, sizeof(LegacyTable));
	test_tags_find_among_a_full_table();
	test_remove_moves_the_tag_with_the_entry();
	test_reuse_of_a_slot_takes_the_tag_of_its_new_key();
	test_uniq_buckets_of_a_stripe_are_a_run_of_whole_lines();
	test_disjoint_stripes_scale();
	return sift_check_summary();
}
