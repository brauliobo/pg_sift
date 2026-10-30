/*
 * agg/cost/cte.c - the rows of a CTE or sub-select the scan reads
 * Also the lists of keys that narrow them, and the ratios by which the rows of a derived
 * relation are lifted or kept.
 */
#include "postgres.h"

#include <math.h>

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/array.h"

#include "agg/agg.h"
#include "agg/cost/int.h"

static PlannerInfo *cte_inner_body(PlannerInfo *body, int *resno);
static double list_keys_worth(PlannerInfo *root, PlannerInfo *body, Var *key);

typedef struct KeyList
{
	List	   *tlist;
	Node	   *clause;
} KeyList;

/*
 * The rows of a sub-select or CTE scan whose filters PostgreSQL has no statistics for (a column of a window, an aggregate or a grouping
 * sets output) are its default guesses multiplied: two of them keep a thousandth of the rows, and every join above reads that as one row and
 * rescans an aggregate for each of the rows that come. In a statement that reads a table with a column index the rows are lifted to the geometric
 * mean of the guess and the rows (rel_filter_ratio), never below PostgreSQL's own. This is a floor, not an estimate: a count carried through
 * the levels from the column index would be one.
 */
void
derived_rel_rows(PlannerInfo *root, RelOptInfo *rel)
{
	double		rows;
	PlannerInfo *top = root;

	if (!sift_enable_agg || agg_stock_rows || rel->baserestrictinfo == NIL || rel->rows >= rel->tuples)
		return;
	rows = clamp_row_est(rel->rows * rel_filter_ratio(root, rel));
	if (rows <= rel->rows)
		return;
	while (top->parent_root)
		top = top->parent_root;
	if (!reads_column_index((Node *) top->parse, NULL))
		return;
	elog(DEBUG1, "sift: rows of a scan whose filters have no statistics %.0f, not %.0f (of %.0f)", rows, rel->rows, rel->tuples);
	agg_rows_lifted = true;
	rel->rows = rows;
	foreach_ptr(Path, p, rel->pathlist)
		p->rows = rows;
}

/*
 * the most rows a node expects among those that read a table with a column index, in a plan; *reads: this node reads one. A node that
 * keeps fewer rows than it reads (an aggregate, a grouping, a LIMIT, a set operation) ends the count: what comes out of it is its own
 * estimate, not the rows of the table below, which a body that aggregates does not return.
 */
static double
plan_leaf_rows(PlannerInfo *subroot, Plan *plan, bool *reads)
{
	double		rows = 0;
	bool		below = false;

	if (plan == NULL)
		return 0;
	check_stack_depth();
	switch (nodeTag(plan))
	{
		case T_Agg:
		case T_Group:
		case T_Unique:
		case T_Limit:
		case T_SetOp:
			{
				bool		r = false;

				plan_leaf_rows(subroot, plan->lefttree, &r);
				*reads = r;
				return plan->plan_rows;
			}
		case T_SeqScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid, subroot->parse->rtable);

				below = rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid);
				break;
			}
		case T_Append:
			foreach_ptr(Plan, c, ((Append *) plan)->appendplans)
			{
				bool		r = false;

				rows = Max(rows, plan_leaf_rows(subroot, c, &r));
				below |= r;
			}
			break;
		default:
			break;
	}
	for (int i = 0; i < 2; i++)
	{
		bool		r = false;

		rows = Max(rows, plan_leaf_rows(subroot, i == 0 ? plan->lefttree : plan->righttree, &r));
		below |= r;
	}
	*reads = below;
	return below ? Max(rows, plan->plan_rows) : rows;
}

/* the arguments of an equality of a column and a constant, or of a column and any of the constants of an array */
static List **
key_clause_args(Node *clause)
{
	if (IsA(clause, OpExpr))
		return &((OpExpr *) clause)->args;
	if (IsA(clause, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) clause)->useOr)
		return &((ScalarArrayOpExpr *) clause)->args;
	return NULL;
}

