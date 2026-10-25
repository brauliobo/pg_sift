/*
 * agg/cte.c - a CTE that several aggregates read, planned inline when that lets them run on the column index.
 *
 * PostgreSQL computes a CTE that the query names more than once exactly once and hands its rows to every reference, so the
 * aggregates over it (a time series at four granularities, each arm a GROUP BY of the same CTE) read a materialized copy and
 * never the index. The same query with the CTE written NOT MATERIALIZED is the same query: PostgreSQL itself inlines a CTE
 * with no side effects and no volatile function whenever the user says so, and refuses otherwise. This hook plans the query
 * both ways when a CTE over a table with a column index is referenced more than once, and keeps the inline plan if it has
 * a Custom Scan (SiftAgg) and costs less. Both plans are plans of the same query, so the rows are the same.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "catalog/pg_class.h"
#include "executor/executor.h"
#include "jit/jit.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/plancache.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/colsrc.h"
#include "agg/plan_int.h"
#include "agg/refuse.h"
#include "agg/rows.h"

/*
 * The inline plan is kept when the scans cost this many times less than what the shared plan spends on the work they replace (the plans of the
 * CTEs and the aggregates that read them). Both are prices of the same rows, so the plan above them, which PostgreSQL estimates from the rows
 * each plan expects (the shared CTE one row, the scans a group count, and a join on an expression key the default selectivity), is not part of
 * the comparison. The factor covers the scans' unnest charge (agg_cost prices the plain plan with the 100 rows per call that PostgreSQL assumes
 * for jsonb_to_recordset, the data has a handful): 56 statements of a 195,000-row slice priced 1.15 to 1.6 times cheaper on the scans ran 63 s shared
 * and 23 s inline, none slower (the one that measured 1.3 times slower under load tied on a repeat). The statements priced less than 1.15 times
 * cheaper are not measured.
 */
#define CTE_INLINE_MARGIN	1.15

/* A plan above the scans priced this many times the shared plan has an estimate that cannot be trusted (a nested loop over a join of rows it takes for one). */
#define CTE_INLINE_ABOVE	20.0

/* ... and where an ordinary plan still reads the table, the inline plan may total this many times the shared one at most (priced 1.06 to 1.22 times above it, run 1.2 to 1.5 times faster) */
#define CTE_INLINE_TOTALS	1.25

static planner_hook_type prev_planner_hook;
static ExecutorStart_hook_type prev_executor_start;

/* sift_planner has planned a statement of this backend */
static bool hook_ran;

/* the first statement to start after the library loaded has been looked at (sift_executor_start) */
static bool late_checked;

/* set by plan_cte: it returned the inline plan, because its scans cost less than the shared plan's work */
static bool inline_won;

/* a table the aggregate scan reads: a sift_table, or a heap table with a column index */
bool
has_column_source(Oid relid)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	bool		found = sift_colsrc_is_table(rel->rd_rel->relam);

	foreach_oid(ix, RelationGetIndexList(rel))
		found |= sift_colsrc_is_index(get_rel_relam(ix));
	relation_close(rel, AccessShareLock);
	return found;
}

/*
 * does the query select from a table that has a column index, itself, through a sub-select or through a CTE of the same WITH (which PostgreSQL
 * inlines when one query reads it)? level: how many query levels the query is below the WITH
 */
static bool
reads_indexed_table(Query *q, List *siblings, int depth, int level)
{
	foreach_node(RangeTblEntry, rte, q->rtable)
	{
		if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid))
			return true;
		if (depth <= 0)
			continue;
		if (rte->rtekind == RTE_SUBQUERY && reads_indexed_table(rte->subquery, siblings, depth - 1, level + 1))
			return true;
		if (rte->rtekind != RTE_CTE || rte->ctelevelsup != level)
			continue;
		foreach_node(CommonTableExpr, sib, siblings)
			if (strcmp(sib->ctename, rte->ctename) == 0 && !sib->cterecursive && IsA(sib->ctequery, Query) &&
				reads_indexed_table(castNode(Query, sib->ctequery), siblings, depth - 1, 1))
				return true;
	}
	return false;
}

/* a CTE that is read twice or more and selects from a table that has a column index; siblings: the CTEs of its WITH */
static bool
shared_cte_over_column_index(CommonTableExpr *cte, List *siblings)
{
	Query	   *q = castNode(Query, cte->ctequery);

	if (cte->cterecursive || cte->cterefcount < 2 || cte->ctematerialized != CTEMaterializeDefault || q->commandType != CMD_SELECT)
		return false;
	return reads_indexed_table(q, siblings, 4, 1);
}

/* does the query hold a Var of a level above the sub-select it is walked as? depth: the sub-selects below it */
static bool
outer_var_walker(Node *n, int *depth)
{
	bool		found;

	if (n == NULL)
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varlevelsup > *depth;
	if (!IsA(n, Query))
		return expression_tree_walker(n, outer_var_walker, depth);
	(*depth)++;
	found = query_tree_walker((Query *) n, outer_var_walker, depth, 0);
	(*depth)--;
	return found;
}

/* does the query, or a sub-select of it, aggregate, window or combine rows (work, not a lookup of a table)? */
static bool
computes_walker(Node *n, void *context)
{
	Query	   *q;

	if (n == NULL)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, computes_walker, context);
	q = (Query *) n;
	return q->hasAggs || q->hasWindowFuncs || q->groupClause != NIL || q->distinctClause != NIL || q->setOperations != NULL ||
		query_tree_walker(q, computes_walker, context, 0);
}

