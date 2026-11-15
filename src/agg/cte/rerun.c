/*
 * agg/cte/rerun.c - a statement whose aggregate scan outgrew its memory runs again as PostgreSQL plans it.
 *
 * The scan of a node that cannot spill ends with an error when its groups pass hash memory times group_nospill_factor; the executor hook below catches
 * that error, ends the executor state and runs the statement planned without the aggregate scan. The plan is made from the statement's query, which the
 * planner kept in the nodes that cannot spill (sift_agg_stash_rerun): a cursor, a function's query with parameters and a prepared statement are all
 * planned again the way they were the first time, with no text to parse.
 */

#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "nodes/readfuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/dest.h"
#include "utils/plancache.h"
#include "utils/portal.h"

#include "agg/agg.h"
#include "agg/cte/int.h"
#include "agg/rows.h"
#include "agg/spec.h"
#include "agg/stats.h"

bool		sift_agg_plan_reruns;

static ExecutorRun_hook_type prev_executor_run;

#if PG_VERSION_NUM >= 180000
#define RUN_PARAMS	QueryDesc *qd, ScanDirection dir, uint64 count
#define RUN_ARGS	qd, dir, count
#else
#define RUN_PARAMS	QueryDesc *qd, ScanDirection dir, uint64 count, bool once
#define RUN_ARGS	qd, dir, count, once
#endif

/* does the query, or a sub-select of it, group (an aggregate, a window the planner turns into groups, a DISTINCT or a set operation)? Only such a statement can hold a node that cannot spill */
static bool
groups_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		Query	   *q = (Query *) n;

		return q->hasAggs || q->hasWindowFuncs || q->groupClause != NIL || q->groupingSets != NIL || q->distinctClause != NIL ||
				q->setOperations != NULL || query_tree_walker(q, groups_walker, context, 0);
	}
	return expression_tree_walker(n, groups_walker, context);
}

/*
 * A copy of the query that the planner is about to plan, when the statement can run again if a node of its plan cannot spill its groups: a SELECT
 * that sends nothing before its groups are done and calls no volatile function (which would run twice), and does not lock rows. NULL for any other.
 */
Query *
sift_agg_rerun_query(Query *parse)
{
	if (parse->commandType != CMD_SELECT || parse->hasModifyingCTE || parse->rowMarks != NIL || IsParallelWorker() || !groups_walker((Node *) parse, NULL) ||
		contain_volatile_functions((Node *) parse))
		return NULL;
	return copyObject(parse);
}

/* the query the planner kept, as the text of its nodes: made once, by the first node of the plan that holds its groups */
typedef struct Stash
{
	Query	   *query;
	int			cursorOptions;
	char	   *text;
} Stash;

static const char *
stash_text(Stash *st)
{
	if (st->text == NULL)
		st->text = nodeToString(list_make2(st->query, makeInteger(st->cursorOptions)));
	return st->text;
}

/* the nodes of the plan that cannot spill their groups, each given the query: called for every CustomScan of the tree */
static void
stash_walk(Plan *p, Stash *st)
{
	if (p == NULL)
		return;
	stash_walk(p->lefttree, st);
	stash_walk(p->righttree, st);
	switch (nodeTag(p))
	{
		case T_SubqueryScan:
			stash_walk(((SubqueryScan *) p)->subplan, st);
			break;
		case T_Append:
			foreach_ptr(Plan, c, ((Append *) p)->appendplans)
				stash_walk(c, st);
			break;
		case T_MergeAppend:
			foreach_ptr(Plan, c, ((MergeAppend *) p)->mergeplans)
				stash_walk(c, st);
			break;
		case T_CustomScan:
			{
				CustomScan *cs = (CustomScan *) p;

				foreach_ptr(Plan, c, cs->custom_plans)
					stash_walk(c, st);
				if (cs->methods == &sift_agg_plan_methods)
				{
					SiftAggSpec *s = sift_aggspec_unpack(cs->custom_private);

					if (sift_aggspec_holds_groups(s))
					{
						s->rerun = (char *) stash_text(st);
						cs->custom_private = sift_aggspec_pack(s);
					}
				}
				else if (cs->methods == &sift_agg_rows_plan_methods)
					cs->custom_private = sift_agg_rows_with_rerun(cs->custom_private, stash_text(st));
			}
			break;
		default:
			break;
	}
}

/* the planner kept query for the plan: each node of it that holds its groups and cannot spill them can run the statement again, planned as PostgreSQL does */
void
sift_agg_stash_rerun(PlannedStmt *stmt, Query *query, int cursorOptions)
{
	Stash		st = {query, cursorOptions, NULL};

	stash_walk(stmt->planTree, &st);
	foreach_ptr(Plan, p, stmt->subplans)
		stash_walk(p, &st);
}

/* the statement as PostgreSQL plans it, for one whose aggregate scan outgrew its memory */
static PlannedStmt *
replan_plain(QueryDesc *qd, const char *rerun)
{
	List	   *kept = stringToNode((char *) rerun);
	bool		enabled = sift_enable_agg;
	List	   *plans;

	sift_enable_agg = false;
	PG_TRY();
	{
		plans = pg_plan_queries(list_make1(linitial(kept)), qd->sourceText, intVal(lsecond(kept)), qd->params);
	}
	PG_FINALLY();
	{
		sift_enable_agg = enabled;
	}
	PG_END_TRY();
	return linitial_node(PlannedStmt, plans);
}

