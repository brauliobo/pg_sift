/*
 * agg/cost/join.c - what the joined tables and their lookups cost
 * The leaves a join adds, the lookup tables and their probes, the keys loaded and the
 * statement-level sum over the joins (cost_joins).
 */
#include "postgres.h"

#include <math.h>

#include "access/relation.h"
#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_statistic.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"

#include "agg/agg.h"
#include "agg/cost/int.h"
#include "agg/agg_int.h"

/*
 * The rows of the path of a joined plan. PostgreSQL assumes 1000 rows a call of a set-returning function in the select list (a date
 * spine), a ceiling nobody generates up to: the rows a call makes are taken as the geometric mean of one and that estimate.
 */
double
child_rows(Path *p)
{
	for (;;)
	{
		if (IsA(p, SubqueryScanPath))
			p = ((SubqueryScanPath *) p)->subpath;
		else if (IsA(p, ProjectionPath))
			p = ((ProjectionPath *) p)->subpath;
		else if (IsA(p, ProjectSetPath))
		{
			Path	   *sub = ((ProjectSetPath *) p)->subpath;

			return child_rows(sub) * sqrt(Max(p->rows / Max(sub->rows, 1.0), 1.0));
		}
		else
			return p->rows;
	}
}

/* the leaves of the combo of a join that are not its key: the values its expressions read, which make an entry for each distinct combination */
int
join_extra_leaves(const SiftAggCombo *cb, const SiftAggJoin *j)
{
	int			extra = 0;

	for (int p = 0; p < cb->nleaves; p++)
	{
		bool		key = false;

		for (int k = 0; k < j->nkey; k++)
			key |= j->keypos[k] == p;
		extra += !key;
	}
	return extra;
}

/* does the rest of the ON clause of combo ci compare the tuple's values with the table's? Then it is tested for each tuple (a condition on the table alone is tested when the table loads) */
bool
on_tests_tuple(const SiftAggSpec *s, int ci)
{
	const SiftAggCombo *cb = &s->combo[ci];

	return cb->join >= 0 && s->join[cb->join].fslot >= 0 && cb->nleaves > 1;
}

/*
 * Does the executor test a tuple against the keys of this join by a byte of a table (semi_lut_build, init.c) rather than by a hash probe? An inner join by a key the
 * table holds about once does; a repeated key falls back to the hash.
 */
bool
join_lut(AggCtx *c, const SiftAggSpec *s, const JoinCtx *jc)
{
	const SiftAggCombo *cb = &s->combo[jc->combo];
	const SiftAggJoin *j = &s->join[cb->join];
	double		rpk;

	if (!semi_lut_possible(s, jc->combo, c->lay->nleaves - c->nvleaf) || c->lay->leaf[cb->leaf[0]].vtype != SCV_I32)
		return false;
	if (j->kind != SJ_INNER)
		return true;
	rpk = OidIsValid(j->relid) && j->tkey[0] == NULL ? sift_agg_rows_per_key(j->relid, j->keyatt[0], c->root->simple_rel_array[jc->rti]->tuples) : 0;
	return rpk > 0 && rpk <= 2.0;
}

/* is the table loaded as keys alone (lookup_keys_only), the columns its filter reads evaluated at the load? */
static bool
join_keys_only(AggCtx *c, const SiftAggSpec *s, const JoinCtx *jc)
{
	const SiftAggJoin *j = &s->join[s->combo[jc->combo].join];

	return jc->cpath == NULL && join_lut(c, s, jc) && lookup_keys_only(s, c->lay, jc->combo, c->lay->nleaves - c->nvleaf) &&
		(j->ncol == 0 || (sift_agg_lookup_pushdown && j->fslot >= 0));
}

/* The correlation of the heap column a leaf is read from with the heap order (0: unknown, or the leaf is a jsonb path, a field of a list or an expression) */
double
leaf_correlation(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL && lf->parent < 0 ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	HeapTuple	tp;
	AttStatsSlot sslot;
	double		corr = 0.0;

	if (e == NULL || !IsA(e, Var))
		return 0.0;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						 Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return 0.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = sslot.numbers[0];
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return corr;
}

