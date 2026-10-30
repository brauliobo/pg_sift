/*
 * agg/cost/zone.c - what the zones, statistics and sets of keys of a joined table keep of its rows
 * The share of the units and rows that survive a filter or a join key, from the zone maps
 * and the column statistics.
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

#include "agg/agg.h"
#include "col/point.h"
#include "agg/cost/int.h"

/*
 * The share of the units whose zone a test on an integer leaf overlaps. A test on the fraction f of the leaf's range overlaps the zone
 * of a unit when its range and the zone's meet: f plus the mean width of the zones (the index stores it: a column the units follow
 * has narrow zones, one they do not has the whole range in each). A source without zones has the correlation of the heap column: the
 * units are consecutive in the heap, so their zones are narrow in the share the column is correlated with (corr^2).
 */
static double
zone_overlap(AggCtx *c, int leaf, double f)
{
	double		span = c->stats.zone_span[leaf],
				corr;

	if (span > 0)
		return Min(f + span, 1.0);
	corr = leaf_correlation(c, leaf);
	return f + (1.0 - f) * (1.0 - corr * corr);
}

/*
 * The share of the units that hold one of `keys` values of the leaf (nd distinct): a unit whose zone is the width w (as a share of the leaf's
 * range) has one in it with the probability 1 - (1 - w)^keys, counted on the sample of the real zones, where the mean width (a few wide
 * zones among narrow ones make a mean that keeps every unit) would say keys times it. The units that pass keep a summary of their values
 * (set_cover of them): the n values of a unit, set_keys on average and in proportion to its zone's width (at least one, at most the
 * values the zone spans), hold one of the keys with the probability 1 - exp(-keys * n / nd). A source with no sample has the mean width.
 */
static double
zone_share(AggCtx *c, int leaf, double keys, double nd)
{
	const SiftSrcZones *z = c->stats.zones;
	double		range = c->stats.max[leaf] - c->stats.min[leaf] + 1.0,
				cover = c->stats.set_cover[leaf],
				mean_w = 0,
				kept = 0;

	if (z == NULL || z->n == 0 || z->lo[leaf] == NULL || range <= 1.0)
		return c->stats.zone_span[leaf] > 0 ? Min(keys * c->stats.zone_span[leaf], 1.0) : zone_overlap(c, leaf, Min(keys / nd, 1.0));
	for (int i = 0; i < z->n; i++)
		mean_w += Max(((double) z->hi[leaf][i] - (double) z->lo[leaf][i] + 1.0) / range, 0.0) / z->n;
	for (int i = 0; i < z->n; i++)
	{
		double		w = ((double) z->hi[leaf][i] - (double) z->lo[leaf][i] + 1.0) / range,
					in_zone,
					n,
					held = 1.0;

		if (w <= 0)
			continue;
		in_zone = w >= 1.0 ? 1.0 : -expm1(keys * log1p(-w));
		if (cover > 0)
		{
			n = Min(Max(c->stats.set_keys[leaf] * w / mean_w, 1.0), Max(w * nd, 1.0));
			held = 1.0 - cover + cover * -expm1(-keys * n / nd);
		}
		kept += in_zone * held;
	}
	return z->covered * kept / z->n + (1.0 - z->covered);
}

/* the same keys of a leaf are asked again by every path and every pricing pass of a plan, and the sum is over the sample of the zones: the last few answers are kept */
static double
zone_keys_kept(AggCtx *c, int leaf, double keys, double nd)
{
	AggZoneMemo *m;

	for (int i = 0; i < c->nzone_memo; i++)
	{
		m = &c->zone_memo[i];
		if (m->leaf == leaf && m->keys == keys && m->nd == nd)
			return m->kept;
	}
	m = &c->zone_memo[c->nzone_memo < lengthof(c->zone_memo) ? c->nzone_memo++ : 0];
	*m = (AggZoneMemo) {leaf, keys, nd, zone_share(c, leaf, keys, nd)};
	return m->kept;
}

/*
 * The share of the units the point directory of a leaf leaves for a list of `keys` of its nd distinct values: the rows it lists (keys x the rows of a key)
 * lie in the units like balls in bins, a unit with one is read (and narrowed to those rows, col/scan.c), and one without is not. A list the directory does not
 * answer (col/point.h: too many keys or rows) or a leaf without one keeps every unit. SiftColSource asks the directory of each segment; the units of one
 * that has none stay, as the share of the units that have one says (point_cover).
 */