/* a column the CTE returns equal to a constant or to any of the constants of an array: the clause of a literal list, which is an OpExpr for one key */
static bool
key_list_walker(Node *node, KeyList *kl)
{
	List	  **args;

	if (node == NULL)
		return false;
	if ((args = key_clause_args(node)) != NULL && list_length(*args) == 2 && IsA(linitial(*args), Var) && IsA(lsecond(*args), Const) &&
		op_mergejoinable(IsA(node, OpExpr) ? ((OpExpr *) node)->opno : ((ScalarArrayOpExpr *) node)->opno, exprType(linitial(*args))))
		foreach_node(TargetEntry, te, kl->tlist)
			if (IsA(te->expr, Var) && ((Var *) te->expr)->varno == ((Var *) linitial(*args))->varno &&
				((Var *) te->expr)->varattno == ((Var *) linitial(*args))->varattno)
			{
				kl->clause = node;
				return true;
			}
	return expression_tree_walker(node, key_list_walker, kl);
}

/*
 * A CTE of the keys a request names (a literal list, kept to the keys a sub-select allows) is estimated at the rows of two selectivities
 * multiplied: a hundred keys joined to a hundred come out as one, and everything that reads the keys (the plan that keeps the CTE) is
 * priced for one key, whatever the keys own. The rows are lifted to the keys the list is worth for the table with a column index that the
 * keys are joined to: its selectivity from the statistics of that column (the share of the table the listed keys own, a key of the
 * most common values with its own frequency), over the share of one key. A list of keys with few rows stays at the rows PostgreSQL expects.
 */
static double
list_keys_worth(PlannerInfo *root, PlannerInfo *body, Var *key)
{
	KeyList		kl = {body->parse->targetList, NULL};
	Node	   *list;
	VariableStatData vardata;
	bool		isdefault;
	double		nd;

	if (body->parse->jointree == NULL)
		return 0;
	key_list_walker((Node *) body->parse->jointree, &kl);
	if ((list = kl.clause) == NULL || key->vartype != exprType(linitial(*key_clause_args(list))) || key->varcollid != exprCollation(linitial(*key_clause_args(list))))
		return 0;
	examine_variable(root, (Node *) key, 0, &vardata);
	nd = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	if (isdefault)
		return 0;
	list = copyObject(list);
	linitial(*key_clause_args(list)) = key;
	return clamp_row_est(nd * clause_selectivity(root, list, 0, JOIN_INNER, NULL));
}

static void
key_list_lift(PlannerInfo *root, RelOptInfo *rel, PlannerInfo *body)
{
	PlannerInfo *top = root;
	Var		   *key = NULL;
	double		rows;

	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		if (!bms_is_member(rel->relid, ec->ec_relids))
			continue;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			RangeTblEntry *rte;

			if (!IsA(em->em_expr, Var) || ((Var *) em->em_expr)->varlevelsup != 0)
				continue;
			rte = planner_rt_fetch(((Var *) em->em_expr)->varno, root);
			if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid))
				key = (Var *) em->em_expr;
		}
	}
	if (key == NULL)
		return;
	rows = list_keys_worth(root, body, key);
	foreach_node(TargetEntry, te, body->parse->targetList)
	{
		int			resno = te->resno;
		PlannerInfo *inner = cte_inner_body(body, &resno);

		if (inner != body)
			rows = Max(rows, list_keys_worth(root, inner, key));
	}
	while (top->parent_root)
		top = top->parent_root;
	if (rows <= rel->rows || !reads_column_index((Node *) top->parse, NULL))
		return;
	elog(DEBUG1, "sift: rows of a CTE scan %.0f, not %.0f (the keys of its list own that share of the table)", rows, rel->rows);
	agg_rows_lifted = true;
	rel->rows = rows;
	foreach_ptr(Path, p, rel->pathlist)
		p->rows = rows;
}

/* the number of the plan of a CTE (an index of root->glob->subroots and subplans, from 1), 0 when it has none */
static int
cte_plan_id(PlannerInfo *root, RangeTblEntry *rte)
{
	PlannerInfo *cteroot = root;
	int			ndx = 0;

	for (Index lv = rte->ctelevelsup; lv > 0; lv--)
		cteroot = cteroot->parent_root;
	foreach_node(CommonTableExpr, cte, cteroot->parse->cteList)
	{
		if (strcmp(cte->ctename, rte->ctename) == 0)
			break;
		ndx++;
	}
	return ndx < list_length(cteroot->cte_plan_ids) ? list_nth_int(cteroot->cte_plan_ids, ndx) : 0;
}