/* the lookup join whose key is the earlier lookup's column `col` (a later inner or semi join: its rows are the only ones that complete a tuple); *key: that key of it */
const JoinCtx *
dependent_join(AggCtx *c, SiftAggSpec *s, const JoinCtx *jc, int *key)
{
	foreach_ptr(JoinCtx, dj, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[dj->combo];
		const SiftAggJoin *j = &s->join[cb->join];

		if (dj == jc || (j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL)
			continue;
		for (int k = 0; k < j->nkey; k++)
		{
			Node	   *kx = j->keyexpr[k];
			int			pos = kx && IsA(kx, Param) ? ((Param *) kx)->paramid : -1;

			if (pos >= 0 && pos < cb->nleaves && cb->leaf[pos] == -1 && cb->refcombo[pos] == jc->combo)
			{
				*key = k;
				return dj;
			}
		}
	}
	return NULL;
}

/* the rows of a joined table or plan: what the scan loads */
double
lookup_rows(AggCtx *c, const JoinCtx *jc)
{
	return Max(jc->cpath ? child_rows(jc->cpath) : c->root->simple_rel_array[jc->rti]->rows, 1.0);
}

/*
 * The keys of a lookup's plan, for the pick between the scan and the normal plan. PostgreSQL clamps an estimate at one row and takes a join of two
 * lists to a fraction of the smaller one (10 ids semi-joined to 32 rows: 2 of them), so the plan is read down to its inputs: a join has as many keys as
 * the smaller of its inputs (a nested loop: as its outer side, the inner one is per row) and a CTE as the plan of its query.
 */
static double
child_keys(PlannerInfo *root, Path *p)
{
	double		rows = child_rows(p);
	JoinPath   *jp;

	for (;;)
	{
		if (IsA(p, SubqueryScanPath))
		{
			/* the paths of a sub-select refer to the range table of its own root */
			root = p->parent->subroot;
			p = ((SubqueryScanPath *) p)->subpath;
		}
		else if (IsA(p, ProjectionPath))
			p = ((ProjectionPath *) p)->subpath;
		else if (sift_is_sorted_unique_path(p))
			p = ((SiftSortedUniquePath *) p)->subpath;
		else if (IsA(p, SortPath))
			p = ((SortPath *) p)->subpath;
		else
			break;
	}
	if (p->pathtype == T_CteScan)
	{
		RangeTblEntry *rte = planner_rt_fetch(p->parent->relid, root);
		PlannerInfo *cr = rte->rtekind == RTE_CTE ? cte_root(root, rte) : NULL;

		return cr ? Max(rows, child_keys(cr, fetch_upper_rel(cr, UPPERREL_FINAL, NULL)->cheapest_total_path)) : rows;
	}
	if (!(IsA(p, NestPath) || IsA(p, MergePath) || IsA(p, HashPath)))
		return rows;
	jp = (JoinPath *) p;
	switch (jp->jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
		SIFT_CASE_JOIN_RIGHT_SEMI
			return Max(rows, IsA(p, NestPath) ? child_keys(root, jp->outerjoinpath) :
					   Min(child_keys(root, jp->outerjoinpath), child_keys(root, jp->innerjoinpath)));
		case JOIN_LEFT:
		case JOIN_ANTI:
			return Max(rows, child_keys(root, jp->outerjoinpath));
		default:
			return rows;
	}
}

double
lookup_sure(AggCtx *c, const JoinCtx *jc)
{
	return jc->cpath ? Max(child_keys(c->root, jc->cpath), 1.0) : lookup_rows(c, jc);
}

/*
 * is the count of a lookup a default guess: a filter of a table that PostgreSQL has no statistics for, or a CTE whose body is counted by defaults (cte_guessed)? A CTE
 * of ids built from sub-selects on an expression key or a jsonb filter is estimated at the clamp of one row whatever it holds, and the normal plan would be priced for one probe
 */
bool
lookup_guessed(AggCtx *c, const JoinCtx *jc)
{
	RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);

	if (rte->rtekind == RTE_CTE)
		return cte_guessed(c, jc);
	return rte->rtekind == RTE_RELATION && !lookup_counted(c, jc);
}