/*
 * A shared CTE written inline is copied into every query that reads it, with the sub-selects of its WHERE: one that computes something (the ranking
 * of the keys with the most rows, two aggregates of the table) is then planned and run once per copy. It becomes a CTE of the same WITH, read
 * by the copies, which PostgreSQL computes once. Only an IN sub-select that reads nothing of the query around it moves.
 */
static void
hoist_sublinks(Node *quals, List **hoisted, int *nhoist)
{
	if (quals != NULL && IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == AND_EXPR)
	{
		foreach_ptr(Node, arg, ((BoolExpr *) quals)->args)
			hoist_sublinks(arg, hoisted, nhoist);
	}
	else if (quals != NULL && IsA(quals, SubLink))
	{
		SubLink    *sl = (SubLink *) quals;
		Query	   *sub;
		int			depth = 0;

		if (sl->subLinkType != ANY_SUBLINK || !IsA(sl->subselect, Query))
			return;
		sub = castNode(Query, sl->subselect);
		if (!computes_walker((Node *) sub, NULL) || query_tree_walker(sub, outer_var_walker, &depth, 0))
			return;
		{
			CommonTableExpr *h = makeNode(CommonTableExpr);
			Query	   *read = makeNode(Query);
			RangeTblEntry *rte = makeNode(RangeTblEntry);
			RangeTblRef *rtr = makeNode(RangeTblRef);

			/* the sub-select leaves the CTE body for the WITH: the CTEs it reads are one level nearer */
			IncrementVarSublevelsUp((Node *) sub, -1, 2);
			h->ctename = psprintf("sift_hoisted_%d", ++*nhoist);
			h->ctematerialized = CTEMaterializeAlways;
			h->ctequery = (Node *) sub;
			h->cterefcount = 1;
			h->location = -1;
			foreach_node(TargetEntry, tle, sub->targetList)
			{
				if (tle->resjunk)
					continue;
				h->ctecolnames = lappend(h->ctecolnames, makeString(tle->resname));
				h->ctecoltypes = lappend_oid(h->ctecoltypes, exprType((Node *) tle->expr));
				h->ctecoltypmods = lappend_int(h->ctecoltypmods, exprTypmod((Node *) tle->expr));
				h->ctecolcollations = lappend_oid(h->ctecolcollations, exprCollation((Node *) tle->expr));
			}
			rte->rtekind = RTE_CTE;
			rte->ctename = h->ctename;
			rte->ctelevelsup = 2;
			rte->coltypes = h->ctecoltypes;
			rte->coltypmods = h->ctecoltypmods;
			rte->colcollations = h->ctecolcollations;
			rte->eref = makeAlias(h->ctename, h->ctecolnames);
			rte->inFromCl = true;
			rtr->rtindex = 1;
			read->commandType = CMD_SELECT;
			read->querySource = QSRC_ORIGINAL;
			read->canSetTag = true;
			read->rtable = list_make1(rte);
			read->jointree = makeFromExpr(list_make1(rtr), NULL);
			for (int i = 0; i < list_length(h->ctecolnames); i++)
				read->targetList = lappend(read->targetList,
										   makeTargetEntry((Expr *) makeVar(1, i + 1, list_nth_oid(h->ctecoltypes, i), list_nth_int(h->ctecoltypmods, i),
																		   list_nth_oid(h->ctecolcollations, i), 0),
														   i + 1, strVal(list_nth(h->ctecolnames, i)), false));
			sl->subselect = (Node *) read;
			*hoisted = lappend(*hoisted, h);
		}
	}
}

/* what the walk below finds: with inline_them the shared CTEs are all written NOT MATERIALIZED, and their names are kept in *names */
typedef struct SharedCteWalk
{
	bool		inline_them;
	List	   *names;
	int			hoisted;
} SharedCteWalk;

/* does the query, or a sub-select or CTE of it, hold a shared CTE over a column index? With inline_them the walk goes on (it is of a copy of the query) */
static bool
shared_cte_walker(Node *n, SharedCteWalk *w)
{
	List	   *ctes = NIL;

	if (n == NULL)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, shared_cte_walker, w);
	foreach_node(CommonTableExpr, cte, ((Query *) n)->cteList)
	{
		Query	   *body = castNode(Query, cte->ctequery);

		if (shared_cte_over_column_index(cte, ((Query *) n)->cteList))
		{
			if (!w->inline_them)
				return true;
			cte->ctematerialized = CTEMaterializeNever;
			w->names = lappend(w->names, cte->ctename);
			/* ahead of the CTE: the WITH is read in order */
			if (body->cteList == NIL && body->jointree != NULL)
				hoist_sublinks(body->jointree->quals, &ctes, &w->hoisted);
		}
		ctes = lappend(ctes, cte);
	}
	if (w->inline_them)
		((Query *) n)->cteList = ctes;
	return query_tree_walker((Query *) n, shared_cte_walker, w, 0);
}

static bool
has_shared_cte(Query *parse)
{
	SharedCteWalk w = {false, NIL};

	return parse->commandType == CMD_SELECT && parse->rowMarks == NIL && shared_cte_walker((Node *) parse, &w);
}

/* the plans a plan reads besides its left and right tree */
static List *
child_plans(Plan *plan)
{
	switch (nodeTag(plan))
	{
		case T_Append:
			return ((Append *) plan)->appendplans;
		case T_MergeAppend:
			return ((MergeAppend *) plan)->mergeplans;
		case T_SubqueryScan:
			return list_make1(((SubqueryScan *) plan)->subplan);
		case T_CustomScan:
			return ((CustomScan *) plan)->custom_plans;
		default:
			return NIL;
	}
}

