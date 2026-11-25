/*
 * col/f2/cost_model.h - the arithmetic of what a scan of an index of format major 2 costs, in the cost parameters of PostgreSQL itself (seq_page_cost, cpu_operator_cost, cpu_index_tuple_cost) and
 * nothing fitted to a machine: the share of the index the key marks and the time window leave, the bytes a scan reads when the columns of its filters are decoded first and the others only for the
 * rows that survive them (PREWHERE), the operators it spends on them, and what a star join adds. The scan of the index (f2_cost.c) and the aggregate path (agg/cost/cost.c) both price through here.
 *
 * Portable: only core/host.h.
 */
#ifndef SIFT_COL_F2_COST_MODEL_H
#define SIFT_COL_F2_COST_MODEL_H

#include "core/host.h"

#include <math.h>

#define SIFT_COST_GRANULE_ROWS	8192.0
#define SIFT_COST_BASE_OPS		40000.0	/* operators a scan spends before its first part: the index opened and its plan read (0.1 ms measured warm) */
#define SIFT_COST_OPEN_OPS		7000.0	/* operators a scan spends to open a part: its header, its marks and zones (40 us measured warm) */
#define SIFT_COST_FILTER_OPS	2.0		/* operators a row costs on a chunk as it is stored, a column of a filter (4 ns measured on a column of numbers no mark prunes) */
#define SIFT_COST_REST_OPS		1.0		/* ... and a column decoded after the filters */
#define SIFT_COST_ROW_OPS		10.0	/* ... and a row that passes, in index tuples (a tuple the access method returns: about 0.5 us warm, a cpu_tuple_cost of the plan above it) */

typedef struct SiftScanCostIn
{
	double		seq_page_cost;
	double		cpu_operator_cost;
	double		cpu_index_tuple_cost;
	double		block_size;
	double		total_rows;		/* the live rows of the index */
	double		total_bytes;	/* what the parts of the index take */
	double		total_granules;
	double		parts_surv;		/* the parts the time window leaves */
	double		f_prune;		/* the share of the granules the marks and the window leave */
	double		sel;			/* the share of the rows that pass every qual */
	int			n_filt;			/* the columns the filters read */
	int			n_rest;			/* the columns read for the rows that pass them */
	double		filt_share;		/* of the bytes of a row: those of the filter columns, and of the others read */
	double		rest_share;
	double		sel_lead;		/* what the most selective filter on a chunk leaves of the rows it is run on */
	double		io_reuse;		/* (0, 1]: the share of the page reads the pages of an earlier loop do not save (1 for one loop) */
	bool		rescan;			/* a probe of a nested loop: the index is open and its plan read, only the parts are opened again */
} SiftScanCostIn;

typedef struct SiftScanCost
{
	double		startup;
	double		total;
	double		bytes_read;
	double		rows_read;
} SiftScanCost;

static inline double
sift_cost_clamp(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* the share of the granules left: the least of the share of the rows of the parts the window leaves and of the share the marks of the key leave, not below a granule of the index */
static inline double
sift_f_prune(double rows_surv, double total_rows, double f_mark, double total_granules)
{
	const double floor_share = 1.0 / Max(total_granules, 1.0);
	const double window = total_rows > 0 ? rows_surv / total_rows : 1.0;

	return sift_cost_clamp(Min(window, f_mark), floor_share, 1.0);
}

static inline SiftScanCost
sift_scan_cost(const SiftScanCostIn *in)
{
	SiftScanCost out;
	const double bytes_per_granule = in->total_granules > 0 ? in->total_bytes / in->total_granules : 0;
	const double rest = in->n_rest > 0 ? in->sel_lead * in->rest_share : 0;
	const double floor_bytes = Min(in->total_bytes, Max(in->parts_surv, 1.0) * bytes_per_granule) * in->filt_share;
	double		bytes = in->total_bytes * in->f_prune * (in->filt_share + rest);
	const double rows = in->total_rows * in->f_prune;

	bytes = Max(bytes, floor_bytes);
	out.bytes_read = bytes;
	out.rows_read = Max(rows, Min(in->total_rows, SIFT_COST_GRANULE_ROWS));
	out.startup = in->cpu_operator_cost * ((in->rescan ? 0.0 : SIFT_COST_BASE_OPS) + SIFT_COST_OPEN_OPS * Max(in->parts_surv, 1.0));
	out.total = out.startup + in->seq_page_cost * (bytes / in->block_size) * in->io_reuse +
		in->cpu_operator_cost * rows * (SIFT_COST_FILTER_OPS * in->n_filt + SIFT_COST_REST_OPS * in->sel_lead * in->n_rest) +
		in->cpu_index_tuple_cost * in->total_rows * in->sel * SIFT_COST_ROW_OPS;
	return out;
}

/*
 * The fanout of the joins of a star: 1 for a dimension whose keys are unique, the average rows a key has otherwise, the rows of a range join that a value falls in (periods * the share each covers of
 * the values of the fact), and the elements a row has of an array that is unnested.
 */
static inline double
sift_star_f_join(bool unique_keys, double dim_rows, double distinct_keys, bool range, double range_overlap)
{
	if (range)
		return Max(1.0, dim_rows * sift_cost_clamp(range_overlap, 0.0, 1.0));
	if (unique_keys || distinct_keys <= 0)
		return 1.0;
	return Max(1.0, dim_rows / distinct_keys);
}

/* the operators of the scan before its first row beyond its own open: the paths of the dimensions, and a build of each row of them */
static inline double
sift_star_fixed_cost(double dims_total_cost, double dim_rows, double cpu_tuple_cost, double cpu_operator_cost)
{
	return dims_total_cost + dim_rows * (cpu_tuple_cost + cpu_operator_cost);
}

#endif							/* SIFT_COL_F2_COST_MODEL_H */
