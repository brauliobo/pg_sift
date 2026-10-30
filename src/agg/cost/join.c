/*
 * agg/cost/join.c - what the joined tables and their lookups cost
 * The leaves a join adds, the lookup tables and their probes, the keys loaded and the
 * statement-level sum over the joins (cost_joins).
 */
#include "postgres.h"

#include <math.h>

#include "access/relation.h"
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
			p = ((SubqueryScanPath *) p)->subpath;
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

/* is the count of a table's lookup a default guess: a filter of it that PostgreSQL has no statistics for? (a CTE or sub-select is estimated through its own plan) */
bool
lookup_guessed(AggCtx *c, const JoinCtx *jc)
{
	return planner_rt_fetch(jc->rti, c->root)->rtekind == RTE_RELATION && !lookup_counted(c, jc);
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

/*
 * A lookup table that a dependent lookup keys is read by a btree probe for each of its `keys`: a descent each and a heap page for each of the `rows` they find (at most
 * the table's pages), all random pages. It pays while that costs less than the pages of the table at the page cost of PostgreSQL's own scan of them (as join_load_rows
 * prices a table read whole).
 */
bool
sift_agg_probe_pays(double keys, double rows, double pages)
{
	return (keys + Min(rows, pages)) * random_page_cost < pages * Min(seq_page_cost, random_page_cost);
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
 * The keys of a dependent lookup that the loader probes in a btree of the table (map_keyed_dependent and map_probe in agg/lookup/): an inner or semi join keyed by a
 * column of the table alone, or a child plan inner or semi joined on the same leaf of the scan, when a btree starts with the column and the probes cost less than its pages
 * (sift_agg_probe_pays). 0: the table is read whole. The keys are the rows PostgreSQL expects of the dependent lookup, taken as the geometric mean with the table's rows
 * when it has no statistics for them (join_key_zone).
 */
static double
join_probe_keys(AggCtx *c, SiftAggSpec *s, JoinCtx *jc, double *rows)
{
	RelOptInfo *jrel = c->root->simple_rel_array[jc->rti];
	const SiftAggCombo *cb = &s->combo[jc->combo];
	const SiftAggJoin *j = &jc->j;
	double		tuples = Max(jrel->tuples, 1.0),
				best = 0;

	*rows = 0;
	if (!sift_agg_lookup_pushdown || (j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || j->nidx > 0 || j->first.qual != NIL || j->first.nord > 0 || cb->level != 0)
		return 0;
	foreach_ptr(JoinCtx, dj, c->joins)
	{
		const SiftAggCombo *cc = &s->combo[dj->combo];
		const SiftAggJoin *d = &dj->j;
		int			att = 0,
					dkey;
		Oid			typ = InvalidOid;
		double		keys,
					rpk;
		bool		usable = false,
					unique = false;

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
		foreach_node(IndexOptInfo, ii, jrel->indexlist)
			if (ii->relam == BTREE_AM_OID && ii->indpred == NIL && !ii->hypothetical && ii->indexkeys[0] == att)
			{
				usable = true;
				unique |= ii->unique && ii->nkeycolumns == 1;
			}
		if (!usable)
			continue;
		rpk = sift_agg_rows_per_key(planner_rt_fetch(jc->rti, c->root)->relid, att, tuples);
		if (rpk <= 0 && !unique)
			continue;
		rpk = Max(rpk, 1.0);
		keys = lookup_rows(c, dj);
		keys = Min(!lookup_counted(c, dj) ? sqrt(keys * tuples) : keys, tuples);
		if (sift_agg_probe_pays(keys, keys * rpk, jrel->pages) && (best == 0 || keys < best))
		{
			best = keys;
			*rows = keys * rpk;
		}
	}
	return best;
}

/* the rows the scan loads of a lookup table, and what each costs: a plan is run whole; an index scan reads the rows that pass the equalities or the keys of a dependent lookup, the rest of a table is read whole */
static double
join_load_rows(AggCtx *c, SiftAggSpec *s, JoinCtx *jc, double *row_cost)
{
	PlannerInfo *root = c->root;
	RelOptInfo *jrel = root->simple_rel_array[jc->rti];
	double		tuples = Max(jrel->tuples, 1.0),
				probes,
				prows;

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
		*row_cost = AGG_IDX_ROW;
		return Min(Max(jrel->rows, 1.0), tuples);
	}
	if ((probes = join_probe_keys(c, s, jc, &prows)) > 0)
	{
		/* the descents and the pages of the rows found, as sift_agg_probe_pays counts them: the price meets that of the whole table where the rule turns */
		*row_cost = AGG_IDX_ROW + (probes + Min(prows, jrel->pages)) * random_page_cost / (Max(prows, 1.0) * cpu_operator_cost);
		return Max(prows, 1.0);
	}
	/* the pages of a table read whole cost what PostgreSQL's own sequential scan of them costs (a 1.5-million-row table: 11,000 of 15,000 more), at most a random fetch each */
	{
		Relation	rel = relation_open(root->simple_rte_array[jc->rti]->relid, NoLock);
		int			nnum = 0;
		bool		cols = map_load_cols_possible(rel, &jc->j, &nnum);

		relation_close(rel, NoLock);
		/* the pages of the heap are not read by a load from the leaves, whose bytes are in the price of the row */
		*row_cost = cols ? AGG_LOAD_ROW_COLS * pow(Max(tuples / AGG_LOAD_BIG, 1.0), 0.4) + nnum * AGG_LOAD_NUMERIC : (join_keys_only(c, s, jc) ? AGG_LOAD_KEY : AGG_LOAD_ROW) + jrel->pages * Min(seq_page_cost, random_page_cost) / (tuples * cpu_operator_cost);
	}
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
			double		run = AGG_CHILD_RUN * (jc->cpath->total_cost + cpu_operator_cost * AGG_EVAL_SLOT);
			double		native = combo_entries(c, s, cb, k->agg_rows, false) * run;

			/*
			 * the child that is a lookup of one row by unique key: the table's rows read once, if that costs less than the child for each input; the
			 * inputs are counted by what the index holds of the elements and of the tuples the scan reads, as the rows PostgreSQL expects of the join
			 * (a few for a filter it knows nothing of) are not what the child runs for
			 */
			f->use = f->ok && jc->frows * AGG_FIRST_ROW < combo_entries(c, s, cb, k->sel_rows * (s->group >= 0 ? k->elems : 1.0), true) * run;
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