typedef struct ScanArms
{
	double		total;
	double		first;			/* the dearest arm */
	double		vet;			/* of total, what the arms pay to check the heap for the pages that are not all-visible */
	double		vet_pct;		/* and the largest share of the pages that are not */
	int			n;
} ScanArms;

/*
 * The SiftAgg nodes of the plan (they hold what is below them). The arms below a SiftAggRows node run as the sets of one pass over the index
 * (agg/fuse.c), so they cost what plan_rows.c credits them; the node's own work and the join below it are the plan above the scans, as they
 * are in the shared plan.
 */
static void
sift_agg_arms(Plan *plan, ScanArms *a)
{
	if (plan == NULL)
		return;
	if (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_plan_methods)
	{
		double		pct = 0;

		a->total += plan->total_cost;
		a->first = Max(a->first, plan->total_cost);
		a->vet += agg_vet_of(plan->total_cost, &pct);
		a->vet_pct = Max(a->vet_pct, pct);
		a->n++;
		return;
	}
	if (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_rows_plan_methods)
	{
		ScanArms	below = {0};
		double		fused;

		foreach_ptr(Plan, p, ((CustomScan *) plan)->custom_plans)
			sift_agg_arms(p, &below);
		fused = below.total - (below.total - below.first) * (1.0 - AGG_FUSED_ARM);
		a->total += fused;
		a->first = Max(a->first, below.first);
		a->vet += below.total > 0 ? below.vet * fused / below.total : 0;
		a->vet_pct = Max(a->vet_pct, below.vet_pct);
		a->n += below.n;
		return;
	}
	foreach_ptr(Plan, p, child_plans(plan))
		sift_agg_arms(p, a);
	sift_agg_arms(plan->lefttree, a);
	sift_agg_arms(plan->righttree, a);
}

static void
sift_agg_arms_list(List *plans, ScanArms *a)
{
	foreach_ptr(Plan, p, plans)
		sift_agg_arms(p, a);
}

static bool
uses_sift_agg(Plan *plan)
{
	ScanArms	a = {0};

	sift_agg_arms(plan, &a);
	return a.n > 0;
}

static bool
any_sift_agg(List *plans)
{
	ScanArms	a = {0};

	sift_agg_arms_list(plans, &a);
	return a.n > 0;
}

/* the CTEs (plan ids) that a CteScan of the plan, or of a CTE it reads, names */
static void cte_refs(PlannedStmt *stmt, Plan *plan, Bitmapset **refs);

static void
cte_refs_list(PlannedStmt *stmt, List *plans, Bitmapset **refs)
{
	foreach_ptr(Plan, p, plans)
		cte_refs(stmt, p, refs);
}

static void
cte_refs(PlannedStmt *stmt, Plan *plan, Bitmapset **refs)
{
	if (plan == NULL)
		return;
	if (IsA(plan, CteScan) && !bms_is_member(((CteScan *) plan)->ctePlanId, *refs))
	{
		*refs = bms_add_member(*refs, ((CteScan *) plan)->ctePlanId);
		cte_refs(stmt, list_nth(stmt->subplans, ((CteScan *) plan)->ctePlanId - 1), refs);
	}
	switch (nodeTag(plan))
	{
		case T_Append:
			cte_refs_list(stmt, ((Append *) plan)->appendplans, refs);
			break;
		case T_MergeAppend:
			cte_refs_list(stmt, ((MergeAppend *) plan)->mergeplans, refs);
			break;
		case T_SubqueryScan:
			cte_refs(stmt, ((SubqueryScan *) plan)->subplan, refs);
			break;
		case T_CustomScan:
			cte_refs_list(stmt, ((CustomScan *) plan)->custom_plans, refs);
			break;
		default:
			break;
	}
	cte_refs(stmt, plan->lefttree, refs);
	cte_refs(stmt, plan->righttree, refs);
}

/*
 * PostgreSQL charges the plan the cost of every CTE it computes, also of one that no node reads: a SiftAgg node that aggregates the
 * arms of a UNION ALL in one scan of the index (or reads the table of the CTE itself) leaves the CTE unread, and it is not run.
 */
static double
plan_cost_without_unread_ctes(PlannedStmt *stmt)
{
	Bitmapset  *refs = NULL;
	double		cost = stmt->planTree->total_cost;
	int			id = 0;

	cte_refs(stmt, stmt->planTree, &refs);
	foreach_ptr(Plan, sub, stmt->subplans)
		if (!bms_is_member(++id, refs) && sub != NULL)
			cost -= sub->total_cost;
	return Max(cost, 0.0);
}

/*
 * What the shared plan spends on the work the scans take over: the plans of the CTEs that the inline plan has no more (once each), and the
 * aggregates that read them (the node a scan replaces; a read with no aggregate above it counts as itself).
 */
typedef struct SharedWork
{
	PlannedStmt *stmt;
	List	   *names;
	Bitmapset  *seen;
	List	   *arms;
	double		cost;
	double		read_rows;		/* the rows of elements the scans of the inline plan read: what the aggregates of the shared plan have to unnest */
	double		charged;		/* what the aggregates are charged for the rows their estimate lost (not in cost) */
	int			unnesting;		/* the aggregates charged */
	double		lift;			/* how many times derived_cte_rows raised the rows of the CTE scans of the shared plan */
} SharedWork;

/* does a plan below this node unnest an array of the row (a lateral function)? */
static bool
unnests(PlannedStmt *stmt, Plan *plan)
{
	if (plan == NULL)
		return false;
	if (IsA(plan, FunctionScan) && rt_fetch(((Scan *) plan)->scanrelid, stmt->rtable)->lateral)
		return true;
	foreach_ptr(Plan, p, child_plans(plan))
		if (unnests(stmt, p))
			return true;
	return unnests(stmt, plan->lefttree) || unnests(stmt, plan->righttree);
}

