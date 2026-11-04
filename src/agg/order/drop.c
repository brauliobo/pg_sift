/*
 * agg/order/drop.c - a Sort the scan below it can do without, the OFFSET of a Limit it takes over
 *
 * A scan under a window emits its groups in the order of the window (agg/plan_ord.c), but an ORDER BY above the window (of a sub-select or a CTE that
 * keeps the rows of one period) is another order and PostgreSQL puts a Sort there, of rows that are all there is. PostgreSQL asks the scan for one order
 * only, the pathkeys of the level it plans, and the Sort's keys are not pathkeys of that level (a sub-select's output, an expression of a column). In the finished plan the
 * chain from the Sort down to the scan is known: sub-selects that pass the columns through, and at most one window. The window needs its partitions
 * together and the rows of a partition in its own order, so a scan that emits the Sort's keys, then the rest of the partition keys, then the window's
 * keys, serves both; the Sort goes. The rows a filter above the window drops leave the others in the Sort's order.
 *
 * A top-N scan that is the output as it is skips the OFFSET rows without reading them (TOPN_SKIP_OFFSET): the Limit above it has no OFFSET to throw
 * them away, so the scan neither returns nor makes a row for each.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/spec.h"
#include "agg/topn.h"

static Node *
unrelabel(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

/* a column of the scan's result: the key it is, and the constant a NULL key stands for (COALESCE(key, constant)) */
typedef struct Col
{
	int			key;
	Const	   *subst;
} Col;

/* column attno of the node's result as a column of the scan, through sub-selects and windows that pass it on; false: it is not one */
static bool
column_key(Plan *p, AttrNumber attno, Col *col)
{
	for (;;)
	{
		TargetEntry *tle;
		Node	   *e;
		Var		   *v;

		if (attno < 1 || attno > list_length(p->targetlist))
			return false;
		tle = list_nth_node(TargetEntry, p->targetlist, attno - 1);
		e = unrelabel((Node *) tle->expr);
		col->subst = NULL;
		if (IsA(p, CustomScan) && IsA(e, CoalesceExpr))
		{
			CoalesceExpr *ce = (CoalesceExpr *) e;

			if (list_length(ce->args) != 2 || !IsA(lsecond(ce->args), Const) || ((Const *) lsecond(ce->args))->constisnull)
				return false;
			col->subst = (Const *) lsecond(ce->args);
			e = unrelabel((Node *) linitial(ce->args));
		}
		v = (Var *) e;
		if (!IsA(v, Var) || v->varlevelsup != 0)
			return false;
		if (IsA(p, CustomScan))
		{
			col->key = v->varattno - 1;
			return v->varno == INDEX_VAR;
		}
		if (IsA(p, SubqueryScan) && v->varno == ((SubqueryScan *) p)->scan.scanrelid)
			p = ((SubqueryScan *) p)->subplan;
		else if (IsA(p, WindowAgg) && v->varno == OUTER_VAR)
			p = p->lefttree;
		else
			return false;
		attno = v->varattno;
	}
}

/* the scan column that a Var of the node's input is */
static bool
input_key(Plan *p, Var *v, Col *col)
{
	if (IsA(p, SubqueryScan) && v->varno == ((SubqueryScan *) p)->scan.scanrelid)
		return column_key(((SubqueryScan *) p)->subplan, v->varattno, col);
	if (IsA(p, WindowAgg) && v->varno == OUTER_VAR)
		return column_key(p->lefttree, v->varattno, col);
	return false;
}

static bool
col_is(const SiftAggOrd *o, const Col *c)
{
	return o->key == c->key && o->eq == NULL && equal(o->subst, c->subst);
}

static bool
sift_scan(Plan *p)
{
	return IsA(p, CustomScan) && ((CustomScan *) p)->methods == &sift_agg_plan_methods;
}

static int
ord_find(const SiftAggSpec *s, const Col *c)
{
	for (int i = 0; i < s->nord; i++)
		if (col_is(&s->ord[i], c))
			return i;
	return -1;
}

/* the keys of the scan that are outer group keys: the ones a result column can be */
static int
outer_keys(const SiftAggSpec *s)
{
	return s->nkeys - s->nhidden - (s->nsets > 0);
}