static double
point_kept(AggCtx *c, int leaf, double keys, double nd)
{
	double		cover = c->stats.point_cover[leaf],
				rows,
				units = Max(c->stats.nunits, 1.0);

	if (cover <= 0 || nd <= 0 || keys > SIFT_CPOINT_MAX_KEYS || (keys > SIFT_CPOINT_MIN_KEYS && keys > c->stats.nrows / 16))
		return 1.0;
	rows = keys * Max(c->stats.nrows, 1.0) / nd;
	if (rows > SIFT_CPOINT_MAX_ROWS || rows > Max((double) SIFT_CPOINT_DENSE_ROWS, c->stats.nrows / SIFT_CPOINT_DENSE_SHARE))
		return 1.0;
	return 1.0 - cover + cover * -expm1(-rows / units);
}

/*
 * The units the join key filters leave (join_filters in agg/init.c): an inner or semi join on an integer leaf tests the leaf against
 * the keys of the lookup, narrowed to the rows a later inner or semi join keyed on one of its columns can match (a scope list joined
 * to the table's rows). The keys are that many of the distinct values of the leaf: the share of the leaf's range the zones keep, as for
 * a comparison. A CTE of a literal list has no more keys than the list holds: key_list_lift raises its rows to those its keys own, so a key of many
 * rows counts as many keys there, and the zones of the units hold keys, not rows. *n: the filters the scan applies; *sel_keys: the keys of the most
 * selective one (a share of the leaf's distinct values),
 * HUGE_VAL without a filter. The keys of a lookup PostgreSQL has no statistics for (a CTE or sub-select of ids) are its floor of one row,
 * whatever the list holds: they stay as estimated where they leave a handful of units (an index cut by the key reads a few for any key),
 * and are the geometric mean of them and the leaf's distinct values where they leave more (the planner cannot tell a key of ten rows from
 * one of ten thousand, and an index not cut by the key reads tens of units for one, so the scan is priced for a guess that is not a floor). The price of
 * the workers (workers_ratio) takes that mean always: a guess of a key or two that leaves three units prices a scan no worker pays for, where the keys are
 * tens and the scan reads a hundred units, and the workers that start for a scan that turns out short are called back by par_scan_small.
 */
static double
join_key_zone(AggCtx *c, SiftAggSpec *s, int *n, double *sel_keys, double *share, double *excess)
{
	double		kept = 1.0,
				selective = 1.0;

	*n = 0;
	*sel_keys = HUGE_VAL;
	*share = 1.0;
	*excess = 1.0;
	if (!sift_agg_join_filter || s->topn)
		return kept;
	/*
	 * A rest of an ON clause that compares values of the tuple holds only for the tuple: the tables are loaded whole, their keys are all there
	 * are, and the rows PostgreSQL expects of a lookup are what the join keeps, not what a filter can cut.
	 */
	for (int ci = 0; ci < s->ncombo; ci++)
		if (on_tests_tuple(s, ci))
			return kept;
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[jc->combo];
		const SiftAggJoin *j = &s->join[cb->join];
		int			dkey = 0;
		const JoinCtx *dj = dependent_join(c, s, jc, &dkey);
		double		keys = Min(lookup_rows(c, jc), dj ? lookup_rows(c, dj) : HUGE_VAL),
					sure = Min(lookup_sure(c, jc), dj ? lookup_sure(c, dj) : HUGE_VAL);

		if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || cb->level != 0)
			continue;
		for (int k = 0; k < j->nkey; k++)
		{
			int			leaf = j->keypos[k] < 0 ? -1 : cb->leaf[j->keypos[k]];
			const SiftSrcLeaf *lf = leaf < 0 || leaf >= c->lay->nleaves - c->nvleaf ? NULL : &c->lay->leaf[leaf];
			Node	   *e = lf && lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
			double		nd,
						nkeys,
						list_keys,
						kept_keys;

			if (lf == NULL || lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->vtype != SCV_I32 || e == NULL || !IsA(e, Var) ||
				(nd = column_distinct(c, (Var *) e)) <= 0)
				continue;
			list_keys = Min(cte_list_keys(c, jc, k), dj ? cte_list_keys(c, dj, dkey) : HUGE_VAL);
			if (sure / nd < selective && !lookup_guessed(c, jc) && (dj == NULL || !lookup_guessed(c, dj)))
			{
				selective = sure / nd;
				*sel_keys = sure;
				*excess = list_keys < HUGE_VAL ? Max(cte_list_worth(c, jc, k, (Var *) e) / sure, 1.0) : 1.0;
				*share = selective * *excess;
			}
			nkeys = Min(keys, list_keys);
			kept_keys = Min(zone_keys_kept(c, leaf, nkeys, nd), point_kept(c, leaf, nkeys, nd));
			if (nd > keys && !lookup_counted(c, jc) && (pricing_wide || kept_keys * Max(c->stats.nunits, 1.0) > AGG_CUT_UNITS))
			{
				nkeys = Min(sqrt(keys * nd), list_keys);
				kept_keys = Min(zone_keys_kept(c, leaf, nkeys, nd), point_kept(c, leaf, nkeys, nd));
			}
			kept = Min(kept, kept_keys);
			(*n)++;
		}
	}
	return kept;
}