/*
 * The aggregate of the shared plan that unnests the rows of a CTE is priced for the rows PostgreSQL expects of it, which fall to a default (a
 * row, the 100 rows of a function) where the scans of the inline plan read tens of thousands: the plan that keeps the CTE has to produce them.
 * The square root of the excess is charged, as input_rows_ratio charges the arm a scan compares with (agg/plan_cost.c). An aggregate that
 * does not unnest keeps its estimate, which the statistics of its tables count.
 */
static double
arm_rows_charge(SharedWork *w, Plan *agg)
{
	double		f;

	if (w->read_rows <= 0 || agg->lefttree == NULL || !unnests(w->stmt, agg->lefttree))
		return 0;
	f = sqrt(w->read_rows * w->lift / Max(agg->lefttree->plan_rows, 1.0));
	return f > 1.0 ? agg->total_cost * (f - 1.0) : 0;
}

static void
shared_work(SharedWork *w, Plan *plan, Plan *agg)
{
	if (plan == NULL)
		return;
	if (IsA(plan, Agg))
		agg = plan;
	if (IsA(plan, CteScan))
	{
		int			id = ((CteScan *) plan)->ctePlanId;
		RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid, w->stmt->rtable);
		bool		named = false;
		Plan	   *body = list_nth(w->stmt->subplans, id - 1);

		foreach_ptr(char, name, w->names)
			named |= strcmp(name, rte->ctename) == 0;
		if (named)
		{
			if (agg == NULL)
				w->cost += plan->total_cost;
			else if (!list_member_ptr(w->arms, agg))
			{
				double		extra = arm_rows_charge(w, agg);

				w->arms = lappend(w->arms, agg);
				w->cost += agg->total_cost;
				w->charged += extra;
				w->unnesting += extra > 0;
			}
		}
		if (!bms_is_member(id, w->seen))
		{
			w->seen = bms_add_member(w->seen, id);
			if (named)
				w->cost += body->total_cost;
			shared_work(w, body, NULL);
		}
	}
	foreach_ptr(Plan, p, child_plans(plan))
		shared_work(w, p, agg);
	shared_work(w, plan->lefttree, agg);
	shared_work(w, plan->righttree, agg);
}

/*
 * The charge stands when the scans replace most of the aggregates that unnest the CTE. The rest stays an ordinary plan in the inline plan, as
 * underpriced there as in the shared one, and the copies of the CTE body it reads in the inline plan decide the comparison.
 */
static double
shared_work_cost(PlannedStmt *stmt, List *names, double read_rows, double lift, int nscans, double *charged, int *unnesting)
{
	SharedWork	w = {stmt, names, NULL, NIL, 0, read_rows, 0, 0, lift};

	shared_work(&w, stmt->planTree, NULL);
	*charged = 2 * nscans > w.unnesting ? w.charged : 0;
	*unnesting = w.unnesting;
	return w.cost + *charged;
}

/* does the plan read a table that has a column index other than through a SiftAgg node? */
static bool
plain_reader(PlannedStmt *stmt, Plan *plan)
{
	if (plan == NULL)
		return false;
	if (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_plan_methods)
		return false;
	if (IsA(plan, SeqScan) || IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) || IsA(plan, BitmapHeapScan))
	{
		RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid, stmt->rtable);

		if (rte->rtekind == RTE_RELATION && has_column_source(rte->relid))
			return true;
	}
	foreach_ptr(Plan, p, child_plans(plan))
		if (plain_reader(stmt, p))
			return true;
	return plain_reader(stmt, plan->lefttree) || plain_reader(stmt, plan->righttree);
}

static bool
plain_reader_stmt(PlannedStmt *stmt)
{
	if (plain_reader(stmt, stmt->planTree))
		return true;
	foreach_ptr(Plan, p, stmt->subplans)
		if (plain_reader(stmt, p))
			return true;
	return false;
}

/*
 * A plan keeps every path and relation the planner built for it, several times the size of the plan itself, and a statement is planned up to
 * four times (shared or inline CTEs, as it is or flattened) of which one is kept. Each plan is made in a context of its own, a child of the
 * caller's, which is deleted when its plan loses: the statement holds the memory of one planning, not of all.
 */
static PlannedStmt *
plan_in_own_context(SIFT_PLANNER_PARAMS, MemoryContext *cx)
{
	MemoryContext old;
	PlannedStmt *stmt;

	*cx = AllocSetContextCreate(CurrentMemoryContext, "sift plan variant", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(*cx);
	stmt = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) : standard_planner(SIFT_PLANNER_ARGS(parse));
	MemoryContextSwitchTo(old);
	return stmt;
}

/*
 * The contexts of the plans that lost are deleted when the hook is done: the memory they free then is the executor's to reuse, where a free
 * between two plans returns it to the system (and the next plan fills fresh pages).
 */
static List *discarded;

static void
discard_plan(MemoryContext cx)
{
	discarded = lappend(discarded, cx);
}

static void
delete_discarded(void)
{
	List	   *dead = discarded;

	discarded = NIL;
	foreach_ptr(MemoryContextData, cx, dead)
		MemoryContextDelete(cx);
	list_free(dead);
}

/* the plans of a statement with its shared CTEs and with them inlined, and what the scans of the inline plan and the work of the shared one cost */
typedef struct CteVariants
{
	PlannedStmt *plain,
			   *inlined;
	MemoryContext plain_cx,
				inlined_cx;
	ScanArms	scans;
	double		plain_cost,
				work;
} CteVariants;

