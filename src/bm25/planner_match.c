/*
 * bm25/planner_match.c - what the query says to the planner: the index key, the match clauses, the score expressions and cutoffs,
 * the sort keys a scan can serve and the limits.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/stratnum.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

#include "agg/agg.h"
#include "bm25/am.h"
#include "bm25/estimate.h"
#include "bm25/eval.h"
#include "bm25/planner.h"
#include "bm25/planner_int.h"
#include "bm25/pushdown.h"

bool
sift_index_matches_key(IndexOptInfo *index, Node *expr)
{
	if (index->ncolumns != 1 || index->relam != sift_catalog()->am)
		return false;
	while (IsA(expr, RelabelType))
		expr = (Node *) ((RelabelType *) expr)->arg;
	if (index->indexkeys[0] != 0)
		return IsA(expr, Var) && ((Var *) expr)->varno == index->rel->relid &&
			((Var *) expr)->varattno == index->indexkeys[0];
	return equal(expr, linitial(index->indexprs));
}

bool
sift_match_clause(Node *clause, IndexOptInfo *index, Expr **query)
{
	const SiftCatalog *cat = sift_catalog();
	List	   *args;
	Node	   *rhs;

	if (IsA(clause, RestrictInfo))
	{
		/* a scan evaluates the match before the security quals of the rel (row-level security, security barrier views), as an index qual would */
		if (!restriction_is_securely_promotable((RestrictInfo *) clause, index->rel))
			return false;
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	}
	if (IsA(clause, OpExpr) && ((OpExpr *) clause)->opno == cat->match_op)
		args = ((OpExpr *) clause)->args;
	else if (IsA(clause, FuncExpr) && ((FuncExpr *) clause)->funcid == cat->match_func)
		args = ((FuncExpr *) clause)->args;
	else if (IsA(clause, FuncExpr) && ((FuncExpr *) clause)->funcid == cat->match_idx_func)
	{
		args = ((FuncExpr *) clause)->args;
		if (!IsA(lthird(args), Const) || !sift_index_serves(index->indexoid, DatumGetObjectId(((Const *) lthird(args))->constvalue)))
			return false;
	}
	else
		return false;
	rhs = lsecond(args);
	if (!sift_index_matches_key(index, linitial(args)) || contain_var_clause(rhs) || contain_volatile_functions(rhs))
		return false;
	*query = (Expr *) rhs;
	return true;
}

/*
 * `key @@> ANY (array)` of the index: the clause is the OR of its elements' queries, which the scan evaluates as one query (the
 * ranker scores it as the OR written in the query text). *array is the array operand. ALL and NOT are not an OR of matches.
 */
static bool
sift_match_any_clause(Node *clause, IndexOptInfo *index, Expr **array)
{
	ScalarArrayOpExpr *sa;
	Node	   *rhs;

	if (IsA(clause, RestrictInfo))
	{
		if (!restriction_is_securely_promotable((RestrictInfo *) clause, index->rel))
			return false;
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	}
	if (!IsA(clause, ScalarArrayOpExpr))
		return false;
	sa = (ScalarArrayOpExpr *) clause;
	if (!sa->useOr || sa->opno != sift_catalog()->match_op)
		return false;
	rhs = lsecond(sa->args);
	if (!sift_index_matches_key(index, linitial(sa->args)) || contain_var_clause(rhs) || contain_volatile_functions(rhs))
		return false;
	*array = (Expr *) rhs;
	return true;
}

/* Do quals of a policy or a security barrier view, which the rel's other quals must not run before, sit on the rel? */
bool
rel_secured(PlannerInfo *root, RelOptInfo *rel)
{
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
		if (rinfo->security_level < root->qual_security_level)
			return true;
	return false;
}

/*
 * A qual the scan answers: a query, or an array of queries ORed. The match is not leakproof, so a rel with security quals
 * keeps it out of the scan, unless sift.rls_scores lets a ranked scan answer the user's own matches (never a policy's: a policy's
 * word is a qual of the heap row, and as a term of the query its weight would enter the score): the scan reads the index, and a
 * row it returns has passed the security quals first (cscan.c scan_verify).
 */
bool
scan_match_clause(PlannerInfo *root, Node *clause, IndexOptInfo *index, bool ranked, Expr **query)
{
	if (ranked && sift_rls_scores && IsA(clause, RestrictInfo))
	{
		if (((RestrictInfo *) clause)->security_level < root->qual_security_level)
			return false;
		clause = (Node *) ((RestrictInfo *) clause)->clause;
	}
	return sift_match_clause(clause, index, query) || sift_match_any_clause(clause, index, query);
}

