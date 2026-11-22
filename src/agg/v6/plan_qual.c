/*
 * agg/v6/plan_qual.c - the WHERE clauses of a SiftAggV6 statement: the quals of the index scan (a comparison, an IN list or a null test of one column with a constant or a parameter) and
 * the test that the index holds every row the statement reads.
 */
/* check-memory: allow-allocs every list here follows the quals of one statement, not the rows of the table */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/group/ht.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_qual.h"
#include "col/am/preds.h"

/* a constant, or a parameter the scan reads when it starts (and again when it is rescanned) */
static bool
operand_usable(const V6Plan *p, const Node *n)
{
	if (IsA(n, Param))
		return !p->in_filter && (((const Param *) n)->paramkind == PARAM_EXTERN || ((const Param *) n)->paramkind == PARAM_EXEC);
	return IsA(n, Const) && !((const Const *) n)->constisnull;
}

static bool
is_operand(const Node *n)
{
	return IsA(n, Const) || IsA(n, Param);
}

static Expr *
index_var(const Var *v, int col)
{
	return (Expr *) makeVar(INDEX_VAR, col + 1, v->vartype, v->vartypmod, v->varcollid, 0);
}

/* The strategy of a comparison of the column with a constant or parameter: of the operator class, or, on an index, <> and NOT LIKE '%literal%' (agg/v6/v6_qual.h) that only a scan outside a FILTER clause has; 0: not one the scan decides */
static int
compare_strategy(const V6Plan *p, const OpExpr *q, const SiftV6Col *col, const Node *operand)
{
	const bool	text = sift_v6_class(col->typid) == SV6C_TEXT;
	const Oid	family = sift_v6_plan_opfamily(p, col->attno);
	const bool	index = p->store == NULL;
	int			strat = get_op_opfamily_strategy(q->opno, family);

	if (strat == 0 && index && !p->in_filter)
		strat = sift_v6_qual_private_strategy(q->opno, family);
	if (text && !sift_text_bytes_equal(q->inputcollid))
		return 0;
	switch (strat)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			return text ? 0 : strat;
		case BTEqualStrategyNumber:
			return strat;
		case COL_STRAT_NE:
			return sift_v6_class(col->typid) == SV6C_F64 || xtime_pair(col->typid, exprType(operand)) ? 0 : strat;
		case COL_STRAT_LIKE:
		case COL_STRAT_NOT_LIKE:
			return index && text && IsA(operand, Const) && sift_v6_qual_like_exact((const Const *) operand) ? strat : 0;
		default:
			return 0;
	}
}

static Expr *
qual_compare(V6Plan *p, OpExpr *op)
{
	Node	   *l,
			   *r;
	OpExpr	   *q = copyObject(op);
	SiftV6Col	col;

	if (list_length(op->args) != 2)
		return NULL;
	l = linitial(q->args);
	r = lsecond(q->args);
	if (IsA(r, Var) && is_operand(l))
	{
		Node	   *t = l;

		q->opno = get_commutator(q->opno);
		if (!OidIsValid(q->opno))
			return NULL;
		q->opfuncid = get_opcode(q->opno);
		l = r;
		r = t;
		q->args = list_make2(l, r);
	}
	if (!sift_v6_plan_column(p, (Expr *) l, false, &col) || !operand_usable(p, r) || compare_strategy(p, q, &col, r) == 0)
		return NULL;
	linitial(q->args) = index_var((Var *) l, col.attno - 1);
	return (Expr *) q;
}

static Expr *
qual_in_list(V6Plan *p, ScalarArrayOpExpr *sa)
{
	ScalarArrayOpExpr *q = copyObject(sa);
	SiftV6Col	col;

	if (!sa->useOr || (p->index != NULL && !p->index->amsearcharray) || list_length(sa->args) != 2 || !IsA(lsecond(sa->args), Const) ||
		!sift_v6_plan_column(p, linitial(sa->args), false, &col) || ((Const *) lsecond(sa->args))->constisnull || sift_v6_class(col.typid) == SV6C_F64 ||
		get_op_opfamily_strategy(sa->opno, sift_v6_plan_opfamily(p, col.attno)) != BTEqualStrategyNumber ||
		(sift_v6_class(col.typid) == SV6C_TEXT && !sift_text_bytes_equal(sa->inputcollid)))
		return NULL;
	linitial(q->args) = index_var(linitial(sa->args), col.attno - 1);
	return (Expr *) q;
}

static Expr *
qual_null_test(V6Plan *p, NullTest *nt)
{
	NullTest   *q = copyObject(nt);
	SiftV6Col	col;

	if (nt->argisrow || !sift_v6_plan_column(p, nt->arg, true, &col))
		return NULL;
	q->arg = index_var((Var *) nt->arg, col.attno - 1);
	return (Expr *) q;
}

Expr *
sift_v6_index_qual(V6Plan *p, Expr *clause)
{
	switch (nodeTag(clause))
	{
		case T_OpExpr:
			return qual_compare(p, (OpExpr *) clause);
		case T_ScalarArrayOpExpr:
			return qual_in_list(p, (ScalarArrayOpExpr *) clause);
		case T_NullTest:
			return qual_null_test(p, (NullTest *) clause);
		default:
			return NULL;
	}
}

/* the clauses of the query imply the predicate of a partial index: every row the query reads is in the index */
bool
sift_v6_index_complete(const V6Plan *p)
{
	List	   *clauses = NIL;

	if (p->index == NULL || p->index->indpred == NIL)
		return true;
	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
		clauses = lappend(clauses, ri->clause);
	return predicate_implied_by(p->index->indpred, clauses, false);
}

bool
sift_v6_compile_quals(V6Plan *p)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
	{
		Expr	   *q;
		List	   *jq;

		if (p->index != NULL && p->index->indpred != NIL && predicate_implied_by(list_make1(ri->clause), p->index->indpred, false))
			continue;
		if (!ri->pseudoconstant && sift_v6_text_qual(p, ri))
			continue;
		q = ri->pseudoconstant ? NULL : sift_v6_index_qual(p, ri->clause);
		jq = ri->pseudoconstant || q != NULL || p->store != NULL ? NIL : sift_v6_json_qual(&rel, ri->clause);
		if (q == NULL && jq == NIL)
			return false;
		if (q != NULL)
			p->spec.indexqual = lappend(p->spec.indexqual, q);
		else
			p->spec.jsonqual = lappend(p->spec.jsonqual, jq);
	}
	return sift_v6_compile_text(p);
}