/*
 * The plan that keeps the shared CTEs, and in lift what agg_cte_lift came to while it was planned (the plan is the only one that is priced with it).
 * Memoize over a lookup of a CTE's rows is priced on hits a near-unique key (a jsonb document) does not give, so the shared plan would look cheaper than it is.
 */
static PlannedStmt *
plan_shared(SIFT_PLANNER_PARAMS, MemoryContext *cx, double *lift)
{
	double		outer_lift = agg_cte_lift;
	bool		outer_memoize = enable_memoize;
	PlannedStmt *stmt;

	agg_cte_lift = 1.0;
	PG_TRY();
	{
		enable_memoize = false;
		stmt = plan_in_own_context(SIFT_PLANNER_ARGS(parse), cx);
	}
	PG_FINALLY();
	{
		enable_memoize = outer_memoize;
	}
	PG_END_TRY();
	*lift = agg_cte_lift;
	agg_cte_lift = outer_lift;
	return stmt;
}

/* both queries are consumed by the planner */
static void
plan_variants(SIFT_PLANNER_PARAMS, Query *inline_parse, List *names, CteVariants *v)
{
	const char *outer_variant;
	double		read_rows,
				lift,
				charged;
	int			unnesting;

	memset(v, 0, sizeof(*v));
	v->plain = plan_shared(SIFT_PLANNER_ARGS(parse), &v->plain_cx, &lift);
	outer_variant = agg_variant_push("inline");
	agg_read_rows = 0;
	v->inlined = plan_in_own_context(SIFT_PLANNER_ARGS(inline_parse), &v->inlined_cx);
	read_rows = agg_read_rows;
	agg_variant = outer_variant;
	v->plain_cost = plan_cost_without_unread_ctes(v->plain);
	sift_agg_arms(v->inlined->planTree, &v->scans);
	sift_agg_arms_list(v->inlined->subplans, &v->scans);
	v->work = shared_work_cost(v->plain, names, read_rows, lift, v->scans.n, &charged, &unnesting);
	/* the shared plan really costs what its aggregates were charged: the totals are compared with it */
	v->plain_cost += charged;
	elog(DEBUG1, "sift: shared CTE plain cost %.2f, inline cost %.2f, scans %.2f, shared work %.2f, %d scans, %.0f rows read, %d unnesting aggregates, charged %.2f",
		 v->plain_cost, v->inlined->planTree->total_cost, v->scans.total, v->work, v->scans.n, read_rows, unnesting, charged);
}

/*
 * When the scans read the table, they are weighed against the work they replace. When an ordinary plan still reads it (the index lacks a
 * column of an aggregate) the plan reads the table twice: the scans must still be cheaper than the work they replace, and the totals must
 * not be far apart (the ordinary plan is priced twice in the inline plan, and a total that far above the shared one is a mis-estimate).
 * vet and shared_vet: the heap check taken off the inline plan's scans and off the SiftAgg nodes of the shared plan (both 0 for the choice).
 */
static bool
inline_wins(const CteVariants *v, double vet, double shared_vet)
{
	double		scans = v->scans.total - vet,
				inline_cost = v->inlined->planTree->total_cost - vet,
				plain_cost = v->plain_cost - shared_vet,
				work = v->work - shared_vet;

	return v->scans.n > 0 && (sift_agg_force || (!plain_reader_stmt(v->inlined) ? CTE_INLINE_MARGIN * scans < work && inline_cost < CTE_INLINE_ABOVE * plain_cost :
							  (CTE_INLINE_MARGIN * scans < work && inline_cost < CTE_INLINE_TOTALS * plain_cost) ||
							  scans < plain_cost - inline_cost));
}

/*
 * The scans cost less than the work they replace but not by the margin: a close call at this size. The choice that is right at 10 times the
 * rows and at 100 times (sift.agg_scale_horizon) is right now: the scans grow with the rows they read, the plan of the shared CTE with its sorts
 * and hash tables that outgrow work_mem grows faster, and a plan that is a tie today and a loss to the normal one at the size the table is going
 * to have is not worth keeping. The statement is planned again at each size, from the untouched copies.
 */
static bool
inline_wins_at_scale(SIFT_PLANNER_PARAMS, Query *pristine_inline, List *names)
{
	double		horizon = sift_agg_scale_horizon,
				saved = sift_plan_scale;
	bool		quiet = agg_quiet_reports(true),
				wins = true;

	PG_TRY();
	{
		for (int i = 0; i < 2 && wins; i++)
		{
			CteVariants v;

			sift_plan_scale = saved * (i == 0 ? sqrt(horizon) : horizon);
			plan_variants(SIFT_PLANNER_ARGS(copyObject(parse)), copyObject(pristine_inline), names, &v);
			wins = inline_wins(&v, 0, 0);
			discard_plan(v.plain_cx);
			discard_plan(v.inlined_cx);
		}
	}
	PG_FINALLY();
	{
		sift_plan_scale = saved;
		agg_quiet_reports(quiet);
	}
	PG_END_TRY();
	return wins;
}

/*
 * The first plan of a statement and its flattened one (plan_flat) are two queries that PostgreSQL may plan to the same costs: the flat one is
 * then decided as the first was, without the plans at the larger sizes.
 */
static struct
{
	bool		set,
				won;
	double		plain,
				inlined,
				scans,
				work;
}			first_plan;
static bool planning_second;

/* set for the plan in full that follows a verdict the plan of the variant did not confirm: that plan reads no verdict */
static bool verdict_replan;

