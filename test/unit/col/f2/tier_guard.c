/*
 * test/unit/col/f2/tier_guard.c - which parts the tiering moves to the object store (col/f2/tier/tier.c): a settled part whose newest value of the time column of the slot is older than the cutoff.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "col/f2/tier/tier.c"

enum { CUTOFF = 1000 };

static SiftPartDesc
cold_part(int slot)
{
	SiftPartDesc d = {.level = 2,.flags = SIFT_PART_FLAG_ALL_VISIBLE | (slot == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1),
	.dv_blk = InvalidBlockNumber,.header_blk = 7};

	d.time_min[slot] = 10;
	d.time_max[slot] = CUTOFF - 1;
	return d;
}

static void
test_only_cold_settled_parts_are_eligible(void)
{
	for (int slot = 0; slot < 2; slot++)
	{
		SiftPartDesc d = cold_part(slot);

		SIFT_CHECKF(sift_tier_part_eligible(&d, slot, CUTOFF), "a settled part older than the cutoff of slot %d is eligible", slot);
		d.time_max[slot] = CUTOFF;
		SIFT_CHECKF(!sift_tier_part_eligible(&d, slot, CUTOFF), "a part with a row at the cutoff is not eligible on slot %d", slot);
		d = cold_part(slot);
		d.dv_blk = 3;
		SIFT_CHECKF(!sift_tier_part_eligible(&d, slot, CUTOFF), "a part with a deletion vector is not eligible on slot %d", slot);
	}
}

int
main(void)
{
	test_only_cold_settled_parts_are_eligible();
	return sift_check_summary();
}
