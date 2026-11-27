/*
 * agg/unnest/elem_quals.c - the quals of the element of an unnest (SiftV6Ext.elem_where, agg/unnest/test.h): a comparison of a column of the element the scan decides, or a clause PostgreSQL's
 * evaluator runs on the elements the quals before it keep, in the order the executor of the statement runs them.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/datum.h"
#include "agg/expr/pg.h"
#include "agg/ht/ht.h"
#include "agg/filter/pg.h"
#include "agg/plan/int.h"
#include "agg/unnest/jsonb.h"

/* the column of the element that n is, as the virtual attribute of its slot; the value of a member is tested by the clauses of the pairs (a FILTER) only: that of a row read again from the heap is made after the quals of the element */
static Expr *
elem_var(const V6Plan *p, const Node *n, bool pairs)
{
	SiftV6Col	col;

	if (n == NULL || !sift_v6_unnest_column((V6Plan *) p, (const Expr *) n, &col) || col.src != SV6SRC_ELEM || (!pairs && sift_v6_elem_late(&p->xseed->elem, col.slot)))
		return NULL;
	return (Expr *) makeVar(INDEX_VAR, SIFT_V6_ELEM_VATT + col.slot, exprType(n), exprTypmod(n), exprCollation(n), 0);
}

static bool
text_compares_bytes(const Node *n, Oid collation)
{
	return sift_v6_class(exprType(n)) != SV6C_TEXT || sift_text_bytes_equal(collation);
}

/* a comparison of a column of the element with a constant, an IN list of constants, or a null test, over the virtual attribute of the column; or an AND, OR or NOT of such tests */
static Expr *
elem_qual(const V6Plan *p, Node *clause, bool pairs)
{
	if (IsA(clause, BoolExpr))
	{
		BoolExpr   *q = copyObject((BoolExpr *) clause);
		List	   *args = NIL;

		foreach_ptr(Node, a, q->args)
		{
			Expr	   *t = elem_qual(p, a, pairs);

			if (t == NULL)
				return NULL;
			args = lappend(args, t);
		}
		q->args = args;
		return (Expr *) q;
	}
	if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *q = copyObject((OpExpr *) clause);
		Node	   *l = linitial(q->args),
				   *r = lsecond(q->args);
		Expr	   *var;

		if (IsA(l, Const) && !IsA(r, Const))
		{
			q->opno = get_commutator(q->opno);
			if (!OidIsValid(q->opno))
				return NULL;
			q->opfuncid = get_opcode(q->opno);
			l = lsecond(q->args);
			r = linitial(q->args);
		}
		if ((var = elem_var(p, l, pairs)) == NULL || !IsA(r, Const) || !text_compares_bytes(l, q->inputcollid))
			return NULL;
		q->args = list_make2(var, r);
		return (Expr *) q;
	}
	if (IsA(clause, ScalarArrayOpExpr) && list_length(((ScalarArrayOpExpr *) clause)->args) == 2)
	{
		ScalarArrayOpExpr *q = copyObject((ScalarArrayOpExpr *) clause);
		Expr	   *var = elem_var(p, linitial(q->args), pairs);

		if (var == NULL || !IsA(lsecond(q->args), Const) || !text_compares_bytes(linitial(q->args), q->inputcollid))
			return NULL;
		linitial(q->args) = var;
		return (Expr *) q;
	}
	if (IsA(clause, NullTest) && !((NullTest *) clause)->argisrow)
	{
		NullTest   *q = copyObject((NullTest *) clause);
		Expr	   *var = elem_var(p, (Node *) q->arg, pairs);

		if (var == NULL)
			return NULL;
		q->arg = var;
		return (Expr *) q;
	}
	return NULL;
}

Expr *
sift_v6_unnest_elem_qual(const V6Plan *p, Node *clause)
{
	return elem_qual(p, clause, true);
}

/*
 * a clause over the columns of the element that no comparison of the scan says (a regex, a function, another operator): a step PostgreSQL's evaluator runs on the elements the steps before it keep,
 * as (the clause over the inputs of SIFT_V6_PGX_RELID, the slot and the type of each input); NULL when it reads anything but columns the element has before its quals, or needs the node to run
 */
static Node *
elem_pg_step(const V6Plan *p, Expr *clause)
{
	List	   *vars,
			   *ints = NIL;

	if (p->xseed->elem.fn == SV6E_KEYS || !sift_v6_pgexpr_plan_ok(p->root, clause) || sift_v6_pgexpr_exec((const Node *) clause) ||
		!sift_v6_pgexpr_vars(clause, p->elem_rel->relid, 0, &vars) || vars == NIL)
		return NULL;
	foreach_node(Var, v, vars)
	{
		const Var  *e = (const Var *) elem_var(p, (const Node *) v, false);

		if (e == NULL || !sift_v6_type_native(v->vartype))
			return NULL;
		ints = lappend_int(lappend_int(ints, e->varattno - SIFT_V6_ELEM_VATT), (int) v->vartype);
	}
	return (Node *) list_make2(sift_v6_pgexpr_renumber(clause, p->elem_rel->relid, 0, vars), ints);
}

/* the qual of the element as a comparison the scan decides (one its clause compiles), else NULL */
static Node *
elem_native_step(const V6Plan *p, Node *clause)
{
	Expr	   *q = elem_qual(p, clause, false);
	SiftClause *compiled = palloc(sizeof(SiftClause));
	int			col[SIFT_V6_MAX_FCOLS],
				nvec;

	return q != NULL && sift_v6_unnest_elem_clause(CurrentMemoryContext, list_make1(q), compiled, col, &nvec) ? (Node *) q : NULL;
}

bool
sift_v6_unnest_elem_quals(V6Plan *p)
{
	List	   *steps = NIL;

	if (p->elem_rel == NULL)
		return true;
	foreach_node(RestrictInfo, ri, sift_v6_quals_in_order(p->root, p->elem_rel->baserestrictinfo))
	{
		Node	   *q = ri->pseudoconstant ? NULL : elem_native_step(p, (Node *) ri->clause);

		if (q == NULL && (ri->pseudoconstant || (q = elem_pg_step(p, ri->clause)) == NULL))
			return false;
		steps = lappend(steps, q);
	}
	p->xseed->elem_where = steps;
	return true;
}

bool
sift_v6_unnest_elem_clause(MemoryContext cx, const List *quals, SiftClause *out, int *col, int *nvec)
{
	SiftV6Spec	tmp = {0};

	if (!sift_v6_clause_make(cx, quals, &tmp, out, NULL))
		return false;
	for (int i = 0; i < tmp.nfcols; i++)
		col[i] = tmp.fcol[i].attno - SIFT_V6_ELEM_VATT;
	*nvec = tmp.nfcols;
	return true;
}

