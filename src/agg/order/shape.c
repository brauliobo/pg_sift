/*
 * agg/order/shape.c - which statements a Top-N may take
 * Windows over the set, joins, array predicates and the runs and heads that decide
 * whether the scan can stop early.
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"

#include "agg/agg.h"
#include "agg/topn.h"
#include "agg/order/int.h"
#include "agg/lookup_int.h"

/* Is the window the whole set of rows: no PARTITION BY and no ORDER BY (every row is a peer of every other), and a frame that is not cut (the default one ends at the last peer) */
bool
window_whole_set(WindowClause *wc)
{
	return wc->partitionClause == NIL && wc->orderClause == NIL &&
		(!(wc->frameOptions & FRAMEOPTION_NONDEFAULT) ||
		 (wc->frameOptions & (FRAMEOPTION_START_UNBOUNDED_PRECEDING | FRAMEOPTION_END_UNBOUNDED_FOLLOWING | FRAMEOPTION_EXCLUSION)) ==
		 (FRAMEOPTION_START_UNBOUNDED_PRECEDING | FRAMEOPTION_END_UNBOUNDED_FOLLOWING));
}

/* The aggregate a window function calls, as the compile of the scan reads it */
static Aggref *
window_aggref(WindowFunc *wf)
{
	Aggref	   *a = makeNode(Aggref);

	a->aggfnoid = wf->winfnoid;
	a->aggtype = wf->wintype;
	a->aggcollid = wf->wincollid;
	a->inputcollid = wf->inputcollid;
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggstar = wf->winstar;
	a->aggfilter = wf->aggfilter;
	a->location = wf->location;
	foreach_ptr(Expr, arg, wf->args)
	{
		a->args = lappend(a->args, makeTargetEntry(arg, foreach_current_index(arg) + 1, NULL, false));
		a->aggargtypes = lappend_oid(a->aggargtypes, exprType((Node *) arg));
	}
	return a;
}

/*
 * The output of a top-N that computes its window functions has them as the aggregates the scan computes (the same in the scan tuple and in the node's
 * target list, which setrefs matches): ruleutils prints a WindowFunc only under a WindowAgg, and EXPLAIN (VERBOSE) of the node failed with "could not
 * find window clause for winref".
 */
Node *
topn_window_aggs(Node *n, void *context)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, WindowFunc))
		return (Node *) window_aggref((WindowFunc *) n);
	return expression_tree_mutator(n, topn_window_aggs, context);
}

/*
 * The window functions of the output are aggregates over the whole set of rows that pass (`count(*) OVER ()`, the page of a list and its total): the
 * scan computes them as it does an aggregate with no GROUP BY, from every row that passes, not only those of the heap, and the node gives the result
 * after the columns of the heap in every row. A window with PARTITION BY, ORDER BY or a frame that cuts the set, or a function that is not an
 * aggregate (row_number, rank, first_value, ...), keeps the normal plan.
 */