/*
 * Are the rows PostgreSQL expects of a lookup its statistics' count? A table whose filters all have statistics, a VALUES list and a function of
 * constants (unnest of a list) are; a sub-select, a CTE or a function of other arguments is estimated through its own joins and defaults.
 */
bool
lookup_counted(AggCtx *c, const JoinCtx *jc)
{
	RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);

	if (rte->rtekind == RTE_VALUES)
		return true;
	if (rte->rtekind == RTE_FUNCTION)
	{
		RangeTblFunction *rtf = list_length(rte->functions) == 1 ? linitial_node(RangeTblFunction, rte->functions) : NULL;

		if (rtf == NULL || !IsA(rtf->funcexpr, FuncExpr))
			return false;
		foreach_ptr(Node, a, ((FuncExpr *) rtf->funcexpr)->args)
			if (!IsA(a, Const))
				return false;
		return true;
	}
	if (rte->rtekind != RTE_RELATION)
		return false;
	foreach_node(RestrictInfo, ri, c->root->simple_rel_array[jc->rti]->baserestrictinfo)
		if (!filter_has_stats(c->root, ri))
			return false;
	return true;
}

bool
extern_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXTERN)
		return true;
	return expression_tree_walker(node, extern_param_walker, context);
}

/* Does a qual of the table compare with a bound parameter? Its selectivity is a default guess of the planner, not the data's. */
bool
rel_has_param_qual(RelOptInfo *rel)
{
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		if (extern_param_walker((Node *) ri->clause, NULL))
			return true;
	return false;
}

/* does a btree index of the joined table start with columns that the table's `column = value` conditions give (map_load picks the same) */
static bool
join_index_usable(RelOptInfo *rel, const SiftAggJoin *j)
{
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		if (ii->relam != BTREE_AM_OID || ii->indpred != NIL || ii->hypothetical || ii->indexkeys[0] <= 0)
			continue;
		for (int k = 0; k < j->nidx; k++)
			if (j->idxatt[k] == ii->indexkeys[0] && op_in_opfamily(j->idxop[k], ii->opfamily[0]) && ii->indexcollations[0] == j->idxcoll[k])
				return true;
	}
	return false;
}

/* the heap pages of the scanned table: those of the partitions of a partitioned table */
double
rel_pages(AggCtx *c)
{
	return c->nparts > 0 ? c->part_pages : c->rel->pages;
}

/* are the rows PostgreSQL expects of the joins counted: no correlated child and no function (its default is a thousand rows a call) */
bool
joins_counted(AggCtx *c)
{
	bool		counted = true;

	foreach_ptr(JoinCtx, jc, c->joins)
		counted &= jc->j.params == NIL && planner_rt_fetch(jc->rti, c->root)->rtekind != RTE_FUNCTION;
	return counted;
}

/* the operators a row of a keyed load costs, the page visits apart; `entries_only`: the share of them taken from the entries of the btree */
double
sift_agg_entry_ops(double entries_only)
{
	return AGG_PROBE_ROW - entries_only * AGG_PROBE_IOS;
}

/*
 * The price, in operators, of reading a lookup table by a btree probe of `keys` values: a descent each, a row for each of the `entries` they find and the heap pages visited
 * (the table's `pages` less those the rows leave out), at what a visit to a page costs where the table's pages are (heap_visit_ops). `entries_only` is the share of the pages
 * whose rows come from the entries of a btree that holds the columns (all-visible pages of a covering index): no heap fetch, no page visit.
 */
double
sift_agg_keyed_ops(double keys, double entries, double pages, double entries_only)
{
	double		visited = pages * (1.0 - exp(-entries / Max(pages, 1.0)));

	return keys * AGG_PROBE_KEY + entries * sift_agg_entry_ops(entries_only) + visited * (1.0 - entries_only) * heap_visit_ops(pages);
}

/*
 * The price, in operators, of a table read whole. From the leaves of a column index that holds its columns (`cols`, with `nnum` numeric ones): the bytes are in the price of the row.
 * From the heap: the load of each row (of its keys alone for `keys_only`) and the pages at the page cost of PostgreSQL's own sequential scan of them (a 1.5-million-row table: 11,000
 * of 15,000 more), at most a random fetch each.
 */
