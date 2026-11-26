/*
 * test/unit/table/f2/vacuum_tids.c - the array of dead row addresses a VACUUM of a sift_table collects before it cleans the indexes (table/f2/vacuum.c): whatever maintenance_work_mem is,
 * the array it grows to is one palloc can make (MaxAllocSize; 2 GB of maintenance_work_mem made it 2 GB and the VACUUM failed with "invalid memory alloc request size").
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"
#include "access/transam.h"
#include "utils/rel.h"

extern FullTransactionId sift_standby_safe_horizon(Relation index, bool urgent_freeze);

#include "table/f2/vacuum.c"

static Size budget;

Size
sift_mem_budget_bytes(SiftBudgetClass cls, int nparticipants, int noperators)
{
	return budget;
}

int
main(void)
{
	static const uint64 mib[] = {1, 64, 1024, 2047, 2048, 4096, 16384, 1024 * 1024};

	for (int i = 0; i < lengthof(mib); i++)
	{
		uint32		max;

		budget = (Size) (mib[i] << 20);
		max = vc_max_tids();
		SIFT_CHECKF((Size) max * sizeof(uint64) <= MaxAllocSize, "%llu MB: %u addresses are %llu bytes, more than palloc makes", (unsigned long long) mib[i], max,
					(unsigned long long) ((Size) max * sizeof(uint64)));
		SIFT_CHECKF(max >= Min(budget / 2 / sizeof(uint64), MaxAllocSize / sizeof(uint64)) && max >= 1024, "%llu MB: %u addresses, the budget holds more", (unsigned long long) mib[i], max);
	}
	return sift_check_summary();
}
