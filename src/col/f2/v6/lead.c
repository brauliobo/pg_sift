/*
 * col/f2/v6/lead.c - the integers the leading key column holds inside the bounds of a partition or a part (f2_v6_bridge.h).
 */
#include "postgres.h"

#include "core/range.h"
#include "col/f2/v6/bridge.h"

/* The integer of the first component of a packed bound, as the column holds it (a descending column keeps the complement) */
static bool
lead_int(const uint8 *packed, unsigned flags, int64 *v)
{
	SiftBound	b;
	int64		stored;

	if (!sift_bound_unpack(packed, &b) || sift_bound_comp(&b, 0, &stored) != SIFT_COMP_VALUE)
		return false;
	*v = (flags & SIFT_KEY_DESC) ? ~stored : stored;
	return true;
}

bool
sift_f2_v6_lead_range(const uint8 *lo, const uint8 *hi, unsigned flags, int64 *min, int64 *max)
{
	int64		a,
				b;

	if (!lead_int(lo, flags, &a) || !lead_int(hi, flags, &b))
		return false;
	*min = Min(a, b);
	*max = Max(a, b);
	return true;
}