double
sift_agg_whole_ops(double tuples, double pages, bool cols, int nnum, bool keys_only)
{
	if (cols)
		return tuples * (AGG_LOAD_ROW_COLS * pow(Max(tuples / AGG_LOAD_BIG, 1.0), 0.4) + nnum * AGG_LOAD_NUMERIC);
	return tuples * (keys_only ? AGG_LOAD_KEY : AGG_LOAD_ROW) + pages * Min(seq_page_cost, random_page_cost) / cpu_operator_cost;
}

/* a keyed load pays while it costs less than the table read whole (`whole`, from sift_agg_whole_ops) */
bool
sift_agg_probe_pays(double keys, double entries, double pages, double entries_only, double whole)
{
	return sift_agg_keyed_ops(keys, entries, pages, entries_only) < whole;
}

/* the rows a value of column att has: the rows of the table over the distinct values its statistics say; 0 when they say nothing */
double
sift_agg_rows_per_key(unsigned int relid, int att, double tuples)
{
	HeapTuple	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(relid), Int16GetDatum(att), BoolGetDatum(false));
	double		nd;

	if (!HeapTupleIsValid(tp))
		return 0;
	nd = ((Form_pg_statistic) GETSTRUCT(tp))->stadistinct;
	ReleaseSysCache(tp);
	if (nd < 0)
		nd = -nd * tuples;
	return nd > 0 && tuples > 0 ? Max(tuples / nd, 1.0) : 0;
}

/*
 * The `column = value` condition of the join (j->idx*) that a btree column of the given operator family and collation takes as a scan key after the key's column, or -1:
 * the rows of the index that can match are the only ones fetched.
 */
int
sift_agg_trail_cond(const SiftAggJoin *j, int att, Oid opfamily, Oid collation)
{
	for (int k = 0; k < j->nidx; k++)
		if (!j->idxany[k] && j->idxatt[k] == att && j->idxcoll[k] == collation && op_in_opfamily(j->idxop[k], opfamily) &&
			get_op_opfamily_strategy(j->idxop[k], opfamily) == BTEqualStrategyNumber)
			return k;
	return -1;
}

/*
 * The rows a probe of one value of column att fetches: the rows of the table over the distinct values of the column, narrowed by the `column = value` conditions on the `ntrail`
 * columns that follow it in the btree (a share of 1 over the distinct values of each). 0: unknown, unless the key is unique.
 */
double
sift_agg_key_rows(unsigned int relid, int att, const int *tatt, int ntrail, double tuples, bool unique)
{
	double		rpk = sift_agg_rows_per_key(relid, att, tuples);

	if (rpk <= 0)
		return unique ? 1.0 : 0;
	for (int i = 0; i < ntrail; i++)
	{
		double		share = sift_agg_rows_per_key(relid, tatt[i], tuples);

		if (share > 0)
			rpk *= share / tuples;
	}
	return Max(rpk, 1.0);
}

/* the price, in operators, of the table of a join read whole */
static double
join_whole_ops(AggCtx *c, SiftAggSpec *s, JoinCtx *jc)
{
	RelOptInfo *jrel = c->root->simple_rel_array[jc->rti];
	Relation	rel = relation_open(c->root->simple_rte_array[jc->rti]->relid, NoLock);
	int			nnum = 0;
	bool		cols = map_load_cols_possible(rel, &jc->j, &nnum);

	relation_close(rel, NoLock);
	return sift_agg_whole_ops(Max(jrel->tuples, 1.0), jrel->pages, cols, nnum, !cols && join_keys_only(c, s, jc));
}

/* does the btree hold, and return, every column a load of the join reads (map_index_covers) */
static bool
index_covers(const IndexOptInfo *ii, const SiftAggJoin *j)
{
	for (int n = 0; n < j->nkey + j->ncol; n++)
	{
		int			att = n < j->nkey ? j->keyatt[n] : j->colatt[n - j->nkey],
					i;

		if (n < j->nkey && j->tkey[n])
			return false;
		for (i = 0; i < ii->ncolumns; i++)
			if (ii->indexkeys[i] == att && ii->canreturn[i])
				break;
		if (i == ii->ncolumns)
			return false;
	}
	return true;
}

