/*
 * col/am/ioss.c - the columns an index-only scan of a sift_col index reads, told to its scan of either format
 */
#include "postgres.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "col/am.h"
#include "core/dispatch.h"

/*
 * An index-only scan of a sift_col index forms its index tuples from the columns the plan node reads, the others NULL (a column it never reads is not decoded,
 * converted or formed: for `select id` over 7 columns that was most of the scan's instructions). The node reads what its target list, its quals and its recheck
 * quals reference (INDEX_VAR); the index conditions are the access method's. The scan descriptor is made by the node's first call, so after ExecutorStart
 * (sift_col_ioss_want) the node's function is replaced by one that, on that first call, tells ambeginscan which columns this scan is for, and puts the node's own
 * function back. A scan that begins without the entry (a plain index scan, a node the hook did not see) reads every column, as before.
 */
typedef struct IossWant
{
	PlanState  *ps;
	Bitmapset  *want;
	bool		project;
	bool		ordered;		/* the plan asked the order of the scan: the name of the first entry of its indextlist is COL_ORDERED_MARK (ordpath.c) */
	bool		ordpair;		/* ... of the first two columns: COL_ORDERED_PAIR_MARK */
	bool		taken;
} IossWant;

static List *ioss_pending;		/* IossWant of the nodes that have not run yet, in TopMemoryContext; emptied at the end of the transaction */

static IossWant *ioss_cur;		/* the entry of the node whose first call is running */

static ExecProcNodeMtd ioss_real;	/* ExecIndexOnlyScan, the same for every node */

static bool ioss_xact_registered;

static void
ioss_free_pending(void)
{
	foreach_ptr(IossWant, w, ioss_pending)
	{
		bms_free(w->want);
		pfree(w);
	}
	list_free(ioss_pending);
	ioss_pending = NIL;
}

static void
ioss_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT)
	{
		ioss_cur = NULL;
		ioss_free_pending();
	}
}

static TupleTableSlot *
ioss_first(PlanState *ps)
{
	IossWant   *mine = NULL;
	IossWant   *outer = ioss_cur;
	TupleTableSlot *slot;

	foreach_ptr(IossWant, w, ioss_pending)
		if (w->ps == ps)
			mine = w;
	if (ps->ExecProcNode == ioss_first)
		ps->ExecProcNode = ioss_real;
	ps->ExecProcNodeReal = ioss_real;
	if (mine == NULL)
		return ioss_real(ps);
	ioss_pending = list_delete_ptr(ioss_pending, mine);
	ioss_cur = mine;
	PG_TRY();
	{
		slot = ioss_real(ps);
	}
	PG_FINALLY();
	{
		ioss_cur = outer;
		bms_free(mine->want);
		pfree(mine);
	}
	PG_END_TRY();
	return slot;
}

static bool
ioss_walk(PlanState *ps, void *context)
{
	if (IsA(ps, IndexOnlyScanState))
	{
		IndexOnlyScanState *is = (IndexOnlyScanState *) ps;
		IndexOnlyScan *plan = (IndexOnlyScan *) ps->plan;

		if (is->ioss_ScanDesc == NULL && is->ioss_RelationDesc->rd_rel->relam == sift_col_am_oid() && (ioss_real == NULL || ioss_real == ps->ExecProcNodeReal))
		{
			Bitmapset  *attrs = NULL;
			IossWant   *w;
			MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
			int			x = -1;

			if (!ioss_xact_registered)
			{
				sift_xact_callback(ioss_xact_callback, NULL);
				ioss_xact_registered = true;
			}
			w = palloc0(sizeof(IossWant));
			w->ps = ps;
			w->project = true;
			w->ordpair = plan->indextlist != NIL && ((TargetEntry *) linitial(plan->indextlist))->resname != NULL &&
				strcmp(((TargetEntry *) linitial(plan->indextlist))->resname, COL_ORDERED_PAIR_MARK) == 0;
			w->ordered = w->ordpair || (plan->indextlist != NIL && ((TargetEntry *) linitial(plan->indextlist))->resname != NULL &&
										strcmp(((TargetEntry *) linitial(plan->indextlist))->resname, COL_ORDERED_MARK) == 0);
			pull_varattnos((Node *) plan->scan.plan.targetlist, INDEX_VAR, &attrs);
			pull_varattnos((Node *) plan->scan.plan.qual, INDEX_VAR, &attrs);
			pull_varattnos((Node *) plan->recheckqual, INDEX_VAR, &attrs);
			while ((x = bms_next_member(attrs, x)) >= 0)
			{
				AttrNumber	a = x + FirstLowInvalidHeapAttributeNumber;

				/* a whole-row reference or a system column of the index tuple: every column */
				w->project &= a > 0;
				w->want = bms_add_member(w->want, a);
			}
			if (w->ordered)
			{
				/*
				 * The planner offered the order for a plan that reads the first column alone (ordpath.c): the other columns of the node's list are the padding of a physical target list (the whole index is
				 * what an index-only scan is given when it has no projection to make), which nothing above reads. The scan gives NULL for them and reads the first column in batches.
				 */
				bms_free(w->want);
				w->want = bms_make_singleton(1);
				if (w->ordpair)
					w->want = bms_add_member(w->want, 2);
				w->project = true;
			}
			ioss_pending = lappend(ioss_pending, w);
			MemoryContextSwitchTo(old);
			bms_free(attrs);
			ioss_real = ps->ExecProcNodeReal;
			ps->ExecProcNodeReal = ioss_first;
		}
	}
	return planstate_tree_walker(ps, ioss_walk, context);
}

void
sift_col_ioss_want(QueryDesc *queryDesc)
{
	(void) ioss_walk(queryDesc->planstate, NULL);
}

bool
sift_col_ioss_take(Relation index, bool *ordered, bool *ordpair, Bitmapset **want)
{
	*ordered = *ordpair = false;
	*want = NULL;
	if (ioss_cur == NULL || ioss_cur->taken || ((IndexOnlyScanState *) ioss_cur->ps)->ioss_RelationDesc != index)
		return false;
	ioss_cur->taken = true;
	*ordered = ioss_cur->ordered;
	*ordpair = ioss_cur->ordpair;
	*want = bms_copy(ioss_cur->want);
	return ioss_cur->project;
}
