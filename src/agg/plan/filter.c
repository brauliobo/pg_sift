/*
 * agg/plan/filter.c - the FILTER (WHERE ..) clause of an aggregate of SiftAggV6: the scan decides it on the stored values, or PostgreSQL's own functions decide it over the datums of its columns.
 */
/* check-memory: allow-allocs every list here follows the clauses of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_operator_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/plan/int.h"
#include "agg/filter/pg.h"
#include "agg/qual/qual.h"
#include "agg/unnest/jsonb.h"

/*
 * `doc ? 'k'`, or IS TRUE or IS NOT TRUE of it, of a key whose presence the scan reads: the test of the presence. A SQL NULL document has not the key, where `?` is NULL: the same to a FILTER, which
 * keeps neither, but not to NOT, which stays the query's
 */
static Expr *
presence_qual(V6Plan *p, Expr *c)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);
	const BooleanTest *bt = IsA(c, BooleanTest) ? (const BooleanTest *) c : NULL;
	const bool	has = bt == NULL || bt->booltesttype == IS_TRUE;
	const int	attno = p->xseed == NULL && (has || bt->booltesttype == IS_NOT_TRUE) ? sift_v6_json_presence(&rel, bt != NULL ? bt->arg : c) : InvalidAttrNumber;
	Var		   *v = makeVar(p->rel->relid, attno, BOOLOID, -1, InvalidOid, 0);

	if (attno == InvalidAttrNumber)
		return NULL;
	return sift_v6_index_qual(p, make_opclause(BooleanEqualOperator, BOOLOID, false, (Expr *) v, (Expr *) makeBoolConst(has, false), InvalidOid, InvalidOid));
}

/* a conjunct of a FILTER: over the columns of the element, as a qual of the rows of the scan; any other over the columns of the index */
static Expr *
filter_qual(V6Plan *p, Expr *c)
{
	const bool	on_elem = p->elem_rel != NULL && bms_is_member(p->elem_rel->relid, pull_varnos(p->root, (Node *) c));
	Expr	   *q = on_elem ? sift_v6_unnest_elem_qual(p, (Node *) c) : NULL;

	if (q != NULL || on_elem)
		return q;
	return (q = presence_qual(p, c)) != NULL ? q : sift_v6_index_qual(p, sift_v6_plan_json_vars(p, c, false));
}

/* a boolean column, or NOT of one, as its comparison with true or false: the forms the quals of a scan take (a NULL passes neither) */
static Expr *
bool_compare(Expr *c)
{
	const bool	negated = is_notclause(c);
	Expr	   *arg = negated ? get_notclausearg(c) : c;

	if (!IsA(arg, Var) || ((const Var *) arg)->vartype != BOOLOID)
		return c;
	return make_opclause(BooleanEqualOperator, BOOLOID, false, arg, (Expr *) makeBoolConst(!negated, false), InvalidOid, InvalidOid);
}

static Expr *conjunct_qual(V6Plan *p, Expr *conjunct);

/* an OR of conjuncts the scan decides: the OR of their quals */
static Expr *
or_qual(V6Plan *p, const BoolExpr *or)
{
	List	   *arms = NIL;

	foreach_ptr(Expr, arm, or->args)
	{
		Expr	   *q = conjunct_qual(p, arm);

		if (q == NULL)
			return NULL;
		arms = lappend(arms, q);
	}
	return makeBoolExpr(OR_EXPR, arms, -1);
}

/* a conjunct of a FILTER as a qual the scan decides on the stored values: a test of the elements of the row, a qual of its columns, or an OR of those (NULL: none) */
static Expr *
conjunct_qual(V6Plan *p, Expr *conjunct)
{
	Expr	   *c = bool_compare(conjunct);
	Expr	   *q = sift_v6_unnest_test_qual(p, (Node *) c);

	if (q != NULL)
		return q;
	return is_orclause(c) ? or_qual(p, (const BoolExpr *) c) : filter_qual(p, c);
}

Expr *
sift_v6_tests_or_qual(V6Plan *p, Expr *clause)
{
	Expr	   *q;

	if (p->tests == NIL || !is_orclause(clause))
		return NULL;
	p->in_filter = true;
	q = or_qual(p, (const BoolExpr *) clause);
	p->in_filter = false;
	return q;
}

/* the conjuncts of the clause the scan decides on the stored values (NIL: one is not of those) */
static List *
native_filter_quals(V6Plan *p, Expr *filter)
{
	List	   *quals = NIL;

	p->in_filter = true;
	foreach_ptr(Expr, conjunct, make_ands_implicit(filter))
	{
		Expr	   *q = conjunct_qual(p, conjunct);

		if (q == NULL)
		{
			quals = NIL;
			break;
		}
		quals = lappend(quals, q);
	}
	p->in_filter = false;
	return quals;
}

/* the clause as a boolean PostgreSQL's own functions compute over the datums of its columns: the test that the derived column is true */
static List *
derived_filter_quals(V6Plan *p, Expr *filter)
{
	SiftV6Col	col;
	BooleanTest *bt;
	int			i = 0;

	if (exprType((Node *) filter) != BOOLOID || !sift_v6_pg_derived_col(p, filter, false, &col))
		return NIL;
	while (i < p->spec.nfcols && !sift_v6_col_same(&p->spec.fcol[i], &col))
		i++;
	if (i == p->spec.nfcols && p->spec.nfcols == SIFT_V6_MAX_FCOLS)
		return NIL;
	p->spec.fcol[i] = col;
	p->spec.nfcols = Max(p->spec.nfcols, i + 1);
	bt = makeNode(BooleanTest);
	bt->arg = (Expr *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(i + 1), false, true);
	bt->booltesttype = IS_TRUE;
	return list_make1(bt);
}

/* FILTER (WHERE ..): a conjunction of the quals of a scan, over the columns of the index; the same clause of two aggregates is one clause (at most 64 of them: a bit of the mask of a row each) */
bool
sift_v6_compile_filter(V6Plan *p, Expr *filter, int *fclause)
{
	List	   *quals;
	SiftClause *clause = palloc(sizeof(SiftClause));
	SiftBytes	canon;
	int			id;

	filter = (Expr *) sift_v6_placeholder_contents((Node *) filter, NULL);
	quals = native_filter_quals(p, filter);
	sift_bytes_init(&canon, CurrentMemoryContext);
	if (quals == NIL || !sift_v6_clause_make_params(CurrentMemoryContext, quals, &p->spec, clause, &canon))
	{
		sift_bytes_init(&canon, CurrentMemoryContext);
		quals = derived_filter_quals(p, filter);
		if (quals == NIL || !sift_v6_clause_make_params(CurrentMemoryContext, quals, &p->spec, clause, &canon))
			return false;
	}
	if ((id = sift_freg_add(p->freg, canon.buf, canon.len)) < 0)
		return false;
	if (id == p->spec.nfilters)
	{
		p->spec.filter = lappend(p->spec.filter, quals);
		p->spec.nfilters++;
	}
	*fclause = id + 1;
	return true;
}
