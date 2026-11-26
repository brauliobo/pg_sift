/*
 * agg/hybrid/semi_plan.c - the tenant semi join of a ranked search, handed to the hybrid scan as a key set to skip by.
 *
 *   WHERE body @@> $1 AND EXISTS (SELECT 1 FROM tenant_links tl WHERE tl.tenant_id = $2 AND tl.deleted_at IS NULL AND tl.item_id = c.id)
 *
 * PostgreSQL turns the EXISTS into a semi join, whose other side the scan of c never sees: every hit is fetched from the heap to be thrown away at the join. A copy of the query is
 * planned as well, in which the EXISTS stays (as a qual of the rows of c, tested by PostgreSQL's own sub-plan) and a hint is added next to it:
 *
 *   AND (c.id = ANY (ARRAY (SELECT tl.item_id FROM tenant_links tl WHERE tl.tenant_id = $2 AND tl.deleted_at IS NULL LIMIT 4097))) IS NOT UNKNOWN
 *
 * The hint is true of every row whatever the array holds (a NULL key is no row of the EXISTS either), so it can never take a row from the answer. The hybrid scan reads it as a key set
 * of the store, an IN list its key marks and zones skip by, when the set holds at most SIFT_HYB_ACL_PUSH_MAX keys and the query is one the set helps; the array is an init plan,
 * run only when the scan asks for its value, and bounded by its LIMIT.
 *
 * The plan of the copy is used only when it holds a hybrid scan and costs no more than the plan of the query as it is, which keeps its semi join. Only a query with a text match is
 * copied, only for an EXISTS at the top of its WHERE whose sub-select reads one plain relation (no policy of it: the array would run before one), joins it to a key column of a
 * sift_col index of c by one equality, and has no other reference to the outer query.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/hybrid/int.h"
#include "bm25/planner/planner.h"
#include "bm25/planner/int.h"
#include "col/am.h"
#include "core/store/ops.h"
#include "core/catalog.h"
#include "core/sift.h"
#include "core/store/ops.h"

static planner_hook_type prev_planner_hook;

static bool
has_match(Node *node, void *context)
{
	const SiftCatalog *cat = sift_catalog();

	if (node == NULL)
		return false;
	if (IsA(node, OpExpr) && ((OpExpr *) node)->opno == cat->match_op)
		return true;
	if (IsA(node, FuncExpr) && (((FuncExpr *) node)->funcid == cat->match_func || ((FuncExpr *) node)->funcid == cat->match_idx_func))
		return true;
	return expression_tree_walker(node, has_match, context);
}

static List *
and_list(Node *quals)
{
	return quals == NULL ? NIL : IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == AND_EXPR ? ((BoolExpr *) quals)->args : list_make1(quals);
}

static Node *
peel(Node *n)
{
	while (IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

static bool
count_outer_refs(Node *node, int *n)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) && ((Var *) node)->varlevelsup > 0)
		(*n)++;
	return expression_tree_walker(node, count_outer_refs, n);
}

static bool
simple_exists(Query *sub)
{
	return sub->commandType == CMD_SELECT && !sub->setOperations && !sub->groupClause && !sub->groupingSets && !sub->hasAggs && !sub->hasWindowFuncs && !sub->hasTargetSRFs &&
		!sub->hasSubLinks && !sub->hasRecursive && sub->cteList == NIL && !sub->limitCount && !sub->limitOffset && !sub->distinctClause && sub->sortClause == NIL &&
		sub->rowMarks == NIL && !sub->havingQual && list_length(sub->rtable) == 1 && sub->jointree->quals != NULL && list_length(sub->jointree->fromlist) == 1 &&
		IsA(linitial(sub->jointree->fromlist), RangeTblRef);
}

/* The equality of the sub-select's relation to the outer query: its clause, and the Var of each side */
static OpExpr *
join_clause(List *quals, Var **inner, Var **outer)
{
	foreach_ptr(Node, q, quals)
	{
		OpExpr	   *op = (OpExpr *) q;
		Node	   *l,
				   *r;

		if (!IsA(q, OpExpr) || list_length(op->args) != 2)
			continue;
		l = peel(linitial(op->args));
		r = peel(lsecond(op->args));
		if (!IsA(l, Var) || !IsA(r, Var) || exprType(l) != exprType(r) || !op_mergejoinable(op->opno, exprType(l)))
			continue;
		if (((Var *) l)->varlevelsup == 0 && ((Var *) r)->varlevelsup == 1)
			return *inner = (Var *) l, *outer = (Var *) r, op;
		if (((Var *) r)->varlevelsup == 0 && ((Var *) l)->varlevelsup == 1)
			return *inner = (Var *) r, *outer = (Var *) l, op;
	}
	return NULL;
}

/* A key column of a sift_col index of the relation: a column of the store the scan can take a key set on */
static bool
col_key_indexed(Oid relid, AttrNumber attno)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	bool		found = false;

	foreach_oid(ixid, RelationGetIndexList(rel))
	{
		Relation	ix;

		if (sift_store_ops_for_oid(ixid, false) == NULL)
			continue;
		ix = index_open(ixid, AccessShareLock);
		for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(ix); i++)
			found |= ix->rd_index->indkey.values[i] == attno;
		index_close(ix, AccessShareLock);
	}
	relation_close(rel, AccessShareLock);
	return found;
}