/*
 * The Sort's column i as an entry of the scan's order: the key it is, or the key compared with a constant (a flag such as category = 'overall'), with the
 * sort operator, collation and nulls order the Sort compares by. False: it is neither.
 */
static bool
sort_entry(Sort *srt, int i, const SiftAggSpec *s, SiftAggOrd *o)
{
	Plan	   *in = srt->plan.lefttree;
	AttrNumber	attno = srt->sortColIdx[i];
	TargetEntry *tle;
	Node	   *e;
	Col			col;

	if (attno < 1 || attno > list_length(in->targetlist))
		return false;
	tle = list_nth_node(TargetEntry, in->targetlist, attno - 1);
	e = unrelabel((Node *) tle->expr);
	memset(o, 0, sizeof(*o));
	o->sortop = srt->sortOperators[i];
	o->collation = srt->collations[i];
	o->nulls_first = srt->nullsFirst[i];
	if (IsA(e, Var))
	{
		if (!column_key(in, attno, &col))
			return false;
	}
	else if (IsA(e, OpExpr) && list_length(((OpExpr *) e)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) e;
		Node	   *l = unrelabel(linitial(op->args)),
				   *r = unrelabel(lsecond(op->args));
		if (!IsA(l, Var) || !IsA(r, Const) || ((Const *) r)->constisnull)
			return false;
		/* a merge join operator is the equality of a btree operator class */
		if (!op_mergejoinable(op->opno, exprType(l)))
			return false;
		if (!input_key(in, (Var *) l, &col))
			return false;
		o->eq = (Const *) r;
		o->eqop = op->opno;
		o->eqcoll = op->inputcollid;
	}
	else
		return false;
	o->key = col.key;
	o->subst = col.subst;
	return col.key >= 0 && col.key < outer_keys(s);
}

static bool
ord_append(SiftAggOrd *ord, int *n, const SiftAggOrd *o)
{
	if (*n >= SIFT_AGG_MAX_KEYS)
		return false;
	ord[(*n)++] = *o;
	return true;
}

static bool
ord_has(const SiftAggOrd *ord, int n, const Col *c)
{
	for (int i = 0; i < n; i++)
		if (col_is(&ord[i], c))
			return true;
	return false;
}

/*
 * The scan under the Sort, if the chain between them lets the Sort go: the new order of its groups is built in ord (n entries) and the windows it passes are
 * counted. NULL: the Sort stays.
 */
static CustomScan *
sort_scan(Sort *srt, SiftAggOrd *ord, int *n)
{
	Plan	   *p = srt->plan.lefttree;
	WindowAgg  *win = NULL;
	CustomScan *cs;
	SiftAggSpec *s;
	SiftAggOrd	o;

	while (!sift_scan(p))
	{
		if (IsA(p, SubqueryScan))
			p = ((SubqueryScan *) p)->subplan;
		else if (IsA(p, WindowAgg) && win == NULL)
		{
			win = (WindowAgg *) p;
			p = p->lefttree;
		}
		else
			return NULL;
	}
	cs = (CustomScan *) p;
	s = sift_aggspec_unpack(cs->custom_private);
	/* the scan orders its groups already (the planner knew they fit its memory), and it is not a top-N, a spilling or a partitioned scan */
	if (s->nord == 0 || s->topn != NULL || s->gtopn_k > 0 || s->part_final)
		return NULL;
	*n = 0;
	for (int i = 0; i < srt->numCols; i++)
		if (!sort_entry(srt, i, s, &o) || !ord_append(ord, n, &o))
			return NULL;
	if (win != NULL)
	{
		Plan	   *in = win->plan.lefttree;
		int			nsort = *n;
		Col			parts[SIFT_AGG_MAX_KEYS];

		/* a window with no ORDER BY gives its rows to the functions in any order: the plan's own is as good as ours, but the answer is not ours to change */
		if (win->partNumCols > SIFT_AGG_MAX_KEYS || win->ordNumCols == 0)
			return NULL;
		for (int i = 0; i < win->partNumCols; i++)
			if (!column_key(in, win->partColIdx[i], &parts[i]) || parts[i].key >= outer_keys(s))
				return NULL;
		/* every column the Sort orders by is a partition column: the rows of a partition stay together whatever follows */
		for (int i = 0; i < nsort; i++)
		{
			bool		part = false;

			for (int j = 0; j < win->partNumCols; j++)
				part |= ord[i].key == parts[j].key && equal(ord[i].subst, parts[j].subst);
			if (!part)
				return NULL;
		}
		/* the rest of the partition columns, then the window's own, in the order and direction the scan had them */
		for (int j = 0; j < win->partNumCols; j++)
		{
			int			at = ord_find(s, &parts[j]);

			if (ord_has(ord, *n, &parts[j]))
				continue;
			if (at < 0 || !ord_append(ord, n, &s->ord[at]))
				return NULL;
		}
		for (int i = 0; i < win->ordNumCols; i++)
		{
			Col			c;
			int			at;

			if (!column_key(in, win->ordColIdx[i], &c) || (at = ord_find(s, &c)) < 0 || ord_has(ord, *n, &c) || !ord_append(ord, n, &s->ord[at]))
				return NULL;
		}
	}
	return cs;
}

