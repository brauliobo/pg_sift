/*
 * agg/two/label.c - what EXPLAIN says of a two-level aggregate, made when it is planned: the outer keys in the terms of the first arm (an element of an unnested array as
 * doc.path[].member, a NULL column of the arms as NULL), the keys of the cells, and the outer aggregates by what they read of a cell (count(DISTINCT row) of the row id,
 * count(DISTINCT key) of a group expression, agg(inner) of an inner aggregate).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "agg/engine.h"
#include "agg/two/int.h"
#include "agg/two/plan_int.h"

static Node *
stripped(Node *e)
{
	while (IsA(e, RelabelType) || IsA(e, PlaceHolderVar))
		e = IsA(e, RelabelType) ? (Node *) ((RelabelType *) e)->arg : (Node *) ((PlaceHolderVar *) e)->phexpr;
	return e;
}

static const char *expr_label(PlannerInfo *sub, Node *e);

/* a column of an unnested array of a jsonb document: the document, the path of the array and the member */
static const char *
elem_label(PlannerInfo *sub, const RangeTblEntry *rte, const Var *v)
{
	const RangeTblFunction *f = list_length(rte->functions) == 1 ? linitial(rte->functions) : NULL;
	const FuncExpr *fe = f != NULL && IsA(f->funcexpr, FuncExpr) ? (const FuncExpr *) f->funcexpr : NULL;
	List	   *path;
	const Var  *doc = fe != NULL && fe->args != NIL ? sift_v6_doc_chain(linitial(fe->args), &path) : NULL;
	StringInfoData buf;

	if (doc == NULL)
		return psprintf("%s.%s", rte->eref->aliasname, strVal(list_nth(rte->eref->colnames, v->varattno - 1)));
	initStringInfo(&buf);
	appendStringInfoString(&buf, expr_label(sub, (Node *) doc));
	foreach_node(String, key, path)
		appendStringInfo(&buf, ".%s", strVal(key));
	appendStringInfo(&buf, "[].%s", strVal(list_nth(rte->eref->colnames, v->varattno - 1)));
	return buf.data;
}

static const char *
expr_label(PlannerInfo *sub, Node *e)
{
	const Var  *v;
	const RangeTblEntry *rte;

	e = stripped(e);
	if (IsA(e, Const))
		return ((Const *) e)->constisnull ? "NULL" : "(constant)";
	if (!IsA(e, Var) || (v = (const Var *) e)->varlevelsup != 0 || v->varattno <= 0)
		return "(expression)";
	rte = planner_rt_fetch(v->varno, sub);
	if (rte->rtekind == RTE_RELATION)
		return get_attname(rte->relid, v->varattno, false);
	if (rte->rtekind == RTE_FUNCTION)
		return elem_label(sub, rte, v);
	return psprintf("%s.%s", rte->eref->aliasname, strVal(list_nth(rte->eref->colnames, v->varattno - 1)));
}

/* the expression of the first arm that column (of the FROM item) of the outer expression e is, when e is a column */
static Node *
arm_col(const TwoPlan *t, const TwoArm *a, const Node *e)
{
	const Var  *v = IsA(e, Var) ? (const Var *) e : NULL;

	return v != NULL && v->varno == t->srti && v->varattno > 0 && v->varattno <= list_length(a->cols) ? list_nth(a->cols, v->varattno - 1) : NULL;
}

static void
add_label(StringInfo buf, const char *label)
{
	appendStringInfo(buf, "%s%s", buf->len ? ", " : "", label);
}

static const char *
keys_label(const TwoPlan *t)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (int i = 0; i < t->spec.nkeys; i++)
	{
		const Node *e = stripped((Node *) list_nth_node(TargetEntry, t->scan_tlist, i)->expr);
		Node	   *c = arm_col(t, &t->arm[0], e);
		const Var  *v = (const Var *) e;

		if (IsA(e, Var) && t->spine.rel != NULL && v->varno == t->spine.rel->relid)
			add_label(&buf, psprintf("spine.%s", get_rte_attribute_name(planner_rt_fetch(v->varno, t->root), v->varattno)));
		else
			add_label(&buf, c != NULL ? expr_label(t->arm[0].sub, c) : "(expression)");
	}
	if (t->spec.nsets > 0)
		add_label(&buf, "grouping set");
	return buf.data;
}

