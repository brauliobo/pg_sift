/*
 * table/f2/scan_prune.c - the granules a plain scan of a sift_table (a SeqScan, under a SELECT, an UPDATE or a DELETE) does not read: its quals that the store decides (core/store/table_qual.h)
 * become predicates on the leaves, and a granule whose zone of a leaf says no row passes one of them is skipped before a byte of it is decoded. The SeqScan keeps every qual and judges each row it
 * gets: the zones only take away granules no row of which it would keep. A granule some column delta of the snapshot patches in a column of the quals keeps no zone that holds for it, and is read.
 *
 * The scan of a SeqScan is begun by the executor at its first row, with no keys: the scans of the plan that the quals can prune are begun when the statement starts, with the predicates, and the
 * executor takes the scan it finds. A parallel scan and the scan of an EvalPlanQual recheck read every granule.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "utils/rel.h"

#include "col/f2/f2.h"
#include "col/f2/part/pin_scan.h"
#include "col/f2/store/scan.h"
#include "core/catalog.h"
#include "core/compat.h"
#include "core/meta_peek.h"
#include "core/store/table_qual.h"
#include "table/dispatch.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/scan_prune.h"
#include "table/f2/tbl.h"

struct SiftTblF2Prune
{
	SiftColPred *preds;
	int			npreds;
	uint64		attnum_bloom;	/* of the attributes of the quals, and of the TID remaps: a column delta with any of them leaves its granule to the rows */
};

static ExecutorStart_hook_type prev_executor_start;

bool
sift_tbl_f2_prune_granule(const SiftTblF2Prune *p, Relation rel, const SiftF2Store *store, SiftF2PartPin *pin, uint32 g, const SiftColDeltaGranMap *gm)
{
	return (gm == NULL || (gm->attnum_bloom & p->attnum_bloom) == 0) && sift_f2_store_zones_exclude(rel, store, pin, p->preds, p->npreds, g);
}

/* The predicates of the quals of the scan the store decides, in the statement's memory; NULL when none */
static SiftTblF2Prune *
prune_of(Relation rel, const List *qual)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(RelationGetRelid(rel), true);
	SiftStoreSchema t;
	List	   *quals = NIL;
	SiftStoreQuals q;
	SiftTblF2Prune *p;

	if (ops == NULL || !ops->schema(RelationGetRelid(rel), true, &t))
		return NULL;
	foreach_ptr(Expr, clause, qual)
	{
		Expr	   *e = sift_store_table_qual(&t, clause);

		if (e != NULL)
			quals = lappend(quals, e);
	}
	if (quals == NIL)
		return NULL;
	sift_f2_store_compile_quals(rel, sift_f2_store_open(rel), quals, NIL, CurrentMemoryContext, &q);
	if (!q.possible || q.npreds == 0)
		return NULL;
	p = palloc(sizeof(SiftTblF2Prune));
	*p = (SiftTblF2Prune) {.preds = q.preds,.npreds = q.npreds,.attnum_bloom = sift_cd_att_bit(SIFT_CD_ATT_TID_REMAP)};
	for (int i = 0; i < q.nkeys; i++)
		p->attnum_bloom |= sift_cd_att_bit(q.keys[i].sk_attno);
	return p;
}

static void
begin_pruned(SeqScanState *node, EState *estate)
{
	Relation	rel = node->ss.ss_currentRelation;
	SiftTblF2Prune *p;

	if (node->ss.ps.plan->parallel_aware || node->ss.ps.plan->qual == NIL || node->ss.ss_currentScanDesc != NULL || !sift_tbl_is_table(rel) ||
		sift_meta_resolve_major(rel) != SIFT_FORMAT2_MAJOR || (p = prune_of(rel, node->ss.ps.plan->qual)) == NULL)
		return;
	node->ss.ss_currentScanDesc = sift_table_beginscan(rel, estate->es_snapshot, 0, NULL);
	sift_tbl_f2_scan_set_prune(node->ss.ss_currentScanDesc, p);
}

static bool
begin_pruned_scans(PlanState *ps, void *estate)
{
	if (ps == NULL)
		return false;
	if (IsA(ps, SeqScanState))
		begin_pruned((SeqScanState *) ps, estate);
	return planstate_tree_walker(ps, begin_pruned_scans, estate);
}

static void
prune_executor_start(QueryDesc *qd, int eflags)
{
	if (prev_executor_start)
		prev_executor_start(qd, eflags);
	else
		standard_ExecutorStart(qd, eflags);
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY) && sift_db_has_extension())
	{
		MemoryContext old = MemoryContextSwitchTo(qd->estate->es_query_cxt);

		(void) begin_pruned_scans(qd->planstate, qd->estate);
		MemoryContextSwitchTo(old);
	}
}

void
sift_tbl_f2_prune_init(void)
{
	prev_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = prune_executor_start;
}