static Plan *drop_sorts(Plan *p);

/* the scan under a Limit skips its OFFSET, so the Limit has none; else the scan returns those rows for the Limit to throw away */
static void
take_offset(Limit *lim)
{
	Plan	   *in = lim->plan.lefttree;
	CustomScan *cs = (CustomScan *) in;
	SiftAggSpec *s;

	if (lim->limitOffset == NULL || !sift_scan(in))
		return;
	s = sift_aggspec_unpack(cs->custom_private);
	if (s->topn == NULL)
		return;
	if (lim->limitOption == LIMIT_OPTION_COUNT && sift_topn_skips_offset(s->topn, lim->limitOffset))
	{
		lim->limitOffset = NULL;
		in->plan_rows = lim->plan.plan_rows;
	}
	else
		sift_topn_keep_offset(s->topn);
	cs->custom_private = sift_aggspec_pack(s);
}

/* the Sort goes when the scan below it can emit its order: the plan under it, or NULL */
static Plan *
drop_sort(Sort *srt)
{
	SiftAggOrd	ord[SIFT_AGG_MAX_KEYS];
	int			n = 0;
	CustomScan *cs = sort_scan(srt, ord, &n);
	SiftAggSpec *s;

	if (cs == NULL)
		return NULL;
	s = sift_aggspec_unpack(cs->custom_private);
	s->ord = memcpy(palloc(n * sizeof(SiftAggOrd)), ord, n * sizeof(SiftAggOrd));
	s->nord = n;
	cs->custom_private = sift_aggspec_pack(s);
	return srt->plan.lefttree;
}

static List *
drop_sorts_list(List *plans)
{
	ListCell   *lc;

	foreach(lc, plans)
		lfirst(lc) = drop_sorts(lfirst(lc));
	return plans;
}

static Plan *
drop_sorts(Plan *p)
{
	Plan	   *below;

	if (p == NULL)
		return NULL;
	p->lefttree = drop_sorts(p->lefttree);
	p->righttree = drop_sorts(p->righttree);
	switch (nodeTag(p))
	{
		case T_SubqueryScan:
			((SubqueryScan *) p)->subplan = drop_sorts(((SubqueryScan *) p)->subplan);
			break;
		case T_Append:
			drop_sorts_list(((Append *) p)->appendplans);
			break;
		case T_MergeAppend:
			drop_sorts_list(((MergeAppend *) p)->mergeplans);
			break;
		case T_CustomScan:
			drop_sorts_list(((CustomScan *) p)->custom_plans);
			break;
		case T_Sort:
			if ((below = drop_sort((Sort *) p)) != NULL)
				return below;
			break;
		case T_Limit:
			take_offset((Limit *) p);
			break;
		default:
			break;
	}
	return p;
}

void
sift_agg_drop_sorts(PlannedStmt *stmt)
{
	stmt->planTree = drop_sorts(stmt->planTree);
	drop_sorts_list(stmt->subplans);
}
