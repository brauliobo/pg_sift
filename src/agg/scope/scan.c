/*
 * agg/scope/scan.c - the executor of a scope aggregate that no cube answers (agg/scope/plan.c, ScopeSpec.scan): the facts of scope key K are the facts whose member is one of the members the
 * pairs give K under the snapshot, so the aggregate is the scan of the relation with that list among its keys, in the granules of K when the format routes the scan (SiftScopeOps.key_members).
 */
/* check-memory: allow-allocs the members of one key and the keys of one scan */
#include "postgres.h"

#include "access/genam.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/qual/qual.h"
#include "agg/scope/scope.h"
#include "core/memtrack.h"

typedef struct ScopeScanState
{
	CustomScanState css;
	ScopeSpec  *spec;
	SiftV6Spec *v6;
	const SiftStoreOps *ops;
	Relation	rel;			/* the index of the facts */
	Relation	heap;
	ExprState  *key;
	Oid			key_type;
	ScanKey		keys;
	int			nkeys;
	IndexRuntimeKeyInfo *rtkeys;	/* the keys of parameters of the quals: their values are those of each run */
	int			nrt;
	bool		started;
	MemoryContext gcx;
	SiftV6Group *group;
	const struct SiftScopeRouter *router;	/* the granules of the key, NULL when the format does not route */
	SiftV6ScanStats stats;
	int64		members;
} ScopeScanState;

static void
scan_begin(CustomScanState *node, EState *estate, int eflags)
{
	ScopeScanState *st = (ScopeScanState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	st->spec = sift_v6_scope_spec_unpack(linitial(cs->custom_private));
	st->v6 = sift_v6_spec_unpack(lsecond(cs->custom_private));
	st->ops = sift_store_ops_for_oid(st->spec->relid, false);
	st->rel = index_open(st->spec->relid, AccessShareLock);
	st->heap = table_open(st->rel->rd_index->indrelid, AccessShareLock);
	st->key = ExecInitExpr(linitial(cs->custom_exprs), &node->ss.ps);
	st->key_type = exprType(linitial(cs->custom_exprs));
	sift_v6_qual_scankeys(&node->ss.ps, st->v6->jsonqual, st->rel, st->v6->indexqual, &st->keys, &st->nkeys, &st->rtkeys, &st->nrt);
}

/* The members of the key, as the snapshot sees the pairs: the array the scan takes as the list of the member column (none for a NULL key) */
static ArrayType *
key_members(ScopeScanState *st, EState *estate)
{
	const Oid	member_type = TupleDescAttr(RelationGetDescr(st->rel), st->spec->member_col - 1)->atttypid;
	bool		no_key;
	const Datum key = ExecEvalExprSwitchContext(st->key, st->css.ss.ps.ps_ExprContext, &no_key);
	uint32		n = 0;
	ArrayType  *members = no_key ? construct_empty_array(member_type) :
		st->ops->scope->key_members(st->heap, st->rel, st->spec->ord, estate->es_snapshot, st->key_type, key, member_type, st->gcx, &n, &st->router);

	sift_budget_check((Size) n * sizeof(Datum), work_mem, "the members of a scope key", "work_mem", NULL);
	st->members = n;
	return members;
}

static void
scan_run(ScopeScanState *st, EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	ScanKey		keys = palloc((st->nkeys + 1) * sizeof(ScanKeyData));

	st->started = true;
	if (st->nrt > 0)
		ExecIndexEvalRuntimeKeys(st->css.ss.ps.ps_ExprContext, st->rtkeys, st->nrt);
	st->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 scope groups");
	memcpy(keys, st->keys, st->nkeys * sizeof(ScanKeyData));
	keys[st->nkeys] = (ScanKeyData) {.sk_flags = SK_SEARCHARRAY,.sk_attno = st->spec->member_col,.sk_strategy = BTEqualStrategyNumber,
	.sk_argument = PointerGetDatum(key_members(st, estate))};
	st->group = sift_v6_group_new(st->v6, st->gcx, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	st->ops->zone->scan_run(st->v6, st->group, st->heap, st->rel, keys, st->nkeys + 1, estate->es_snapshot, NULL, 0, st->router, &st->stats, NULL, NULL, NULL);
	sift_v6_group_finish(st->group);
	MemoryContextSwitchTo(old);
}

static TupleTableSlot *
scan_next(ScanState *ss)
{
	ScopeScanState *st = (ScopeScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (!st->started)
		scan_run(st, ss->ps.state);
	ExecClearTuple(slot);
	MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	if (!sift_v6_group_next(st->group, slot->tts_values, slot->tts_isnull))
		return NULL;
	return ExecStoreVirtualTuple(slot);
}

static bool
scan_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
scan_exec(CustomScanState *node)
{
	MemoryContext old = CurrentMemoryContext;
	TupleTableSlot *slot = ExecScan(&node->ss, scan_next, scan_recheck);

	MemoryContextSwitchTo(old);
	return slot;
}

static void
scan_reset(ScopeScanState *st)
{
	if (st->group != NULL)
		sift_v6_group_free(st->group);
	if (st->gcx != NULL)
		MemoryContextDelete(st->gcx);
	st->gcx = NULL;
	st->group = NULL;
	st->router = NULL;
	st->stats = (SiftV6ScanStats) {0};
	st->started = false;
}

static void
scan_end(CustomScanState *node)
{
	ScopeScanState *st = (ScopeScanState *) node;

	scan_reset(st);
	table_close(st->heap, AccessShareLock);
	index_close(st->rel, AccessShareLock);
}

static void
scan_rescan(CustomScanState *node)
{
	scan_reset((ScopeScanState *) node);
}

static void
scan_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ScopeScanState *st = (ScopeScanState *) node;
	int64		kept,
				total;

	ExplainPropertyText("Index", RelationGetRelationName(st->rel), es);
	ExplainPropertyText("Scope Read", "scan of the facts (the members of the key)", es);
	if (!st->started)
		return;
	ExplainPropertyInteger("Scope Members", NULL, st->members, es);
	if (st->router != NULL)
	{
		st->ops->scope->router_counts(st->router, &kept, &total);
		ExplainPropertyInteger("Scope Granules Kept", NULL, kept, es);
		ExplainPropertyInteger("Scope Granules Total", NULL, total, es);
	}
	ExplainPropertyUInteger("Rows Vetted Against The Heap", NULL, st->stats.heap_vet_rows, es);
}

static const CustomExecMethods scan_exec_methods = {
	.CustomName = "SiftAggV6Scope",
	.BeginCustomScan = scan_begin,
	.ExecCustomScan = scan_exec,
	.EndCustomScan = scan_end,
	.ReScanCustomScan = scan_rescan,
	.ExplainCustomScan = scan_explain,
};

static Node *
scan_create_state(CustomScan *cscan)
{
	ScopeScanState *st = (ScopeScanState *) newNode(sizeof(ScopeScanState), T_CustomScanState);

	st->css.methods = &scan_exec_methods;
	return (Node *) st;
}

const CustomScanMethods sift_v6_scope_scan_methods = {"SiftAggV6Scope", scan_create_state};
