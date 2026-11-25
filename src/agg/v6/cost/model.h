/*
 * agg/v6/cost/model.h - the cost of a SiftAgg path from five inputs and the cost parameters of PostgreSQL itself (seq_page_cost,
 * cpu_operator_cost, cpu_tuple_cost, parallel_setup_cost, parallel_tuple_cost): no constant fitted to a machine, so the price follows
 * the settings of the user.
 */
#ifndef SIFT_AGG_V6_COST_MODEL_H
#define SIFT_AGG_V6_COST_MODEL_H

#include "core/host.h"

#include "nodes/nodes.h"

#define SIFT_GRANULE_ROWS		8192
#define SIFT_DIRECT_MAX			65536
#define SIFT_LLC_EST_BYTES		(16.0 * 1024.0 * 1024.0)
/* the part of a worker that is not lost to the barrier of the two-phase merge and the traffic of the shared partitions */
#define SIFT_PAR_EFFICIENCY		0.85
/* a parallel plan must be this much cheaper than the serial one: the launch of the workers and the merge of their groups cost tens of milliseconds the plan prices only in part */
#define SIFT_PAR_MIN_GAIN		0.50
/* the operators the scan spends before its first row: the source opened and its first unit decoded (0.1 ms measured warm), and for each partition it reads, the open of its own index (40 us) */
#define SIFT_OPEN_OPS			40000.0
#define SIFT_OPEN_PART_OPS		7000.0

typedef struct SiftCostInputs
{
	double		g_eff;			/* granules left by the zone maps and the primary index */
	double		b_dec;			/* bytes decoded from the columns the query reads, over those granules */
	double		b_io;			/* bytes those columns take in storage over the same granules: what a page read costs (b_dec when nothing is stored smaller) */
	double		n_groups;		/* output groups (estimate_num_groups), >= 1 */
	double		f_join;			/* fanout of the star-schema lookups, >= 1 (1: one table) */
	double		r_bw_cpu;		/* cpu_operator_costs per 64 bytes decoded (12: about 2.7 GB/s, what a warm scan of a column of numbers measures) */
	double		f_cold;			/* share of those bytes that is not in shared buffers or the cache of the operating system, and costs a page read */
	double		n_ship;			/* records the workers hand to the merge when they ship more than the groups (a count(DISTINCT) ships its values), else 0 */
} SiftCostInputs;

#define SIFT_COST_R_BW_CPU		12.0

/* The share of the pages of a relation that is neither in shared buffers nor in the cache of the operating system (effective_cache_size): what f_cold is for a scan of it */
extern double sift_agg_cold_share(double pages);

/* The operators (cpu_operator_cost units) the scan spends decoding the bytes and aggregating the rows, before the page reads and the lookups */
extern double sift_agg_scan_ops(const SiftCostInputs *in, double selectivity, bool is_preagg, bool is_streaming);
extern void sift_agg_estimate_cost(const SiftCostInputs *in, double selectivity, bool is_preagg, bool is_streaming,
								   double fixed_cost, int n_workers, Cost *startup_out, Cost *total_out);

/*
 * The number of workers (0: serial) that prices lowest, at most max_workers; startup and total are those of the choice.
 * fixed_cost is what the scan pays before its first row and no worker shortens: the open of its indexes, the plans and the lookup tables of its joins.
 */
extern int	sift_agg_choose_workers(const SiftCostInputs *in, double selectivity, bool is_preagg, bool is_streaming,
									double fixed_cost, int max_workers, Cost *startup_out, Cost *total_out);

#endif							/* SIFT_AGG_V6_COST_MODEL_H */
