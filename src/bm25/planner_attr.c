/*
 * bm25/planner_attr.c - which residual quals of a ranked scan the ranker can test on the INCLUDE columns of the index (bm25/attr.c): a comparison of a
 * column of an integer-like type with a constant, an IN list of constants, IS [NOT] NULL and a boolean column. The quals stay among the scan's own
 * quals: a doc that the segment holds no value of (a write buffer, a tuple gone) passes the test and is checked on the heap as always.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "catalog/pg_class_d.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "bm25/planner_int.h"
#include "bm25/segment.h"

#define MAX_IN_LIST		1024

static bool
int_type(Oid t)
{
	return t == BOOLOID || t == INT2OID || t == INT4OID || t == INT8OID || t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

static bool
date_type(Oid t)
{
	return t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

/* The position of the INCLUDE column that expr is, -1 when it is none */
static int
include_col(IndexOptInfo *ii, Node *expr, Oid *type)
{
	Var		   *var;

	while (expr && IsA(expr, RelabelType))
		expr = (Node *) ((RelabelType *) expr)->arg;
	if (expr == NULL || !IsA(expr, Var))
		return -1;
	var = (Var *) expr;
	if (var->varlevelsup != 0 || var->varno != ii->rel->relid || !int_type(var->vartype))
		return -1;
	for (int j = ii->nkeycolumns; j < ii->ncolumns && j - ii->nkeycolumns < SIFT_ATTR_MAXCOLS; j++)
		if (ii->indexkeys[j] == var->varattno)
		{
			*type = var->vartype;
			return j - ii->nkeycolumns;
		}
	return -1;
}

/* The comparison of strategy (of the btree family of the column's type), or <>, as the op of a test, -1 for another */
static int
cmp_op(Oid opno, Oid coltype)
{
	Oid			opclass = GetDefaultOpClass(coltype, BTREE_AM_OID);
	int			strat;

	if (!OidIsValid(opclass))
		return -1;
	strat = get_op_opfamily_strategy(opno, get_opclass_family(opclass));
	if (strat == 0)				/* <> is the negator of the btree equality */
	{
		Oid			neg = get_negator(opno);

		if (OidIsValid(neg) && get_op_opfamily_strategy(neg, get_opclass_family(opclass)) == BTEqualStrategyNumber)
			return SIFT_AT_NE;
	}
	switch (strat)
	{
		case BTLessStrategyNumber:
			return SIFT_AT_LT;
		case BTLessEqualStrategyNumber:
			return SIFT_AT_LE;
		case BTEqualStrategyNumber:
			return SIFT_AT_EQ;
		case BTGreaterEqualStrategyNumber:
			return SIFT_AT_GE;
		case BTGreaterStrategyNumber:
			return SIFT_AT_GT;
		default:
			return -1;
	}
}

static int
commute(int op)
{
	switch (op)
	{
		case SIFT_AT_LT:
			return SIFT_AT_GT;
		case SIFT_AT_LE:
			return SIFT_AT_GE;
		case SIFT_AT_GT:
			return SIFT_AT_LT;
		case SIFT_AT_GE:
			return SIFT_AT_LE;
		default:
			return op;
	}
}

static Const *
int8_const(int64 v)
{
	return makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(v), false, FLOAT8PASSBYVAL);
}

static List *
add_test(List *tests, int col, int op, int64 v, const int64 *set, int nset)
{
	tests = lappend(tests, int8_const(col));
	tests = lappend(tests, int8_const(op));
	tests = lappend(tests, int8_const(v));
	tests = lappend(tests, int8_const(nset));
	for (int i = 0; i < nset; i++)
		tests = lappend(tests, int8_const(set[i]));
	return tests;
}