/*
 * The statement planned as the verdict kept for it says (verdict.c): the inline plan when the scans won, else the plan that keeps the shared CTEs.
 * The totals are those of the plan in full that made the verdict.
 */
static PlannedStmt *
plan_cached(SIFT_PLANNER_PARAMS, const CteVerdict *vd, MemoryContext *cx)
{
	PlannedStmt *stmt;

	agg_report("verdict_cached", AGG_REFUSE_NONE, NULL, true, 0, 0);
	if (vd->won)
	{
		const char *outer_variant = agg_variant_push("inline");
		Query	   *inline_parse = copyObject(parse);

		shared_cte_walker((Node *) inline_parse, &(SharedCteWalk) {true, NIL});

		stmt = plan_in_own_context(SIFT_PLANNER_ARGS(inline_parse), cx);
		agg_variant = outer_variant;
		inline_won = true;
		agg_report("cte_inline", AGG_REFUSE_NONE, NULL, true, vd->scans, vd->shared);
	}
	else
	{
		double		lift;

		stmt = plan_shared(SIFT_PLANNER_ARGS(parse), cx, &lift);
		if (vd->scans > 0)
			agg_report("cte_inline", AGG_REFUSE_CTE_COST, "from the verdict cached", false, vd->scans, vd->shared);
	}
	elog(DEBUG1, "sift: shared CTE verdict from the cache: %s", vd->won ? "inline" : "shared");
	return stmt;
}

/* the plan of the statement and the context it is in, which the caller deletes when it plans another statement instead */
static PlannedStmt *
plan_cte(SIFT_PLANNER_PARAMS, MemoryContext *cx)
{
	Query	   *inline_parse,
			   *pristine = NULL,
			   *pristine_inline = NULL;
	CteVariants v;
	SharedCteWalk walk = {true, NIL};
	CteVerdict	vd;
	bool		same,
				won,
				replan = verdict_replan;

	inline_won = false;
	verdict_replan = false;
	if (!sift_enable_agg || !has_shared_cte(parse))
		return plan_in_own_context(SIFT_PLANNER_ARGS(parse), cx);
	agg_vet_forget();
	verdict_lookup(parse, VERDICT_CTE, &vd);
	if (vd.known && !replan)
	{
		/*
		 * The shared plan consumes the tree and the inline plan a copy of it: where the plan costs far from what the verdict was made from (verdict_accept), the
		 * statement is planned again from the tree that is left (the inline plan) or from a copy made for it (the shared plan).
		 */
		Query	   *again = vd.won ? parse : vd.inl > 0 ? copyObject(parse) : NULL;
		PlannedStmt *stmt = plan_cached(SIFT_PLANNER_ARGS(parse), &vd, cx);
		int			r = verdict_accept(&vd, stmt->planTree->total_cost);

		if (r == VERDICT_TAKE)
			return stmt;
		discard_plan(*cx);
		if (r == VERDICT_SWITCH)
			return plan_cached(SIFT_PLANNER_ARGS(again), &vd, cx);
		verdict_replan = true;
		return plan_cte(SIFT_PLANNER_ARGS(again), cx);
	}
	inline_parse = copyObject(parse);
	shared_cte_walker((Node *) inline_parse, &walk);
	if (sift_agg_scale_horizon > 1.0)
	{
		pristine = copyObject(parse);
		pristine_inline = copyObject(inline_parse);
	}
	plan_variants(SIFT_PLANNER_ARGS(parse), inline_parse, walk.names, &v);
	same = planning_second && first_plan.set && first_plan.plain == v.plain_cost && first_plan.inlined == v.inlined->planTree->total_cost &&
		first_plan.scans == v.scans.total && first_plan.work == v.work;
	won = inline_wins(&v, 0, 0);
	if (!won && pristine != NULL && v.scans.n > 0 && v.scans.total < v.work && CTE_INLINE_MARGIN * v.scans.total >= v.work)
		won = same ? first_plan.won : inline_wins_at_scale(SIFT_PLANNER_ARGS(pristine), pristine_inline, walk.names);
	verdict_remember(&vd, won, v.scans.n > 0 ? v.inlined->planTree->total_cost : 0, v.plain_cost, v.inlined->planTree->total_cost, v.plain->planTree->total_cost);
	if (!planning_second)
	{
		first_plan.set = true;
		first_plan.won = won;
		first_plan.plain = v.plain_cost;
		first_plan.inlined = v.inlined->planTree->total_cost;
		first_plan.scans = v.scans.total;
		first_plan.work = v.work;
	}
	if (won)
	{
		agg_report("cte_inline", AGG_REFUSE_NONE, NULL, true, v.inlined->planTree->total_cost, v.plain_cost);
		inline_won = true;
		discard_plan(v.plain_cx);
		*cx = v.inlined_cx;
		return v.inlined;
	}
	if (sift_explain_refusals && v.scans.n > 0)
	{
		ScanArms	shared = {0};
		const char *detail = psprintf("inline costs %.2f (the scans %.2f), the shared CTE %.2f (the work the scans replace %.2f)", v.inlined->planTree->total_cost, v.scans.total, v.plain_cost, v.work);

		sift_agg_arms(v.plain->planTree, &shared);
		sift_agg_arms_list(v.plain->subplans, &shared);
		/* the heap check decided it when the scans would have won without it, in both plans (a rewrite leaves the visibility map empty) */
		if (v.scans.vet > 0 && inline_wins(&v, v.scans.vet, shared.vet))
			detail = psprintf("%s; %.0f%% of the heap pages are not all-visible, which adds %.2f to the scans: VACUUM (ANALYZE) the table", detail, v.scans.vet_pct, v.scans.vet);
		agg_report("cte_inline", AGG_REFUSE_CTE_COST, detail, false, v.inlined->planTree->total_cost, v.plain_cost);
	}
	discard_plan(v.inlined_cx);
	*cx = v.plain_cx;
	return v.plain;
}

