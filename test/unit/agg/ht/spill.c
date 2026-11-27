/*
 * agg/ht/spill.c - the spill of the group tables (agg/ht/spill.c): a cancel that comes while a partition re-spills to the next level stops the scan within a few records, before the
 * groups of another partition are merged.
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "miscadmin.h"

#include "agg/ht/spill.h"
#include "core/memtrack.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define GROUPS		1000

volatile sig_atomic_t InterruptPending = false;
static uint32 merged_after_cancel;
static bool sorted;
static SiftGroupSpillIter *iter;

void
ProcessInterrupts(void)
{
	ereport(ERROR, (errcode(ERRCODE_QUERY_CANCELED), errmsg("canceling statement due to user request")));
}

TupleDesc
CreateTemplateTupleDesc(int natts)
{
	sorted = true;
	ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("the partition reached the sort of the last level")));
}

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return parent;
}

void
sift_mem_release_os(Size freed_bytes)
{
}

static void
count_group(const SiftRadixShape *sh, SiftAggSlot64 *slot, uint8 *ext)
{
	merged_after_cancel += InterruptPending;
}

static uint64
cancel_on_check(const SiftRadixShape *sh)
{
	InterruptPending = true;
	return 0;
}

static const SiftRadixShape shape = {.ext_dist_off = SIFT_HT_NO_EXT,.init = count_group,.held = cancel_on_check};

static void
read_all(void *arg)
{
	while (sift_gspill_iter_next(iter) != NULL)
		;
}

static void
test_cancel_during_respill(void)
{
	SiftSaltedHT ht;
	SiftGroupSpill *sp;

	sift_ht_init(&ht, 64);
	for (uint64 i = 1; i <= GROUPS; i++)
	{
		const SiftInlineKey16 k = {.u64 = {i, 0}};
		bool		found;

		sift_ht_upsert(&ht, (i * UINT64CONST(0x9E3779B97F4A7C15)) >> 6, &k, &found);
	}
	sp = sift_gspill_new(&shape);
	sift_gspill_put_ht(sp, &ht);
	sift_ht_destroy(&ht);
	iter = sift_gspill_iter_begin(sp, 1, true);
	SIFT_CHECK(!pgshim_try(read_all, NULL));
	SIFT_CHECK(merged_after_cancel == 0 && !sorted);
	InterruptPending = false;
	sift_gspill_free(sp);
}

int
main(void)
{
	if (unit_wants("spill_cancel_during_respill"))
		test_cancel_during_respill();
	puts("spill: ok");
	return 0;
}
