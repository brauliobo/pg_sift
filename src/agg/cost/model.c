/*
 * agg/cost/model.c - the cost model of the scan
 * The entry points (agg_cost, agg_floor_cost, agg_open_cost) and the terms every part adds
 * to: elements of an aggregate, distinct counts, tuples and work. The parts are the files next to it.
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/syscache.h"

#include "agg/cost/int.h"

double
elems_of(AggCtx *c, int grp)
{
	return c->stats.elems_per_row[grp] > 0 ? c->stats.elems_per_row[grp] : AGG_DEFAULT_ELEMS;
}

/*
 * The distinct values of a column under the table's own conditions on it: the values of the table (PostgreSQL's statistics) times the
 * share the conditions keep (a range of dates keeps the dates of the range). 0 when there are no statistics.
 */
double
column_distinct(AggCtx *c, Var *v)
{
	List	   *clauses = NIL;
	HeapTuple	tup;
	double		nd;

	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid), Int16GetDatum(v->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return 0.0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	ReleaseSysCache(tup);
	nd = nd > 0 ? nd : -nd * c->rel->tuples;
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
	{
		Bitmapset  *atts = NULL;

		pull_varattnos((Node *) ri->clause, c->relid, &atts);
		if (bms_membership(atts) == BMS_SINGLETON && bms_is_member(v->varattno - FirstLowInvalidHeapAttributeNumber, atts))
			clauses = lappend(clauses, ri);
	}
	return nd * (clauses == NIL ? 1.0 : clauselist_selectivity(c->root, clauses, 0, JOIN_INNER, NULL));
}

/* the distinct values of column col of a joined table by PostgreSQL's statistics, its rows when there are none */
double
joined_distinct(AggCtx *c, JoinCtx *jc, int col)
{
	RelOptInfo *rel = c->root->simple_rel_array[jc->rti];
	HeapTuple	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(jc->rti, c->root)->relid),
									  Int16GetDatum(jc->j.colatt[col]), BoolGetDatum(false));
	double		nd = 0;

	if (HeapTupleIsValid(tup))
	{
		nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
		ReleaseSysCache(tup);
		nd = nd > 0 ? nd : -nd * rel->tuples;
	}
	return nd > 0 ? Min(nd, Max(rel->rows, 1.0)) : Max(rel->rows, 1.0);
}

/* agg_cost prices a path again for the tuples its lookup joins would not filter (workers_ratio): nothing it reports comes from that pass */
bool		pricing_wide,
			pricing_scratch;

/*
 * How many times derived_cte_rows raised the rows of a CTE scan in the plan being made (the largest ratio): plan_variants takes it after the
 * shared plan and passes it on, so what is left here after an error or a nested plan is not read.
 */
double		agg_cte_lift = 1.0;

/* the three lifts of rows (derived_rel_rows, key_list_lift, derived_cte_rows) are off while the plan that is the fallback is made (plan_shared), and say when one was made */
bool		agg_stock_rows;
bool		agg_rows_lifted;

/* The rows read and tested, the zones kept and the units opened */

/* The tuples that reach the groups and what each takes */
static void
cost_tuples(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, CostAcc *k)
{
	k->agg_rows = k->sel_rows * (s->group >= 0 ? k->elems : 1.0);
	/* the lookup joins reach the groups with the rows PostgreSQL expects of the join: fewer where they filter, more where a key of the table repeats (an unnest has its own count, tuple_group_count) */
	if (c->joins != NIL && !pricing_wide)
		k->agg_rows = s->group < 0 && joins_counted(c) ? Max(input_rel->rows, 1.0) : Min(k->agg_rows, Max(input_rel->rows, 1.0));
	else if (pricing_wide)
		k->agg_rows *= sqrt(Max(Min(k->zone_kept, 1.0), 1.0 / k->units));
	k->scan_rows = k->agg_rows;
	/* the rows of a preserved side no tuple matches are tuples too */
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind))
			k->agg_rows += Max(c->root->simple_rel_array[jc->rti]->tuples, 1.0);
	/* a group takes a tuple: the groups PostgreSQL expects of the unnested rows it assumes (a cap of 100 a call) are more than the elements hold */
	k->rows_out = Min(k->rows_out, Max(Max(s->nsets, 1) * k->agg_rows, 1.0));
	k->scan_cost += k->agg_rows * (Max(s->nsets, 1) * (AGG_TUPLE + s->nkeys * AGG_KEY_ROW + s->naggs * AGG_AGG_ROW) + s->ncombo * AGG_COMBO_ROW);
}

