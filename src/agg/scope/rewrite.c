/*
 * agg/scope/rewrite.c - two forms of a condition of a WHERE over the relations of a scope, each the same rows as the form it replaces, that the statement is also planned with (agg/plan/cte.c
 * keeps the cheaper plan that runs the aggregate scan):
 * - `x = ANY (ARRAY(SELECT c ..))` over a relation of a scope is `x IN (SELECT c ..)`: PostgreSQL runs the ARRAY as an InitPlan before the scan, where no scope read takes it, and pulls the IN up
 *   into a semi join the scope planner reads (agg/scope/plan.c);
 * - `s.key IN (SELECT k FROM t ..)` over the key column of a scope relation is `s.key = ANY (ARRAY(SELECT k FROM t ..))`: a table of keys, which the scope node evaluates when it starts.
 * The sub-select reads nothing of the query around it, calls no volatile function and has no ORDER BY, LIMIT, DISTINCT or aggregate. A NULL matches no row in either form, and a WHERE keeps only
 * the rows its condition is true for.
 */
#include "postgres.h"

#include "access/relation.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/scope/scope.h"
#include "core/catalog.h"

/* The relations of the scopes of the sift_col indexes of the tables the statement reads: (relation, key column) of each path, and the relations a path reads */
typedef struct ScopeRels
{
	List	   *key_rel;
	List	   *key_att;
	List	   *rels;
	bool		apply;
	int			found;
} ScopeRels;

static void
add_scopes(ScopeRels *s, Relation index)
{
	const SiftScopeSpecs *specs = sift_scope_specs(index);

	for (int ord = 0; specs != NULL && ord < specs->n; ord++)
	{
		SiftScopeLeg legs[2];
		const int	n = sift_scope_legs(index, ord, legs);

		for (int l = 0; l < n; l++)
		{
			s->key_rel = lappend_oid(s->key_rel, legs[l].rel);
			s->key_att = lappend_int(s->key_att, legs[l].key);
			s->rels = list_append_unique_oid(s->rels, legs[l].rel);
			if (OidIsValid(legs[l].via_rel))
				s->rels = list_append_unique_oid(s->rels, legs[l].via_rel);
		}
	}
}

static void
add_table(ScopeRels *s, Oid relid)
{
	const Oid	col_am = sift_catalog()->col_am;
	Relation	heap = relation_open(relid, AccessShareLock);

	foreach_oid(ix, RelationGetIndexList(heap))
		if (get_rel_relam(ix) == col_am)
		{
			Relation	index = index_open(ix, AccessShareLock);

			add_scopes(s, index);
			index_close(index, AccessShareLock);
		}
	relation_close(heap, AccessShareLock);
}

static bool
tables_walker(Node *n, ScopeRels *s)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, tables_walker, s, QTW_EXAMINE_RTES_BEFORE);
	if (!IsA(n, RangeTblEntry))
		return expression_tree_walker(n, tables_walker, s);
	if (((RangeTblEntry *) n)->rtekind == RTE_RELATION && ((RangeTblEntry *) n)->relkind == RELKIND_RELATION)
		add_table(s, ((RangeTblEntry *) n)->relid);
	return false;
}

static bool
reads_walker(Node *n, ScopeRels *s)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, reads_walker, s, QTW_EXAMINE_RTES_BEFORE);
	if (IsA(n, RangeTblEntry))
		return ((RangeTblEntry *) n)->rtekind == RTE_RELATION && list_member_oid(s->rels, ((RangeTblEntry *) n)->relid);
	return expression_tree_walker(n, reads_walker, s);
}

/* One column of one table or of a select of it, by no order, limit, grouping or volatile function, that reads nothing of the query around it */
static bool
plain_subselect(const Query *q)
{
	int			n = 0;

	foreach_node(TargetEntry, te, q->targetList)
		n += !te->resjunk;
	return q->commandType == CMD_SELECT && n == 1 && !q->hasAggs && !q->hasWindowFuncs && !q->hasTargetSRFs && !q->hasModifyingCTE && q->groupClause == NIL && q->groupingSets == NIL &&
		q->havingQual == NULL && q->distinctClause == NIL && q->sortClause == NIL && q->limitCount == NULL && q->limitOffset == NULL && q->rowMarks == NIL &&
		!contain_vars_of_level((Node *) q, 1) && !contain_volatile_functions((Node *) q);
}

static TargetEntry *
output_of(const Query *q)
{
	foreach_node(TargetEntry, te, q->targetList)
		if (!te->resjunk)
			return te;
	return NULL;
}

/* x = ANY (ARRAY(sub-select)) as x IN (sub-select): the operator compares x to the output of the sub-select */
static Node *
array_as_in(ScalarArrayOpExpr *sa, ScopeRels *s)
{
	SubLink    *arr = list_length(sa->args) == 2 && IsA(lsecond(sa->args), SubLink) ? lsecond(sa->args) : NULL;
	const TargetEntry *te;
	SubLink    *in;
	Param	   *p;

	if (!sa->useOr || arr == NULL || arr->subLinkType != ARRAY_SUBLINK || !plain_subselect((Query *) arr->subselect) || !reads_walker(arr->subselect, s))
		return NULL;
	te = output_of((Query *) arr->subselect);
	p = makeNode(Param);
	*p = (Param) {.xpr.type = T_Param,.paramkind = PARAM_SUBLINK,.paramid = te->resno,.paramtype = exprType((Node *) te->expr),.paramtypmod = exprTypmod((Node *) te->expr),
	.paramcollid = exprCollation((Node *) te->expr),.location = -1};
	in = makeNode(SubLink);
	in->subLinkType = ANY_SUBLINK;
	in->testexpr = (Node *) make_opclause(sa->opno, BOOLOID, false, linitial(sa->args), (Expr *) p, InvalidOid, sa->inputcollid);
	((OpExpr *) in->testexpr)->opfuncid = get_opcode(sa->opno);
	in->subselect = arr->subselect;
	in->location = -1;
	return (Node *) in;
}

