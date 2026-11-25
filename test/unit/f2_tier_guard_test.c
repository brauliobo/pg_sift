/*
 * test/unit/f2_tier_guard_test.c - which parts the tiering moves to the object store (col/f2/tier/tier.c): a settled part older than the cutoff, unless the column delta chain of its partition
 * patched the time column of the slot, in which case the time bounds of its descriptor say nothing of the rows that read back.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/unit/pg_shim.h"
#include "test/unit/harness.h"

#include "col/f2/tier/tier.c"

enum { CUTOFF = 1000 };

static SiftPartDesc
cold_part(int slot)
{
	SiftPartDesc d = {.level = 2,.flags = SIFT_PART_FLAG_ALL_VISIBLE | (slot == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1),
	.dv_blk = InvalidBlockNumber,.col_delta_blk = InvalidBlockNumber,.header_blk = 7};

	d.time_min[slot] = 10;
	d.time_max[slot] = CUTOFF - 1;
	return d;
}

static void
test_dirty_time_is_not_eligible(void)
{
	for (int slot = 0; slot < 2; slot++)
	{
		SiftPartDesc d = cold_part(slot);

		CHECK(sift_tier_part_eligible(&d, slot, CUTOFF, false), "a settled part older than the cutoff of slot %d is eligible", slot);
		CHECK(!sift_tier_part_eligible(&d, slot, CUTOFF, true), "a part whose time bounds the column delta chain patched is not eligible on slot %d", slot);
		d.time_max[slot] = CUTOFF;
		CHECK(!sift_tier_part_eligible(&d, slot, CUTOFF, false), "a part with a row at the cutoff is not eligible on slot %d", slot);
	}
}

int
main(void)
{
	test_dirty_time_is_not_eligible();
	printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures != 0;
}
