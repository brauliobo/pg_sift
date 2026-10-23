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
#include "optimizer/planner.h"
#include "parser/parsetree.h"
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

static bool
has_column_index(Oid relid)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	bool		found = false;

	foreach_oid(ix, RelationGetIndexList(rel))
		found |= sift_colsrc_is_index(get_rel_relam(ix));
	relation_close(rel, AccessShareLock);
	return found;
}

/* does the query select from a table that has a column index, itself or through a CTE of the same WITH (which PostgreSQL inlines when one query reads it)? */
static bool
reads_indexed_table(Query *q, List *siblings, int depth)
{
	foreach_node(RangeTblEntry, rte, q->rtable)
	{
		if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_index(rte->relid))
			return true;
		if (rte->rtekind != RTE_CTE || rte->ctelevelsup != 1 || depth <= 0)
			continue;
		foreach_node(CommonTableExpr, sib, siblings)
			if (strcmp(sib->ctename, rte->ctename) == 0 && !sib->cterecursive && IsA(sib->ctequery, Query) &&
				reads_indexed_table(castNode(Query, sib->ctequery), siblings, depth - 1))
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
	return reads_indexed_table(q, siblings, 4);
}

/* what the walk below finds: with inline_them the shared CTEs are all written NOT MATERIALIZED, and their names are kept in *names */
typedef struct SharedCteWalk
{
	bool		inline_them;
	List	   *names;
} SharedCteWalk;

/* does the query, or a sub-select or CTE of it, hold a shared CTE over a column index? With inline_them the walk goes on (it is of a copy of the query) */
static bool
shared_cte_walker(Node *n, SharedCteWalk *w)
{
	if (n == NULL)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, shared_cte_walker, w);
	foreach_node(CommonTableExpr, cte, ((Query *) n)->cteList)
		if (shared_cte_over_column_index(cte, ((Query *) n)->cteList))
		{
			if (!w->inline_them)
				return true;
			cte->ctematerialized = CTEMaterializeNever;
			w->names = lappend(w->names, cte->ctename);
		}
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
		a->total += plan->total_cost;
		a->first = Max(a->first, plan->total_cost);
		a->n++;
		return;
	}
	if (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_rows_plan_methods)
	{
		ScanArms	below = {0};

		foreach_ptr(Plan, p, ((CustomScan *) plan)->custom_plans)
			sift_agg_arms(p, &below);
		a->total += below.total - (below.total - below.first) * (1.0 - AGG_FUSED_ARM);
		a->first = Max(a->first, below.first);
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
} SharedWork;

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
				w->arms = lappend(w->arms, agg);
				w->cost += agg->total_cost;
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

static double
shared_work_cost(PlannedStmt *stmt, List *names)
{
	SharedWork	w = {stmt, names, NULL, NIL, 0};

	shared_work(&w, stmt->planTree, NULL);
	return w.cost;
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

		if (rte->rtekind == RTE_RELATION && has_column_index(rte->relid))
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

/* the plans of a statement with its shared CTEs and with them inlined, and what the scans of the inline plan and the work of the shared one cost */
typedef struct CteVariants
{
	PlannedStmt *plain,
			   *inlined;
	ScanArms	scans;
	double		plain_cost,
				work;
} CteVariants;

/* both queries are consumed by the planner */
static void
plan_variants(SIFT_PLANNER_PARAMS, Query *inline_parse, List *names, CteVariants *v)
{
	const char *outer_variant;

	memset(v, 0, sizeof(*v));
	v->plain = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) :
		standard_planner(SIFT_PLANNER_ARGS(parse));
	outer_variant = agg_variant_push("inline");
	v->inlined = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(inline_parse)) :
		standard_planner(SIFT_PLANNER_ARGS(inline_parse));
	agg_variant = outer_variant;
	v->plain_cost = plan_cost_without_unread_ctes(v->plain);
	sift_agg_arms(v->inlined->planTree, &v->scans);
	sift_agg_arms_list(v->inlined->subplans, &v->scans);
	v->work = shared_work_cost(v->plain, names);
	elog(DEBUG1, "sift: shared CTE plain cost %.2f, inline cost %.2f, scans %.2f, shared work %.2f, %d scans",
		 v->plain_cost, v->inlined->planTree->total_cost, v->scans.total, v->work, v->scans.n);
}

/*
 * When the scans read the table, they are weighed against the work they replace. When an ordinary plan still reads it (the index lacks a
 * column of an aggregate) the plan reads the table twice: the scans must still be cheaper than the work they replace, and the totals must
 * not be far apart (the ordinary plan is priced twice in the inline plan, and a total that far above the shared one is a mis-estimate).
 */