static bool reads_column_index(Query *q);

static bool
sublink_reads_index(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
		return reads_column_index((Query *) n);
	return expression_tree_walker(n, sublink_reads_index, context);
}

/* does the query, or one it holds (a sub-select, a CTE, a sublink), read a table that has a column index? */
static bool
reads_column_index(Query *q)
{
	foreach_node(RangeTblEntry, rte, q->rtable)
		if ((rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid)) ||
			(rte->rtekind == RTE_SUBQUERY && reads_column_index(rte->subquery)))
			return true;
	foreach_node(CommonTableExpr, cte, q->cteList)
		if (reads_column_index(castNode(Query, cte->ctequery)))
			return true;
	return query_tree_walker(q, sublink_reads_index, NULL, QTW_IGNORE_RANGE_TABLE | QTW_IGNORE_CTE_SUBQUERIES);
}

/*
 * PostgreSQL prices each rescan of a function scan at the rows it returns (0.11 for the unnest of a group's top rows), and the executor spends
 * 1.4 to 2 us on it: 6,263 groups of the top 3 rows cost 12.3 ms above the scan, priced 4.5. Measured per group on the unnest of two arrays.
 */
#define FLAT_RESCAN_OPS	360.0

/* the groups that a nested loop unnests: the rescans of a function scan below this node */
static double
function_rescans(Plan *plan)
{
	if (plan == NULL)
		return 0;
	return (IsA(plan, NestLoop) && IsA(plan->righttree, FunctionScan) ? plan->lefttree->plan_rows : 0) +
		function_rescans(plan->lefttree) + function_rescans(plan->righttree);
}

/*
 * The query planned as it is, and with its lateral sub-selects pulled up when that is possible (agg/flat.c): the flat plan wins if it
 * runs in the scan and costs less. Both are plans of the same query.
 */
static PlannedStmt *
plan_flat(SIFT_PLANNER_PARAMS)
{
	Query	   *flat = NULL;
	PlannedStmt *plain,
			   *flatplan = NULL;
	MemoryContext plain_cx,
				flat_cx;
	CteVerdict	fv = {0};
	bool		flat_inlined = false,
				by_cost = false;

	first_plan.set = planning_second = false;
	if (sift_enable_agg && parse->commandType == CMD_SELECT && sift_agg_has_flattenable_laterals(parse) && reads_column_index(parse))
	{
		flat = copyObject(parse);
		by_cost = sift_agg_flatten_laterals(flat);
		/* the two forms are both planned where the flat plan is no better in every scan, and the inline plan of a shared CTE costs most of it: their choice is kept as the one between the CTE and its inline plan is (verdict.c), and a hit plans the form that won */
		if (has_shared_cte(parse))
		{
			verdict_lookup(parse, VERDICT_FLAT, &fv);
			if (fv.known && verdict_accept(&fv, 0) == VERDICT_TAKE)
				return fv.won ? plan_cte(SIFT_PLANNER_ARGS(flat), &flat_cx) : plan_cte(SIFT_PLANNER_ARGS(parse), &plain_cx);
		}
		flatplan = plan_cte(SIFT_PLANNER_ARGS(flat), &flat_cx);
		flat_inlined = inline_won;
		/*
		 * A flat plan that reads every table that has a column index in a scan is the plan: the first one would only be set against it. Not a
		 * table grouped in place of an index lookup for each row of the outer query: the lookups cost less for a few rows.
		 */
		if (!by_cost && (uses_sift_agg(flatplan->planTree) || any_sift_agg(flatplan->subplans)) && !plain_reader_stmt(flatplan))
			return flatplan;
		planning_second = true;
	}
	plain = plan_cte(SIFT_PLANNER_ARGS(parse), &plain_cx);
	planning_second = false;
	/* the flat plan runs more of the statement in the scan only where an ordinary plan still reads a table that has a column index */
	if (flatplan == NULL || !plain_reader_stmt(plain))
	{
		if (flatplan != NULL)
		{
			discard_plan(flat_cx);
			verdict_remember(&fv, false, 0, 0, 0, 0);
		}
		return plain;
	}
	/*
	 * The scans of an inline CTE were weighed against the work of the shared one already: the totals above them are estimates of other rows,
	 * and the first plan, which still reads a table that has a column index in an ordinary plan (we are here for that), is no measure of them.
	 */
	if ((uses_sift_agg(flatplan->planTree) || any_sift_agg(flatplan->subplans)) &&
		((flat_inlined && !uses_sift_agg(plain->planTree) && !any_sift_agg(plain->subplans)) ||
		 flatplan->planTree->total_cost + function_rescans(flatplan->planTree) * FLAT_RESCAN_OPS * cpu_operator_cost < plain->planTree->total_cost))
	{
		discard_plan(plain_cx);
		verdict_remember(&fv, true, 0, 0, 0, 0);
		return flatplan;
	}
	discard_plan(flat_cx);
	verdict_remember(&fv, false, 0, 0, 0, 0);
	return plain;
}

/*
 * The cost of a SiftAgg plan turns on JIT (jit_above_cost) for work that does not use it: the node reads columns, and the nodes
 * above it handle its groups. Compiling their projections takes 3 to 5 ms of a 15 to 40 ms query.
 */
