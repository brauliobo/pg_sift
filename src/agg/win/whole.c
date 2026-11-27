/*
 * agg/win/whole.c - a window with no PARTITION BY over parts that are not sorted by its order: the whole input is the one partition that partsort.c sorts in memory.
 *
 * The planner takes it while the rows it holds fit the bytes partsort.c holds before it spills (a quarter of work_mem), so that a good estimate never spills; a row_number() <= K
 * holds only the best K. The sort is priced as PostgreSQL prices its own (two operator costs a compare, N log2 N compares an order key), against the Seq Scan and Sort of a WindowAgg.
 */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"
#include "optimizer/optimizer.h"

#include "agg/win/window.h"

static double
rows_held(const SiftV6WindowSpec *spec, double rows)
{
	const SiftWinTopK *t = &spec->win.topk;

	return t->active && spec->win.funcs[t->func].kind == SWIN_ROW_NUMBER ? Min(rows, 2.0 * (double) t->k + 2.0) : rows;
}

bool
sift_v6_window_whole_fits(const SiftV6WindowSpec *spec, const SiftV6Input *in, double rows)
{
	double		row_bytes = sizeof(uint64);

	for (int c = 0; c < in->ncols; c++)
		row_bytes += sift_win_ps_elem_size(in->col[c].type) + 1;
	return rows_held(spec, rows) * row_bytes <= (double) work_mem * 1024.0 / 4.0;
}

Cost
sift_v6_window_whole_cost(const SiftV6WindowSpec *spec, double rows)
{
	const double held = Max(rows_held(spec, rows), 2.0);

	return 2.0 * cpu_operator_cost * rows * log2(held) * Max(spec->win.n_ord, 1);
}
