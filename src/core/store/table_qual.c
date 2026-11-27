/*
 * core/store/table_qual.c - the quals of a sift_table its store decides as they are written (core/store/table_qual.h).
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"

#include "col/am.h"
#include "core/store/table_qual.h"

/* The types of a constant a sift_table's store compares an integer-like column with, whatever the width of the column (the other types are compared with a column of their own type) */
static bool
table_const_type(Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return true;
		default:
			return false;
	}
}

/* The types of a constant the store turns into a value of a predicate (col/am/preds.c): the integer-like and time types, the floats and numeric */
static bool
table_key_type(Oid typid)
{
	return table_const_type(typid) || typid == FLOAT4OID || typid == FLOAT8OID || typid == NUMERICOID;
}

static bool
table_holds(const SiftStoreSchema *t, AttrNumber attno)
{
	return attno > 0 && attno <= t->natts && t->att_first[attno] >= 0;
}

bool
sift_store_table_col_exact(const SiftStoreSchema *t, AttrNumber attno)
{
	const SiftColDef *d;

	if (!table_holds(t, attno))
		return false;
	d = &t->leaves->cols[t->att_first[attno]];
	return !(d->flags & SIFT_COLD_LOSSY) && !sift_col_def_is_wide(d) && d->store != SIFT_COLS_STR && d->type != SIFT_COLT_VARLENA && d->type != SIFT_COLT_TIMETZ;
}

static Node *
strip_relabel(Node *e)
{
	while (e && IsA(e, RelabelType))
		e = (Node *) ((RelabelType *) e)->arg;
	return e;
}

/* The attribute of the table a Var of the query's own level is whose comparisons the store decides, 0 for any other expression */
static AttrNumber
table_column(const SiftStoreSchema *t, Node *e)
{
	e = strip_relabel(e);
	if (e == NULL || !IsA(e, Var) || ((Var *) e)->varlevelsup != 0 || !sift_store_table_col_exact(t, ((Var *) e)->varattno))
		return 0;
	return ((Var *) e)->varattno;
}

static Expr *
table_compare(const SiftStoreSchema *t, OpExpr *op)
{
	OpExpr	   *q = copyObject(op);
	Node	   *l = linitial(q->args),
			   *r = lsecond(q->args);
	AttrNumber	att;
	int			strat;

	if (list_length(q->args) != 2)
		return NULL;
	if (IsA(r, Var) && IsA(l, Const))
	{
		q->opno = get_commutator(q->opno);
		if (!OidIsValid(q->opno))
			return NULL;
		q->opfuncid = get_opcode(q->opno);
		q->args = list_make2(r, l);
		l = r;
		r = lsecond(q->args);
	}
	att = table_column(t, l);
	if (att == 0 || !IsA(r, Const) || ((Const *) r)->constisnull || !table_key_type(((Const *) r)->consttype) ||
		!(((Const *) r)->consttype == exprType(l) || (table_const_type(((Const *) r)->consttype) && table_const_type(exprType(l)))))
		return NULL;
	strat = get_op_opfamily_strategy(q->opno, t->att_opfamily[att]);
	if (strat < BTLessStrategyNumber || strat > BTGreaterStrategyNumber)
		return NULL;
	linitial(q->args) = strip_relabel(l);
	return (Expr *) q;
}

/* An IN list of constants on a column of a sift_table: the store takes it as one array key of equalities */
static Expr *
table_in_list(const SiftStoreSchema *t, ScalarArrayOpExpr *sa)
{
	Node	   *l = linitial(sa->args),
			   *r = lsecond(sa->args);
	AttrNumber	att = table_column(t, l);

	if (!sa->useOr || att == 0 || !IsA(r, Const) || ((Const *) r)->constisnull || !table_key_type(exprType(l)) ||
		get_element_type(((Const *) r)->consttype) != exprType(l) ||
		get_op_opfamily_strategy(sa->opno, t->att_opfamily[att]) != BTEqualStrategyNumber)
		return NULL;
	sa = copyObject(sa);
	linitial(sa->args) = strip_relabel(l);
	return (Expr *) sa;
}

static Expr *
table_null_test(const SiftStoreSchema *t, NullTest *nt)
{
	if (nt->argisrow || table_column(t, (Node *) nt->arg) == 0)
		return NULL;
	nt = copyObject(nt);
	nt->arg = (Expr *) strip_relabel((Node *) nt->arg);
	return (Expr *) nt;
}

Expr *
sift_store_table_qual(const SiftStoreSchema *t, Expr *clause)
{
	switch (nodeTag(clause))
	{
		case T_OpExpr:
			return table_compare(t, (OpExpr *) clause);
		case T_ScalarArrayOpExpr:
			return table_in_list(t, (ScalarArrayOpExpr *) clause);
		case T_NullTest:
			return table_null_test(t, (NullTest *) clause);
		default:
			return NULL;
	}
}
