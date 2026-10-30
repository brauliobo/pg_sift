/*
 * agg/cte/hook.c - a CTE that several aggregates read, planned inline when that lets them run on the column index.
 *
 * PostgreSQL computes a CTE that the query names more than once exactly once and hands its rows to every reference, so the
 * aggregates over it (a time series at four granularities, each arm a GROUP BY of the same CTE) read a materialized copy and
 * never the index. The same query with the CTE written NOT MATERIALIZED is the same query: PostgreSQL itself inlines a CTE
 * with no side effects and no volatile function whenever the user says so, and refuses otherwise. This hook plans the query
 * both ways when a CTE over a table with a column index is referenced more than once, and keeps the inline plan if it has
 * a Custom Scan (SiftAgg) and costs less. Both plans are plans of the same query, so the rows are the same. A CTE written
 * MATERIALIZED is planned both ways too: the keyword fixes where PostgreSQL computes the rows, not which rows there are (the notes of
 * 500,000 events, read by three aggregates, ran 1.9 s shared and 0.35 s inline).
 */
#include "postgres.h"

#include "executor/executor.h"
#include "jit/jit.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"

#include "agg/agg.h"
#include "core/sql.h"
#include "agg/cte/int.h"
#include "col/am.h"

/*
 * The share of a flat plan's cost that its ordinary reads of tables with a column index must reach for the plan to be set against the one as written: the
 * lookups of a CTE of keys (the members of a group) are a few probes of the rows the scan reads in bulk, and the estimates of the plan as written
 * (the rows of the CTE, of each key) are low by two orders of magnitude where the keys are few and their tables large
 */
#define PLAIN_READER_SHARE	0.25

planner_hook_type prev_planner_hook;
static ExecutorStart_hook_type prev_executor_start;

/* sift_planner has planned a statement of this backend */
static bool hook_ran;

/* the first statement to start after the library loaded has been looked at (sift_executor_start) */
static bool late_checked;

/* set by plan_cte: it returned the inline plan, because its scans cost less than the shared plan's work */
bool		inline_won;

/* the CTEs (plan ids) that a CteScan of the plan, or of a CTE it reads, names */

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