/*
 * Does a keyed load of `keys` values of column att pay for the table of jc (sift_agg_probe_pays against the table read whole)? The btree starts with att and, with the
 * join's own `column = value` conditions, may go on with columns they name: the rows fetched are those of the entries that match them. *rows: the rows fetched.
 */
static bool
join_keys_pay(AggCtx *c, SiftAggSpec *s, JoinCtx *jc, int att, double keys, double *rows, double *entries_only)
{
	RelOptInfo *jrel = c->root->simple_rel_array[jc->rti];
	const SiftAggJoin *j = &jc->j;
	Oid			relid = planner_rt_fetch(jc->rti, c->root)->relid;
	double		tuples = Max(jrel->tuples, 1.0),
				rpk = 0,
				best = HUGE_VAL;

	/* the index whose entries cost least for a key: the rows it finds (the join's conditions on its next columns narrow them), a row cheaper where it holds the columns (as map_probe) */
	foreach_node(IndexOptInfo, ii, jrel->indexlist)
	{
		int			ta[SIFT_AGG_MAX_JIDX],
					n = 0;
		bool		cov;
		double		r,
					price;

		if (!(ii->relam == BTREE_AM_OID && ii->indpred == NIL && !ii->hypothetical && ii->indexkeys[0] == att))
			continue;
		for (int col = 1; col < ii->nkeycolumns && n < SIFT_AGG_MAX_JIDX && sift_agg_trail_cond(j, ii->indexkeys[col], ii->opfamily[col], ii->indexcollations[col]) >= 0; col++)
			ta[n++] = ii->indexkeys[col];
		cov = index_covers(ii, j);
		r = sift_agg_key_rows(relid, att, ta, n, tuples, ii->unique && ii->nkeycolumns == 1);
		price = r * sift_agg_entry_ops(cov ? jrel->allvisfrac : 0);
		if (r > 0 && price < best)
		{
			best = price;
			rpk = r;
			*entries_only = cov ? jrel->allvisfrac : 0;
		}
	}
	if (rpk <= 0)
		return false;
	keys = Min(keys, tuples);
	if (!sift_agg_probe_pays(keys, keys * rpk, jrel->pages, *entries_only, join_whole_ops(c, s, jc)))
		return false;
	*rows = keys * rpk;
	return true;
}

/*
 * The keys of the IN list of the scan that names the key of the join: its elements, when the join is an inner or semi one keyed by a column of the table alone. 0: none.
 */
static double
join_in_keys(AggCtx *c, SiftAggSpec *s, const JoinCtx *jc)
{
	const SiftAggCombo *cb = &s->combo[jc->combo];
	const SiftAggJoin *j = &jc->j;

	if (j->nkey != 1 || j->keynull[0] || j->tkey[0] != NULL || j->keypos[0] < 0 || cb->leaf[j->keypos[0]] < 0 || (j->keytype[0] != INT4OID && j->keytype[0] != INT8OID))
		return 0;
	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];

		if (p->kind == SAP_IN && p->op == 0 && p->level == 0 && !p->late && p->expr != NULL && p->leaf == cb->leaf[j->keypos[0]] && p->argtype == j->keytype[0])
			return in_list_keys(c, p);
	}
	return 0;
}

/*
 * The keys of a lookup that the loader probes in a btree of the table (map_keyed_dependent, map_in_keys and map_probe in agg/lookup/): an inner or semi join keyed by a column
 * of the table alone, from a dependent lookup (a child plan inner or semi joined on the same leaf of the scan, or a lookup keyed by it) or from the scan's IN list of the
 * key, when a btree starts with the column and the probes cost less than the table read whole (sift_agg_probe_pays). 0: the table is read whole. The keys of a dependent lookup are
 * the rows PostgreSQL expects of it, taken as the geometric mean with the table's rows when it has no statistics for them (join_key_zone).
 */