bool
topn_windows(AggCtx *c, SiftAggSpec *s, List *funcs, List **scan_tlist)
{
	s->key = palloc0(sizeof(SiftAggKey));
	s->agg = palloc0(Max(list_length(funcs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(WindowFunc, wf, funcs)
	{
		if (!wf->winagg)
			return agg_refuse_f(TOPN_SHAPE, "%s() OVER is not an aggregate: only aggregates over the whole set are computed with the top-N", get_func_name(wf->winfnoid));
		if (!compile_agg(c, window_aggref(wf), &s->agg[s->naggs], s->naggs))
			return agg_refuse_f(AGGREGATE, "%s OVER () is not an aggregate the scan computes over its arguments and filter", get_func_name(wf->winfnoid));
		*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) window_aggref(wf), list_length(*scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	return true;
}

/* Does the table of the join have a unique index on the join column alone (no two of its rows have the same key, a NULL key matches nothing)? */
static bool
topn_unique_key(AggCtx *c, JoinCtx *jc)
{
	RelOptInfo *dim = c->root->simple_rel_array[jc->rti];

	if (dim == NULL)
		return false;
	foreach_node(IndexOptInfo, ii, dim->indexlist)
		if (ii->unique && ii->immediate && ii->indpred == NIL && ii->indexprs == NIL && ii->nkeycolumns == 1 && ii->indexkeys[0] == jc->j.keyatt[0])
			return true;
	return false;
}

/*
 * The joins a top-N takes (agg/init_table.c, join_filters): the keys of the loaded table are a conjunct of the scan, `key IN (keys)`, which the rows of
 * the heap hold exactly, so nothing is left to join and no row can match twice. That is a semi join (IN, EXISTS), or an inner join of a table with a
 * unique index on its column, by one integer column of the scanned table, whose ON clause and WHERE read the table's columns alone. The table adds no column to the result (the
 * columns the query reads are those of the scanned table, topn_columns).
 */
bool
topn_joins_ok(AggCtx *c, SiftAggSpec *s)
{
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggCombo *cb = &s->combo[ci];
		const SiftAggJoin *j;
		JoinCtx    *jc;
		const SiftSrcLeaf *lf;

		if (cb->join < 0)
			continue;
		j = &s->join[cb->join];
		jc = list_nth(c->joins, cb->join);
		if ((j->kind != SJ_INNER && j->kind != SJ_SEMI && j->kind != SJ_ANTI) || j->params != NIL || j->nkey != 1 || j->keypos[0] != 0 || j->keynull[0] || cb->level != 0 ||
			cb->nleaves != 1 || cb->leaf[0] < 0 || cb->leaf[0] >= c->lay->nleaves || cb->nslots != (j->fslot >= 0 ? 1 : 0) ||
			(j->fslot >= 0 && slot_reads_leaf(cb, j->fslot)))
			return agg_refuse_on(TOPN_SHAPE, "a joined table is not an inner join by one key that the rows of the heap hold, with ON and WHERE on its own columns");
		lf = &c->lay->leaf[cb->leaf[0]];
		if (lf->kind != SCL_SCALAR || lf->parent >= 0 || (lf->vtype != SCV_I32 && lf->typid != INT8OID) || (j->kind == SJ_INNER && !topn_unique_key(c, jc)))
			return agg_refuse_f(TOPN_SHAPE, "the join key of %s is not an integer column of the scanned table, or the joined table has no unique index on its column", get_rel_name(jc->j.relid));
		/* the keys of an anti join are the conjunct `key NOT IN (keys)`: a row with no key is kept by the join, and dropped by the conjunct */
		if (j->kind == SJ_ANTI)
		{
			Node	   *k = list_nth(c->attexpr, lf->attno - 1);

			if (!IsA(k, Var) || ((Var *) k)->varno != c->relid || !agg_att_not_null(c->heapoid, ((Var *) k)->varattno))
				return agg_refuse_f(TOPN_SHAPE, "the anti join with %s is keyed by a column of the scanned table that may hold NULL", get_rel_name(jc->j.relid));
		}
	}
	return true;
}

/*
 * The columns of the joined tables that the select list reads (topn_columns) are in the scan tuple after the heap columns, filled from the rows the scan loaded
 * for each joined table when the node hands out a row (exec/topn.c): the key of the row is a heap column of the tuple (added when the output does not read it).
 * *nout: the heap columns, key columns included. *spec: for each column of a joined table its combination, its column there, and the place and type of the key.
 */
bool
topn_lookup_cols(AggCtx *c, SiftAggSpec *s, List *lookups, List **scan_tlist, int *attno, int *nout, List **spec)
{
	int		   *kpos = palloc(list_length(lookups) * sizeof(int));

	foreach_ptr(TopLookup, l, lookups)
	{
		const SiftAggCombo *cb = &s->combo[l->jc->combo];
		const SiftSrcLeaf *lf = &c->lay->leaf[cb->leaf[0]];
		Node	   *k = list_nth(c->attexpr, lf->attno - 1);
		int			i = foreach_current_index(l);

		if (!IsA(k, Var) || ((Var *) k)->varno != c->relid || (lf->typid != INT2OID && lf->typid != INT4OID && lf->typid != INT8OID))
			return agg_refuse_on(TOPN_SHAPE, "the key of a joined table whose columns the select list reads is not an integer column of the scanned table");
		for (kpos[i] = 0; kpos[i] < *nout; kpos[i]++)
			if (equal(list_nth_node(TargetEntry, *scan_tlist, kpos[i])->expr, k))
				break;
		if (kpos[i] == *nout)
		{
			*scan_tlist = list_insert_nth(*scan_tlist, *nout, makeTargetEntry((Expr *) copyObject(k), *nout + 1, NULL, false));
			attno[(*nout)++] = ((Var *) k)->varattno;
		}
	}
	foreach_ptr(TopLookup, l, lookups)
	{
		int			at = *nout + foreach_current_index(l);

		*scan_tlist = list_insert_nth(*scan_tlist, at, makeTargetEntry((Expr *) copyObject(l->var), at + 1, NULL, false));
		*spec = lappend_int(lappend_int(lappend_int(lappend_int(*spec, l->jc->combo), l->col), kpos[foreach_current_index(l)]),
							c->lay->leaf[s->combo[l->jc->combo].leaf[0]].typid);
	}
	foreach_ptr(TargetEntry, te, *scan_tlist)
		te->resno = foreach_current_index(te) + 1;
	return true;
}

/*
 * A condition the array of the key answers as sift_topn_gorder tests it (topn_gpred, gcond_loose): comparisons of integer or time columns with constants, the columns
 * other than the key only where the key or (at the top of the conjunction) the group is pinned by an equality. Any other comparison, IS [NOT] NULL or boolean column
 * is tested on the heap row of each entry the walk takes (*loose): the walk is worth its fetches only while the rows pass (topn_walk_fetches).
 */
static bool
topn_array_pred(AggCtx *c, const SiftAggPred *p, int key, int group, bool pinned, bool top, Oid collid, bool *loose)
{
	/* the walk tests integer, time and text columns (topn_gpred): a numeric one is left to the units */
	if (p->kind != SAP_AND && p->kind != SAP_OR && p->leaf >= 0 && p->leaf < c->lay->nleaves && leaf_of(c, p->leaf)->typid == NUMERICOID)
		return false;
	if (p->kind == SAP_AND || p->kind == SAP_OR)
	{
		for (int i = 0; p->kind == SAP_AND && i < p->nchild && !pinned; i++)
			pinned = p->child[i]->kind == SAP_CMP && p->child[i]->leaf == key && p->child[i]->op == SCMP_EQ;
		for (int i = 0; i < p->nchild; i++)
			if (!topn_array_pred(c, p->child[i], key, group, pinned, top && p->kind == SAP_AND, collid, loose))
				return false;
		return true;
	}
	if (p->kind == SAP_NULL && p->op <= 1 && p->level == 0 && !p->late && p->leaf < c->lay->nleaves && leaf_of(c, p->leaf)->parent < 0 && sift_topn_leaf_attno(c->lay, p->leaf) > 0)
	{
		*loose |= !(p->op == 1 && (p->leaf == key || pinned));
		return true;
	}
	if (p->kind == SAP_LUT && p->level == 0 && !p->late && p->leaf != key && p->leaf < c->lay->nleaves && sift_topn_bool_attno(c->lay, p->leaf) > 0)
	{
		*loose = true;
		return true;
	}
	/* a text key: its strings against a constant, under the collation the array ranks by (a string bound is found in the strings of each segment: sift_gord_open) */
	if (p->kind == SAP_LUT)
	{
		SiftSrcStrTest st = {0};

		if (!sift_aggspec_text_bound(p, c->lay, c->lay->nleaves - c->nvleaf, &st))
			return false;
		if (p->leaf == key)
			return st.collid == collid;
		/* the string of another column is tested on the heap row of each entry, under its own collation, as the equalities and the flags are */
		*loose = true;
		return p->level == 0 && !p->late && p->leaf < c->lay->nleaves - c->nvleaf && sift_topn_text_attno(c->lay, p->leaf) > 0;
	}
	/* the IN list of the group (as many elements as one scan reads at once: sift_topn_gorder, SIFT_GORD_PIN_MAX), tested by the heap row as the equalities are */
	if (p->kind == SAP_IN && p->op == 0 && p->level == 0 && !p->late && p->leaf < c->lay->nleaves && leaf_of(c, p->leaf)->parent < 0 &&
		sift_topn_same_scale(p->argtype, leaf_of(c, p->leaf)->typid))
		return (top && p->leaf == group) || p->leaf == key || pinned;
	if (p->kind != SAP_CMP || p->level != 0 || p->late || p->leaf >= c->lay->nleaves || leaf_of(c, p->leaf)->parent >= 0 ||
		!sift_topn_same_scale(p->argtype, leaf_of(c, p->leaf)->typid) || sift_topn_leaf_attno(c->lay, p->leaf) <= 0)
		return false;
	*loose |= !(p->leaf == key || pinned || (top && p->op == SCMP_EQ && p->leaf == group));
	return true;
}

/*
 * The heap rows a walk of the order array fetches for the n wanted when the heap row of each entry tests a condition on another column: n over the share of the
 * entries in the bounds of the key (and of the group the array is read by) that pass, which the statistics of the columns give as the rows of the table's
 * restrictions over the rows of those that name no other column.
 */
static double
topn_walk_fetches(AggCtx *c, int key, int group, double rows)
{
	RelOptInfo *rel = c->rel;
	Bitmapset  *want = NULL;
	List	   *bound = NIL;
	double		pass;

	want = bms_add_member(want, sift_topn_leaf_attno(c->lay, key) - FirstLowInvalidHeapAttributeNumber);
	if (group >= 0)
		want = bms_add_member(want, sift_topn_leaf_attno(c->lay, group) - FirstLowInvalidHeapAttributeNumber);
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
	{
		Bitmapset  *used = NULL;

		pull_varattnos((Node *) ri->clause, rel->relid, &used);
		if (bms_is_subset(used, want))
			bound = lappend(bound, ri);
	}
	pass = clauselist_selectivity(c->root, rel->baserestrictinfo, rel->relid, JOIN_INNER, NULL) / Max(clauselist_selectivity(c->root, bound, rel->relid, JOIN_INNER, NULL), 1e-9);
	return rows / Min(Max(pass, 1e-9), 1.0);
}

/* does the condition leave the key a lower or an upper bound (an AND does when a conjunct does, an OR when every arm does) */
static bool
topn_array_bounded(const SiftAggPred *p, int key)
{
	if (p->kind == SAP_NULL)
		return p->leaf == key && p->op == 1;
	if (p->kind == SAP_LUT)
		return p->leaf == key;
	if (p->kind == SAP_CMP)
		return p->leaf == key && p->op != SCMP_NE;
	for (int i = 0; i < p->nchild; i++)
		if (topn_array_bounded(p->child[i], key) == (p->kind == SAP_OR))
			return p->kind == SAP_AND;
	return p->kind == SAP_OR;
}

/*
 * Does the scan read the order array of the first key, not the units (sift_topn_gorder opens it for the shapes below and falls back to the units for any
 * other)? A key with an array in every segment, integer or time keys, and conditions the array answers: bounds on the key on a plain array, or an equality of the
 * column a grouped array is grouped by (the unpinned group of a grouped array is refused, the plain array of the leaf, if any, answers instead).
 * A condition on another column is tested on the heap row of each entry the walk takes (the walk gives the rows back to the units when it needs more fetches than
 * they cost: gord_giveup): the walk is priced when the fetches for the `rows` wanted, from the pass rate of the statistics, are at most four times the rows or a
 * quarter of the heap's pages, as the executor's budget is, and only for the rows of the page that the heap queues within work_mem (*walked: those fetches, 0 for an
 * array that answers alone).
 */
bool
topn_array_ok(AggCtx *c, SiftAggSpec *s, const int *leaf, const SiftTopXform *xf, int nkey, Oid collid, double rows, double *walked)
{
	int			gl = leaf[0] >= 0 ? c->stats.gord_group[leaf[0]] : -1;
	bool		bounded = false,
				key_pinned = false,
				group_pinned = false,
				loose = false;

	*walked = 0;
	if (leaf[0] < 0 || (c->stats.gord_cover[leaf[0]] < 0.9 && gl < 0) || c->search != NULL || c->nparts > 0 || s->naggs > 0 || !sift_aggspec_plain(s) ||
		nkey > 4)
		return false;
	for (int j = 0; j < nkey; j++)
	{
		Oid			typ = leaf[j] >= 0 ? leaf_of(c, leaf[j])->typid : InvalidOid;

		if (xf[j].kind != TXK_NONE || (typ != INT2OID && typ != INT4OID && typ != INT8OID && typ != DATEOID && typ != TIMESTAMPOID && typ != TIMESTAMPTZOID &&
									   !(typ == NUMERICOID && leaf_of(c, leaf[j])->vtype == SCV_I64 && !leaf_of(c, leaf[j])->wide) &&
									   !(j == 0 && nkey == 1 && (typ == TEXTOID || typ == VARCHAROID))))
			return false;
	}
	/* the strings of a text array are ranked by the collation of the column: a query that sorts by another reads the units; any other key has no collation */
	if (c->stats.gord_coll[leaf[0]] != (leaf_of(c, leaf[0])->typid == TEXTOID || leaf_of(c, leaf[0])->typid == VARCHAROID ? (uint32) collid : 0))
		return false;
	for (int i = 0; i < s->npred; i++)
		key_pinned |= s->pred[i]->kind == SAP_CMP && s->pred[i]->leaf == leaf[0] && s->pred[i]->op == SCMP_EQ;
	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];

		if (!topn_array_pred(c, p, leaf[0], gl, key_pinned, true, collid, &loose))
			return false;
		bounded |= topn_array_bounded(p, leaf[0]);
		group_pinned |= (p->kind == SAP_CMP && p->op == SCMP_EQ && p->leaf == gl) || (p->kind == SAP_IN && p->op == 0 && p->leaf == gl);
	}
	if (!loose)
		return (gl >= 0 && group_pinned) || (c->stats.gord_cover[leaf[0]] >= 0.9 && (s->npred == 0 || bounded));
	/* the walk queues the rows of the page before the first goes out (gord_prep), in work_mem, for one key and a page under the cap of the heap */
	if (nkey != 1 || rows > sift_limit(topn_rows, SIFT_TOPN_MAX_ROWS) || rows * SIFT_TOPN_GROW_BYTES > work_mem * 1024.0 ||
		!((gl >= 0 && group_pinned) || c->stats.gord_cover[leaf[0]] >= 0.9))
		return false;
	*walked = topn_walk_fetches(c, leaf[0], group_pinned ? gl : -1, rows);
	return *walked <= Max(4.0 * rows, rel_pages(c) / 4.0);
}

/*
 * Does the scan read each unit through its sorted run (run_unit): the first key is a plain integer or time column that has runs in the units. Comparisons of that key
 * bound the part of the run that is read; any other condition is tested on every row of the unit first (the mask the run keeps the rows of), which the price of the runs adds
 * (*resid).
 */
bool
topn_runs_ok(const AggCtx *c, const SiftAggSpec *s, const int *leaf, const SiftTopXform *xf, bool *resid)
{
	*resid = false;
	if (!sift_topn_runs || leaf[0] < 0 || xf[0].kind != TXK_NONE || c->stats.head_cover[leaf[0]] < 0.9 || c->search != NULL)
		return false;
	for (int i = 0; i < s->npred; i++)
		*resid |= s->pred[i]->leaf != leaf[0] || !(s->pred[i]->kind == SAP_CMP || (s->pred[i]->kind == SAP_NULL && s->pred[i]->op == 1));
	return true;
}

/*
 * Do the heads of the sorted runs answer the LIMIT for less than the order array? The head holds the best 32 keys at each end of a unit with the addresses of their rows, and a
 * key that follows the order of the heap (a zone walk reads about 8 units) reads a few of them where the array opens and decodes a chunk of each segment (ascending: 25 to
 * 30 thousand instructions more for 10 rows of 1,000,000 in 4 segments; descending, which cannot decode the end of a chunk without its start: 130 thousand). Past 32 rows
 * the heads are not enough and the run of a unit is read whole (100 rows: 1.2 M against 0.5 M instructions), and a scan with a condition reads no head.
 */
bool
topn_heads_win(const AggCtx *c, const SiftAggSpec *s, int leaf, double rows)
{
	return sift_topn_heads && sift_topn_walk && s->npred == 0 && c->stats.head_cover[leaf] >= 0.9 && rows <= SIFT_SRC_HEAD_MAX && c->topn_frac * Max(c->stats.nunits, 1.0) <= TOPN_HEADS_UNITS;
}