static int
cmp_int64(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

/* The test of one qual, appended to tests; false when the qual is not one the ranker can test */
static bool
qual_test(IndexOptInfo *ii, Node *clause, List **tests)
{
	Oid			type;
	int			col;

	if (IsA(clause, Var) && (col = include_col(ii, clause, &type)) >= 0 && type == BOOLOID)
	{
		*tests = add_test(*tests, col, SIFT_AT_EQ, 1, NULL, 0);
		return true;
	}
	if (IsA(clause, BoolExpr) && ((BoolExpr *) clause)->boolop == NOT_EXPR)
	{
		Node	   *arg = linitial(((BoolExpr *) clause)->args);

		if (IsA(arg, Var) && (col = include_col(ii, arg, &type)) >= 0 && type == BOOLOID)
		{
			*tests = add_test(*tests, col, SIFT_AT_EQ, 0, NULL, 0);
			return true;
		}
		return false;
	}
	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		if (nt->argisrow || (col = include_col(ii, (Node *) nt->arg, &type)) < 0)
			return false;
		*tests = add_test(*tests, col, nt->nulltesttype == IS_NULL ? SIFT_AT_ISNULL : SIFT_AT_NOTNULL, 0, NULL, 0);
		return true;
	}
	if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) clause;
		Node	   *l = linitial(op->args),
				   *r = lsecond(op->args);
		int			code;
		Const	   *c;

		if ((col = include_col(ii, l, &type)) >= 0 && IsA(r, Const))
			c = (Const *) r, code = cmp_op(op->opno, type);
		else if ((col = include_col(ii, r, &type)) >= 0 && IsA(l, Const))
			c = (Const *) l, code = commute(cmp_op(op->opno, type));
		else
			return false;
		if (code < 0 || c->constisnull || !int_type(c->consttype) || (date_type(type) && c->consttype != type) || (type == BOOLOID) != (c->consttype == BOOLOID))
			return false;
		*tests = add_test(*tests, col, code, sift_attr_datum(c->consttype, c->constvalue), NULL, 0);
		return true;
	}
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) clause;
		Const	   *c;
		ArrayType  *arr;
		int16		len;
		bool		byval;
		char		align;
		Datum	   *elems;
		bool	   *nulls;
		int			n,
					m = 0;
		int64	   *set;

		if (!sa->useOr || list_length(sa->args) != 2 || (col = include_col(ii, linitial(sa->args), &type)) < 0 || !IsA(lsecond(sa->args), Const) ||
			cmp_op(sa->opno, type) != SIFT_AT_EQ)
			return false;
		c = (Const *) lsecond(sa->args);
		if (c->constisnull || !int_type(get_element_type(c->consttype)) || (date_type(type) && get_element_type(c->consttype) != type) ||
			(type == BOOLOID) != (get_element_type(c->consttype) == BOOLOID))
			return false;
		arr = DatumGetArrayTypeP(c->constvalue);
		get_typlenbyvalalign(ARR_ELEMTYPE(arr), &len, &byval, &align);
		deconstruct_array(arr, ARR_ELEMTYPE(arr), len, byval, align, &elems, &nulls, &n);
		if (n == 0 || n > MAX_IN_LIST)
			return false;
		set = palloc(n * sizeof(int64));
		for (int i = 0; i < n; i++)
			if (!nulls[i])
				set[m++] = sift_attr_datum(ARR_ELEMTYPE(arr), elems[i]);
		if (m == 0)
			return false;
		qsort(set, m, sizeof(int64), cmp_int64);
		*tests = add_test(*tests, col, SIFT_AT_IN, 0, set, m);
		return true;
	}
	return false;
}

List *
scan_attr_tests(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii, List *residual, double *sel, bool *all)
{
	List	   *tests = NIL,
			   *tested = NIL;

	if (ii->ncolumns <= ii->nkeycolumns)
		return NIL;
	foreach_node(RestrictInfo, rinfo, residual)
	{
		List	   *before = tests;

		if (contain_volatile_functions((Node *) rinfo->clause) || !qual_test(ii, (Node *) rinfo->clause, &tests))
			tests = before;
		else
			tested = lappend(tested, rinfo);
	}
	if (tested != NIL)
		*sel = clauselist_selectivity(root, tested, rel->relid, JOIN_INNER, NULL);
	*all = tested != NIL && list_length(tested) == list_length(residual);
	return tests;
}