/* a column of a CTE that the query equates to the column v: the key of an inner join of a table with the keys of that CTE */
static Var *
cte_equal_var(PlannerInfo *body, Node *n, Var *v)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, FromExpr))
	{
		Var		   *r;

		foreach_ptr(Node, c, ((FromExpr *) n)->fromlist)
			if ((r = cte_equal_var(body, c, v)) != NULL)
				return r;
		return cte_equal_var(body, ((FromExpr *) n)->quals, v);
	}
	if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;
		Var		   *r;

		/* the rows of the preserved side of a left join are those of the keys, whatever it adds */
		if (j->jointype != JOIN_INNER && j->jointype != JOIN_LEFT)
			return NULL;
		if ((r = cte_equal_var(body, j->larg, v)) != NULL)
			return r;
		if (j->jointype == JOIN_LEFT)
			return NULL;
		if ((r = cte_equal_var(body, j->rarg, v)) != NULL)
			return r;
		return cte_equal_var(body, j->quals, v);
	}
	if (IsA(n, List) || (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop == AND_EXPR))
	{
		foreach_ptr(Node, a, IsA(n, List) ? (List *) n : ((BoolExpr *) n)->args)
		{
			Var		   *r = cte_equal_var(body, a, v);

			if (r != NULL)
				return r;
		}
		return NULL;
	}
	if (IsA(n, OpExpr) && list_length(((OpExpr *) n)->args) == 2 && op_mergejoinable(((OpExpr *) n)->opno, exprType(linitial(((OpExpr *) n)->args))))
	{
		Node	   *a = linitial(((OpExpr *) n)->args),
				   *b = lsecond(((OpExpr *) n)->args);

		if (IsA(a, Var) && IsA(b, Var))
		{
			Var		   *x = (Var *) a,
					   *y = (Var *) b;

			if (x->varno == v->varno && x->varattno == v->varattno && x->varlevelsup == 0 && y->varlevelsup == 0 && rt_fetch(y->varno, body->parse->rtable)->rtekind == RTE_CTE)
				return y;
			if (y->varno == v->varno && y->varattno == v->varattno && y->varlevelsup == 0 && x->varlevelsup == 0 && rt_fetch(x->varno, body->parse->rtable)->rtekind == RTE_CTE)
				return x;
		}
	}
	return NULL;
}

/*
 * The body a CTE column comes from when the CTE only passes it on: a join of the keys of another CTE with the table that names them (a CTE
 * of the table's rows for the ids of another) exports the column of the table, equal to that of the inner CTE, and the list that column holds is the inner
 * CTE's. *resno becomes the column of the inner body; body itself when the column is anything else.
 */
static PlannerInfo *
cte_inner_body(PlannerInfo *body, int *resno)
{
	for (;;)
	{
		RangeTblEntry *rte;
		Var		   *v = NULL;
		int			id;

		foreach_node(TargetEntry, te, body->parse->targetList)
			if (te->resno == *resno && !te->resjunk && IsA(te->expr, Var))
				v = (Var *) te->expr;
		if (v == NULL || v->varlevelsup != 0)
			return body;
		rte = rt_fetch(v->varno, body->parse->rtable);
		if (rte->rtekind == RTE_RELATION && body->parse->jointree != NULL)
		{
			Var		   *eq = cte_equal_var(body, (Node *) body->parse->jointree, v);

			if (eq == NULL)
				return body;
			v = eq;
			rte = rt_fetch(v->varno, body->parse->rtable);
		}
		if (rte->rtekind != RTE_CTE || rte->self_reference || (id = cte_plan_id(body, rte)) <= 0)
			return body;
		body = list_nth(body->glob->subroots, id - 1);
		*resno = v->varattno;
	}
}