static double
join_probe_keys(AggCtx *c, SiftAggSpec *s, JoinCtx *jc, double *rows, double *entries_only)
{
	RelOptInfo *jrel = c->root->simple_rel_array[jc->rti];
	const SiftAggCombo *cb = &s->combo[jc->combo];
	const SiftAggJoin *j = &jc->j;
	const SiftAggJoin *sj = &s->join[cb->join];
	double		tuples = Max(jrel->tuples, 1.0),
				best = 0,
				keys,
				found,
				only;

	*rows = *entries_only = 0;
	if (!sift_agg_lookup_pushdown || sift_aggjoin_preserves(j->kind) || j->params != NIL || j->first.qual != NIL || j->first.nord > 0 || cb->level != 0)
		return 0;
	/* the keys of a dependent lookup are those of the rows an inner or semi join keeps */
	if (j->nidx == 0 && (j->kind == SJ_INNER || j->kind == SJ_SEMI))
		foreach_ptr(JoinCtx, dj, c->joins)
		{
			const SiftAggCombo *cc = &s->combo[dj->combo];
			const SiftAggJoin *d = &dj->j;
			int			att = 0,
						dkey;
			Oid			typ = InvalidOid;

			if (dj == jc || cc->level != 0 || (d->kind != SJ_INNER && d->kind != SJ_SEMI) || d->params != NIL || d->nkey != 1 || d->keynull[0])
				continue;
			if (dependent_join(c, s, jc, &dkey) == dj && d->keyexpr[0] != NULL && IsA(d->keyexpr[0], Param))
			{
				int			col = cc->refcol[((Param *) d->keyexpr[0])->paramid];

				if (col >= 0 && col < j->ncol)
				{
					att = j->colatt[col];
					typ = j->coltype[col];
				}
			}
			else if (dj->cpath != NULL && j->nkey == 1 && !j->keynull[0] && j->tkey[0] == NULL && j->keypos[0] >= 0 && d->keypos[0] >= 0 &&
					 cc->leaf[d->keypos[0]] == cb->leaf[j->keypos[0]] && cb->leaf[j->keypos[0]] >= 0)
			{
				att = j->keyatt[0];
				typ = j->keytype[0];
			}
			if (att <= 0 || (typ != INT4OID && typ != INT8OID))
				continue;
			keys = lookup_rows(c, dj);
			keys = Min(!lookup_counted(c, dj) ? sqrt(keys * tuples) : keys, tuples);
			if ((best == 0 || keys < best) && join_keys_pay(c, s, jc, att, keys, &found, &only))
			{
				best = keys;
				*rows = found;
				*entries_only = only;
			}
		}
	/* the conditions of the join's own are tested by the filter of the table on each row fetched (map_probe) */
	if ((j->nidx == 0 || (sj->fslot >= 0 && !slot_reads_leaf(cb, sj->fslot))) && (keys = join_in_keys(c, s, jc)) > 0 && (best == 0 || keys < best) &&
		join_keys_pay(c, s, jc, j->keyatt[0], keys, &found, &only))
	{
		best = keys;
		*rows = found;
		*entries_only = only;
	}
	return best;
}

/* how well the order of the rows in the heap follows the values of column att, -1 to 1 (0 without statistics) */
static double
column_correlation(Oid relid, int att)
{
	HeapTuple	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(relid), Int16GetDatum(att), BoolGetDatum(false));
	AttStatsSlot sslot;
	double		corr = 0;

	if (!HeapTupleIsValid(tp))
		return 0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = sslot.numbers[0];
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return corr;
}

/*
 * The cost, in operators, of the heap pages the rows of a lookup read through a btree are fetched from. The btree gives the rows of a key one after the other, so a table
 * that is not in the order of the key (correlation near 0) is a page visit for each row read: the rows that pass the `column = value` conditions the index starts with,
 * before the table's other filters, counted by the statistics of the column (a hot value is not a typical one: 285,000 rows read for 45,000 kept). 1 to 1.5 us a page
 * visit measured (vet_cost), 0.9 us a row read of 13,000 to 285,000 rows of a table of 1,500,000.
 */