/* does PostgreSQL know the values a filter of a table compares (statistics of the column or of an expression index)? Its guess of what the filter keeps is a default otherwise */
bool
filter_has_stats(PlannerInfo *root, RestrictInfo *ri)
{
	List	   *args = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->args :
		IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->args : NIL;
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft,
				known;

	if (list_length(args) != 2 || !get_restriction_variable(root, args, 0, &vardata, &other, &varonleft))
		return true;
	known = HeapTupleIsValid(vardata.statsTuple) || vardata.isunique;
	ReleaseVariableStats(vardata);
	return known;
}

/*
 * How many times more rows than PostgreSQL expects the filters of a rel keep, when one of them has no statistics (a jsonb path, an expression,
 * a column of a sub-select's output): its guess is a default, a few hundred of a large table, and the groups of that many rows are fewer
 * than the keys have. The rows are taken as the geometric mean of the guess and the rel's rows, as child_rows does. A test for NULL keeps
 * its default only on a rel that is not a table: a table's column has statistics once it is analyzed, so a table's plan is as it was.
 */
double
rel_filter_ratio(PlannerInfo *root, RelOptInfo *rel)
{
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
	{
		bool		known = filter_has_stats(root, ri);

		if (known && rel->rtekind != RTE_RELATION && IsA(ri->clause, NullTest))
		{
			VariableStatData vardata;

			examine_variable(root, (Node *) ((NullTest *) ri->clause)->arg, 0, &vardata);
			known = HeapTupleIsValid(vardata.statsTuple);
			ReleaseVariableStats(vardata);
		}
		if (!known)
			return sqrt(Max(rel->tuples / Max(rel->rows, 1.0), 1.0));
	}
	return 1.0;
}

double
filter_rows_ratio(AggCtx *c)
{
	return rel_filter_ratio(c->root, c->rel);
}

bool
reads_column_index(Node *node, void *context)
{
	bool		found = false;

	if (node != NULL && IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		found = rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid);
	}
	else if (node != NULL && IsA(node, Query))
		found = query_tree_walker((Query *) node, reads_column_index, context, QTW_EXAMINE_RTES_BEFORE);
	else if (node != NULL)
		found = expression_tree_walker(node, reads_column_index, context);
	return found;
}

/*
 * How many times more rows the key of a row owns than the mean: nd * sum(freq^2) over the values of the column, the frequencies of its
 * most common values by PostgreSQL's statistics and the rest of the rows spread evenly. 1 for a flat column or one without statistics.
 * The mean is what PostgreSQL prices a probe of the key with, and the keys a query passes are the ones with rows to look at.
 */
static double
column_skew(AggCtx *c, Var *v)
{
	HeapTuple	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid), Int16GetDatum(v->varattno), BoolGetDatum(false));
	AttStatsSlot sslot;
	double		nd,
				left,
				sq = 0;

	if (!HeapTupleIsValid(tup))
		return 1.0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	nd = nd > 0 ? nd : -nd * c->rel->tuples;
	left = 1.0 - ((Form_pg_statistic) GETSTRUCT(tup))->stanullfrac;
	if (nd >= 1 && get_attstatsslot(&sslot, tup, STATISTIC_KIND_MCV, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		for (int i = 0; i < sslot.nnumbers; i++)
		{
			sq += (double) sslot.numbers[i] * sslot.numbers[i];
			left -= sslot.numbers[i];
		}
		if (left > 0 && nd > sslot.nnumbers)
			sq += left * left / (nd - sslot.nnumbers);
		free_attstatsslot(&sslot);
	}
	else
		nd = 0;
	ReleaseSysCache(tup);
	return Max(nd * sq, 1.0);
}

/*
 * How many times more keys than PostgreSQL expects the scope lists of the query hold, for the normal plan's price. A lookup join on a
 * column of the scanned table (a list of ids, the rows of a sub-select) is counted by PostgreSQL from its own rows, and those are a
 * default guess (a row or two) when the filters of the lookup read a jsonb path, an expression or the output of a plan: the normal plan
 * is then priced for a handful of keys, though the lists the queries pass hold a good share of the table's distinct values. The keys
 * are taken as the geometric mean of the rows PostgreSQL expects and the distinct values of the scanned column (as child_rows does for
 * the rows of a set-returning function). A lookup PostgreSQL has statistics for keeps its estimate of the keys, and the rows of each are priced
 * by the skew of the column (column_skew, sift.agg_key_skew). With groups set (the rows the path reports, spec_finish) the keys are all the
 * distinct values and the skew is not counted: the groups are the keys' own.
 */
