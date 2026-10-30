/*
 * agg/cte/variants.c - the plans of a statement with its shared CTEs kept and inlined
 * Each plan is made in a context of its own; this file also chooses between them and
 * caches the verdict.
 */
#include "postgres.h"

#include <math.h>

#include "optimizer/cost.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "utils/memutils.h"

#include "agg/agg.h"
#include "agg/cte/int.h"

/*
 * The contexts of the plans that lost are deleted when the hook is done: the memory they free then is the executor's to reuse, where a free
 * between two plans returns it to the system (and the next plan fills fresh pages).
 */
static List *discarded;

/*
 * A plan keeps every path and relation the planner built for it, several times the size of the plan itself, and a statement is planned up to
 * four times (shared or inline CTEs, as it is or flattened) of which one is kept. Each plan is made in a context of its own, a child of the
 * caller's, which is deleted when its plan loses: the statement holds the memory of one planning, not of all.
 */
PlannedStmt *
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
 * The inline plan is searched for join orders in groups of at most INLINE_JOIN_LIMIT relations: its bodies join a dozen tables, and the full search is most of the 19 ms
 * a statement costs to plan.
 */
#define INLINE_JOIN_LIMIT 4

static PlannedStmt *
plan_inline(SIFT_PLANNER_PARAMS, MemoryContext *cx)
{
	int			join_limit = join_collapse_limit,
				from_limit = from_collapse_limit;
	PlannedStmt *stmt;

	join_collapse_limit = Min(join_limit, INLINE_JOIN_LIMIT);
	from_collapse_limit = Min(from_limit, INLINE_JOIN_LIMIT);
	PG_TRY();
	{
		stmt = plan_in_own_context(SIFT_PLANNER_ARGS(parse), cx);
	}
	PG_FINALLY();
	{
		join_collapse_limit = join_limit;
		from_collapse_limit = from_limit;
	}
	PG_END_TRY();
	return stmt;
}

void
discard_plan(MemoryContext cx)
{
	Assert(MemoryContextIsValid(cx));
	discarded = lappend(discarded, cx);
}

/* the contexts of a plan that failed are gone: they are not deleted again */
void
forget_discarded(void)
{
	discarded = NIL;
}

void
delete_discarded(void)
{
	List	   *dead = discarded;

	discarded = NIL;
	foreach_ptr(MemoryContextData, cx, dead)
		MemoryContextDelete(cx);
	list_free(dead);
}

/*
 * The plan that keeps the shared CTEs, and in lift what agg_cte_lift came to while it was planned (the plan is the only one that is priced with it).
 * Memoize over a lookup of a CTE's rows is priced on hits a near-unique key (a jsonb document) does not give, so the shared plan would look cheaper than it is.
 * stock: the rows of CTEs and sub-selects are PostgreSQL's own (agg_stock_rows): the plan that survives when the scans lose is the one the planner
 * makes without the library, where the lifted rows (a floor that prices the scans against the work they replace) would have chosen other joins.
 * lifted says whether a lift changed the rows of this plan.
 */
PlannedStmt *
plan_shared(SIFT_PLANNER_PARAMS, MemoryContext *cx, double *lift, bool stock, bool *lifted)
{
	double		outer_lift = agg_cte_lift;
	bool		outer_memoize = enable_memoize,
				outer_stock = agg_stock_rows,
				outer_lifted = agg_rows_lifted;
	PlannedStmt *stmt;

	agg_cte_lift = 1.0;
	agg_stock_rows = stock;
	agg_rows_lifted = false;
	PG_TRY();
	{
		enable_memoize = false;
		stmt = plan_in_own_context(SIFT_PLANNER_ARGS(parse), cx);
	}
	PG_FINALLY();
	{
		*lift = agg_cte_lift;
		*lifted = agg_rows_lifted;
		enable_memoize = outer_memoize;
		agg_stock_rows = outer_stock;
		agg_cte_lift = outer_lift;
		agg_rows_lifted = outer_lifted;
	}
	PG_END_TRY();
	return stmt;
}

/*
 * Both queries are consumed by the planner. The inline plan is made first: where it holds no scan the choice is made (the shared plan stays), and that plan
 * is the one the planner makes without the library (stock rows); where it holds scans the shared plan is made with the lifted rows that price them against the
 * work they replace, and a copy of the statement is kept for the fallback (plan_stock).
 */
void
plan_variants(SIFT_PLANNER_PARAMS, Query *inline_parse, List *names, CteVariants *v)
{
	const char *outer_variant;
	double		read_rows,
				lift,
				charged;
	int			unnesting;

	memset(v, 0, sizeof(*v));
	outer_variant = agg_variant_push("inline");
	agg_read_rows = 0;
	v->inlined = plan_inline(SIFT_PLANNER_ARGS(inline_parse), &v->inlined_cx);
	read_rows = agg_read_rows;
	agg_variant = outer_variant;
	v->read_rows = read_rows;
	sift_agg_arms(v->inlined->planTree, &v->scans);
	sift_agg_arms_list(v->inlined->subplans, &v->scans);
	if (v->scans.n > 0)
		v->stock_parse = copyObject(parse);
	v->plain = plan_shared(SIFT_PLANNER_ARGS(parse), &v->plain_cx, &lift, v->scans.n == 0, &v->lifted);
	v->plain_cost = plan_cost_without_unread_ctes(v->plain);
	v->work = shared_work_cost(v->plain, names, read_rows, lift, v->scans.n, &charged, &unnesting);
	/* the shared plan really costs what its aggregates were charged: the totals are compared with it */
	v->plain_cost += charged;
	v->charged = charged;
	elog(DEBUG1, "sift: shared CTE plain cost %.2f, inline cost %.2f, scans %.2f, shared work %.2f, %d scans, %.0f rows read, %d unnesting aggregates, charged %.2f",
		 v->plain_cost, v->inlined->planTree->total_cost, v->scans.total, v->work, v->scans.n, read_rows, unnesting, charged);
}