/*
 * The receiver of a statement that may run twice: the client sees the start of the first run only, and a row sent ends the chance of a second run. The
 * receiver starts in the context of the statement, not in the first run's executor state, which ends before the second run starts.
 */
typedef struct CountDest
{
	DestReceiver pub;
	DestReceiver *inner;
	MemoryContext cxt;
	uint64		sent;
	bool		started;
} CountDest;

static bool
count_receive(TupleTableSlot *slot, DestReceiver *self)
{
	CountDest  *d = (CountDest *) self;

	d->sent++;
	return d->inner->receiveSlot(slot, d->inner);
}

static void
count_startup(DestReceiver *self, int operation, TupleDesc tupdesc)
{
	CountDest  *d = (CountDest *) self;
	MemoryContext old = MemoryContextSwitchTo(d->cxt);

	if (!d->started)
		d->inner->rStartup(d->inner, operation, tupdesc);
	d->started = true;
	MemoryContextSwitchTo(old);
}

static void
count_shutdown(DestReceiver *self)
{
	CountDest  *d = (CountDest *) self;

	d->inner->rShutdown(d->inner);
}

static void
count_destroy(DestReceiver *self)
{
}

static void
run_chain(RUN_PARAMS)
{
	if (prev_executor_run)
		prev_executor_run(RUN_ARGS);
	else
		standard_ExecutorRun(RUN_ARGS);
}

/*
 * A node that holds its groups to the end and cannot spill them (an ordered scan on a text key, a two level aggregate) ends the scan with an error when the
 * groups it holds pass hash memory times group_nospill_factor, which the planner's estimate said they would not: statistics that predate a bulk load. The
 * error is raised where the node holds no lock, and no row of the statement has reached the client, so the statement is run again as PostgreSQL plans it,
 * after its executor state ended as a state ends that was stopped early. The plan of a prepared statement is dropped, so that its next planning meets the statistics as they are.
 */
static void
run_or_replan(RUN_PARAMS, const char *rerun)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	DestReceiver *dest = qd->dest;
	CountDest  *cd = palloc0(sizeof(CountDest));
	int			eflags = qd->estate->es_top_eflags;
	PlannedStmt *plain = NULL;

	cd->pub = (DestReceiver) {.receiveSlot = count_receive, .rStartup = count_startup, .rShutdown = count_shutdown, .rDestroy = count_destroy, .mydest = dest->mydest};
	cd->inner = dest;
	cd->cxt = oldcxt;
	sift_agg_overflow = false;
	qd->dest = &cd->pub;
	PG_TRY();
	{
		run_chain(RUN_ARGS);
	}
	PG_CATCH();
	{
		qd->dest = dest;
		MemoryContextSwitchTo(oldcxt);
		if (!(sift_agg_overflow || sift_agg_workers_overflowed(qd->planstate)) || cd->sent > 0)
			PG_RE_THROW();
		FlushErrorState();
		/* the text lives in the state that ends next */
		rerun = pstrdup(rerun);
		/* the state ends first: a parallel context of the scan that is left reads the messages of its workers, which are gone, when the planner checks for interrupts */
		standard_ExecutorFinish(qd);
		standard_ExecutorEnd(qd);
		plain = replan_plain(qd, rerun);
	}
	PG_END_TRY();
	sift_agg_overflow = false;
	if (plain != NULL)
	{
		sift_stat_add(SIFT_STAT_STATEMENTS_RERUN, 1);
		ereport(NOTICE,
				(errmsg("sift: the aggregate scan outgrew its memory; the statement ran as PostgreSQL plans it"),
				 errhint("ANALYZE the table so that the planner knows how many groups it has.")));
		qd->dest = &cd->pub;
		qd->plannedstmt = plain;
		sift_executor_start(qd, eflags);
		if (ActivePortal != NULL && ActivePortal->queryDesc == qd)
		{
			ActivePortal->tupDesc = qd->tupDesc;
			/* a prepared statement is planned again by its next execution */
			if (ActivePortal->cplan != NULL)
				ActivePortal->cplan->is_valid = false;
		}
		run_chain(RUN_ARGS);
	}
	qd->dest = dest;
}

static void
sift_executor_run(RUN_PARAMS)
{
	/* a node that cannot spill holds its groups to the end, so it fails before its statement sent a row: the first fetch of a cursor can run again */
	const char *rerun = qd->estate->es_processed == 0 && ScanDirectionIsForward(dir) && sift_enable_agg && !IsInParallelMode() && !IsParallelWorker() &&
		!(qd->estate->es_top_eflags & EXEC_FLAG_MARK) ? sift_agg_rerun_text(qd->planstate) : NULL;

	if (rerun != NULL)
		run_or_replan(RUN_ARGS, rerun);
	else
		run_chain(RUN_ARGS);
}

void
sift_agg_rerun_init(void)
{
	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = sift_executor_run;
}