static double
idx_read_cost(AggCtx *c, RelOptInfo *jrel, const SiftAggJoin *j)
{
	List	   *conds = NIL;
	Oid			relid = planner_rt_fetch(jrel->relid, c->root)->relid;
	double		rows,
				corr = 0;

	foreach_node(RestrictInfo, ri, jrel->baserestrictinfo)
	{
		List	   *args = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->args :
			IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->args : NIL;
		Oid			opno = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->opno : IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->opno : InvalidOid;

		for (int k = 0; k < j->nidx; k++)
			foreach_ptr(Node, a, args)
			{
				Node	   *v = strip_relabel(a);

				if (opno == j->idxop[k] && IsA(v, Var) && ((Var *) v)->varno == jrel->relid && ((Var *) v)->varattno == j->idxatt[k])
				{
					conds = list_append_unique_ptr(conds, ri);
					corr = column_correlation(relid, j->idxatt[k]);
				}
			}
	}
	rows = conds != NIL ? clauselist_selectivity(c->root, conds, jrel->relid, JOIN_INNER, NULL) * Max(jrel->tuples, 1.0) : jrel->rows;
	return Min(rows, Max(jrel->tuples, 1.0)) * ((1.0 - corr * corr) + Min(1.0, jrel->pages / Max(rows, 1.0)) * corr * corr) * heap_visit_ops(jrel->pages);
}

/* the rows the scan loads of a lookup table, and what each costs: a plan is run whole; an index scan reads the rows that pass the equalities or the keys of a dependent lookup, the rest of a table is read whole */
static double
join_load_rows(AggCtx *c, SiftAggSpec *s, JoinCtx *jc, double *row_cost)
{
	PlannerInfo *root = c->root;
	RelOptInfo *jrel = root->simple_rel_array[jc->rti];
	double		tuples = Max(jrel->tuples, 1.0),
				probes,
				prows,
				only;

	if (jc->cpath != NULL)
	{
		int			nnum = 0;

		foreach_ptr(Var, v, jc->cvars)
			nnum += v->vartype == NUMERICOID;
		*row_cost = jc->cvars == NIL ? AGG_JOIN_ROW : AGG_PLAN_ROW + nnum * AGG_LOAD_NUMERIC;
		return Max(jc->cpath->rows, 1.0);
	}
	if (jc->j.nidx > 0 && join_index_usable(jrel, &jc->j))
	{
		prows = Min(Max(jrel->rows, 1.0), tuples);
		*row_cost = AGG_IDX_ROW + idx_read_cost(c, jrel, &jc->j) / prows;
		return prows;
	}
	if ((probes = join_probe_keys(c, s, jc, &prows, &only)) > 0)
	{
		*row_cost = sift_agg_keyed_ops(probes, prows, jrel->pages, only) / Max(prows, 1.0);
		return Max(prows, 1.0);
	}
	*row_cost = join_whole_ops(c, s, jc) / tuples;
	return tuples;
}

/* the rows of the lookup tables a parallel scan has the leader write to the shared memory (agg/lookshare.c) */
double
agg_share_rows(AggCtx *c, SiftAggSpec *s)
{
	double		rows = 0;

	foreach_ptr(JoinCtx, jc, c->joins)
	{
		double		row_cost;

		if (jc->j.params == NIL)
			rows += join_load_rows(c, s, jc, &row_cost);
	}
	return rows;
}

/* The lookup joins: the children's plans and the lookup tables' reads */
/*
 * The price, in operators, of a run of a correlated child that the scan probes in its btree directly (agg/combo/probe.c): LIMIT 1 over a forward index scan, or an index scan alone. PostgreSQL's cost
 * takes the first match to come after 1 / selectivity entries of the key, as if the rows that pass were spread evenly among them, and prices an entry at a fraction of what it
 * takes; a condition the index does not take (a column that is only INCLUDEd, a filter) makes the probe examine the entries of the key until one passes, as many as the key
 * has where the matches lie behind the others (a status that follows the label), so those are counted. A child that gives every row of the key (OFFSET 0) examines the entries
 * that the conditions of the index let through. 0: not a child of those shapes.
 */