double
lookup_set_ratio(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, bool groups)
{
	double		f = 1.0;
	int			nseen = 0;
	AttrNumber *seen;
	double	   *seen_f;

	if (!sift_agg_join_filter || s->topn || sift_agg_param_set_size <= 0)
		return f;
	seen = palloc(list_length(c->joins) * sizeof(AttrNumber));
	seen_f = palloc(list_length(c->joins) * sizeof(double));
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[jc->combo];
		const SiftAggJoin *j = &s->join[cb->join];
		double		keys = lookup_rows(c, jc),
					nd = 0,
					g = 1.0;
		Var		   *key = NULL;
		int			at;

		if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || cb->level != 0)
			continue;
		for (int k = 0; k < j->nkey; k++)
		{
			int			leaf = j->keypos[k] < 0 ? -1 : cb->leaf[j->keypos[k]];
			const SiftSrcLeaf *lf = leaf < 0 || leaf >= c->lay->nleaves - c->nvleaf ? NULL : &c->lay->leaf[leaf];
			Node	   *e = lf && lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
			double		d;

			if (lf != NULL && lf->kind == SCL_SCALAR && lf->parent < 0 && e != NULL && IsA(e, Var) && (d = column_distinct(c, (Var *) e)) > nd)
			{
				key = (Var *) e;
				nd = d;
			}
		}
		if (key == NULL)
			continue;
		/* a counted lookup's keys are as many as PostgreSQL says, but each has the rows of the keys a query passes, not the mean's */
		if (lookup_counted(c, jc))
		{
			if (sift_agg_key_skew && !groups)
				g = sqrt(Max(column_skew(c, key) / AGG_KEY_SKEW_FLOOR, 1.0));
		}
		else if (nd > keys)
			g = groups ? nd / keys : sqrt(nd / keys);
		/* lookups on one column (a CTE of ids joined and again in an IN list) restrict the same keys: the normal plan reads them once */
		for (at = 0; at < nseen && seen[at] != key->varattno; at++)
			;
		if (at == nseen)
		{
			seen[nseen] = key->varattno;
			seen_f[nseen++] = g;
		}
		else
			seen_f[at] = Max(seen_f[at], g);
	}
	for (int i = 0; i < nseen; i++)
		f *= seen_f[i];
	/* the normal plan reads no more than the rows the table's own filters keep, whatever the keys */
	return Min(f, Max(c->rel->rows / Max(input_rel->rows, 1.0), 1.0));
}

/*
 * The share of the units the zone tests of p leave (the scan tests each disjunct of an OR on its own, init_filter.c): a comparison keeps the
 * zones its selectivity overlaps, an AND the most selective of its conjuncts' share, an OR the sum of its disjuncts' (1: none refuses a unit).
 */
static double
pred_zone_kept(AggCtx *c, const SiftAggPred *p)
{
	double		kept = 1.0;

	if (p->kind == SAP_AND)
		for (int i = 0; i < p->nchild; i++)
			kept = Min(kept, pred_zone_kept(c, p->child[i]));
	else if (p->kind == SAP_OR)
	{
		kept = 0.0;
		for (int i = 0; i < p->nchild; i++)
			kept += pred_zone_kept(c, p->child[i]);
		kept = Min(kept, 1.0);
	}
	else if (sift_aggspec_zone_cmp(p, c->lay, c->lay->nleaves))
		kept = zone_overlap(c, p->leaf, Min(Max(p->sel, 0.0), 1.0));
	return kept;
}

/*
 * The share of the units the small-set summaries of the leaves the conjuncts test leave (col/setsum.h): an equality or IN on the share s of the leaf's
 * distinct values (p->sel for an integer leaf, nvals / ndistinct for a text one) keeps a unit that holds n of them with the probability
 * 1 - (1 - n / distinct) ^ (s * distinct), about 1 - exp(-s * n); a unit without a summary stays. A summary that keeps nearly every unit is not counted:
 * the scan reads it for nothing then, as for a leaf that has none.
 */
