/*
 * test/unit/core/shm/lease.c - the slots of the lease table of core/shm/lease.c: two backends that claim a slot for one relation file at once end with one slot of it, and the word of the page pool
 * of a slot is never the same after a filling as after the one before, however many fillings the slot has had (a pop that read the word before a drain and a filling must fail its swap).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "core/shm/lease.c"

#include "test/unit/core/shm/stub_lwlock.h"

StaticAssertDecl(offsetof(SiftMetaLeaseTable, slot) == PG_CACHE_LINE_SIZE, "the claim lock lies in the line of the tranche: the table keeps its size");

static bool claim_inside_clock;
static TimestampTz clock_now = 1000;

static const RelFileLocator test_loc = {.spcOid = 1663,.dbOid = 5,.relNumber = 16384};

/* The other backend claims the slot of the file while this one reads the clock in slot_claim, after it found no slot and before it looks at the window */
TimestampTz
GetCurrentTimestamp(void)
{
	if (claim_inside_clock)
	{
		uint64		seq;

		claim_inside_clock = false;
		SIFT_CHECKF(slot_get(lease_table, &test_loc, &seq) != NULL, "the backend that comes between claims a slot");
	}
	return __atomic_add_fetch(&clock_now, 1, __ATOMIC_RELAXED);
}

void *
sift_dsm_try(const char *name, Size size, SiftDsmInit init)
{
	return stub_segment(size, init);
}

static void
fresh_table(void)
{
	if (lease_table != NULL)
		free(lease_table);
	lease_table = NULL;
	lease_tried = false;
	SIFT_CHECK(lease_registry() != NULL);
}

static int
slots_of(const RelFileLocator *loc)
{
	int			n = 0;

	for (int i = 0; i < SIFT_LEASE_SLOTS; i++)
		n += lease_table->slot[i].live && RelFileLocatorEquals(lease_table->slot[i].locator, *loc);
	return n;
}

static void
test_claim_between(void)
{
	uint64		seq;

	fresh_table();
	claim_inside_clock = true;
	SIFT_CHECK(slot_get(lease_table, &test_loc, &seq) != NULL);
	SIFT_CHECKF(slots_of(&test_loc) == 1, "a claim that another came between leaves %d slots of the file", slots_of(&test_loc));
}

static pg_atomic_uint32 race_go;

static void
race_worker(void *arg pg_attribute_unused(), int w pg_attribute_unused())
{
	uint64		seq;

	pg_atomic_fetch_add_u32(&race_go, 1);
	while (pg_atomic_read_u32(&race_go) < 2)
		;
	(void) slot_get(lease_table, &test_loc, &seq);
}

static void
test_claim_race(void)
{
	int			worst = 0;

	for (int round = 0; round < 2000; round++)
	{
		fresh_table();
		pg_atomic_init_u32(&race_go, 0);
		unit_run_workers(2, race_worker, NULL);
		worst = Max(worst, slots_of(&test_loc));
	}
	SIFT_CHECKF(worst == 1, "two backends that claim at once leave %d slots of the file", worst);
}

static void
test_fills_wrap(void)
{
	uint64		seq,
				last = 0;
	bool		lost = false;
	SiftMetaLeaseSlot *s;
	BlockNumber blk = 77,
				got[SIFT_LEASE_PAGES];
	int			repeats = 0,
				zeros = 0;

	fresh_table();
	SIFT_CHECK((s = slot_get(lease_table, &test_loc, &seq)) != NULL);
	for (int i = 0; i < 65540; i++)
	{
		uint64		w;

		SIFT_CHECK(sift_lease_page_room(&test_loc, &lost));
		SIFT_CHECK(lost == (i == 0));
		SIFT_CHECK(sift_lease_page_fill(&blk, 1) == 1);
		w = pg_atomic_read_u64(&s->pool.word);
		repeats += w == last;
		zeros += POOL_FILLS(w) == 0;
		last = w;
		SIFT_CHECK(sift_lease_page_drain(&test_loc, got) == 1 && got[0] == blk);
	}
	SIFT_CHECKF(repeats == 0, "the word of the pool is the one of the filling before %d times in 65540 fillings", repeats);
	SIFT_CHECKF(zeros == 0, "the fillings read 0 (a pool never filled) %d times", zeros);
}

int
main(void)
{
	test_claim_between();
	test_claim_race();
	test_fills_wrap();
	return sift_check_summary();
}
