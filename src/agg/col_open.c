/*
 * agg/col_open.c - what the source of a sift_col index opens on: the keys of a key_cut that the zone tests of an aggregate confine its scan to (the source then opens only the
 * partitions that may hold them), and the directory it hands to the participants of a parallel scan
 */
#include "postgres.h"
#include "col/am.h"
#include "agg/col_int.h"

/* The keys of the cut the zone tests confine the rows to: a leaf of the cut is the range of its test (the tests of a leaf are one entry, sets in their range) */
static bool
zone_keys(int leaf, const void *arg, int64 *lo, int64 *hi, bool *null)
{
	const ColZones *zt = arg;

	*null = false;
	for (int i = 0; i < zt->nz; i++)
		if (zt->z[i].leaf == leaf && zt->z[i].nkids == 0 && !zt->z[i].any && !zt->z[i].nulltest)
		{
			*lo = zt->z[i].lo;
			*hi = zt->z[i].hi;
			return true;
		}
	return false;
}

bool
col_key_range(Relation index, const SiftSrcPrune *z, int nz, SiftKeyRange *out)
{
	return sift_col_key_range_by(index, zone_keys, &(ColZones) {z, nz}, out);
}

Size
col_view_size(void *impl)
{
	return sift_col_view_size(((ColSource *) impl)->rd.cs);
}

void
col_view_export(void *impl, void *buf)
{
	sift_col_view_export(((ColSource *) impl)->rd.cs, buf);
}