/*
 * When the scans read the table, they are weighed against the work they replace. When an ordinary plan still reads it (the index lacks a
 * column of an aggregate) the plan reads the table twice: the scans must still be cheaper than the work they replace, and the totals must
 * not be far apart (the ordinary plan is priced twice in the inline plan, and a total that far above the shared one is a mis-estimate).
 * vet and shared_vet: the heap check taken off the inline plan's scans and off the SiftAgg nodes of the shared plan (both 0 for the choice).
 */
bool
inline_wins(const CteVariants *v, double vet, double shared_vet)
{
	double		scans = v->scans.total - vet,
				inline_cost = v->inlined->planTree->total_cost - vet,
				plain_cost = v->plain_cost - shared_vet,
				work = v->work - shared_vet;

	/*
	 * What the scans leave in the inline plan (the copies of the CTE bodies they do not replace, each reference one) is priced by PostgreSQL's own estimates, as the
	 * shared plan is before its aggregates are charged: the charge makes up for the rows those aggregates lost, not for the rest. An inline plan whose rest costs more
	 * than the whole shared plan uncharged (by the margin of a totals comparison) repeats work that the shared plan does once, and no charge on the aggregates
	 * pays for it.
	 */
	if (!sift_agg_force && v->inlined->planTree->total_cost - v->scans.total > CTE_INLINE_TOTALS * (v->plain_cost - v->charged))
		return false;
	return v->scans.n > 0 && (sift_agg_force || (!plain_reader_stmt(v->inlined, 0) ? CTE_INLINE_MARGIN * scans < work && inline_cost < CTE_INLINE_ABOVE * plain_cost :
							  (CTE_INLINE_MARGIN * scans < work && inline_cost < CTE_INLINE_TOTALS * plain_cost) ||
							  scans < plain_cost - inline_cost));
}

/*
 * The scans cost less than the work they replace but not by the margin: a close call at this size. The choice that is right at 10 times the
 * rows and at 100 times (sift.agg_scale_horizon) is right now: the scans grow with the rows they read, the plan of the shared CTE with its sorts
 * and hash tables that outgrow work_mem grows faster, and a plan that is a tie today and a loss to the normal one at the size the table is going
 * to have is not worth keeping. The statement is planned again at each size, from the untouched copies.
 */
bool
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
 * The total of the plan the planner makes without the library: the alternative to every plan of the statement that uses it, which the cte_inline row of
 * explain_why prices it against (the shared plan after its aggregates were charged is the price of a plan nobody runs).
 */
double
planner_total(SIFT_PLANNER_PARAMS)
{
	bool		outer = sift_enable_agg,
				quiet = agg_quiet_reports(true);
	MemoryContext cx;
	double		total;

	PG_TRY();
	{
		sift_enable_agg = false;
		total = plan_in_own_context(SIFT_PLANNER_ARGS(parse), &cx)->planTree->total_cost;
	}
	PG_FINALLY();
	{
		sift_enable_agg = outer;
		agg_quiet_reports(quiet);
	}
	PG_END_TRY();
	discard_plan(cx);
	return total;
}

/*
 * The statement planned as the verdict kept for it says (verdict.c): the inline plan when the scans won, else the plan that keeps the shared CTEs.
 * The totals are those of the plan in full that made the verdict, a statement of other constants: the cte_inline row reports no costs, since
 * those would be the other statement's (the verdict_cached row says why).
 */
PlannedStmt *
plan_cached(SIFT_PLANNER_PARAMS, const CteVerdict *vd, MemoryContext *cx)
{
	PlannedStmt *stmt;

	agg_report("verdict_cached", AGG_REFUSE_NONE, NULL, true, 0, 0);
	if (vd->won)
	{
		const char *outer_variant = agg_variant_push("inline");
		Query	   *inline_parse = copyObject(parse);
		SharedCteWalk survey = {.survey = true},
					walk = {.inline_them = true, .only = ~(uint32) 0};

		/* the verdict names the shared CTEs it inlined by their place in the walk: a statement that has another number of them is inlined whole */
		shared_cte_walker((Node *) parse, &survey);
		if (vd->ncand > 0 && vd->ncand == survey.pos)
			walk.only = vd->mask;
		shared_cte_walker((Node *) inline_parse, &walk);

		stmt = plan_inline(SIFT_PLANNER_ARGS(inline_parse), cx);
		agg_variant = outer_variant;
		inline_won = true;
		agg_report("cte_inline", AGG_REFUSE_NONE, NULL, true, 0, 0);
	}
	else
	{
		double		lift;
		bool		lifted;

		stmt = plan_shared(SIFT_PLANNER_ARGS(parse), cx, &lift, true, &lifted);
		if (vd->scans > 0)
			agg_report("cte_inline", AGG_REFUSE_CTE_COST, "from the verdict cached", false, 0, 0);
	}
	elog(DEBUG1, "sift: shared CTE verdict from the cache: %s", vd->won ? "inline" : "shared");
	return stmt;
}
