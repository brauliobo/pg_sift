/*
 * agg/leaf/bound.c - lower(r) and upper(r) of a range, the values of a leaf after the source's read from the lanes of the range
 */
#include "postgres.h"

#include "agg/agg_int.h"
#include "agg/leaf_int.h"

/*
 * The leaf of a SiftAggVLeaf with bound: lower(r) or upper(r) of a range, from the lanes (col/shape.c): the position 2v of an inclusive bound or 2v + 1 of an exclusive lower one is
 * shifted back to v, 2v - 1 of an exclusive upper one is (position + 1) shifted; NULL where the range is NULL, empty or has no such bound. The lane is NULL for a bound too large for it:
 * the planner refuses the scan for a leaf that holds such rows, and a row that comes after the plan is the overflow error of any leaf.
 */
void
read_bound_leaf(AggExec *ex, int leaf, SiftSrcVec *out)
{
	const SiftAggVLeaf *vl = &ex->spec->vleaf[leaf - ex->nbase];
	const SiftSrcVec *lane = get_vec(ex, vl->obj + vl->bound);
	const int64 *pos = lane->data;
	bool		i32 = ex->lay->leaf[leaf].vtype == SCV_I32,
				upper = vl->bound == 2;
	int			nrow = ex->unit.nrow;
	size_t		esz = i32 ? 4 : 8;
	char	   *data = (char *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAllocZero(ex->ucxt, (nrow + SIFT_AGG_PAD) * esz + SIFT_SIMD_ALIGN));
	uint8	   *isnull = MemoryContextAllocZero(ex->ucxt, nrow + SIFT_AGG_PAD);

	for (int r = 0; r < nrow; r++)
	{
		int64		p = pos[r];

		if ((lane->isnull && lane->isnull[r]) || p == (upper ? PG_INT64_MAX : PG_INT64_MIN) || p == (upper ? PG_INT64_MIN : PG_INT64_MAX))
		{
			isnull[r] = 1;
			continue;
		}
		p = upper ? (p + 1) >> 1 : p >> 1;
		if (i32)
			((int32 *) data)[r] = (int32) p;
		else
			((int64 *) data)[r] = p;
	}
	memset(isnull + nrow, 1, SIFT_AGG_PAD);
	out->data = data;
	out->isnull = isnull;
	out->dict = lane->dict;
	out->n = nrow;
	out->ovf = NULL;
	out->eoff = NULL;
}