/* a group expression of arm a that is one of the outer keys */
static bool
outer_key(const TwoPlan *t, const TwoArm *a, const Node *g)
{
	for (int i = 0; i < t->spec.nkeys; i++)
	{
		const Node *c = arm_col(t, a, stripped((Node *) list_nth_node(TargetEntry, t->scan_tlist, i)->expr));

		if (c != NULL && equal(stripped((Node *) c), stripped((Node *) g)))
			return true;
	}
	return false;
}

static const char *
cells_label(const TwoPlan *t)
{
	StringInfoData buf;
	List	   *seen = NIL;

	initStringInfo(&buf);
	for (int k = 0; k < t->narms; k++)
	{
		const TwoArm *a = &t->arm[k];

		foreach_node(SortGroupClause, sgc, a->sub->parse->groupClause)
		{
			Node	   *g = (Node *) get_sortgroupclause_expr(sgc, a->sub->parse->targetList);
			const char *l;

			if (equal(g, a->id) || IsA(stripped(g), Const) || outer_key(t, a, g))
				continue;
			l = expr_label(a->sub, g);
			if (list_member(seen, makeString((char *) l)))
				continue;
			seen = lappend(seen, makeString((char *) l));
			add_label(&buf, l);
		}
	}
	return buf.data;
}

/* what column of the FROM item every arm has: the row id, a group expression, an aggregate (NULL arms aside) */
typedef enum CellRead
{
	CELL_ROW,
	CELL_KEY,
	CELL_INNER,
	CELL_EXPR
} CellRead;

static CellRead
cell_read(const TwoPlan *t, const Node *arg)
{
	CellRead	r = CELL_ROW;
	bool		any = false;

	arg = arg != NULL ? stripped((Node *) arg) : NULL;
	if (arg == NULL || !IsA(arg, Var))
		return CELL_EXPR;
	for (int k = 0; k < t->narms; k++)
	{
		const TwoArm *a = &t->arm[k];
		Node	   *c = arm_col(t, a, arg);
		bool		grouped = false;

		if (c == NULL)
			return CELL_EXPR;
		c = stripped(c);
		if (IsA(c, Const) && ((Const *) c)->constisnull)
			continue;
		any = true;
		foreach_node(SortGroupClause, sgc, a->sub->parse->groupClause)
			grouped |= equal(stripped((Node *) get_sortgroupclause_expr(sgc, a->sub->parse->targetList)), c);
		if (IsA(c, Aggref))
			return CELL_INNER;
		if (!equal(c, stripped(a->id)))
			r = grouped ? CELL_KEY : CELL_EXPR;
	}
	return any ? r : CELL_KEY;
}

static const char *
agg_label(const TwoPlan *t, const Node *n)
{
	const Aggref *a = (const Aggref *) n;
	const Node *arg;
	const char *name;
	CellRead	r;

	if (!IsA(n, Aggref))
		return "grouping()";
	if (a->aggstar)
		return "count(*)";
	arg = (Node *) linitial_node(TargetEntry, a->args)->expr;
	r = cell_read(t, arg);
	name = get_func_name(a->aggfnoid);
	if (name == NULL)
		return "(aggregate)";
	if (strcmp(name, "count") == 0 && a->aggdistinct == NIL && r == CELL_ROW)
		return "count(*)";
	if (strcmp(name, "count") == 0 && a->aggdistinct != NIL && (r == CELL_ROW || r == CELL_KEY))
		return r == CELL_ROW ? "count(DISTINCT row)" : "count(DISTINCT key)";
	return psprintf("%s(%s%s%s)", name, a->aggdistinct != NIL ? "DISTINCT " : "", r == CELL_INNER ? "inner" : r == CELL_EXPR ? "expression" : r == CELL_ROW ? "row" : "key",
					strcmp(name, "sum") == 0 && exprType(arg) == INT8OID ? " int8" : "");
}

static const char *
aggs_label(const TwoPlan *t)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (int j = 0; j < t->spec.naggs; j++)
		add_label(&buf, agg_label(t, (Node *) list_nth_node(TargetEntry, t->scan_tlist, t->spec.nkeys + j)->expr));
	return buf.data;
}

List *
sift_two_labels(const TwoPlan *t)
{
	return list_make3(makeString((char *) keys_label(t)), makeString((char *) cells_label(t)), makeString((char *) aggs_label(t)));
}
