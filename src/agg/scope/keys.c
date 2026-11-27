/*
 * agg/scope/keys.c - the key of the scope a statement binds the pairs to: key = constant or parameter, or key = ANY(array), stated on the pairs relation or in the sub-select that is the pairs,
 * and the other conditions on the pairs, which must be the predicate of the unique btree that makes them (agg/scope/scope.h).
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"

#include "agg/scope/scope.h"
#include "core/catname.h"

Node *
sift_v6_strip_relabel(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

/* The attribute of the pairs relation a Var of the pairs side is: its own, or the one the sub-select or CTE takes it from */
AttrNumber
sift_v6_scope_pairs_attno(const ScopePlan *p, const Var *v)
{
	if (v->varno != p->pairs->relid || v->varlevelsup != 0)
		return InvalidAttrNumber;
	if (p->pairs_query != NULL)
	{
		const TargetEntry *te = v->varattno >= 1 && v->varattno <= list_length(p->pairs_query->targetList) ? list_nth(p->pairs_query->targetList, v->varattno - 1) : NULL;

		return te != NULL && IsA(te->expr, Var) ? ((Var *) te->expr)->varattno : InvalidAttrNumber;
	}
	return v->varattno;
}

bool
sift_v6_scope_key_var(const ScopePlan *p, const Node *n)
{
	return IsA(n, Var) && sift_v6_scope_pairs_attno(p, (const Var *) n) == p->leg.key;
}

/* Is opno the equality of the scope key column, under a deterministic collation for a string key */
static bool
key_equality(Oid key_type, Oid opno, Oid inputcollid)
{
	const Oid	opclass = GetDefaultOpClass(key_type, BTREE_AM_OID);

	if (type_is_collatable(key_type) && OidIsValid(inputcollid) && !get_collation_isdeterministic(inputcollid))
		return false;
	return OidIsValid(opclass) && get_op_opfamily_strategy(opno, get_opclass_family(opclass)) == BTEqualStrategyNumber;
}

/* key = rhs (or rhs = key) or key = ANY(rhs) by the equality of the key, on the pairs relation (inner: in the sub-select that is the pairs), rhs a constant or a parameter whose values the summary codes */
static bool
key_args(const ScopePlan *p, List *args, Oid opno, Oid inputcollid, bool inner, bool array, Expr **rhs)
{
	Node	   *l,
			   *r;

	if (list_length(args) != 2)
		return false;
	l = sift_v6_strip_relabel(linitial(args));
	r = sift_v6_strip_relabel(lsecond(args));
	if (!array && !IsA(l, Var) && IsA(r, Var))
	{
		Node	   *t = l;

		l = r;
		r = t;
		opno = get_commutator(opno);
	}
	if (!OidIsValid(opno) || !key_equality(p->spec.key_type, opno, inputcollid))
		return false;
	if (!(inner ? IsA(l, Var) && ((Var *) l)->varattno == p->leg.key : sift_v6_scope_key_var(p, l)) || !(IsA(r, Const) ? !((Const *) r)->constisnull : IsA(r, Param)) ||
		!p->scope->key_codes(p->spec.key_type, array ? get_element_type(exprType(r)) : exprType(r)))
		return false;
	*rhs = (Expr *) r;
	return true;
}

static bool
key_clause(const ScopePlan *p, Node *clause, bool inner, Expr **rhs)
{
	if (IsA(clause, OpExpr))
		return key_args(p, ((OpExpr *) clause)->args, ((OpExpr *) clause)->opno, ((OpExpr *) clause)->inputcollid, inner, false, rhs);
	return IsA(clause, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) clause)->useOr &&
		key_args(p, ((ScalarArrayOpExpr *) clause)->args, ((ScalarArrayOpExpr *) clause)->opno, ((ScalarArrayOpExpr *) clause)->inputcollid, inner, true, rhs);
}

static void
refuse(ScopePlan *p, const char *why)
{
	if (p->why == NULL)
		p->why = why;
}

/* A condition on the pairs as the statement wrote it: over the relation of the sub-select (inner), or over the pairs side of the statement */
static char *
condition_text(const ScopePlan *p, Node *c, bool inner, bool prefix)
{
	Node	   *e = copyObject(c);

	if (inner)
		return deparse_expression(e, deparse_context_for(sift_rel_name(p->leg.rel), p->leg.rel), prefix, false);
	ChangeVarNodes(e, p->pairs->relid, 1, 0);
	return deparse_expression(e, deparse_context_for(planner_rt_fetch(p->pairs->relid, p->root)->eref->aliasname, p->leg.rel), prefix, false);
}

/* The conditions of the statement on the pairs besides the key are the predicate of the unique btree that makes the pairs, no more and no less: it reads exactly the rows that are pairs */
static bool
pairs_predicate_ok(ScopePlan *p, List *rest, bool inner)
{
	List	   *pred = copyObject(p->leg.pred);
	const int	varno = inner ? 1 : p->pairs->relid;

	ChangeVarNodes((Node *) pred, 1, varno, 0);
	if (pred != NIL && (rest == NIL || !predicate_implied_by(pred, rest, false)))
	{
		refuse(p, psprintf("the pairs of the scope are the rows of its unique btree that satisfy %s: the statement does not state it",
						   condition_text(p, (Node *) make_ands_explicit(pred), inner, false)));
		return false;
	}
	foreach_ptr(Node, c, rest)
		if (pred == NIL || !predicate_implied_by(list_make1(c), pred, false))
		{
			refuse(p, psprintf("the scope relation has another condition: %s", condition_text(p, c, inner, true)));
			return false;
		}
	return true;
}

/* The conditions on the pairs are the key = K or key IN (K1, K2, ...), from the statement or from the sub-select, and the predicate of the pairs; no key: every key (only a statement that groups by the key) */
bool
sift_v6_scope_key_filter(ScopePlan *p)
{
	List	   *outer = p->pairs->baserestrictinfo;
	Node	   *quals = p->pairs_query != NULL ? p->pairs_query->jointree->quals : NULL;
	List	   *inner = quals == NULL ? NIL : IsA(quals, List) ? (List *) quals : make_ands_implicit((Expr *) quals);
	List	   *rest_outer = NIL,
			   *rest_inner = NIL;
	Expr	   *rhs = NULL;
	int			nkeys = 0;

	foreach_node(RestrictInfo, ri, outer)
		if (key_clause(p, (Node *) ri->clause, false, &rhs))
			nkeys++;
		else
			rest_outer = lappend(rest_outer, ri->clause);
	foreach_ptr(Node, q, inner)
		if (key_clause(p, q, true, &rhs))
			nkeys++;
		else
			rest_inner = lappend(rest_inner, q);
	if (rhs == NULL && !p->spec.by_key)
	{
		refuse(p, "the scope key is not bound by an equality or an IN list of constants or parameters");
		return false;
	}
	if (nkeys > 1)
	{
		refuse(p, "the scope key is bound twice");
		return false;
	}
	if (p->pairs_query != NULL && rest_outer != NIL)
	{
		refuse(p, "the sub-select over the scope relation has a condition outside it");
		return false;
	}
	if (!(p->pairs_query != NULL ? pairs_predicate_ok(p, rest_inner, true) : pairs_predicate_ok(p, rest_outer, false)))
		return false;
	if (rhs == NULL)
		return true;
	p->key_expr = rhs;
	p->spec.key_given = true;
	p->spec.key_array = type_is_array(exprType((Node *) rhs));
	return true;
}
