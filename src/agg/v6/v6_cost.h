/*
 * agg/v6/v6_cost.h - the price of a SiftAggV6 scan for the planner, for a sift_col index and a sift_table alike. The constants are the planner's own (seq_page_cost, cpu_tuple_cost); no setting is
 * added.
 *
 *   cost = io_granules * col_frac * seq_page_cost + n_surv * (0.05 * cpu_tuple_cost) + n_vet * cpu_tuple_cost + groups * cpu_tuple_cost
 *
 * io_granules is the granules of the relation (SIFT_GRANULE_ROWS rows each) the quals leave, at least one, taking the selectivity of the quals as the share the marks and the zones prune;
 * col_frac is the share of the host's columns the statement reads, so a granule costs the chunks of those columns only; n_surv is the rows that pass the quals, 0.05 * cpu_tuple_cost the price of
 * a row in a vector of 2048; n_vet is the rows whose heap block the visibility map does not show all-visible, which a sift_col index asks the heap about and a sift_table, whose versions are its
 * own, never does.
 */
#ifndef SIFT_AGG_V6_COST_H
#define SIFT_AGG_V6_COST_H

#include "postgres.h"

#include "agg/v6/v6.h"

typedef struct SiftV6CostIn
{
	double		tuples;			/* the rows of the relation */
	double		sel;			/* the share of them the quals leave */
	double		allvisfrac;		/* a sift_col: the share of the heap the visibility map shows all-visible; unused for a table */
	double		groups;
	int			ncols_read;		/* the attributes of the host the statement reads: keys, aggregate arguments, quals */
	int			ncols_host;		/* the attributes of the host */
	bool		table;
} SiftV6CostIn;

extern double sift_v6_scan_cost(const SiftV6CostIn *in);

/* The number of attributes of the host the spec reads: the keys, the arguments of the aggregates, the columns of the FILTER clauses and of the quals */
extern int	sift_v6_spec_ncols_read(const SiftV6Spec *spec);

#endif							/* SIFT_AGG_V6_COST_H */