/* the plan of the statement and the context it is in, which the caller deletes when it plans another statement instead */
static PlannedStmt *
plan_cte(SIFT_PLANNER_PARAMS, MemoryContext *cx)
{
	Query	   *inline_parse,
			   *pristine = NULL,
			   *pristine_inline = NULL,
			   *bare = NULL;
	CteVariants v;
	SharedCteWalk walk = {.inline_them = true, .only = ~(uint32) 0},
				survey = {.survey = true};
	CteVerdict	vd;
	Query	   *whole = NULL;
	PlannedStmt *priced;
	bool		choosing,
				quiet;
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
		 * The shared plan consumes the tree and the inline plan a copy of it. The verdict is checked against the size of the plan the hit plans (verdict_accept);
		 * where it is far from what the verdict was made for, the statement is planned again from the tree that is left (the inline plan) or from a copy made
		 * for it (the shared plan).
		 */
		Query	   *again = vd.won ? parse : copyObject(parse);
		PlannedStmt *stmt;
		double		size;
		int			r;

		agg_read_rows = 0;
		stmt = plan_cached(SIFT_PLANNER_ARGS(parse), &vd, cx);
		size = vd.won ? agg_read_rows : cte_size(stmt);
		r = verdict_accept(&vd, size);
		if (r == VERDICT_TAKE)
			return stmt;
		discard_plan(*cx);
		if (r == VERDICT_SWITCH)
			return plan_cached(SIFT_PLANNER_ARGS(again), &vd, cx);
		verdict_replan = true;
		return plan_cte(SIFT_PLANNER_ARGS(again), cx);
	}
	shared_cte_walker((Node *) parse, &survey);
	/*
	 * Inlining a shared CTE copies its body at each reference, and PostgreSQL plans (and copies again, level by level) each of the copies; the scans use the
	 * tables of a few of them. A plan in full inlines all, then plans again with those that read a table a scan reads (the plan that follows from the verdict
	 * inlines them only: 5 scans of a statement of 6 shared CTEs in 25 ms where the whole inline plan takes 700). The set that a verdict kept is not made
	 * again by the plan in full that checks it.
	 */
	if (vd.prior_seen && vd.ncand > 0 && vd.ncand == survey.pos)
		walk.only = vd.mask;
	else if (survey.pos > 1)
		whole = copyObject(parse);
	inline_parse = copyObject(parse);
	shared_cte_walker((Node *) inline_parse, &walk);
	if (sift_agg_scale_horizon > 1.0)
	{
		pristine = copyObject(parse);
		pristine_inline = copyObject(inline_parse);
	}
	if (sift_explain_refusals)
		bare = copyObject(parse);
	agg_variant_hold();
	/* explain_why reports the arms of the plan that stays: the plans that choose the set report nothing, that plan is made again (explain only) */
	choosing = whole != NULL && sift_explain_refusals;
	quiet = choosing ? agg_quiet_reports(true) : false;
	plan_variants(SIFT_PLANNER_ARGS(parse), inline_parse, walk.names, &v);
	if (whole != NULL && v.scans.n > 0)
	{
		SharedCteWalk used = {.survey = true};
		SharedCteWalk subset = {.inline_them = true};
		List	   *tables = NIL;
		Query	   *sub = copyObject(whole);
		CteVariants part;

		arm_tables(v.inlined->planTree, &tables);
		foreach_ptr(Plan, p, v.inlined->subplans)
			arm_tables(p, &tables);
		used.reads = tables;
		shared_cte_walker((Node *) whole, &used);
		if (used.reading != 0 && used.reading != SHARED_CTES(survey.pos))
		{
			subset.only = used.reading;
			shared_cte_walker((Node *) sub, &subset);
			plan_variants(SIFT_PLANNER_ARGS(copyObject(whole)), sub, subset.names, &part);
			if (inline_wins(&part, 0, 0) && part.inlined->planTree->total_cost <= CTE_INLINE_TOTALS * v.inlined->planTree->total_cost)
			{
				discard_plan(v.plain_cx);
				discard_plan(v.inlined_cx);
				v = part;
				walk.names = subset.names;
				walk.only = subset.only;
			}
			else
			{
				discard_plan(part.plain_cx);
				discard_plan(part.inlined_cx);
			}
		}
	}
	if (choosing)
	{
		SharedCteWalk again = {.inline_them = true, .only = walk.only};
		Query	   *again_inline = copyObject(whole);

		agg_quiet_reports(quiet);
		shared_cte_walker((Node *) again_inline, &again);
		discard_plan(v.plain_cx);
		discard_plan(v.inlined_cx);
		plan_variants(SIFT_PLANNER_ARGS(copyObject(whole)), again_inline, again.names, &v);
	}
	same = planning_second && first_plan.set && first_plan.plain == v.plain_cost && first_plan.inlined == v.inlined->planTree->total_cost &&
		first_plan.scans == v.scans.total && first_plan.work == v.work;
	won = inline_wins(&v, 0, 0);
	if (!won && pristine != NULL && v.scans.n > 0 && v.scans.total < v.work && CTE_INLINE_MARGIN * v.scans.total >= v.work)
		won = same ? first_plan.won : inline_wins_at_scale(SIFT_PLANNER_ARGS(pristine), pristine_inline, walk.names);
	vd.mask = walk.only;
	vd.ncand = survey.pos;
	priced = v.plain;
	if (!won && v.lifted)
	{
		/* the scans lost: the plan that stays is the one the planner makes without the lifted rows that priced them (its arms are the priced plan's) */
		bool		quiet = agg_quiet_reports(true),
					lifted;
		double		lift;

		discard_plan(v.plain_cx);
		v.plain = plan_shared(SIFT_PLANNER_ARGS(v.stock_parse), &v.plain_cx, &lift, true, &lifted);
		agg_quiet_reports(quiet);
	}
	verdict_remember(&vd, won, v.scans.n > 0 ? v.inlined->planTree->total_cost : 0, v.plain_cost, cte_size(v.plain), v.read_rows);
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
		agg_variant_settle(true, NULL);
		if (bare != NULL)
			agg_report("cte_inline", AGG_REFUSE_NONE, NULL, true, v.inlined->planTree->total_cost, planner_total(SIFT_PLANNER_ARGS(bare)));
		inline_won = true;
		discard_plan(v.plain_cx);
		*cx = v.inlined_cx;
		return v.inlined;
	}
	agg_variant_settle(false, "the arm is in the plan with the CTE inlined, which lost: see cte_inline");
	if (sift_explain_refusals && v.scans.n > 0)
	{
		ScanArms	shared = {0};
		const char *detail = psprintf("inline costs %.2f (the scans %.2f), the shared CTE %.2f (the work the scans replace %.2f)", v.inlined->planTree->total_cost, v.scans.total, v.plain_cost, v.work);

		sift_agg_arms(priced->planTree, &shared);
		sift_agg_arms_list(priced->subplans, &shared);
		/* the heap check decided it when the scans would have won without it, in both plans (a rewrite leaves the visibility map empty) */
		if (v.scans.vet > 0 && inline_wins(&v, v.scans.vet, shared.vet))
			detail = psprintf("%s; %.0f%% of the heap pages are not all-visible, which adds %.2f to the scans: VACUUM (ANALYZE) the table", detail, v.scans.vet_pct, v.scans.vet);
		agg_report("cte_inline", AGG_REFUSE_CTE_COST, detail, false, v.inlined->planTree->total_cost, planner_total(SIFT_PLANNER_ARGS(bare)));
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
 * The query planned as it is, and with its lateral sub-selects pulled up when that is possible (agg/flat/): the flat plan wins if it
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
		/* the two forms are both planned where the flat plan is no better in every scan, and the inline plan of a shared CTE costs most of it: their choice is kept as the one between the CTE and its inline plan is (verdict.c), and a hit plans the form that won */
		if (has_shared_cte(parse))
		{
			verdict_lookup(parse, VERDICT_FLAT, &fv);
			if (fv.known)
			{
				/* a verdict holds for the size of the request it was made for: the plan of the form that won is checked against it, else the statement is planned in full */
				Query	   *again = fv.won ? parse : copyObject(parse);
				MemoryContext cx;
				PlannedStmt *stmt;

				if (fv.won)
				{
					flat = copyObject(parse);
					by_cost = sift_agg_flatten_laterals(flat);
				}
				stmt = plan_cte(SIFT_PLANNER_ARGS(fv.won ? flat : parse), &cx);

				/* verdict_accept reads the verdict of the band of this size into fv, which may be the other form's */
				if (verdict_accept(&fv, cte_size(stmt)) == VERDICT_TAKE)
					return stmt;
				discard_plan(cx);
				parse = again;
				flat = copyObject(parse);
				by_cost = sift_agg_flatten_laterals(flat);
			}
		}
		if (flat == NULL)
		{
			flat = copyObject(parse);
			by_cost = sift_agg_flatten_laterals(flat);
		}
		flatplan = plan_cte(SIFT_PLANNER_ARGS(flat), &flat_cx);
		flat_inlined = inline_won;
		/*
		 * A flat plan that reads every table that has a column index in a scan is the plan: the first one would only be set against it. Not a
		 * table grouped in place of an index lookup for each row of the outer query: the lookups cost less for a few rows.
		 */
		if (!by_cost && (uses_sift_agg(flatplan->planTree) || any_sift_agg(flatplan->subplans)) && !plain_reader_stmt(flatplan, PLAIN_READER_SHARE))
		{
			verdict_remember(&fv, true, 0, 0, cte_size(flatplan), cte_size(flatplan));
			return flatplan;
		}
		planning_second = true;
	}
	plain = plan_cte(SIFT_PLANNER_ARGS(parse), &plain_cx);
	planning_second = false;
	/* the flat plan runs more of the statement in the scan only where an ordinary plan still reads a table that has a column index */
	if (flatplan == NULL || !plain_reader_stmt(plain, 0))
	{
		if (flatplan != NULL)
		{
			discard_plan(flat_cx);
			verdict_remember(&fv, false, 0, 0, cte_size(plain), cte_size(plain));
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
		verdict_remember(&fv, true, 0, 0, cte_size(flatplan), cte_size(flatplan));
		return flatplan;
	}
	discard_plan(flat_cx);
	verdict_remember(&fv, false, 0, 0, cte_size(plain), cte_size(plain));
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
	bool		col_plan = sift_col_plan_begin();
	bool		reads;
	PlannedStmt *stmt;

	hook_ran = true;
	verdict_new_planning();
	forget_discarded();			/* those of a plan that failed are gone with its context */
	/*
	 * The planner changes the tree it is given, with nodes and strings of the context it plans in, which is deleted when its plan loses.
	 * The statement that called us keeps its tree (CREATE TABLE AS plans one and copies it for the event triggers afterwards). A statement that
	 * reads no table with a column index and has no shared CTE is planned once, in place, as without the extension: no plan loses, no copy.
	 */
	reads = sift_enable_agg && parse->commandType == CMD_SELECT && reads_column_index(parse);
	if (reads || (sift_enable_agg && has_shared_cte(parse)))
		parse = copyObject(parse);
	if (reads)
	{
		sift_json_build_object_swap(parse);
		if (parse->rowMarks == NIL)
			sift_agg_preinline(parse);
	}
	PG_TRY();
	{
		stmt = plan_flat(SIFT_PLANNER_ARGS(parse));
	}
	PG_FINALLY();
	{
		sift_agg_memo_end(memo);
		sift_col_plan_end(col_plan);
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
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		sift_col_ioss_want(queryDesc);
		sift_col_stmt_start(queryDesc);
	}
}

void
sift_aggcte_init(void)
{
	prev_planner_hook = planner_hook;
	planner_hook = sift_planner;
	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = sift_executor_start;
}