/* The aggregates, combos, keys and predicates of every tuple, less what the plain path saves, plus the bytes read */
static void
cost_work(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	double		chunks = 0,
				entry_cost;

	for (int i = 0; i < s->naggs; i++)
	{
		if (s->agg[i].kind == SAA_COUNT_DISTINCT)
			chunks += distinct_chunks(c, &s->agg[i], k);
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			k->scan_cost += k->agg_rows * AGG_ORDERED_ROW;
		k->scan_cost += k->agg_rows * (num_cost(&s->agg[i].num) + direct_agg_cost(&s->agg[i])) + k->rows * pred_num_cost(s->agg[i].filter);
		if (s->agg[i].kind == SAA_GEN)
			k->gen_cost += k->agg_rows * s->agg[i].gen->cost + k->rows_out * (s->agg[i].gen->ordered || s->agg[i].gen->sorted || s->agg[i].gen->hyp ? AGG_GEN_GROUP : AGG_GEN_STATE);
	}
	for (int a = 0; a < s->narms; a++)
		k->scan_cost += k->agg_rows * (s->arm[a].hmask ? AGG_ARM_HIDDEN : AGG_ARM_TUPLE);
	for (int ci = 0; ci < s->ncombo; ci++)
		if (s->combo[ci].join >= 0 && ((JoinCtx *) list_nth(c->joins, s->combo[ci].join))->j.params == NIL)
		{
			JoinCtx    *jc = list_nth(c->joins, s->combo[ci].join);
			double		past = c->root->simple_rte_array[jc->rti]->rtekind == RTE_RELATION ? c->root->simple_rel_array[jc->rti]->rows - AGG_FAN_CACHE : 0.0;

			k->scan_cost += Max(k->agg_rows - k->sel_rows * k->elems, 0.0) * (AGG_FAN_ROW + s->combo[ci].nslots * AGG_FAN_SLOT) * Min(sqrt(Max(past, 0.0) / AGG_FAN_CACHE), AGG_FAN_CAP);
		}
	k->scan_cost += combo_eval_cost(c, s, k->rows, k->agg_rows, k->scan_rows, k->rows * Max(k->zone_kept, 1.0 / k->units), k->sel_rows * k->elems * k->key_share, &entry_cost) + bucket_eval_cost(c, s, k->agg_rows) + distinct_id_ops(c, s, k);
	/* each process evaluates the entries it meets: the workers divide the probes, not those (cost_parallel divides the cold operators with the rest) */
	k->cold_ops += entry_cost * (parallel_divisor(s->nworkers) - 1.0);
	for (int i = 0; i < s->nkeys; i++)
		k->scan_cost += k->agg_rows * num_cost(&s->key[i].num);
	for (int i = 0; i < s->npred; i++)
		k->scan_cost += k->rows * pred_num_cost(s->pred[i]);
	k->scan_cost = Max(k->scan_cost - plain_credit(c, s, k->agg_rows), k->scan_cost * 0.1);
	/* after the credit, which prices what the normal plan under-estimates: the rows read again from the heap are work it does not do */
	k->scan_cost += recheck_cost(c, s, k) * Max(k->zone_kept, 1.0 / k->units);
	k->scan_cost += chunks * AGG_DIST_CHUNK;
	k->scan_cost += scan_bytes(c, s, k->rows) * AGG_BYTE * Max(k->zone_kept, 1.0 / k->units);
}

/* The least any path of the scan costs in total: the start of a plain aggregate (the cheapest start there is). A normal plan under half of it has no scan to lose to. */
Cost
agg_floor_cost(void)
{
	return AGG_START_PLAIN * cpu_operator_cost;
}