/*
 * The keys a lookup that is a CTE of a literal list (key_list_walker) can hold, when its key `key` is the column the list is of: the constants of
 * the list. HUGE_VAL for any other lookup, and for a key that is another column of the CTE (the list says nothing of its values).
 */
double
cte_list_keys(AggCtx *c, const JoinCtx *jc, int key)
{
	RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);
	int			plan_id = rte->rtekind == RTE_CTE ? cte_plan_id(c->root, rte) : 0;
	PlannerInfo *body;
	KeyList		kl = {NULL, NULL};
	List	  **args;
	ArrayType  *arr;
	bool		found = false;
	int			resno = jc->j.keyatt[key];

	if (plan_id <= 0)
		return HUGE_VAL;
	body = cte_inner_body(list_nth(c->root->glob->subroots, plan_id - 1), &resno);
	kl.tlist = body->parse->targetList;
	if (body->parse->jointree == NULL)
		return HUGE_VAL;
	key_list_walker((Node *) body->parse->jointree, &kl);
	if (kl.clause == NULL || (args = key_clause_args(kl.clause)) == NULL || ((Const *) lsecond(*args))->constisnull)
		return HUGE_VAL;
	foreach_node(TargetEntry, te, kl.tlist)
		if (te->resno == resno)
		{
			Var		   *listed = linitial(*args);

			if (!IsA(te->expr, Var) || ((Var *) te->expr)->varno != listed->varno || ((Var *) te->expr)->varattno != listed->varattno)
				return HUGE_VAL;
			found = true;
		}
	if (!found)
		return HUGE_VAL;
	if (IsA(kl.clause, OpExpr))
		return 1.0;
	arr = DatumGetArrayTypeP(((Const *) lsecond(*args))->constvalue);
	return ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
}

/* the keys of the table the literal list a lookup CTE holds is worth (list_keys_worth), for its column k: 0 for any other lookup */
double
cte_list_worth(AggCtx *c, const JoinCtx *jc, int k, Var *key)
{
	RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);
	int			plan_id = rte->rtekind == RTE_CTE ? cte_plan_id(c->root, rte) : 0;
	int			resno = jc->j.keyatt[k];

	return plan_id > 0 ? list_keys_worth(c->root, cte_inner_body(list_nth(c->root->glob->subroots, plan_id - 1), &resno), key) : 0;
}

/*
 * A shared CTE whose body joins a table with a column index to filtered tables is estimated at a row or two (each join keeps the share of a
 * default guess, and a dimension filter that owns all the rows of the table keeps a hundredth), and everything that reads it is priced from
 * that: the shared plan costs the same at ten times the rows as at one, and the scans that replace it lose the comparison. The rows of the
 * scan are lifted to the geometric mean of the estimate and the largest result the body builds from the table, as derived_rel_rows does for a
 * filter; never fewer rows than PostgreSQL's own. This is a floor, not an estimate. It is made for the shared plan only: the inline plan
 * (agg_variant) has no such CTE to price, and the aggregates the scans leave in it would be priced for more rows than the scans read.
 */
void
derived_cte_rows(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	int			plan_id;
	double		leaf,
				rows;
	bool		reads;

	if (!sift_enable_agg || agg_stock_rows || agg_variant != NULL || rte->self_reference || (plan_id = cte_plan_id(root, rte)) <= 0)
		return;
	leaf = plan_leaf_rows(list_nth(root->glob->subroots, plan_id - 1), list_nth(root->glob->subplans, plan_id - 1), &reads);
	if (!reads)
	{
		key_list_lift(root, rel, list_nth(root->glob->subroots, plan_id - 1));
		return;
	}
	rows = clamp_row_est(sqrt(rel->rows * leaf));
	if (rows <= rel->rows)
		return;
	elog(DEBUG1, "sift: rows of a CTE scan %.0f, not %.0f (its body builds %.0f from a table with a column index)", rows, rel->rows, leaf);
	agg_cte_lift = Max(agg_cte_lift, rows / rel->rows);
	agg_rows_lifted = true;
	rel->rows = rows;
	foreach_ptr(Path, p, rel->pathlist)
		p->rows = rows;
}
