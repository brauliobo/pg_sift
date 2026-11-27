/*
 * agg/cost/model.c - the five-input cost of a SiftAgg path (agg/cost/model.h).
 */
#include "postgres.h"

#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"

#include "agg/cost/model.h"

double
sift_agg_cold_share(double pages)
{
	pages = Max(pages, 1.0);
	return 1.0 - Max(Min(effective_cache_size / pages, 1.0), Min(NBuffers / pages, 1.0));
}

double
sift_agg_scan_ops(const SiftCostInputs *in, double selectivity, bool is_preagg, bool is_streaming)
{
	const double r_scan = in->g_eff * SIFT_GRANULE_ROWS,
				r_sel = r_scan * selectivity * in->f_join,
				decode = in->b_dec / 64.0 * in->r_bw_cpu;

	if (is_preagg)
		return decode + in->g_eff * 2.0;
	if (is_streaming || in->n_groups <= SIFT_DIRECT_MAX)
		return decode + r_sel * 0.25;
	return decode + r_sel * 0.5 * (in->n_groups * 64.0 > SIFT_LLC_EST_BYTES ? 2.5 : 1.0);
}

void
sift_agg_estimate_cost(const SiftCostInputs *in, double selectivity, bool is_preagg, bool is_streaming,
					   double fixed_cost, int n_workers, Cost *startup_out, Cost *total_out)
{
	double		work = in->b_io / BLCKSZ * seq_page_cost * in->f_cold +
		sift_agg_scan_ops(in, selectivity, is_preagg, is_streaming) * cpu_operator_cost + in->g_eff * SIFT_GRANULE_ROWS * in->qual_row,
				emit = in->n_groups * cpu_tuple_cost;

	if (n_workers > 0)
	{
		work = parallel_setup_cost + work / (1.0 + SIFT_PAR_EFFICIENCY * n_workers);
		emit = Max(in->n_groups, in->n_ship) * parallel_tuple_cost;
	}
	*total_out = fixed_cost + work + emit;
	*startup_out = fixed_cost + (is_streaming ? work / Max(in->n_groups, 1.0) : work);
}

int
sift_agg_choose_workers(const SiftCostInputs *in, double selectivity, bool is_preagg, bool is_streaming,
						double fixed_cost, int max_workers, Cost *startup_out, Cost *total_out)
{
	const int	w = max_workers;
	Cost		par_startup,
				par_total;

	sift_agg_estimate_cost(in, selectivity, is_preagg, is_streaming, 0.0, 0, startup_out, total_out);
	if (w > 0)
	{
		sift_agg_estimate_cost(in, selectivity, is_preagg, is_streaming, 0.0, w, &par_startup, &par_total);
		if (par_total < *total_out * SIFT_PAR_MIN_GAIN)
		{
			*startup_out = par_startup + fixed_cost;
			*total_out = par_total + fixed_cost;
			return w;
		}
	}
	*startup_out += fixed_cost;
	*total_out += fixed_cost;
	return 0;
}