/* What every path of the scan pays to start, whatever it reads: its source and the partitions it opens. No path costs less in total. */
Cost
agg_open_cost(int nparts)
{
	return (AGG_START_COST + AGG_PART_PAIR * nparts * nparts) * cpu_operator_cost;
}

/*
 * the most rows of elements a scan of the plan being made reads: what a plan that keeps the shared CTE has to produce (cte.c). A filter without
 * statistics keeps a default guess of the table's rows: the rows are the geometric mean of the guess and the table's, as filter_rows_ratio has them.
 */
/* the tuples the cap of cost_tuples removes, as a share of those the scan reads, above which workers are judged on the tuples that are read */
#define AGG_WIDE_CAP		0.5

double		agg_read_rows;

void
agg_cost(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, Path *p, double rows_out)
{
	CostAcc		k = {0};
	double		vis = 1.0 - Min(Max(c->stats.allvisible_frac, 0.0), 1.0),
				exact,
				vet,
				spill,
				table,
				groups,
				share,
				entries,
				key_ops,
				ids;
	Cost		startup;

	k.rows =Max(c->stats.nrows, Max(c->rel->tuples, 1.0));
	k.elems = s->group >= 0 ? elems_of(c, s->group) : 1.0;
	k.sel_rows = Min(Max(c->rel->rows, 1.0), k.rows);
	if (!pricing_scratch)
		agg_read_rows = Max(agg_read_rows, k.sel_rows * filter_rows_ratio(c) * k.elems);
	k.units = Max(c->stats.nunits, ceil(k.rows / 2000.0));
	k.rows_out = rows_out;
	k.open_cost = c->search ? sift_search_open_cost(c->search, k.rows) : 0;
	k.scan_cost = c->search ? sift_search_cost(c->search, k.rows, k.units, &k.rows, &k.units) : 0;
	cost_read(c, s, &k);
	cost_tuples(c, s, input_rel, &k);
	cost_joins(c, s, &k);
	cost_work(c, s, &k);
	entries = key_entries(c, s, &k, &ids, &key_ops);
	k.scan_cost += key_ops;
	/* the reads of the lookup tables are PostgreSQL's own, priced as its plans are */
	k.scan_cost = (k.scan_cost - k.load_cost) * AGG_SCAN_SPEED + k.load_cost;
	/* a top-N reads a fraction of the units, but opens the match set whole */
	if (s->topn)
	{
		double		open = k.open_cost * AGG_SCAN_SPEED,
					scan = (k.scan_cost - open) * c->topn_frac + open;

		/* through the sorted runs, a unit costs a seek and the rows wanted, never more than its rows scanned */
		k.scan_cost = c->topn_runs ? Min(scan, open + Max(c->stats.nunits, 1.0) * c->topn_frac * (TOPN_RUN_UNIT + rows_out * TOPN_RUN_ROW)) : scan;
	}
	k.scan_cost *= cpu_operator_cost;
	/*
	 * The normal plan of an unnest expects the rows of the function (100 per call for jsonb_to_recordset, jsonb_array_elements,
	 * jsonb_each) minus its filters, and prices its sort, join and aggregate with them, though the data has a handful per row: when it
	 * expects more unnested rows than the selected orders hold elements, this path is charged the square root of the excess. The
	 * estimates of the two plans then cross where the times do: a selective store set keeps the btree plan, a whole table or a
	 * filter on the elements (which cuts the expected rows) keeps its price. A qual with a bound parameter has no such estimate (a generic
	 * plan guesses its selectivity; the values are known at execution): no charge.
	 */
	k.lift = s->group >= 0 && !rel_has_param_qual(c->rel) ? Max(sqrt(input_rel->rows / (k.sel_rows * k.elems)), 1.0) : 1.0;
	k.scan_cost *= k.lift;
	/* the calls of the generic aggregates are the elements the data has, whatever the normal plan expects of the unnest */
	k.scan_cost += k.gen_cost * cpu_operator_cost;
	/* rows on pages that are not all-visible are checked in the heap: the rows that reach the groups, not the rows the table's own filters keep (the lookup joins filter them first) */
	vet = vet_cost(c, vis, Min(k.sel_rows, k.scan_rows / k.elems), Min(Max(k.zone_kept, 1.0 / k.units), 1.0)) * (s->topn ? c->topn_frac : 1.0);
	k.scan_cost += vet;
	/* the exact units cost their groups, not their rows */
	exact = preagg_share(c, s);
	if (exact > 0)
	{
		double		pre_cost = k.units * Max(k.zone_kept, 1.0 / k.units) * (AGG_UNIT_COST + preagg_unit_ops(c, s, k.units, k.rows, k.rows_out)) * AGG_SCAN_SPEED * cpu_operator_cost;

		k.scan_cost += exact * (Min(pre_cost, k.scan_cost) - k.scan_cost);
	}
	startup = (c->level_params != NIL ? AGG_RESTART_COST * cpu_operator_cost : agg_open_cost(c->nparts)) + k.child_cost + c->subplan_cost;
	/* a search is opened first (k.open_cost) and its native plan is the index scan of the search, which the scan starts no cheaper than */
	if (s->nkeys == 0 && s->naggs > 0 && !s->topn && c->nparts == 0 && c->level_params == NIL && !c->search)
		startup -= (AGG_START_COST - AGG_START_PLAIN) * cpu_operator_cost;
	for (int i = 0; i < s->npred; i++)
		startup += in_tables_cost(c, s->pred[i]);
	if (s->nworkers > 0)
		cost_parallel(c, s, &k, &startup);
	/* each process holds a table of its own (and spills it) */
	groups = group_count(c, s);
	/* a top-N reads a fraction of the units, and makes the ids of those */
	/* a serial scan that finishes the groups of a partition when its units end (part_flush_on) holds the table of one partition at a time */
	share = s->part_final && s->nparts > 0 && s->nworkers == 0 ? s->nparts : 1;
	spill = group_spill_cost(s, groups / share, Min(ids, entries / (s->nworkers + 1)) * (s->topn ? c->topn_frac : 1.0) / share, k.agg_rows / (s->nworkers + 1), &table);
	k.scan_cost += spill;
	if (!pricing_scratch)
	{
		agg_arm_spill(spill, groups, table);
		agg_arm_vet(vet, vis * 100.0);
	}
	/* no unit is read where the order array gives the rows (sift_topn_gorder): the heap confirms the n of them */
	if (s->topn && c->topn_array && s->nworkers == 0)
	{
		startup = (AGG_GORD_START + AGG_GORD_SEG * ceil(Max(c->rel->tuples, 1.0) / AGG_GORD_SEG_ROWS)) * cpu_operator_cost;
		k.scan_cost = 0;
	}
	p->startup_cost = startup + k.scan_cost;
	p->total_cost = p->startup_cost + k.rows_out * cpu_tuple_cost;
	/* the rows of a top-N are fetched from the heap, one page each */
	if (s->topn)
		p->total_cost += c->topn_fetch * random_page_cost * 0.5;
	/* the groups of the rows the lists pass (spec_finish) tell the plan above how many rows to expect; they are not part of the path's own price */
	p->rows = s->topn ? k.rows_out : Max(k.rows_out, c->path_rows);
	if (c->having_sel > 0 && !s->topn)
		p->rows = clamp_row_est(p->rows * c->having_sel);
	/*
	 * The rows of a fused scan add the groups of every arm, though no arm has more groups than rows it reads: the node reports at most the rows
	 * its arms aggregate. The plan above is priced for them (agg/cte.c weighs the inline plan of a shared CTE against the shared one), and the
	 * sum of the arms' groups made the inline plan look 6 times dearer than it is.
	 */
	if (s->narms > 0 && !s->topn)
		p->rows = Min(p->rows, Max(s->est_rows, 1.0));
	if (s->nworkers > 0 && !pricing_scratch && parallel_setup_cost > 0 && c->joins != NIL && k.agg_rows * AGG_WIDE_CAP < k.sel_rows * k.elems)
		workers_ratio(c, s, input_rel, p, rows_out, k.rows_out * cpu_tuple_cost);
}

