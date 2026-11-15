/*
 * agg/cte/rerun.c - a statement whose aggregate scan outgrew its memory runs again as PostgreSQL plans it.
 *
 * The scan of a node that cannot spill ends with an error when its groups pass hash memory times group_nospill_factor; the executor hook below catches
 * that error, ends the executor state and runs the statement planned without the aggregate scan.
 */

#include "postgres.h"

#include "access/xact.h"
#include "executor/executor.h"
#include "optimizer/planner.h"
#include "tcop/pquery.h"
#include "tcop/dest.h"
#include "utils/plancache.h"
#include "utils/portal.h"

#include "agg/agg.h"
#include "agg/cte/int.h"

static ExecutorRun_hook_type prev_executor_run;

#if PG_VERSION_NUM >= 180000
#define RUN_PARAMS	QueryDesc *qd, ScanDirection dir, uint64 count
#define RUN_ARGS	qd, dir, count
#else
#define RUN_PARAMS	QueryDesc *qd, ScanDirection dir, uint64 count, bool once
#define RUN_ARGS	qd, dir, count, once
#endif

/* the statement as PostgreSQL plans it, for one whose aggregate scan outgrew its memory (NULL: its text does not give it back, or it has volatile functions that ran) */
static PlannedStmt *
replan_plain(QueryDesc *qd)
{
	bool		enabled = sift_enable_agg;
	PlannedStmt *plain;

	sift_enable_agg = false;
	PG_TRY();
	{
		plain = sift_replan_text(qd, true);
	}
	PG_FINALLY();
	{
		sift_enable_agg = enabled;
	}
	PG_END_TRY();
	return plain;
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
 * A statement whose text cannot be planned again (a function's query with parameters) keeps the error.
 */
static void
run_or_replan(RUN_PARAMS)
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
		ErrorData  *edata;

		qd->dest = dest;
		MemoryContextSwitchTo(oldcxt);
		if (!sift_agg_overflow || cd->sent > 0)
			PG_RE_THROW();
		edata = CopyErrorData();
		FlushErrorState();
		if ((plain = replan_plain(qd)) == NULL)
			ReThrowError(edata);
		FreeErrorData(edata);
		standard_ExecutorFinish(qd);
		standard_ExecutorEnd(qd);
	}
	PG_END_TRY();
	sift_agg_overflow = false;
	if (plain != NULL)
	{
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
	if (count == 0 && ScanDirectionIsForward(dir) && sift_enable_agg && !IsInParallelMode() && !IsParallelWorker() &&
		!(qd->estate->es_top_eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) && sift_agg_nospill_planned(qd->planstate))
		run_or_replan(RUN_ARGS);
	else
		run_chain(RUN_ARGS);
}

void
sift_agg_rerun_init(void)
{
	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = sift_executor_run;
}