/*
 * A table that an inner join keys to an INCLUDE column of the scan and that has quals of its own: every row the join keeps of the scan has a
 * key in the set the table's rows with those quals hold, so the scan may test the column against that set (SIFT_PRIV_SEMI; the join itself still
 * runs). Small tables only: the set is read when the scan starts.
 */

static bool
semi_expr_ok(Node *node, void *ctx)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) || IsA(node, SubPlan) || IsA(node, SubLink) || IsA(node, CurrentOfExpr))
		return true;
	return expression_tree_walker(node, semi_expr_ok, ctx);
}

static bool
add_semi(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii, Node *docs_side, Node *key_side, Oid opno, List **semi, Relids *rels, double *sel)
{
	Oid			type;
	int			col = include_col(ii, docs_side, &type);
	Var		   *kv;
	RelOptInfo *dim;
	RangeTblEntry *rte;
	List	   *quals = NIL;

	if (col < 0 || key_side == NULL || !IsA(key_side, Var) || cmp_op(opno, type) != SIFT_AT_EQ)
		return false;
	kv = (Var *) key_side;
	if (kv->varlevelsup != 0 || kv->varno == rel->relid || !int_type(kv->vartype) || (date_type(type) && kv->vartype != type) ||
		(type == BOOLOID) != (kv->vartype == BOOLOID) || bms_is_member(kv->varno, *rels))
		return false;
	dim = root->simple_rel_array[kv->varno];
	rte = root->simple_rte_array[kv->varno];
	if (dim == NULL || dim->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION || rte->inh || rte->relkind != RELKIND_RELATION ||
		dim->baserestrictinfo == NIL || dim->rows > SIFT_SEMI_MAX_SET || dim->tuples <= 0)
		return false;
	foreach_node(RestrictInfo, ri, dim->baserestrictinfo)
	{
		if (contain_volatile_functions((Node *) ri->clause) || semi_expr_ok((Node *) ri->clause, NULL))
			return false;
		quals = lappend(quals, copyObject(ri->clause));
	}
	*semi = lappend(*semi, makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(rte->relid), false, true));
	*semi = lappend(*semi, int8_const(col));
	*semi = lappend(*semi, int8_const(kv->varattno));
	*semi = lappend(*semi, quals);
	*rels = bms_add_member(*rels, kv->varno);
	*sel *= Min(1.0, dim->rows / dim->tuples);
	return true;
}

List *
scan_semi_tests(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii, Relids *rels, double *sel)
{
	List	   *semi = NIL;
	int			i = -1;

	*rels = NULL;
	*sel = 1.0;
	if (root->join_info_list != NIL || bms_membership(root->all_baserels) == BMS_SINGLETON)
		return NIL;
	while ((i = bms_next_member(rel->eclass_indexes, i)) >= 0)
	{
		EquivalenceClass *ec = list_nth_node(EquivalenceClass, root->eq_classes, i);
		Node	   *mine = NULL,
				   *other = NULL;
		Oid			opno = InvalidOid;

		if (ec->ec_has_const || ec->ec_broken || list_length(ec->ec_members) != 2)
			continue;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			Node	   *e = (Node *) em->em_expr;

			while (IsA(e, RelabelType))
				e = (Node *) ((RelabelType *) e)->arg;
			if (em->em_is_child || !IsA(e, Var))
				continue;
			if (((Var *) e)->varno == rel->relid)
				mine = e;
			else
				other = e;
		}
		if (mine == NULL || other == NULL)
			continue;
		opno = get_opfamily_member(linitial_oid(ec->ec_opfamilies), ((Var *) mine)->vartype, ((Var *) other)->vartype, BTEqualStrategyNumber);
		if (OidIsValid(opno))
			add_semi(root, rel, ii, mine, other, opno, &semi, rels, sel);
	}
	return semi;
}
