/*
 * agg/scope/scan.c - the executor of a scope aggregate that no cube answers (agg/scope/plan.c, ScopeSpec.scan): the facts of scope key K are the facts whose member is one of the members the
 * pairs give K under the snapshot, so the aggregate is the scan of the relation with that list among its keys, in the granules of K when the format routes the scan (SiftScopeOps.key_members).
 * The plan prices that scan over every granule; the node counts the granules the keys of its run route to and runs the join as written, its child, when the join costs less for them.
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
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/qual/qual.h"
#include "agg/scan_src.h"
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
	int64		nscope_keys;	/* the keys of the run: 0 for a NULL key */
	Cost		open;			/* the price of the plan: opening the relation, reading every granule, leaving the groups */
	Cost		body;
	Cost		out;
	const char *fallback;		/* why the run took the join (NULL: it scanned the members) */
} ScopeScanState;

static void
scan_begin(CustomScanState *node, EState *estate, int eflags)
{
	ScopeScanState *st = (ScopeScanState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	st->spec = sift_v6_scope_spec_unpack(linitial(cs->custom_private));
	st->v6 = sift_v6_spec_unpack(lsecond(cs->custom_private), estate->es_param_list_info);
	st->open = floatVal(linitial(lthird(cs->custom_private)));
	st->body = floatVal(lsecond(lthird(cs->custom_private)));
	st->out = floatVal(lthird(lthird(cs->custom_private)));
	foreach_ptr(Plan, child, cs->custom_plans)
		node->custom_ps = lappend(node->custom_ps, ExecInitNode(child, estate, eflags));
	st->ops = sift_store_ops_for_oid(st->spec->relid, false);
	st->rel = index_open(st->spec->relid, AccessShareLock);
	st->heap = table_open(st->rel->rd_index->indrelid, AccessShareLock);
	st->key = ExecInitExpr(linitial(cs->custom_exprs), &node->ss.ps);
	st->key_type = st->spec->key_array ? get_element_type(exprType(linitial(cs->custom_exprs))) : exprType(linitial(cs->custom_exprs));
	sift_v6_qual_scankeys(&node->ss.ps, st->v6->jsonqual, st->rel, st->v6->indexqual, &st->keys, &st->nkeys, &st->rtkeys, &st->nrt);
}

/* The keys of the run: the one key, or the distinct elements of the array that are not NULL (none for a NULL key) */
static Datum *
run_keys(ScopeScanState *st, int *n)
{
	bool		isnull;
	const Datum d = ExecEvalExprSwitchContext(st->key, st->css.ss.ps.ps_ExprContext, &isnull);
	Datum	   *vals;
	bool	   *nulls;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	int			m = 0;

	*n = 0;
	if (isnull)
		return NULL;
	if (!st->spec->key_array)
	{
		vals = palloc(sizeof(Datum));
		vals[(*n)++] = d;
		return vals;
	}
	get_typlenbyvalalign(st->key_type, &typlen, &typbyval, &typalign);
	deconstruct_array(DatumGetArrayTypeP(d), st->key_type, typlen, typbyval, typalign, &vals, &nulls, &m);
	for (int i = 0; i < m; i++)
	{
		bool		seen = nulls[i];

		for (int j = 0; j < *n && !seen; j++)
			seen = datumIsEqual(vals[i], vals[j], typbyval, typlen);
		if (!seen)
			vals[(*n)++] = vals[i];
	}
	return vals;
}

/* The members of the keys, as the snapshot sees the pairs: the array the scan takes as the list of the member column. A member of several keys is listed once for each, which the list of
 * the scan holds as one; the granules of a single key are the router's */
static ArrayType *
key_members(ScopeScanState *st, EState *estate, const Datum *keys, int nkeys, const struct SiftScopeRouter **router)
{
	const Form_pg_attribute att = TupleDescAttr(RelationGetDescr(st->rel), st->spec->member_col - 1);
	ArrayBuildState *acc = initArrayResult(att->atttypid, CurrentMemoryContext, false);
	Size		total = 0;

	*router = NULL;
	for (int k = 0; k < nkeys; k++)
	{
		const struct SiftScopeRouter *r;
		uint32		n = 0;
		ArrayType  *m = st->ops->scope->key_members(st->heap, st->rel, st->spec->ord, estate->es_snapshot, st->key_type, keys[k], att->atttypid, st->gcx, &n, &r);
		Datum	   *vals;
		bool	   *nulls;
		int			nv;

		total += n;
		sift_budget_check(total * sizeof(Datum), work_mem, "the members of the scope keys", "work_mem", NULL);
		if (nkeys == 1)
			*router = r;
		deconstruct_array(m, att->atttypid, att->attlen, att->attbyval, att->attalign, &vals, &nulls, &nv);
		for (int i = 0; i < nv; i++)
			accumArrayResult(acc, vals[i], nulls[i], att->atttypid, CurrentMemoryContext);
	}
	st->members += total;
	return DatumGetArrayTypeP(makeArrayResult(acc, CurrentMemoryContext));
}

/* The share of the granules a scan of the members of keys reads: those of the keys when the format routes it, else all of them */
static double
routed_share(ScopeScanState *st, const struct SiftScopeRouter *router)
{
	int64		kept,
				total;

	if (router == NULL)
		return 1.0;
	st->ops->scope->router_counts(router, &kept, &total);
	return total > 0 ? (double) kept / total : 0.0;
}

static void
scan_members(ScopeScanState *st, EState *estate, ScanKey keys, ArrayType *members, const struct SiftScopeRouter *router)
{
	keys[st->nkeys] = (ScanKeyData) {.sk_flags = SK_SEARCHARRAY,.sk_attno = st->spec->member_col,.sk_strategy = BTEqualStrategyNumber,.sk_argument = PointerGetDatum(members)};
	st->ops->zone->scan_run(st->v6, st->group, st->heap, st->rel, keys, st->nkeys + 1, estate->es_snapshot, NULL, 0, router, &st->stats, NULL, NULL, NULL);
}

/*
 * The facts of the members of the keys folded into the groups: one scan for a key, or for a semi join that counts a fact once whichever keys its member has; a join counts a fact once for each
 * pair of its member with the keys, so the facts of each key are folded into the same groups. The scans cost the plan's price of the granules they read: the join runs when it costs less.
 */
static void
scan_run(ScopeScanState *st, EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	ScanKey		keys = palloc((st->nkeys + 1) * sizeof(ScanKeyData));
	Datum	   *scope_keys;
	int			n,
				nscans;
	ArrayType **members;
	const struct SiftScopeRouter **routers;
	double		share = 0;

	st->started = true;
	sift_v6_source_enter(st->rel, false, estate->es_snapshot);
	if (st->nrt > 0)
		ExecIndexEvalRuntimeKeys(st->css.ss.ps.ps_ExprContext, st->rtkeys, st->nrt);
	st->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 scope groups");
	memcpy(keys, st->keys, st->nkeys * sizeof(ScanKeyData));
	st->group = sift_v6_group_new(st->v6, st->gcx, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	scope_keys = run_keys(st, &n);
	st->nscope_keys = n;
	nscans = n <= 1 || st->spec->once ? 1 : n;
	members = palloc(nscans * sizeof(ArrayType *));
	routers = palloc(nscans * sizeof(*routers));
	for (int i = 0; i < nscans; i++)
	{
		members[i] = key_members(st, estate, nscans == 1 ? scope_keys : &scope_keys[i], nscans == 1 ? n : 1, &routers[i]);
		share += n == 0 ? 0.0 : routed_share(st, routers[i]);
	}
	st->router = n == 1 ? routers[0] : NULL;
	if (st->open + share * st->body + st->out > ((Plan *) linitial(((CustomScan *) st->css.ss.ps.plan)->custom_plans))->total_cost)
		st->fallback = psprintf("join is cheaper for %d key%s", n, n == 1 ? "" : "s");
	else
		for (int i = 0; i < nscans; i++)
			scan_members(st, estate, keys, members[i], routers[i]);
	sift_v6_group_finish(st->group);
	MemoryContextSwitchTo(old);
}

static TupleTableSlot *
scan_next(ScanState *ss)
{
	ScopeScanState *st = (ScopeScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

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

/* The rows of the join are the node's own: the slot the child leaves its rows in is not the kind of slot the parents of this node were compiled against */
static TupleTableSlot *
join_next(ScanState *ss)
{
	TupleTableSlot *row = ExecProcNode((PlanState *) linitial(((CustomScanState *) ss)->custom_ps));

	return TupIsNull(row) ? NULL : ExecCopySlot(ss->ss_ScanTupleSlot, row);
}

static TupleTableSlot *
scan_exec(CustomScanState *node)
{
	ScopeScanState *st = (ScopeScanState *) node;
	MemoryContext old = CurrentMemoryContext;
	TupleTableSlot *slot;

	if (!st->started)
		scan_run(st, node->ss.ps.state);
	slot = ExecScan(&node->ss, st->fallback != NULL ? join_next : scan_next, scan_recheck);

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
	st->members = st->nscope_keys = 0;
	st->started = false;
	st->fallback = NULL;
}

static void
scan_end(CustomScanState *node)
{
	ScopeScanState *st = (ScopeScanState *) node;

	scan_reset(st);
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecEndNode(child);
	table_close(st->heap, AccessShareLock);
	index_close(st->rel, AccessShareLock);
}

static void
scan_rescan(CustomScanState *node)
{
	scan_reset((ScopeScanState *) node);
	foreach_ptr(PlanState, child, node->custom_ps)
	{
		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(child, node->ss.ps.chgParam);
		if (child->chgParam == NULL)
			ExecReScan(child);
	}
}

static void
scan_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ScopeScanState *st = (ScopeScanState *) node;
	int64		kept,
				total;

	const ScopeShow show = {st->rel, st->spec, linitial(((CustomScan *) node->ss.ps.plan)->custom_exprs), st->started ? st->nscope_keys : -1, st->fallback};

	ExplainPropertyText("Index", RelationGetRelationName(st->rel), es);
	sift_v6_scope_show(&show, &node->ss.ps, ancestors, es);
	ExplainPropertyText("Scope Read", st->fallback != NULL ? "join" : "scan of the facts (the members of the key)", es);
	if (!st->started || st->fallback != NULL)
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