static double
set_kept(AggCtx *c, SiftAggSpec *s)
{
	double		kept = 1.0;

	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];
		SiftSrcStrTest st;
		double		share;

		if (p->kind == SAP_LUT && sift_aggspec_str_test(p, c->lay, c->lay->nleaves - c->nvleaf, &st) && (st.op == SSTR_EQ || st.op == SSTR_IN) &&
			c->stats.ndistinct[p->leaf] > 0)
			share = st.nvals / c->stats.ndistinct[p->leaf];
		else if ((p->kind == SAP_CMP && p->op == SCMP_EQ) || (p->kind == SAP_IN && p->op == 0))
		{
			share = Min(Max(p->sel, 0.0), 1.0);
			if (c->stats.ndistinct[p->leaf] > 0)
				kept = Min(kept, point_kept(c, p->leaf, share * c->stats.ndistinct[p->leaf], c->stats.ndistinct[p->leaf]));
		}
		else
			continue;
		if (c->stats.set_cover[p->leaf] > 0)
		{
			double		cover = c->stats.set_cover[p->leaf],
						held = 1.0 - exp(-share * c->stats.set_keys[p->leaf]);

			if (held < AGG_SET_USEFUL)
				kept = Min(kept, 1.0 - cover + cover * held);
		}
	}
	return kept;
}

/* does the point directory narrow a unit of the scan to the rows of an equality or IN list (set_kept), which are then the only rows its filters test? */
bool
point_narrows(AggCtx *c, SiftAggSpec *s)
{
	bool		narrows = false;

	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];

		narrows |= ((p->kind == SAP_CMP && p->op == SCMP_EQ) || (p->kind == SAP_IN && p->op == 0)) && c->stats.ndistinct[p->leaf] > 0 &&
			point_kept(c, p->leaf, Min(Max(p->sel, 0.0), 1.0) * c->stats.ndistinct[p->leaf], c->stats.ndistinct[p->leaf]) < 1.0;
	}
	return narrows;
}

/*
 * The share of the units the zone tests leave: the comparisons (their selectivity is also the fraction of the leaf's range they keep) and
 * the ORs of them, the join key filters and a ctid range. The most selective counts, and the scan reads at least one unit (docs/COL-PLAN.md, "Zone pruning").
 */
double
zone_kept(AggCtx *c, SiftAggSpec *s, int *nkeyfilters, double *key_share)
{
	double		sel_keys,
				share,
				excess,
				kept;
	List	   *rest;

	kept = join_key_zone(c, s, nkeyfilters, &sel_keys, &share, &excess);
	*key_share = Min(share, 1.0);
	kept = Min(kept, zone_sample_kept(c, s, &rest));
	kept = Min(kept, set_kept(c, s));
	foreach_ptr(SiftAggPred, p, rest)
		kept = Min(kept, p->kind == SAP_TID ? p->sel : pred_zone_kept(c, p));
	return kept;
}

/*
 * does the lookup that narrows the scan by its key hold a few keys, as PostgreSQL counts them? *rows: the rows of the table those keys own;
 * *excess: how many times the rows of one of them exceed the mean's, which PostgreSQL prices a probe of the key by; *sure: the keys are
 * that few whatever the units they leave (a literal list, a lookup with statistics), so the rows PostgreSQL expects of a probe are no lost estimate
 */
bool
few_keys_lookup(AggCtx *c, SiftAggSpec *s, double *rows, double *excess, bool *sure)
{
	double		sel_keys,
				share,
				kept;
	int			n;

	kept = join_key_zone(c, s, &n, &sel_keys, &share, excess);
	*rows = c->rel->rows * Min(share, 1.0);
	*sure = sel_keys <= AGG_PROBE_KEYS;
	return *sure && (kept >= AGG_CUT_SHARE || kept * Max(c->stats.nunits, 1.0) > AGG_CUT_UNITS);
}

/*
 * The rows of an unnest PostgreSQL expects of the aggregate fall to a default when a join is on a range or the rows come from a function (a row,
 * or the 100 rows of a jsonb_each), though the table's own filters keep far more: the normal plan is priced for a few rows where the data has
 * thousands, and the scan, which reads the table's rows whatever the join keeps, loses a comparison it wins. The paths of the grouping stage that
 * are not the scan are charged the square root of the excess, as the normal plan of an unnest is on the other side of the same estimate (agg_cost).
 * Rows without an unnest keep PostgreSQL's estimate: the statistics of the joined tables are the ones it counts them by.
 */
double
input_rows_ratio(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, double rows)
{
	if (s->group < 0 || rel_has_param_qual(c->rel))
		return 1.0;
	return Max(sqrt(Max(rows, 1.0) * elems_of(c, s->group) / Max(input_rel->rows, 1.0)), 1.0);
}