/* Is column attno of entry rti of the query the key column of a scope relation: the column itself, or what a select, or every arm of a union, takes it from */
static bool
key_column(const ScopeRels *s, const Query *q, Index rti, AttrNumber attno)
{
	const RangeTblEntry *rte = rt_fetch(rti, q->rtable);
	const Query *sq = rte->rtekind == RTE_SUBQUERY ? rte->subquery : NULL;
	const TargetEntry *te;

	if (rte->rtekind == RTE_RELATION)
	{
		for (int i = 0; i < list_length(s->key_rel); i++)
			if (list_nth_oid(s->key_rel, i) == rte->relid && list_nth_int(s->key_att, i) == attno)
				return true;
		return false;
	}
	if (sq == NULL || attno < 1 || attno > list_length(sq->targetList))
		return false;
	if (sq->setOperations != NULL)
	{
		bool		all = true;
		int			arms = 0;

		foreach_node(RangeTblEntry, arm, sq->rtable)
			if (arm->rtekind == RTE_SUBQUERY && arm->subquery != NULL)
			{
				te = list_nth(arm->subquery->targetList, attno - 1);
				all &= IsA(te->expr, Var) && ((Var *) te->expr)->varlevelsup == 0 && key_column(s, arm->subquery, ((Var *) te->expr)->varno, ((Var *) te->expr)->varattno);
				arms++;
			}
		return all && arms > 0;
	}
	te = list_nth(sq->targetList, attno - 1);
	return IsA(te->expr, Var) && ((Var *) te->expr)->varlevelsup == 0 && key_column(s, sq, ((Var *) te->expr)->varno, ((Var *) te->expr)->varattno);
}

/* s.key IN (sub-select) as s.key = ANY (ARRAY(sub-select)), the key column of a scope relation compared by the operator of the IN */
static Node *
in_as_array(const Query *q, SubLink *in, ScopeRels *s)
{
	const OpExpr *op = in->subLinkType == ANY_SUBLINK && in->testexpr != NULL && IsA(in->testexpr, OpExpr) ? (const OpExpr *) in->testexpr : NULL;
	Var		   *v = op != NULL && list_length(op->args) == 2 ? (Var *) linitial(op->args) : NULL;
	const TargetEntry *te;
	ScalarArrayOpExpr *sa;
	SubLink    *arr;

	if (v == NULL || !IsA(v, Var) || v->varlevelsup != 0 || !IsA(lsecond(op->args), Param) || !plain_subselect((Query *) in->subselect) ||
		!key_column(s, q, v->varno, v->varattno))
		return NULL;
	te = output_of((Query *) in->subselect);
	if (!OidIsValid(get_array_type(exprType((Node *) te->expr))))
		return NULL;
	arr = makeNode(SubLink);
	arr->subLinkType = ARRAY_SUBLINK;
	arr->subselect = in->subselect;
	arr->location = -1;
	sa = makeNode(ScalarArrayOpExpr);
	sa->opno = op->opno;
	sa->opfuncid = op->opfuncid;
	sa->useOr = true;
	sa->inputcollid = op->inputcollid;
	sa->args = list_make2(copyObject(v), arr);
	sa->location = -1;
	return (Node *) sa;
}

static Node *
rewritten(const Query *q, Node *c, ScopeRels *s)
{
	if (IsA(c, ScalarArrayOpExpr))
		return array_as_in((ScalarArrayOpExpr *) c, s);
	if (IsA(c, SubLink))
		return in_as_array(q, (SubLink *) c, s);
	return NULL;
}

/* The conjuncts of the WHERE of the query, and of the queries of its sub-selects and its FROM */
static bool
query_walker(Node *n, ScopeRels *s)
{
	Query	   *q;

	if (n == NULL)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, query_walker, s);
	q = (Query *) n;
	if (q->jointree != NULL && q->jointree->quals != NULL)
	{
		List	   *quals = NIL;

		foreach_ptr(Node, c, make_ands_implicit((Expr *) q->jointree->quals))
		{
			Node	   *r = rewritten(q, c, s);

			s->found += r != NULL;
			quals = lappend(quals, r != NULL ? r : c);
		}
		if (s->apply)
			q->jointree->quals = (Node *) make_ands_explicit(quals);
	}
	return query_tree_walker(q, query_walker, s, 0);
}

bool
sift_v6_scope_rewrite(Query *q, bool apply)
{
	ScopeRels	s = {.apply = apply};

	tables_walker((Node *) q, &s);
	if (s.rels == NIL)
		return false;
	query_walker((Node *) q, &s);
	return s.found > 0;
}
