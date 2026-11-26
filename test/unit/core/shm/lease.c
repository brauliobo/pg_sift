/*
 * test/unit/core/shm/lease.c - the slots of the lease table of core/shm/lease.c: two backends that claim a slot for one relation file at once end with one slot of it, and the word of the page pool
 * of a slot is never the same after a filling as after the one before, however many fillings the slot has had (a pop that read the word before a drain and a filling must fail its swap). A claim
 * gives up a slot that holds no pages before one that does, a probe window of busy pools still finds a slot, a file that is gone (or the files of a database) give their slots back at once, and a
 * backend that exits while it fills a pool ends the filling.
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
static pg_on_exit_callback exit_cb;

void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
	SIFT_CHECKF(exit_cb == NULL || exit_cb == function, "one exit callback for the leases");
	exit_cb = function;
}
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

/* A slot of the window of test_loc taken by another file, live, its tick `tick` and holding a page where pages is true */
static SiftMetaLeaseSlot *
occupy(int probe, uint64 tick, bool pages)
{
	SiftMetaLeaseSlot *s = &lease_table->slot[(slot_hash(&test_loc) + probe) & (SIFT_LEASE_SLOTS - 1)];
	RelFileLocator other = {.spcOid = 1663,.dbOid = 5,.relNumber = 900000 + probe};

	slot_reset(s, &other, 1, (TimestampTz) tick);
	pg_atomic_write_u64(&s->seq, 2);
	if (pages)
		pg_atomic_write_u64(&s->pool.word, POOL_WORD(SEQ_CLAIM(2), 1, 1));
	return s;
}

static void
test_victim_keeps_pages(void)
{
	uint64		seq;
	TimestampTz now;
	SiftMetaLeaseSlot *paged,
			   *got;

	fresh_table();
	clock_now = 10 * LEASE_IDLE_US;
	now = clock_now + 1;
	paged = occupy(0, (uint64) (now - LEASE_IDLE_US - 1), true);
	for (int p = 1; p < SIFT_LEASE_PROBES; p++)
		(void) occupy(p, (uint64) now - 10, false);
	got = slot_claim(lease_table, &test_loc, &seq);
	SIFT_CHECKF(got != NULL && got != paged, "the claim gives up a slot that holds pages while the window has slots that hold none");
	SIFT_CHECKF(slot_holds_pages(paged), "the pages of the slot that was not given up stay in its pool");
}

static void
test_busy_window(void)
{
	uint64		seq;

	fresh_table();
	for (int p = 0; p < 8; p++)
		(void) occupy(p, (uint64) clock_now + 1000, true);
	SIFT_CHECKF(slot_claim(lease_table, &test_loc, &seq) != NULL, "a file finds a slot past 8 slots whose pools are busy");
}

static void
test_invalidate(void)
{
	uint64		seq;
	bool		lost;
	BlockNumber blk = 42,
				got;
	SiftMetaLeaseSlot *s;
	RelFileLocator other_db = {.spcOid = 1663,.dbOid = 6,.relNumber = 16384},
				same_db = {.spcOid = 1663,.dbOid = 5,.relNumber = 16390};

	fresh_table();
	SIFT_CHECK((s = slot_get(lease_table, &test_loc, &seq)) != NULL);
	SIFT_CHECK(sift_lease_page_room(&test_loc, &lost) && sift_lease_page_fill(&blk, 1) == 1);
	sift_lease_invalidate(&test_loc);
	SIFT_CHECKF(slot_find(lease_table, &test_loc, &seq) == NULL && !s->live, "a file that is gone has no slot");
	SIFT_CHECKF(!sift_lease_page_pop(&test_loc, &got), "and no page of its pool is handed out");

	SIFT_CHECK(slot_get(lease_table, &test_loc, &seq) != NULL && slot_get(lease_table, &same_db, &seq) != NULL && slot_get(lease_table, &other_db, &seq) != NULL);
	sift_lease_invalidate_db(5);
	SIFT_CHECKF(slots_of(&test_loc) == 0 && slots_of(&same_db) == 0, "the files of a database that is gone have no slot");
	SIFT_CHECKF(slots_of(&other_db) == 1, "the files of another database keep theirs");
}

static void
test_exit_while_filling(void)
{
	uint64		seq;
	bool		lost;
	SiftMetaLeaseSlot *s;

	fresh_table();
	SIFT_CHECK((s = slot_get(lease_table, &test_loc, &seq)) != NULL);
	SIFT_CHECK(sift_lease_page_room(&test_loc, &lost));
	SIFT_CHECKF(exit_cb != NULL, "a backend that fills a pool has an exit callback");
	if (exit_cb != NULL)
		exit_cb(1, (Datum) 0);
	SIFT_CHECKF(pg_atomic_read_u32(&s->pool.filling) == 0, "a backend that exits while it fills the pool ends the filling");
	SIFT_CHECKF(sift_lease_page_room(&test_loc, &lost), "and another backend fills it");
	(void) sift_lease_page_fill(NULL, 0);
}

int
main(void)
{
	test_victim_keeps_pages();
	test_busy_window();
	test_invalidate();
	test_exit_while_filling();
	test_claim_between();
	test_claim_race();
	test_fills_wrap();
	return sift_check_summary();
}
