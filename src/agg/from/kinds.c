/*
 * agg/from/kinds.c - what a column of a scan's source is
 * The kind of a function leaf, the leaf a group key reads, and the variables of a partition
 * mapped to its parent.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "utils/fmgroids.h"

#include "agg/part.h"
#include "agg/from/int.h"

FuncKind
func_kind_of(RangeTblFunction *f)
{
	FuncExpr   *fe;

	if (!IsA(f->funcexpr, FuncExpr))
		return FK_NONE;
	fe = (FuncExpr *) f->funcexpr;
	switch (fe->funcid)
	{
		case F_JSONB_TO_RECORDSET:
			return FK_RECORDSET;
		case F_JSONB_ARRAY_ELEMENTS:
			return FK_ELEMENTS;
		case F_JSONB_ARRAY_ELEMENTS_TEXT:
			return FK_ELEMENTS_TEXT;
		case F_JSONB_EACH:
			return FK_EACH;
		case F_JSONB_EACH_TEXT:
			return FK_EACH_TEXT;
		case F_UNNEST_ANYARRAY:
			return FK_UNNEST;
		default:
			return FK_NONE;
	}
}

/* the kind of group leaf a set-returning function reads */
SiftSrcLeafKind
func_leaf_kind(FuncKind k)
{
	return k == FK_EACH || k == FK_EACH_TEXT ? SCL_OBJECT : SCL_ARRAY;
}

/* the group leaf of the given kind the keys below an expression lead to: the jsonb array (object) leaf an unnested function reads */
int
group_leaf_below(AggCtx *c, Node *arg, List *keys, SiftSrcLeafKind kind)
{
	JsonRef		r = {0};

	if (!json_access(c, arg, &r) || r.as_text || r.att == 0)
		return -1;
	r.path = list_concat_copy(r.path, keys);
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == r.att && l->parent < 0 && paths_equal(l->path, r.path) && l->kind == kind)
			return i;
	}
	return -1;
}

int
group_leaf_of(AggCtx *c, Node *arg, SiftSrcLeafKind kind)
{
	return group_leaf_below(c, arg, NIL, kind);
}

/* the attribute of the parent that has the partition's attribute att (0: none) */
static AttrNumber
parent_attno(AppendRelInfo *ai, AttrNumber att)
{
	foreach_ptr(Node, tv, ai->translated_vars)
		if (tv != NULL && IsA(tv, Var) && ((Var *) tv)->varattno == att)
			return foreach_current_index(tv) + 1;
	return 0;
}

/* A column of a partition as the column of the partitioned table, through every level (the attribute numbers differ after a dropped column) */
AttrNumber
part_attno(PartVars *pv, AttrNumber att)
{
	for (Index r = pv->rti; r != pv->top && att > 0; r = pv->root->append_rel_array[r]->parent_relid)
		att = parent_attno(pv->root->append_rel_array[r], att);
	return att;
}

Node *
part_vars_mutator(Node *n, PartVars *pv)
{
	Var		   *v;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Var))
		return expression_tree_mutator(n, part_vars_mutator, pv);
	v = copyObject((Var *) n);
	v->varattno = part_attno(pv, v->varattno);
	pv->ok &= v->varattno > 0;
	return (Node *) v;
}