static bool
inline_wins(const CteVariants *v)
{
	return v->scans.n > 0 && (!plain_reader_stmt(v->inlined) ? CTE_INLINE_MARGIN * v->scans.total < v->work && v->inlined->planTree->total_cost < CTE_INLINE_ABOVE * v->plain_cost :
							  (CTE_INLINE_MARGIN * v->scans.total < v->work && v->inlined->planTree->total_cost < CTE_INLINE_TOTALS * v->plain_cost) ||
							  v->scans.total < v->plain_cost - v->inlined->planTree->total_cost);
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
			wins = inline_wins(&v);
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

static PlannedStmt *
plan_cte(SIFT_PLANNER_PARAMS)
{
	Query	   *inline_parse,
			   *pristine = NULL,
			   *pristine_inline = NULL;
	CteVariants v;
	SharedCteWalk walk = {true, NIL};

	inline_won = false;
	if (!sift_enable_agg || !has_shared_cte(parse))
		return prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) :
			standard_planner(SIFT_PLANNER_ARGS(parse));
	inline_parse = copyObject(parse);
	shared_cte_walker((Node *) inline_parse, &walk);
	if (sift_agg_scale_horizon > 1.0)
	{
		pristine = copyObject(parse);
		pristine_inline = copyObject(inline_parse);
	}
	plan_variants(SIFT_PLANNER_ARGS(parse), inline_parse, walk.names, &v);
	if (inline_wins(&v) ||
		(pristine != NULL && v.scans.n > 0 && v.scans.total < v.work && inline_wins_at_scale(SIFT_PLANNER_ARGS(pristine), pristine_inline, walk.names)))
	{
		agg_report("cte_inline", AGG_REFUSE_NONE, NULL, true);
		inline_won = true;
		return v.inlined;
	}
	if (sift_explain_refusals && v.scans.n > 0)
		agg_report("cte_inline", AGG_REFUSE_CTE_COST,
				   psprintf("inline costs %.2f (the scans %.2f), the shared CTE %.2f (the work the scans replace %.2f)", v.inlined->planTree->total_cost, v.scans.total, v.plain_cost, v.work), false);
	return v.plain;
}

/* does the query, or one it holds, read a table that has a column index? */
static bool
reads_column_index(Query *q)
{
	foreach_node(RangeTblEntry, rte, q->rtable)
		if ((rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_index(rte->relid)) ||
			(rte->rtekind == RTE_SUBQUERY && reads_column_index(rte->subquery)))
			return true;
	foreach_node(CommonTableExpr, cte, q->cteList)
		if (reads_column_index(castNode(Query, cte->ctequery)))
			return true;
	return false;
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
			   *flatplan;

	if (sift_enable_agg && parse->commandType == CMD_SELECT && sift_agg_has_flattenable_laterals(parse) && reads_column_index(parse))
	{
		flat = copyObject(parse);
		sift_agg_flatten_laterals(flat);
	}
	plain = plan_cte(SIFT_PLANNER_ARGS(parse));
	if (flat == NULL)
		return plain;
	flatplan = plan_cte(SIFT_PLANNER_ARGS(flat));
	if (!uses_sift_agg(flatplan->planTree) && !any_sift_agg(flatplan->subplans))
		return plain;
	/* the scans of an inline CTE were weighed against the work of the shared one already: the totals above them are estimates of other rows */
	if (inline_won && !uses_sift_agg(plain->planTree) && !any_sift_agg(plain->subplans))
		return flatplan;
	return flatplan->planTree->total_cost < plain->planTree->total_cost ? flatplan : plain;
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
	PG_TRY();
	{
		stmt = plan_flat(SIFT_PLANNER_ARGS(parse));
	}
	PG_FINALLY();
	{
		sift_agg_memo_end(memo);
	}
	PG_END_TRY();
	if (uses_sift_agg(stmt->planTree) || any_sift_agg(stmt->subplans))
		stmt->jitFlags = PGJIT_NONE;
	return stmt;
}

/* does the plan read a table that has a column index? */
static bool
reads_indexed_plan(PlannedStmt *ps)
{
	foreach_node(RangeTblEntry, rte, ps->rtable)
		if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_index(rte->relid))
			return true;
	return false;
}

/*
 * Without shared_preload_libraries or session_preload_libraries the library is loaded when the planner first opens a column index, which is
 * inside planner() and after it decided there was no hook: that one statement of the backend is planned without sift_planner (no inline CTE,
 * no flattened laterals) and the aggregates that every later statement runs in the scan run their ordinary plan. The statement is planned
 * again here, from its text, when it is a plain SELECT of the top level; the same text through the hook gives the plan the next statement of
 * the session gets. Anything else (a statement with parameters, EXPLAIN, CREATE TABLE AS, a cursor, a function's query) keeps its plan.
 */
static PlannedStmt *
replan(QueryDesc *qd)
{
	PlannedStmt *ps = qd->plannedstmt;
	RawStmt    *raw = NULL;
	List	   *trees,
			   *plans;

	if (ps->commandType != CMD_SELECT || ps->utilityStmt != NULL || ps->hasModifyingCTE || qd->params != NULL || qd->sourceText == NULL ||
		IsParallelWorker() || !sift_enable_agg || !reads_indexed_plan(ps))
		return NULL;
	foreach_node(RawStmt, r, pg_parse_query(qd->sourceText))
		if (r->stmt_location == ps->stmt_location)
			raw = r;
	/* the text of a prepared statement is its PREPARE */
	if (raw != NULL && IsA(raw->stmt, PrepareStmt))
	{
		RawStmt    *inner = makeNode(RawStmt);

		*inner = *raw;
		inner->stmt = castNode(PrepareStmt, raw->stmt)->query;
		raw = inner;
	}
	if (raw == NULL || !IsA(raw->stmt, SelectStmt) || castNode(SelectStmt, raw->stmt)->intoClause != NULL)
		return NULL;
	trees = pg_analyze_and_rewrite_fixedparams(raw, qd->sourceText, NULL, 0, qd->queryEnv);
	if (list_length(trees) != 1 || linitial_node(Query, trees)->commandType != CMD_SELECT)
		return NULL;
	plans = pg_plan_queries(trees, qd->sourceText, CURSOR_OPT_PARALLEL_OK, NULL);
	if (list_length(plans) != 1 || !IsA(linitial(plans), PlannedStmt))
		return NULL;
	return linitial_node(PlannedStmt, plans);
}

static void
sift_executor_start(QueryDesc *queryDesc, int eflags)
{
	if (!late_checked)
	{
		late_checked = true;
		if (!hook_ran && (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
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