static PlannedStmt *
sift_planner(SIFT_PLANNER_PARAMS)
{
	MemoryContext memo = sift_agg_memo_begin();
	PlannedStmt *stmt;

	hook_ran = true;
	verdict_new_planning();
	discarded = NIL;			/* those of a plan that failed are gone with its context */
	/*
	 * The planner changes the tree it is given, with nodes and strings of the context it plans in, which is deleted when its plan loses.
	 * The statement that called us keeps its tree (CREATE TABLE AS plans one and copies it for the event triggers afterwards).
	 */
	parse = copyObject(parse);
	if (sift_enable_agg && parse->commandType == CMD_SELECT && parse->rowMarks == NIL && reads_column_index(parse))
		sift_agg_preinline(parse);
	PG_TRY();
	{
		stmt = plan_flat(SIFT_PLANNER_ARGS(parse));
	}
	PG_FINALLY();
	{
		sift_agg_memo_end(memo);
	}
	PG_END_TRY();
	delete_discarded();
	if (uses_sift_agg(stmt->planTree) || any_sift_agg(stmt->subplans))
		stmt->jitFlags = PGJIT_NONE;
	return stmt;
}

/* does the plan read a table that has a column index? */
static bool
reads_indexed_plan(PlannedStmt *ps)
{
	foreach_node(RangeTblEntry, rte, ps->rtable)
		if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid))
			return true;
	return false;
}

/*
 * Without shared_preload_libraries or session_preload_libraries the library is loaded when the planner first opens a column index, which is
 * inside planner() and after it decided there was no hook: that one statement of the backend is planned without sift_planner (no inline CTE,
 * no flattened laterals) and the aggregates that every later statement runs in the scan run their ordinary plan. The statement is planned
 * again here, from its text, when it is a SELECT of the top level, alone or the query of an EXPLAIN or a CREATE TABLE AS; the same text through
 * the hook gives the plan the next statement of the session gets, and an EXPLAIN prints the plan that replaces the first one. The parameters of the
 * extended protocol and of EXECUTE are planned as a custom plan is (their types and values are the statement's own). Anything else (a cursor,
 * a function's query) keeps its plan: a cursor's plan depends on its options.
 */
static Node *
replan_query(Node *stmt)
{
	for (;;)
		switch (nodeTag(stmt))
		{
			case T_PrepareStmt:
				return castNode(PrepareStmt, stmt)->query;
			case T_ExplainStmt:
				stmt = castNode(ExplainStmt, stmt)->query;
				break;
			case T_CreateTableAsStmt:
				return castNode(CreateTableAsStmt, stmt)->query;
			default:
				return stmt;
		}
}

static PlannedStmt *
replan(QueryDesc *qd)
{
	PlannedStmt *ps = qd->plannedstmt;
	ParamListInfo params = qd->params;
	RawStmt    *raw = NULL;
	Oid		   *ptypes = NULL;
	List	   *trees,
			   *plans;
	PlannedStmt *fresh;

	if (ps->commandType != CMD_SELECT || ps->utilityStmt != NULL || ps->hasModifyingCTE || qd->sourceText == NULL ||
		IsParallelWorker() || !sift_enable_agg || !reads_indexed_plan(ps))
		return NULL;
	/* the parameters of a function's query are named by parser hooks, which the text alone does not give back: that statement keeps its plan */
	if (params != NULL && (ActivePortal == NULL || qd->sourceText != ActivePortal->sourceText))
		return NULL;
	if (params != NULL && params->numParams > 0)
	{
		ptypes = palloc(params->numParams * sizeof(Oid));
		for (int i = 0; i < params->numParams; i++)
			if (!OidIsValid(ptypes[i] = params->params[i].ptype))
				return NULL;
	}
	foreach_node(RawStmt, r, pg_parse_query(qd->sourceText))
		if (r->stmt_location == ps->stmt_location)
			raw = r;
	if (raw != NULL)
	{
		RawStmt    *inner = makeNode(RawStmt);

		*inner = *raw;
		inner->stmt = replan_query(raw->stmt);
		raw = inner;
	}
	if (raw == NULL || !IsA(raw->stmt, SelectStmt) || castNode(SelectStmt, raw->stmt)->intoClause != NULL)
		return NULL;
	trees = pg_analyze_and_rewrite_fixedparams(raw, qd->sourceText, ptypes, params ? params->numParams : 0, qd->queryEnv);
	if (list_length(trees) != 1 || linitial_node(Query, trees)->commandType != CMD_SELECT)
		return NULL;
	plans = pg_plan_queries(trees, qd->sourceText, CURSOR_OPT_PARALLEL_OK, params);
	if (list_length(plans) != 1 || !IsA(linitial(plans), PlannedStmt))
		return NULL;
	fresh = linitial_node(PlannedStmt, plans);
	/* an EXPLAIN's plan has the location of its query inside the string, which another statement of the string may share: it must read what that one reads */
	return reads_indexed_plan(fresh) ? fresh : NULL;
}

static void
sift_executor_start(QueryDesc *queryDesc, int eflags)
{
	if (!late_checked)
	{
		late_checked = true;
		if (!hook_ran)
		{
			PlannedStmt *fresh = replan(queryDesc);

			if (fresh != NULL)
				queryDesc->plannedstmt = fresh;
			/* a prepared statement keeps no plan made without the hook */
			ResetPlanCache();
		}
	}
	if (prev_executor_start)
		prev_executor_start(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

void
sift_aggcte_init(void)
{
	prev_planner_hook = planner_hook;
	planner_hook = sift_planner;
	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = sift_executor_start;
}
