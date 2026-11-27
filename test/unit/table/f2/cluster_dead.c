/*
 * test/unit/table/f2/cluster_dead.c - the versions a VACUUM FULL or CLUSTER of a sift_table holds before it writes their deletes (table/f2/am.c, copy_for_cluster): whatever
 * maintenance_work_mem is, the array they grow to is one palloc can make (MaxAllocSize; 5 GB of maintenance_work_mem let it grow past 1 GB and the rewrite failed with
 * "invalid memory alloc request size").
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "table/f2/am.c"

static Size budget;

Size
sift_mem_budget_bytes(SiftBudgetClass cls, int nparticipants, int noperators)
{
	return budget / noperators;
}

int
main(void)
{
	static const uint64 mib[] = {1, 64, 1024, 4096, 5120, 8192, 16384, 1024 * 1024};

	for (int i = 0; i < lengthof(mib); i++)
	{
		Size		max;
		Size		grown = 64;

		budget = (Size) (mib[i] << 20);
		max = dead_versions_max();
		while (grown < max)
			grown = Min(grown * 2, max);
		SIFT_CHECKF(grown * sizeof(SiftTblDeadVersion) <= MaxAllocSize, "%llu MB: %zu versions are %zu bytes, more than palloc makes", (unsigned long long) mib[i], grown,
					grown * sizeof(SiftTblDeadVersion));
		SIFT_CHECKF(max >= Min(budget / 4, MaxAllocSize) / sizeof(SiftTblDeadVersion) && max >= 64, "%llu MB: %zu versions, the budget holds more", (unsigned long long) mib[i], max);
	}
	return sift_check_summary();
}
