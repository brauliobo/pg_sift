/*
 * agg/cost/cost.h - the price of a SiftAggV6 scan for the planner, for a sift_col index and a sift_table alike. The constants are the planner's own (seq_page_cost, cpu_tuple_cost); no setting is
 * added.
 *
 * A sift_table has no statistics of its own, so the granules its quals leave come from the statistics of PostgreSQL (v6_card.h): the bytes of the columns the statement reads are decoded for the rows
 * of those granules, and the price is that of the aggregate scan of a sift_col (agg/cost/model.h), with the open of the table before the first row.
 */
#ifndef SIFT_AGG_V6_COST_H
#define SIFT_AGG_V6_COST_H

#include "postgres.h"

#include "agg/engine.h"

typedef struct SiftV6CostIn
{
	double		tuples;			/* the rows of the relation */
	double		rows;			/* the rows that pass its quals */
	double		pages;			/* of the relation */
	double		f_prune;		/* the share of the granules the quals leave */
	double		groups;
	int			ncols_read;		/* the attributes of the host the statement reads: keys, aggregate arguments, quals */
} SiftV6CostIn;

extern void sift_v6_scan_cost(const SiftV6CostIn *in, Cost *startup, Cost *total);

/* The number of attributes of the host the spec reads: the keys, the arguments of the aggregates, the columns of the FILTER clauses and of the quals */
extern int	sift_v6_spec_ncols_read(const SiftV6Spec *spec);

/* The selectivity of conditions a scan prices: the planner's own for a RestrictInfo it estimated, the scan's own estimate for any other (qual_selectivity_estimated counts those) */
extern Selectivity sift_v6_quals_sel(PlannerInfo *root, List *quals, int var_relid);

#endif							/* SIFT_AGG_V6_COST_H */
