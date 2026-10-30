/*
 * agg/plan/quals.c - the quals and dependencies of a spec
 * Which quals the scan takes, which columns they miss, and what the spec depends on
 * (tables, statistics, parameters).
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "agg/plan/int.h"

typedef struct MissingCol
{
	AggCtx	   *c;
	AttrNumber	att;
} MissingCol;

/* the share of the heap pages the TID range of the conjuncts leaves, on the first of them (zone_kept) */
static void
tid_share(AggCtx *c, SiftAggSpec *s)
{
	uint64		lo,
				hi;

	if (!sift_aggspec_tid_range(s, &lo, &hi))
		return;
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TID)
		{
			s->pred[i]->sel = lo >= hi ? 0.0 : Min(((double) ((hi - 1) >> 16) - (double) (lo >> 16) + 1.0) / Max(rel_pages(c), 1.0), 1.0);
			return;
		}
}

/* a conjunct over the joined rows: a boolean slot of a combo, or an EXISTS over the elements of an array */
static SiftAggPred *
post_pred(AggCtx *c, Node *q)
{
	return IsA(q, SubPlan) ? compile_exists_post(c, (SubPlan *) q) : compile_combo_pred(c, normalize_expr(c, q));
}

/* the first column of the scanned table that a condition reads and the index does not hold */
static bool
missing_column_walker(Node *n, MissingCol *m)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var) && ((Var *) n)->varno == m->c->relid && ((Var *) n)->varlevelsup == 0)
	{
		foreach_ptr(Node, a, m->c->attexpr)
			if (a && IsA(a, Var) && ((Var *) a)->varattno == ((Var *) n)->varattno)
				return false;
		m->att = ((Var *) n)->varattno;
		return true;
	}
	return expression_tree_walker(n, missing_column_walker, m);
}

static bool
row_compare_walker(Node *n, void *ctx)
{
	return n != NULL && (IsA(n, RowCompareExpr) || expression_tree_walker(n, row_compare_walker, ctx));
}

/* the refusal of a condition the scan cannot test: it names the column of the table that the index lacks, when that is why (the tie column of a keyset comparison (a, id) > (x, y) says so) */
static bool
where_refused(AggCtx *c, Node *q)
{
	MissingCol	m = {c, InvalidAttrNumber};

	if (sift_explain_refusals && missing_column_walker(q, &m) && m.att > 0)
		return agg_refuse_f(WHERE, "the index does not hold the column \"%s\"%s", get_attname(planner_rt_fetch(c->relid, c->root)->relid, m.att, false),
							row_compare_walker(q, NULL) ? ": the tie column of a keyset comparison (a, id) > (x, y) is served from the index only when it is in it" : "");
	return agg_refuse(WHERE);
}

/* the WHERE of the scan: bitmap predicates, and the conjuncts over the joined rows */
bool
agg_quals(AggCtx *c, SiftAggSpec *s, FromScan *fs)
{
	List	   *quals = fs->quals,
			   *joined = NIL,
			   *scan_quals = fs->scan_quals,
			   *scan_post = NIL;
	StringInfoData txt;

	if (!setup_joins(c, &quals, &joined, &scan_quals, &scan_post))
		return agg_refuse(JOIN_CONDITION);
	quals = list_concat(quals, scan_quals);
	s->pred = palloc(Max(list_length(quals), 1) * ARRAY_EQ_CONJ * sizeof(SiftAggPred *));
	s->post = palloc(Max(list_length(quals) + list_length(joined) + list_length(scan_post), 1) * sizeof(SiftAggPred *));
	initStringInfo(&txt);
	foreach_ptr(Node, q, quals)
	{
		Node	   *e = normalize_expr(c, q);
		bool		complete;
		List	   *pre = array_eq_conjuncts(c, e, &complete);
		SiftAggPred *p = NULL;

		if (!complete && (p = compile_typeof(c, e)) == NULL)
			p = compile_wide(c, e);
		if (!complete && p == NULL)
			p = compile_tid(c, e);
		if (!complete && p == NULL)
			p = compile_pred(c, e);
		if ((!complete && p == NULL) || unsupported_clause(c, q))
			return where_refused(c, q);
		foreach_ptr(SiftAggPred, f, pre)
		{
			s->pred[s->npred++] = f;
			appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", f->desc);
		}
		if (p == NULL)
			continue;
		if (p->level > 1)
			s->post[s->npost++] = p;
		else
			s->pred[s->npred++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	foreach_ptr(Node, q, scan_post)
	{
		SiftAggPred *p = post_pred(c, q);

		if (p == NULL || unsupported_clause(c, q))
			return where_refused(c, q);
		s->post[s->npost++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->nprepost = preserved_join(c) ? s->npost : 0;
	foreach_ptr(Node, q, joined)
	{
		SiftAggPred *p = post_pred(c, q);

		if (p == NULL || unsupported_clause(c, q))
			return where_refused(c, q);
		s->post[s->npost++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->desc_quals = txt.data;
	tid_share(c, s);
	return true;
}

void
spec_deps(PlannerInfo *root, SiftAggSpec *s)
{
	if (s->search)
		record_deps(root, s->search);
	for (int i = 0; i < s->npred; i++)
		pred_deps(root, s->pred[i]);
	for (int i = 0; i < s->nkeys; i++)
		record_deps(root, s->key[i].expr);
	for (int i = 0; i < s->naggs; i++)
		pred_deps(root, s->agg[i].filter);
	for (int i = 0; i < s->ncombo; i++)
		for (int k = 0; k < s->combo[i].nslots; k++)
			record_deps(root, s->combo[i].slot[k].expr);
	for (int i = 0; i < s->njoin; i++)
		for (int k = 0; k < s->join[i].nkey; k++)
		{
			record_deps(root, s->join[i].keyexpr[k]);
			record_deps(root, s->join[i].tkey[k]);
		}
	for (int a = 0; a < s->narms; a++)
	{
		for (int i = 0; i < s->arm[a].nfilter; i++)
			pred_deps(root, s->arm[a].filter[i]);
		for (int i = 0; i < s->arm[a].ninner; i++)
			pred_deps(root, s->arm[a].inner[i].filter);
	}
}

/* what PostgreSQL has statistics of for a key: the column behind an expression that denotes a stored generated column, else the expression */
Node *
stats_expr(AggCtx *c, Node *e)
{
	int			att = attr_of_expr(c, e);

	return att > 0 ? list_nth(c->attexpr, att - 1) : e;
}