static double
child_probe_ops(const JoinCtx *jc)
{
	Path	   *p = jc->cpath;
	IndexPath  *ip;
	RelOptInfo *rel;
	bool		first = false;
	double		entries = 1.0,
				rpk;

	for (;;)
	{
		if (IsA(p, SubqueryScanPath))
			p = ((SubqueryScanPath *) p)->subpath;
		else if (IsA(p, ProjectionPath))
			p = ((ProjectionPath *) p)->subpath;
		else
			break;
	}
	if (IsA(p, LimitPath))
	{
		LimitPath  *lp = (LimitPath *) p;

		if (lp->limitOffset != NULL || lp->limitCount == NULL || !IsA(lp->limitCount, Const) || DatumGetInt64(((Const *) lp->limitCount)->constvalue) != 1)
			return 0;
		first = true;
		for (p = lp->subpath; IsA(p, ProjectionPath);)
			p = ((ProjectionPath *) p)->subpath;
	}
	if (!IsA(p, IndexPath))
		return 0;
	ip = (IndexPath *) p;
	rel = ip->path.parent;
	if (ip->indexinfo->relam != BTREE_AM_OID || ip->indexorderbys != NIL || ip->indexscandir != ForwardScanDirection || jc->subroot == NULL)
		return 0;
	rpk = sift_agg_rows_per_key(planner_rt_fetch(rel->relid, jc->subroot)->relid, ip->indexinfo->indexkeys[0], rel->tuples);
	if (first)
	{
		/* the first match: as many entries as the key has where a condition the index does not take decides it */
		if (list_length(rel->baserestrictinfo) + (ip->path.param_info ? list_length(ip->path.param_info->ppi_clauses) : 0) > list_length(ip->indexclauses))
			entries = Max(rpk > 0 ? rpk : ip->path.rows, 1.0);
	}
	else
	{
		/* every entry of the key that the conditions on the index's other columns let through */
		List	   *own = NIL;

		foreach_node(IndexClause, ic, ip->indexclauses)
			if (ic->indexcol > 0)
				own = lappend(own, ic->rinfo);
		entries = Max(rpk > 0 ? rpk : ip->path.rows, 1.0) * clauselist_selectivity(jc->subroot, own, 0, JOIN_INNER, NULL);
		entries = Max(entries, 1.0);
	}
	return AGG_PROBE_RUN + entries * (ip->path.pathtype == T_IndexOnlyScan ? AGG_PROBE_SCAN : AGG_PROBE_FETCH);
}

/* is the child a lateral probe that the planner hook made of a sublink (agg/flat/probe.c)? */
static bool
probe_made(AggCtx *c, const JoinCtx *jc)
{
	RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);

	return rte->rtekind == RTE_SUBQUERY && rte->eref != NULL && strcmp(rte->eref->aliasname, SIFT_PROBE_ALIAS) == 0;
}

void
cost_joins(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		double		rows,
					row_cost;

		/* Correlated children run once per distinct input, with their native path's cost. */
		if (jc->j.params != NIL)
		{
			const SiftAggCombo *cb = &s->combo[jc->combo];
			SiftAggFirst *f = &s->join[foreach_current_index(jc)].first;
			double		probe = child_probe_ops(jc),
						run = probe > 0 ? probe * cpu_operator_cost : AGG_CHILD_RUN * (jc->cpath->total_cost + cpu_operator_cost * AGG_EVAL_SLOT);
			double		native = combo_entries(c, s, cb, k->agg_rows, false) * run;

			/*
			 * the child that is a lookup of one row by unique key: the table's rows read once, if that costs less than the child for each input; the
			 * inputs are counted by what the index holds of the elements and of the tuples the scan reads, as the rows PostgreSQL expects of the join
			 * (a few for a filter it knows nothing of) are not what the child runs for
			 */
			/* a lateral probe the planner made (agg/flat/probe.c) is priced by what a probe takes: the rows of its table are not loaded for it */
			f->use = f->ok && !probe_made(c, jc) && jc->frows * AGG_FIRST_ROW < combo_entries(c, s, cb, k->sel_rows * (s->group >= 0 ? k->elems : 1.0), true) * run;
			if (f->use)
			{
				k->load_rows += jc->frows;
				k->load_cost += jc->frows * AGG_FIRST_ROW;
			}
			else
				k->child_cost += native;
			continue;
		}
		rows = join_load_rows(c, s, jc, &row_cost);
		k->load_rows += rows;
		k->load_cost += rows * row_cost;
		if (jc->cpath != NULL)
			k->child_cost += jc->cpath->total_cost;
		if (jc->kind == SJ_SEMI || jc->kind == SJ_ANTI)
			k->scan_cost += k->sel_rows * AGG_SEMI_ROW;
	}
	k->scan_cost += k->load_cost;
}
