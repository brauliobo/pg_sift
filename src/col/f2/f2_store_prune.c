/*
 * col/f2/f2_store_prune.c - the decisions of col/f2/f2_store_scan.h that need no server.
 */
#include "postgres.h"

#include "col/f2/f2_store_scan.h"

bool
sift_store_time_may_hold(const SiftStoreTimeWindow *w, bool bounds_valid, int64 tmin, int64 tmax, bool dirty)
{
	if (!w->use || dirty || !bounds_valid)
		return true;
	return tmin <= tmax && w->lo <= tmax && w->hi >= tmin;
}

bool
sift_store_gen_skipped(const uint64 *gens, int n, uint64 gen)
{
	int			lo = 0,
				hi = n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (gens[mid] == gen)
			return true;
		if (gens[mid] < gen)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}
