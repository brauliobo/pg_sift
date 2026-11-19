/*
 * agg/col_miss.c - the shares of the units of a column index that lack a pre-aggregate section (SiftSrcProvider.miss), read when a plan prices an answer from them
 */
#include "postgres.h"

#if __has_include("col/col.h")

#include "col/col.h"
#include "col/pg.h"

#include "agg/col_int.h"

static double
miss_share(double have, double units)
{
	return units > 0 ? 1.0 - Min(have / units, 1.0) : 0.0;
}

void
sift_col_miss(Relation index, SiftSrcStats *st)
{
	const SiftColPreStats *pre = sift_col_pre_stats(index);

	st->miss_pending = false;
	st->rows_miss = miss_share(pre->rows, pre->units);
	for (int i = 0; i < sift_col_index_defs(index)->ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
	{
		st->dim_miss[i] = miss_share(pre->dim[i], pre->units);
		st->mm_miss[i] = miss_share(pre->mm[i], pre->units);
		st->roll_miss[i] = miss_share(pre->roll[i], pre->units);
	}
}

#endif							/* __has_include("col/col.h") */