/* The key set of the sub-select, outer = ANY (ARRAY (... LIMIT n)), or NULL when the EXISTS is not of the shape */
static Node *
exists_key_set(Query *parse, SubLink *sl)
{
	Query	   *sub = (Query *) sl->subselect;
	RangeTblEntry *rte,
			   *orte;
	Var		   *inner,
			   *outer;
	OpExpr	   *join;
	List	   *rest = NIL;
	int			outer_refs = 0;
	SubLink    *arr;
	ScalarArrayOpExpr *saop;

	if (sl->subLinkType != EXISTS_SUBLINK || !IsA(sub, Query) || !simple_exists(sub))
		return NULL;
	rte = linitial_node(RangeTblEntry, sub->rtable);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || (rte->inh && has_subclass(rte->relid)) || rte->securityQuals != NIL || rte->tablesample)
		return NULL;
	if ((join = join_clause(and_list(sub->jointree->quals), &inner, &outer)) == NULL)
		return NULL;
	foreach_ptr(Node, q, and_list(sub->jointree->quals))
		if (q != (Node *) join)
			rest = lappend(rest, q);
	count_outer_refs((Node *) rest, &outer_refs);
	if (outer_refs > 0 || contain_volatile_functions((Node *) rest) || outer->varlevelsup != 1)
		return NULL;
	orte = rt_fetch(outer->varno, parse->rtable);
	if (orte->rtekind != RTE_RELATION || orte->securityQuals != NIL || !col_key_indexed(orte->relid, outer->varattno))
		return NULL;
	sub = copyObject(sub);
	sub->targetList = list_make1(makeTargetEntry((Expr *) copyObject(inner), 1, NULL, false));
	sub->jointree->quals = rest == NIL ? NULL : (Node *) make_ands_explicit(copyObject(rest));
	sub->limitCount = (Node *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(SIFT_HYB_ACL_PUSH_MAX + 1), false, FLOAT8PASSBYVAL);
	arr = makeNode(SubLink);
	arr->subLinkType = ARRAY_SUBLINK;
	arr->subselect = (Node *) sub;
	arr->location = -1;
	saop = makeNode(ScalarArrayOpExpr);
	saop->opno = join->opno;
	saop->opfuncid = get_opcode(join->opno);
	saop->useOr = true;
	saop->inputcollid = join->inputcollid;
	saop->args = list_make2(copyObject(outer), arr);
	((Var *) linitial(saop->args))->varlevelsup = 0;
	saop->location = -1;
	return (Node *) saop;
}

static Node *
boolean_test(Node *arg, BoolTestType type)
{
	BooleanTest *t = makeNode(BooleanTest);

	t->arg = (Expr *) arg;
	t->booltesttype = type;
	t->location = -1;
	return (Node *) t;
}

static bool
has_exists_sublink(List *quals)
{
	foreach_ptr(Node, q, quals)
		if (IsA(q, SubLink) && ((SubLink *) q)->subLinkType == EXISTS_SUBLINK)
			return true;
	return false;
}

/* The query reads one plain table, the only shape of a hybrid scan (sift_hybrid_pathlist): a partitioned table, an inheritance parent or a join gets no plan with its EXISTS as key sets */
static bool
one_plain_rel(Query *parse)
{
	RangeTblEntry *rte;

	if (list_length(parse->jointree->fromlist) != 1 || !IsA(linitial(parse->jointree->fromlist), RangeTblRef))
		return false;
	rte = rt_fetch(linitial_node(RangeTblRef, parse->jointree->fromlist)->rtindex, parse->rtable);
	return rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && !(rte->inh && has_subclass(rte->relid));
}

/* The query with a key set hint next to each EXISTS, or NULL when there is none to say (the query itself is never changed: PostgreSQL plans it as it is first) */
static Query *
exists_as_arrays(Query *parse)
{
	List	   *quals = and_list(parse->jointree != NULL ? parse->jointree->quals : NULL),
			   *out = NIL;
	Query	   *alt;
	bool		changed = false;

	if (parse->commandType != CMD_SELECT || parse->rowMarks != NIL || !has_exists_sublink(quals) || !one_plain_rel(parse) || !has_match((Node *) quals, NULL))
		return NULL;
	alt = copyObject(parse);
	foreach_ptr(Node, q, and_list(alt->jointree->quals))
	{
		Node	   *set = IsA(q, SubLink) ? exists_key_set(alt, (SubLink *) q) : NULL;

		changed |= set != NULL;
		if (set == NULL)
			out = lappend(out, q);
		else
			out = lappend(lappend(out, boolean_test(q, IS_TRUE)), boolean_test(set, IS_NOT_UNKNOWN));
	}
	if (!changed)
		return NULL;
	alt->jointree->quals = (Node *) make_ands_explicit(out);
	return alt;
}

static bool
is_hybrid_node(Plan *plan)
{
	if (plan == NULL)
		return false;
	if (IsA(plan, CustomScan) && (strcmp(((CustomScan *) plan)->methods->CustomName, "SiftHybridScan") == 0 || strcmp(((CustomScan *) plan)->methods->CustomName, "SiftHybridAgg") == 0))
		return true;
	return is_hybrid_node(plan->lefttree) || is_hybrid_node(plan->righttree);
}

/* The standard plan, unless the plan of the query with its EXISTS as key sets is one of a hybrid scan that costs no more */
static PlannedStmt *
semi_planner(SIFT_PLANNER_PARAMS)
{
	Query	   *alt = sift_enable_scan && sift_db_has_extension() ? exists_as_arrays(parse) : NULL;
	PlannedStmt *std = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) : standard_planner(SIFT_PLANNER_ARGS(parse));
	PlannedStmt *hyb;

	if (alt == NULL)
		return std;
	hyb = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(alt)) : standard_planner(SIFT_PLANNER_ARGS(alt));
	return is_hybrid_node(hyb->planTree) && hyb->planTree->total_cost <= std->planTree->total_cost ? hyb : std;
}

void
sift_hybrid_semi_init(void)
{
	prev_planner_hook = planner_hook;
	planner_hook = semi_planner;
}