bool
sift_is_score_expr(Node *expr, Index relid, bool *is_max)
{
	const SiftCatalog *cat = sift_catalog();
	FuncExpr   *f;
	Node	   *arg;

	if (!IsA(expr, FuncExpr))
		return false;
	f = (FuncExpr *) expr;
	if (f->funcid != cat->score_func && f->funcid != cat->max_score_func)
		return false;
	arg = linitial(f->args);
	while (IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;
	if (!IsA(arg, Var) || ((Var *) arg)->varno != relid || ((Var *) arg)->varlevelsup != 0 ||
		((Var *) arg)->varattno != SelfItemPointerAttributeNumber)
		return false;
	if (is_max)
		*is_max = f->funcid == cat->max_score_func;
	return true;
}

bool
score_walker(Node *node, ScoreRefs *refs)
{
	if (node == NULL)
		return false;
	if (sift_is_score_expr(node, refs->relid, NULL))
	{
		refs->found = list_append_unique(refs->found, node);
		return false;
	}
	return expression_tree_walker(node, score_walker, refs);
}

/* The score in an operand of a comparison: sift.score(rel.ctid), or the float8 PostgreSQL makes of it */
static bool
score_operand(Node *arg, Index relid)
{
	bool		is_max;

	if (IsA(arg, FuncExpr) && ((FuncExpr *) arg)->funcid == F_FLOAT8_FLOAT4)
		arg = linitial(((FuncExpr *) arg)->args);
	return sift_is_score_expr(arg, relid, &is_max) && !is_max;
}

/*
 * Is the clause `sift.score(rel.ctid) {>, >=, <, <=} c` (either side) for a c the scan can read when it starts: no column,
 * no volatile function, no sub-plan? *lower: the score is bounded from below, so the rows it drops are the last ones of a ranked scan.
 */
bool
score_cutoff(Node *clause, Index relid, bool *lower)
{
	static Oid	float_family;	/* a system catalog row: the lookup scans pg_opclass, and this runs for every clause of every partition */
	OpExpr	   *op;
	Node	   *cutoff;
	int			strategy;
	bool		score_left;

	if (!IsA(clause, OpExpr) || list_length((op = (OpExpr *) clause)->args) != 2)
		return false;
	if (!OidIsValid(float_family))
		float_family = get_opclass_family(GetDefaultOpClass(FLOAT8OID, BTREE_AM_OID));
	strategy = get_op_opfamily_strategy(op->opno, float_family);
	if (strategy == 0 || strategy == BTEqualStrategyNumber)
		return false;
	score_left = score_operand(linitial(op->args), relid);
	if (!score_left && !score_operand(lsecond(op->args), relid))
		return false;
	cutoff = score_left ? lsecond(op->args) : linitial(op->args);
	if (contain_var_clause(cutoff) || contain_volatile_functions(cutoff) || contain_subplans(cutoff))
		return false;
	if (lower)
		*lower = score_left ? strategy > BTEqualStrategyNumber : strategy < BTEqualStrategyNumber;
	return true;
}

/* Does a cutoff bound the score from above? The best rows are the ones it drops, so a limit hint cannot apply before it */
bool
cuts_from_above(List *cuts, Index relid)
{
	foreach_node(RestrictInfo, rinfo, cuts)
	{
		bool		lower;

		if (score_cutoff((Node *) rinfo->clause, relid, &lower) && !lower)
			return true;
	}
	return false;
}

/* The members of the sort key's class that are about rel: its own, and for a partition the ones derived from its parent's */
static List *
pathkey_members(PathKey *pk, RelOptInfo *rel)
{
	EquivalenceMemberIterator it;
	EquivalenceMember *em;
	List	   *members = NIL;

	setup_eclass_member_iterator(&it, pk->pk_eclass, rel->relids);
	while ((em = eclass_member_iterator_next(&it)) != NULL)
		members = lappend(members, em);
	return members;
}

static List *
pathkey_exprs(PathKey *pk, RelOptInfo *rel)
{
	List	   *exprs = NIL;

	foreach_ptr(EquivalenceMember, em, pathkey_members(pk, rel))
		exprs = lappend(exprs, em->em_expr);
	return exprs;
}

/* A partition or inheritance child: the query is written for the top parent, so its score expressions are the parent's translated to the child */
bool
is_partition(RelOptInfo *rel)
{
	return rel->reloptkind == RELOPT_OTHER_MEMBER_REL;
}

/* sift.score / max_score of rel in the final target list, in HAVING and in the query's first sort key */
List *
score_exprs(PlannerInfo *root, RelOptInfo *rel)
{
	ScoreRefs	refs = {is_partition(rel) ? bms_singleton_member(rel->top_parent_relids) : rel->relid, NIL};
	List	   *own = NIL;

	score_walker((Node *) root->processed_tlist, &refs);
	score_walker(root->parse->havingQual, &refs);
	foreach_ptr(Node, e, refs.found)
		own = lappend(own, is_partition(rel) ? adjust_appendrel_attrs_multilevel(root, e, rel, rel->top_parent) : e);
	refs = (ScoreRefs) {rel->relid, own};
	if (root->query_pathkeys != NIL)
		score_walker((Node *) pathkey_exprs(linitial_node(PathKey, root->query_pathkeys), rel), &refs);
	return refs.found;
}

/* Is the sort key sift.score(rel.ctid) DESC? */
static bool
is_score_key(PathKey *pk, RelOptInfo *rel)
{
	bool		is_max;

	if (!sift_pathkey_descending(pk))
		return false;
	foreach_ptr(Expr, e, pathkey_exprs(pk, rel))
		if (sift_is_score_expr((Node *) e, rel->relid, &is_max) && !is_max)
			return true;
	return false;
}

/* Is the first sort key sift.score(rel.ctid) DESC? */
bool
sorted_by_score(PlannerInfo *root, RelOptInfo *rel, PathKey **pk)
{
	if (root->query_pathkeys == NIL)
		return false;
	*pk = linitial_node(PathKey, root->query_pathkeys);
	return is_score_key(*pk, rel);
}

/* The rows of the query's LIMIT and OFFSET, 0 when it has none the planner can read */
int64
query_limit(PlannerInfo *root)
{
	return root->limit_tuples > 0 ? (int64) ceil(root->limit_tuples) : 0;
}

/*
 * The rows the query takes from the scan before it stops, 0 when it drains the scan: the limit, unless a Sort
 * sits between them (the scan is not what the first sort key asks for)
 */
int64
scan_limit(PlannerInfo *root, bool ranked)
{
	return ranked || root->query_pathkeys == NIL ? query_limit(root) : 0;
}

/*
 * The LIMIT (OFFSET) of a generic plan, which cannot be costed: the Params of a plain ungrouped, non-WITH TIES
 * query (the scan reads them when it starts), NIL for any other
 */
List *
param_limits(PlannerInfo *root)
{
	Query	   *q = root->parse;

	if (q->limitCount == NULL || q->limitOption != LIMIT_OPTION_COUNT || q->groupClause || q->groupingSets ||
		q->distinctClause || q->hasAggs || q->hasWindowFuncs || q->havingQual || q->hasTargetSRFs)
		return NIL;
	if (!IsA(q->limitCount, Param) || ((Param *) q->limitCount)->paramkind != PARAM_EXTERN)
		return NIL;
	if (q->limitOffset == NULL)
		return list_make1(q->limitCount);
	if (IsA(q->limitOffset, Const) || (IsA(q->limitOffset, Param) && ((Param *) q->limitOffset)->paramkind == PARAM_EXTERN))
		return list_make2(q->limitCount, q->limitOffset);
	return NIL;
}

/* The member of the sort key's class that reads this rel only, from its columns: not a score, not volatile, no sub-plan */
static EquivalenceMember *
pathkey_rel_member(PlannerInfo *root, RelOptInfo *rel, PathKey *pk)
{
	foreach_ptr(EquivalenceMember, em, pathkey_members(pk, rel))
	{
		Relids		varnos = pull_varnos(root, (Node *) em->em_expr);
		ScoreRefs	refs = {rel->relid, NIL};

		score_walker((Node *) em->em_expr, &refs);
		if (!bms_is_empty(varnos) && bms_is_subset(varnos, rel->relids) && refs.found == NIL &&
			!contain_volatile_functions((Node *) em->em_expr) && !contain_subplans((Node *) em->em_expr))
			return em;
	}
	return NULL;
}

static void
add_key(KeyedOrder *ko, PathKey *pk, EquivalenceMember *em)
{
	Oid			sortop = sift_pathkey_sortop(pk, em->em_datatype);

	SIFT_INVARIANT(OidIsValid(sortop), "sift: missing ordering operator for type %u in operator family %u", em->em_datatype, pk->pk_opfamily);
	ko->exprs = lappend(ko->exprs, em->em_expr);
	ko->keyops = list_concat(ko->keyops,
							 list_make3(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(sortop), false, true),
										makeConst(OIDOID, -1, InvalidOid, sizeof(Oid), ObjectIdGetDatum(pk->pk_eclass->ec_collation), false, true),
										makeConst(BOOLOID, -1, InvalidOid, sizeof(bool), BoolGetDatum(pk->pk_nulls_first), false, true)));
}

/* False when the first sort key is not one of the rel's columns */
bool
keyed_order(PlannerInfo *root, RelOptInfo *rel, KeyedOrder *ko)
{
	*ko = (KeyedOrder) {NIL, NIL, NIL};
	foreach_node(PathKey, pk, root->query_pathkeys)
	{
		EquivalenceMember *em;

		if (is_score_key(pk, rel))
		{
			ko->pathkeys = lappend(ko->pathkeys, pk);
			break;
		}
		if ((em = pathkey_rel_member(root, rel, pk)) == NULL)
			break;
		ko->pathkeys = lappend(ko->pathkeys, pk);
		add_key(ko, pk, em);
	}
	return ko->exprs != NIL;
}
