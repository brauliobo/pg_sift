/*
 * agg/plan/filter.c - the FILTER (WHERE ..) clause of an aggregate of SiftAggV6: the scan decides it on the stored values, or PostgreSQL's own functions decide it over the datums of its columns.
 */
/* check-memory: allow-allocs every list here follows the clauses of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_operator_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

#include "agg/plan/int.h"
#include "agg/filter/pg.h"
#include "agg/qual/contains.h"
#include "agg/qual/qual.h"
#include "agg/unnest/jsonb.h"

/* the presence column `doc ? 'k'` reads, or e is (sift_v6_plan_json_test made it one); InvalidAttrNumber: neither */
static int
presence_of(const V6Plan *p, const Expr *e)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);

	return IsA(e, Var) && sift_v6_vatt_is_presence(((const Var *) e)->varattno) ? ((const Var *) e)->varattno : sift_v6_json_presence(&rel, e);
}

/*
 * `doc ? 'k'`, or IS TRUE or IS NOT TRUE of it, of a key whose presence the scan reads: the test of the presence. A SQL NULL document has not the key, where `?` is NULL: the same to a FILTER, which
 * keeps neither, but not to NOT, which stays the query's
 */
static Expr *
presence_qual(V6Plan *p, Expr *c)
{
	const BooleanTest *bt = IsA(c, BooleanTest) ? (const BooleanTest *) c : NULL;
	const bool	has = bt == NULL || bt->booltesttype == IS_TRUE;
	const int	attno = p->xseed == NULL && (has || bt->booltesttype == IS_NOT_TRUE) ? presence_of(p, bt != NULL ? bt->arg : c) : InvalidAttrNumber;
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
	return (q = presence_qual(p, c)) != NULL ? q : sift_v6_index_qual(p, sift_v6_plan_json_test(p, c, false));
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

/*
 * IS TRUE, IS FALSE, IS NOT TRUE or IS NOT FALSE of `x op c`, x a column of the table, c a constant that is not NULL and op strict with a negator: the comparison, its negation, or either
 * with the rows where x is NULL, which the comparison makes NULL (c: any other clause)
 */
static Expr *
truth_compare(Expr *c)
{
	const BooleanTest *bt = IsA(c, BooleanTest) ? (const BooleanTest *) c : NULL;
	OpExpr	   *op = bt != NULL && IsA(bt->arg, OpExpr) ? (OpExpr *) bt->arg : NULL;
	const bool	has = bt != NULL && (bt->booltesttype == IS_TRUE || bt->booltesttype == IS_NOT_FALSE);
	const Oid	negator = op == NULL || has ? InvalidOid : get_negator(op->opno);
	Expr	   *x,
			   *k,
			   *test;
	NullTest   *null;

	if (op == NULL || list_length(op->args) != 2 || !op_strict(op->opno) || (!has && !OidIsValid(negator)))
		return c;
	x = IsA(linitial(op->args), Const) ? lsecond(op->args) : linitial(op->args);
	k = x == linitial(op->args) ? lsecond(op->args) : linitial(op->args);
	if (!IsA(x, Var) || ((Var *) x)->varattno <= 0 || sift_v6_vatt_is_json(((Var *) x)->varattno) || !IsA(k, Const) || ((Const *) k)->constisnull)
		return c;
	test = has ? (Expr *) op : make_opclause(negator, BOOLOID, false, linitial(op->args), lsecond(op->args), InvalidOid, op->inputcollid);
	if (!has)
		((OpExpr *) test)->opfuncid = get_opcode(negator);
	if (bt->booltesttype == IS_TRUE || bt->booltesttype == IS_FALSE)
		return test;
	null = makeNode(NullTest);
	null->arg = x;
	null->nulltesttype = IS_NULL;
	null->location = -1;
	return makeBoolExpr(OR_EXPR, list_make2(test, null), -1);
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

/* a containment whose lists of strings the tests of the plan read: the quals of their flags and of the tests of the leaves of its other members (NULL: none, or one the scan does not decide) */
static Expr *
members_qual(V6Plan *p, Expr *c)
{
	const SiftV6Rel rel = sift_v6_plan_rel(p);
	Expr	   *rest;
	List	   *quals = sift_v6_tests_contains_quals(p, c, &rest);
	List	   *tests = quals != NIL && rest != NULL ? sift_v6_contains_quals(&rel, rest) : NIL;

	if (quals == NIL || (rest != NULL && tests == NIL))
		return NULL;
	foreach_ptr(Expr, t, tests)
	{
		Expr	   *q = filter_qual(p, t);

		if (q == NULL)
			return NULL;
		quals = lappend(quals, q);
	}
	return make_ands_explicit(quals);
}

/* a conjunct of a FILTER as a qual the scan decides on the stored values: a test of the elements of the row, a qual of its columns, or an OR of those (NULL: none) */
static Expr *
conjunct_qual(V6Plan *p, Expr *conjunct)
{
	Expr	   *c = truth_compare(bool_compare(conjunct));
	Expr	   *q = sift_v6_unnest_test_qual(p, (Node *) c);

	if (q != NULL || (q = members_qual(p, c)) != NULL)
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

/* the clause as a boolean PostgreSQL's own functions compute over the datums of its columns: the test that the derived column is true (NULL: none) */
static Expr *
derived_qual(V6Plan *p, Expr *e)
{
	SiftV6Col	col;
	BooleanTest *bt;
	int			i = 0;

	if (exprType((Node *) e) != BOOLOID || !sift_v6_pg_derived_col(p, e, false, &col))
		return NULL;
	while (i < p->spec.nfcols && !sift_v6_col_same(&p->spec.fcol[i], &col))
		i++;
	if (i == p->spec.nfcols && p->spec.nfcols == SIFT_V6_MAX_FCOLS)
		return NULL;
	p->spec.fcol[i] = col;
	p->spec.nfcols = Max(p->spec.nfcols, i + 1);
	bt = makeNode(BooleanTest);
	bt->arg = (Expr *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(i + 1), false, true);
	bt->booltesttype = IS_TRUE;
	return (Expr *) bt;
}

static bool
raising_read(Node *n, void *context)
{
	return n != NULL && ((IsA(n, Var) && sift_v6_vatt_raises(((const Var *) n)->varattno)) || expression_tree_walker(n, raising_read, context));
}

/* the conjunct may raise for a row an earlier conjunct drops: a function that is not leakproof reads a column, or a column whose conversion raises */
static bool
may_raise(Expr *conjunct)
{
	return contain_leaked_vars((Node *) conjunct) || raising_read((Node *) conjunct, NULL);
}

/*
 * the conjuncts of the clause the scan decides on the stored values, or of a conjunct that is none of those and cannot raise the derived column PostgreSQL's functions compute for every
 * row (NIL: one is neither); an AND or OR the index stores, or its negation, is one
 */
static List *
native_filter_quals(V6Plan *p, Expr *filter)
{
	List	   *quals = NIL;
	Expr	   *whole;

	p->in_filter = true;
	whole = is_andclause(filter) || is_orclause(filter) ? sift_v6_index_qual(p, filter) : NULL;
	if (whole != NULL)
	{
		p->in_filter = false;
		return list_make1(whole);
	}
	foreach_ptr(Expr, conjunct, sift_v6_conjuncts(filter))
	{
		Expr	   *q = conjunct_qual(p, conjunct);

		p->in_filter = false;
		q = q != NULL || may_raise(conjunct) ? q : derived_qual(p, conjunct);
		p->in_filter = true;
		if (q == NULL)
		{
			quals = NIL;
			break;
		}
		quals = list_concat(quals, sift_v6_conjuncts(q));
	}
	p->in_filter = false;
	return quals;
}

/* FILTER (WHERE ..): a conjunction of the quals of a scan, over the columns of the index; the same clause of two aggregates is one clause (at most 64 of them: a bit of the mask of a row each) */
bool
sift_v6_compile_filter(V6Plan *p, Expr *filter, int *fclause)
{
	List	   *quals;
	Expr	   *q;
	SiftClause *clause = palloc(sizeof(SiftClause));
	SiftBytes	canon;
	int			id;

	if (!sift_v6_placeholder_values(p, (Node *) filter, (Node **) &filter))
		return false;
	filter = sift_v6_plan_json_test(p, filter, false);
	quals = native_filter_quals(p, filter);
	sift_bytes_init(&canon, CurrentMemoryContext);
	if (quals == NIL || !sift_v6_clause_make_params(CurrentMemoryContext, quals, &p->spec, clause, &canon))
	{
		sift_bytes_init(&canon, CurrentMemoryContext);
		quals = (q = derived_qual(p, filter)) != NULL ? list_make1(q) : NIL;
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
