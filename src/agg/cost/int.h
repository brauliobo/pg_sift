/*
 * agg/cost/int.h - what the files of agg/cost/ share
 * The running terms of agg_cost (CostAcc), the flags of a second pricing pass, and the
 * functions one part calls in another.
 */
#ifndef SIFT_AGG_COST_INT_H
#define SIFT_AGG_COST_INT_H

#include "agg/plan_int.h"

/* The running terms of agg_cost: the work (in operator costs) is added in this order, which fixes its rounding */
typedef struct CostAcc
{
	double		rows;
	double		elems;
	double		sel_rows;
	double		units;
	double		open_cost;
	double		scan_cost;
	double		load_cost;		/* of scan_cost, the reads of the lookup tables (the leader's alone) and their rows */
	double		load_rows;
	double		cold_ops;		/* operators of the image leaves whose dictionaries a worker inflates for itself (AGG_WIDE_COLD) */
	double		gen_cost;
	double		child_cost;
	double		zone_kept;
	double		sparse;			/* of the rows of a unit that is read, the share whose dictionary entries are mapped: the kept rows when few pass (cost_read), else all */
	double		key_share;		/* of the rows the filters keep, the share whose join key a lookup holds (join_key_zone) */
	double		agg_rows;
	double		scan_rows;
	double		rows_out;
	double		lift;			/* the factor the scan is charged for the rows of an unnest the normal plan expects (agg_cost): the workers it starts cost as much */
} CostAcc;

extern bool pricing_wide;
extern bool pricing_scratch;

extern double rel_filter_ratio(PlannerInfo *root, RelOptInfo *rel);
extern bool reads_column_index(Node *node, void *context);
extern double cte_list_keys(AggCtx *c, const JoinCtx *jc, int key);
extern double cte_list_worth(AggCtx *c, const JoinCtx *jc, int k, Var *key);

extern double parallel_divisor(int workers);
extern double zone_kept(AggCtx *c, SiftAggSpec *s, int *nkeyfilters, double *key_share);
extern double distinct_text_values(AggCtx *c, const SiftAggAgg *a, const CostAcc *k);
extern void cost_parallel(AggCtx *c, SiftAggSpec *s, CostAcc *k, Cost *startup);
extern double group_spill_cost(const SiftAggSpec *s, double groups, double ids, double rows, double *table);
extern double vet_cost(AggCtx *c, double vis, double sel_rows, double zone_kept);
extern double in_tables_cost(AggCtx *c, const SiftAggPred *p);
extern void workers_ratio(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, Path *p, double rows_out, double out_cost);

extern double num_cost(const SiftAggNum *o);
extern double pred_num_cost(const SiftAggPred *p);
extern double joined_distinct(AggCtx *c, JoinCtx *jc, int col);
extern double combo_entries(AggCtx *c, const SiftAggSpec *s, const SiftAggCombo *cb, double tuples, bool counted);
extern double child_rows(Path *p);
extern int join_extra_leaves(const SiftAggCombo *cb, const SiftAggJoin *j);
extern bool on_tests_tuple(const SiftAggSpec *s, int ci);
extern bool join_lut(AggCtx *c, const SiftAggSpec *s, const JoinCtx *jc);
extern double combo_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows, double agg_rows, double scan_rows, double kept_rows, double alive, double *entry_cost);
extern double bucket_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows);
extern double plain_credit(AggCtx *c, const SiftAggSpec *s, double agg_rows);
extern double direct_agg_cost(const SiftAggAgg *a);

extern bool point_narrows(AggCtx *c, SiftAggSpec *s);
extern double preagg_unit_ops(AggCtx *c, SiftAggSpec *s, double units, double rows, double rows_out);
extern double scan_bytes(AggCtx *c, SiftAggSpec *s, double rows);
extern double recheck_cost(AggCtx *c, SiftAggSpec *s, CostAcc *k);
extern double key_entries(AggCtx *c, const SiftAggSpec *s, const CostAcc *k, double *ids, double *ops);
extern void cost_read(AggCtx *c, SiftAggSpec *s, CostAcc *k);
extern double distinct_chunks(AggCtx *c, SiftAggAgg *a, CostAcc *k);
extern double distinct_id_ops(AggCtx *c, const SiftAggSpec *s, const CostAcc *k);

extern bool rel_has_param_qual(RelOptInfo *rel);
extern bool joins_counted(AggCtx *c);
extern void cost_joins(AggCtx *c, SiftAggSpec *s, CostAcc *k);

extern const JoinCtx * dependent_join(AggCtx *c, SiftAggSpec *s, const JoinCtx *jc, int *key);
extern double lookup_rows(AggCtx *c, const JoinCtx *jc);
extern double lookup_sure(AggCtx *c, const JoinCtx *jc);
extern bool lookup_guessed(AggCtx *c, const JoinCtx *jc);
extern bool filter_has_stats(PlannerInfo *root, RestrictInfo *ri);
extern bool lookup_counted(AggCtx *c, const JoinCtx *jc);

#endif							/* SIFT_AGG_COST_INT_H */
