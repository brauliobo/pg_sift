/*
 * agg/hybrid_store.c - what SiftHybridScan and SiftHybridAgg do the same way (agg/hybrid_state.h): the right hand sides of the query and of the quals are evaluated when the scan
 * starts (and again after a rescan, when a parameter changed): the query binds, the keys of the index quals and the predicates of agg/hybrid_quals.h are made, and an adaptive scan takes the
 * direction the bound query makes cheaper (sift_hybrid_pick), with the document frequency of the query and the granules the zone maps leave.
 *
 * check-memory: allow-allocs every array here follows one chunk of hits or one batch of rows, which the byte budget of the bridge and the granule bound
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "storage/predicate.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/f2_store.h"
#include "agg/hybrid_semi.h"
#include "agg/hybrid_state.h"
#include "core/compat.h"
#include "core/sift.h"
#include "core/memtrack.h"

ItemPointerData
sift_hybrid_tid_of(SiftKey key)
{
	ItemPointerData tid;

	sift_f2_tid_set(&tid, key);
	return tid;
}

/* The clauses of the heap's are in terms of the scan tuple (the entries of custom_scan_tlist): its Var entries are the columns of the heap's row, the others are not read */
static void
load_scan_slot(HybridState *st)
{
	TupleTableSlot *ss = st->css.ss.ss_ScanTupleSlot;
	int			i = 0;

	ExecClearTuple(ss);
	foreach_node(TargetEntry, te, st->scan_tlist)
	{
		ss->tts_isnull[i] = true;
		if (IsA(te->expr, Var))
			ss->tts_values[i] = slot_getattr(st->heapslot, ((Var *) te->expr)->varattno, &ss->tts_isnull[i]);
		i++;
	}
	ExecStoreVirtualTuple(ss);
}

/* The heap's judgment of a TID: visible (the version is fetched into heapslot, its TID kept) and, with quals, passing the structured quals */
bool
sift_hybrid_heap_check(void *arg, SiftKey key, bool quals)
{
	HybridState *st = arg;
	ExprContext *econtext = st->qual_econtext;
	ItemPointerData tid = sift_hybrid_tid_of(key);

	if (!sift_vis_fetch(&st->vis, &tid, st->heapslot))
		return false;
	st->fetched = true;
	st->fetched_tid = tid;
	if (!quals)
		return true;
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = st->heapslot;
	if (!ExecQual(st->heap_quals, econtext))
		return false;
	if (st->residual_state == NULL && (st->exists_state == NULL || st->hints_exact))
		return true;
	load_scan_slot(st);
	econtext->ecxt_scantuple = st->css.ss.ss_ScanTupleSlot;
	return ExecQual(st->residual_state, econtext) && (st->hints_exact || ExecQual(st->exists_state, econtext));
}

bool
sift_hybrid_visible_only(void *arg, SiftKey key, bool quals)
{
	HybridState *st = arg;
	ItemPointerData tid = sift_hybrid_tid_of(key);
	bool		all_dead;

	if (VM_ALL_VISIBLE(st->heap, ItemPointerGetBlockNumber(&tid), &st->vmbuf))
		return true;
	return table_index_fetch_tuple_check(st->heap, &tid, st->css.ss.ps.state->es_snapshot, &all_dead);
}

static text *
index_text(HybridState *st)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	FormIndexDatum(st->iinfo, st->heapslot, st->css.ss.ps.state, values, isnull);
	return isnull[0] ? NULL : DatumGetTextPP(values[0]);
}

static bool
with_text(HybridState *st, ItemPointer tid, bool want_doc, text **doc)
{
	if (!want_doc)
		return true;
	return sift_vis_fetch(&st->vis, tid, st->heapslot) && (*doc = index_text(st)) != NULL;
}

bool
sift_hybrid_verify_a(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	HybridState *st = arg;

	st->fetched = false;
	if (!sift_hybrid_a_accept(st->run, sift_f2_tid(tid)))
		return false;
	if (st->fetched)
		*tid = st->fetched_tid;
	return with_text(st, tid, want_doc, doc);
}

/* B with a few hits: the heap decides each, as the standard plan does, and the store is not read */
bool
sift_hybrid_verify_point(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	HybridState *st = arg;

	st->point_verifies++;
	return sift_hybrid_heap_check(st, sift_f2_tid(tid), true) && with_text(st, tid, want_doc, doc);
}

bool
sift_hybrid_rest_row(HybridState *st, ItemPointer tid)
{
	ExprContext *econtext = st->qual_econtext;

	if (!sift_vis_fetch(&st->vis, tid, st->heapslot))
		return false;
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = st->heapslot;
	return !ExecQual(st->heap_quals, econtext);
}

bool
sift_hybrid_verify_rest(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	return sift_hybrid_rest_row(arg, tid) && with_text(arg, tid, want_doc, doc);
}

bool
sift_hybrid_verify_b(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	return with_text(arg, tid, want_doc, doc);
}

typedef struct F2Arg
{
	SiftHybridSink sink;
	void	   *sink_arg;
	bool		table;			/* a sift_table: its store decides visibility, and has no undecided rows or scores */
} F2Arg;

static void
f2_sink(void *arg, const SiftF2Batch *b)
{
	F2Arg	   *a = arg;
	const int	words = (b->n + 63) / 64;
	uint64	   *any;
	SiftHybridBatch hb;

	if (a->table)
	{
		hb = (SiftHybridBatch) {.tid = b->tid,.live = b->live,.n = b->n,.exact_vis = true,.cols = b->views};
		a->sink(a->sink_arg, &hb);
		return;
	}
	any = palloc(Max(words, 1) * sizeof(uint64));
	hb = (SiftHybridBatch) {.tid = b->tid,.live = any,.maybe = b->maybe,.score = b->scores,.n = b->n,.exact_vis = false,.cols = b->views};
	for (int w = 0; w < words; w++)
		any[w] = b->live[w] | b->maybe[w];
	a->sink(a->sink_arg, &hb);
	pfree(any);
}

static void
table_scan(HybridState *st, const SiftTidBridge *br, F2Arg *a)
{
	SiftStoreScanOpts o = {.rel = st->heap,.store = st->table,.snap = st->css.ss.ps.state->es_snapshot,.preds = st->table_quals.preds,.npreds = st->table_quals.npreds,
		.leaves = br ? st->leaves : NULL,.nleaves = br ? st->nleaves : 0,.bridge = br,.bridge_seen = &st->granules_seen,.bridge_skipped = &st->granules_skipped};

	if (st->table_quals.possible)
		sift_f2_store_scan_sink(&o, f2_sink, a);
}

static void
col_scan(HybridState *st, const SiftTidBridge *br, SiftHybridSink sink, void *sink_arg)
{
	F2Arg		a = {sink, sink_arg, st->table != NULL};
	IndexScanDesc scan;

	if (st->table != NULL)
	{
		table_scan(st, br, &a);
		return;
	}
	scan = sift_index_beginscan(st->heap, st->store_index, st->css.ss.ps.state->es_snapshot, st->nscan);
	index_rescan(scan, st->scan_keys, st->nscan, NULL, 0);
	if (st->n_ext_preds > 0 || br)
		sift_f2_scan_set_ext_preds(scan, st->ext_preds, st->n_ext_preds, &st->parts_skipped);
	if (br)
	{
		sift_f2_scan_set_bridge(scan, br, &st->granules_seen, &st->granules_skipped);
		if (st->heap_decide)
			sift_f2_scan_set_heap_decide(scan);
	}
	sift_f2_scan_sink_maybe(scan, f2_sink, &a, br ? st->leaves : NULL, br ? st->nleaves : 0);
	index_endscan(scan);
}

static void
store_scan_quals(void *arg, SiftHybridSink sink, void *sink_arg)
{
	col_scan(arg, NULL, sink, sink_arg);
}

static void
store_scan_bridge(void *arg, const SiftTidBridge *br, SiftHybridSink sink, void *sink_arg, SiftHybridStats *stats)
{
	HybridState *st = arg;

	col_scan(st, br, sink, sink_arg);
	stats->granules_seen = st->granules_seen;
	stats->granules_skipped = st->granules_skipped;
}

bool
sift_hybrid_next_hit(void *arg, SiftKey *key, float4 *score)
{
	HybridState *st = arg;
	ItemPointerData tid;

	if (!sift_rank_next(st->rank, &tid, score))
		return false;
	*key = sift_f2_tid(&tid);
	return true;
}

/* A Var of the rows the scan hands over (INDEX_VAR over the custom scan's tlist) as the heap's column, or as the column of the store's index it is */
typedef struct VarMap
{
	List	   *tlist;
	Relation	index;			/* NULL: the heap's Var */
	bool		table;			/* the Var of a sift_table's attribute: its store is numbered by the attributes */
} VarMap;

static Node *
map_var(Node *node, VarMap *m)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR)
	{
		Var		   *v = (Var *) list_nth_node(TargetEntry, m->tlist, ((Var *) node)->varattno - 1)->expr;

		if (m->table)
			return (Node *) makeVar(INDEX_VAR, v->varattno, v->vartype, v->vartypmod, v->varcollid, 0);
		if (m->index == NULL)
			return (Node *) copyObject(v);
		for (int i = 0; i < m->index->rd_index->indnatts; i++)
			if (m->index->rd_index->indkey.values[i] == v->varattno)
				return (Node *) makeVar(INDEX_VAR, i + 1, v->vartype, v->vartypmod, v->varcollid, 0);
		SIFT_INVARIANT(false, "sift: a column of a qual of the hybrid scan is not in its store");
	}
	return expression_tree_mutator(node, map_var, m);
}

static void
init_quals(HybridState *st, CustomScan *cs)
{
	List	   *qinfo = lsecond(cs->custom_private),
			   *quals = list_delete_first(list_copy(cs->custom_exprs)),
			   *heap_forms = NIL,
			   *residual_forms = NIL,
			   *exists_forms = NIL;
	VarMap		to_heap = {cs->custom_scan_tlist, NULL, false},
				to_index = {cs->custom_scan_tlist, st->store_index, st->table != NULL};
	int			i = 0;

	foreach_ptr(Expr, q, quals)
	{
		SiftHybQual hq = {list_nth_int(qinfo, 3 * i), list_nth_int(qinfo, 3 * i + 1), list_nth_int(qinfo, 3 * i + 2) != 0};

		switch (hq.kind)
		{
			case SHQ_KEYSET:
				st->hint_quals = lappend(st->hint_quals, map_var((Node *) q, &to_index));
				break;
			case SHQ_RESIDUAL:
				residual_forms = lappend(residual_forms, q);
				break;
			case SHQ_EXISTS:
				exists_forms = lappend(exists_forms, q);
				break;
			case SHQ_STD:
				heap_forms = lappend(heap_forms, map_var((Node *) q, &to_heap));
				st->std_quals = lappend(st->std_quals, map_var((Node *) q, &to_index));
				break;
			default:
				heap_forms = lappend(heap_forms, map_var((Node *) q, &to_heap));
				SIFT_INVARIANT(st->table == NULL, "sift: a sift_table takes only standard quals in a hybrid scan");
				st->ext = lappend(st->ext, sift_hybrid_ext_init(&st->css.ss.ps, &hq, q, sift_col_rel(st->store_index)->first[hq.col + 1]));
		}
		i++;
	}
	st->heap_quals = ExecInitQual(heap_forms, NULL);
	st->residual_state = ExecInitQual(residual_forms, &st->css.ss.ps);
	st->exists_state = ExecInitQual(exists_forms, &st->css.ss.ps);
	st->nexists = list_length(exists_forms);
}

#define HYB_ARRAY_GUESS 10		/* the values the planner assumes of an array it cannot see (estimate_array_length) */

/* The values of a key set: one of more than the store takes as an IN list is left to the quals the rows are tested by */
static int
key_set_size(const ScanKeyData *sk)
{
	ArrayType  *a;

	if (!(sk->sk_flags & SK_SEARCHARRAY))
		return 0;
	a = DatumGetArrayTypeP(sk->sk_argument);
	return ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
}

/* The share of the rows a key set of n values keeps, over the HYB_ARRAY_GUESS values the planner assumed of one it cannot see; all the rows for a set the store does not take */
static double
key_set_scale(const HybridState *st, int n)
{
	return (n > SIFT_HYB_ACL_PUSH_MAX ? st->est[0] : n) / (double) HYB_ARRAY_GUESS;
}

/*
 * The key sets of the tenant EXISTS go to the store when the query has more hits than a point bridge holds (or the scan reads the store first): a few hits are tested by the
 * EXISTS of the plan, and the sub-select of a set is then never run. A set the store does not take (NULL, or more values than it takes as an IN list) is left to the EXISTS too.
 */
static void
take_hints(HybridState *st)
{
	bool		pays = sift_plan_docs(st->plan) > SIFT_TIDB_POINT_MAX || (!st->adaptive && st->dir == SIFT_HYB_COL_TO_BM25);

	st->hints_taken = 0;
	st->hints_exact = false;
	if (st->nhint == 0)
		return;
	if (pays && st->nhrt > 0)
		ExecIndexEvalRuntimeKeys(st->rt_econtext, st->hint_rtkeys, st->nhrt);
	for (int k = 0; k < st->nhint; k++)
	{
		int			n = pays && !(st->hint_keys[k].sk_flags & SK_ISNULL) ? key_set_size(&st->hint_keys[k]) : INT_MAX;

		st->key_scale *= key_set_scale(st, n);
		if (n <= SIFT_HYB_ACL_PUSH_MAX)
		{
			st->scan_keys[st->nscan++] = st->hint_keys[k];
			st->hints_taken++;
		}
	}
	st->hints_exact = st->nexists > 0 && st->hints_taken == st->nhint && st->nhint == st->nexists;
}

/* The right hand sides of the quals and the query are values now: the keys of the index quals, the predicates of the others; false when a value leaves no row */
static bool
eval_quals(HybridState *st)
{
	ExprContext *ec = st->rt_econtext;
	int			n = 0;

	ResetExprContext(ec);
	st->exact_quals = true;
	st->n_ext_preds = 0;
	if (st->nrt > 0)
		ExecIndexEvalRuntimeKeys(ec, st->rtkeys, st->nrt);
	for (int k = 0; k < st->nkeys; k++)
		if ((st->keys[k].sk_flags & (SK_ISNULL | SK_SEARCHNULL | SK_SEARCHNOTNULL)) == SK_ISNULL)
			return false;
	st->scan_keys = MemoryContextAlloc(st->cxt, Max(st->nkeys + st->nhint, 1) * sizeof(ScanKeyData));
	st->nscan = 0;
	st->key_scale = 1.0;
	for (int i = 0; i < st->nrt; i++)
		if (key_set_size(st->rtkeys[i].scan_key) > 0)
			st->key_scale *= key_set_scale(st, key_set_size(st->rtkeys[i].scan_key));
	for (int k = 0; k < st->nkeys; k++)
		if (key_set_size(&st->keys[k]) > SIFT_HYB_ACL_PUSH_MAX)
			st->exact_quals = false;
		else
			st->scan_keys[st->nscan++] = st->keys[k];
	st->ext_preds = MemoryContextAlloc(st->cxt, Max(2 * list_length(st->ext), 1) * sizeof(SiftColPred));
	foreach_ptr(SiftHybExt, e, st->ext)
	{
		sift_hybrid_ext_eval(e, ec, st->cxt);
		if (e->unsat)
			return false;
		st->exact_quals &= !e->to_heap;
		for (int i = 0; i < e->npreds; i++)
			st->ext_preds[n++] = e->pred[i];
	}
	st->n_ext_preds = n;
	return true;
}

/* The granules of the store that the bound structured quals leave after its key ranges, zones and skipping indexes */
static double
surviving_granules(HybridState *st)
{
	IndexScanDesc scan = sift_index_beginscan(st->heap, st->store_index, st->css.ss.ps.state->es_snapshot, st->nscan);
	uint64		n;

	index_rescan(scan, st->scan_keys, st->nscan, NULL, 0);
	if (st->n_ext_preds > 0)
		sift_f2_scan_set_ext_preds(scan, st->ext_preds, st->n_ext_preds, NULL);
	n = sift_f2_scan_survey(scan);
	index_endscan(scan);
	return (double) n;
}

/* The estimate the planner made, with what the bound values say: the documents the query matches and the granules the structured quals leave */
bool
sift_hybrid_has_store_quals(const HybridState *st)
{
	return st->table != NULL ? st->table_quals.npreds > 0 : st->nscan > 0 || st->n_ext_preds > 0;
}

/* The rows of tid[0..n) left in live that the heap shows: the rows of an all-visible page are no heap check, the others are one each */
void
sift_hybrid_visible_batch(void *arg, const uint64 *tid, uint64 *live, int n)
{
	HybridState *st = arg;
	int			m = 0;
	BlockNumber *blocks = palloc(Max(n, 1) * sizeof(BlockNumber));
	int		   *row = palloc(Max(n, 1) * sizeof(int));
	uint64	   *allvis;

	for (int i = 0; i < n; i++)
		if (live[i / 64] >> (i % 64) & 1)
		{
			row[m] = i;
			blocks[m++] = (BlockNumber) (tid[i] >> 16);
		}
	allvis = palloc(((m + 63) / 64 + 1) * sizeof(uint64));
	sift_vis_blocks_allvisible(&st->vis, blocks, m, allvis);
	for (int j = 0; j < m; j++)
		if (!(allvis[j / 64] >> (j % 64) & 1) && !sift_hybrid_visible_only(st, tid[row[j]], false))
			live[row[j] / 64] &= ~(UINT64CONST(1) << (row[j] % 64));
	pfree(blocks);
	pfree(row);
	pfree(allvis);
}

SiftHybridDir
sift_hybrid_pick_direction(HybridState *st, bool agg)
{
	SiftHybridEst e = {
		.host = st->host,.rows = st->est[0],.granules = st->est[1],.col_granules = Min(st->est[1], surviving_granules(st)),.heap_fetch = st->est[3],.sel_col = Min(1.0, st->est[4] * st->key_scale),
		.sel_bm25 = Min(1.0, sift_plan_docs(st->plan) / Max(st->est[0], 1.0)),.limit = st->est[5],.hit_budget = work_mem * 1024.0,
		.ranked = list_nth_int(linitial(((CustomScan *) st->css.ss.ps.plan)->custom_private), SIFT_HYBP_RANKED) != 0,.has_col_quals = sift_hybrid_has_store_quals(st),
		.covered = st->covered,.sel_acl = 1.0,.has_residual = st->residual,.agg_mode = agg,.allvisfrac = st->est[6]
	};

	return sift_hybrid_pick(&e);
}

bool
sift_hybrid_rows_exact(const HybridState *st)
{
	return st->exact_quals && st->residual_state == NULL && (st->exists_state == NULL || st->hints_exact);
}

void
sift_hybrid_open(HybridState *st, CustomScan *cs, EState *estate, Relation heap)
{
	PlanState  *ps = &st->css.ss.ps;
	List	   *ints = linitial(cs->custom_private),
			   *estc = lthird_node(List, cs->custom_private);
	IndexArrayKeyInfo *arrkeys = NULL;
	int			narr = 0;

	st->dir = (SiftHybridDir) list_nth_int(ints, SIFT_HYBP_DIR);
	st->host = (SiftHybridHost) list_nth_int(ints, SIFT_HYBP_HOST);
	st->adaptive = st->dir == SIFT_HYB_ADAPTIVE;
	st->text_oid = (Oid) list_nth_int(ints, SIFT_HYBP_TEXT_INDEX);
	st->store_oid = (Oid) list_nth_int(ints, SIFT_HYBP_STORE_OID);
	st->k = list_nth_int(ints, SIFT_HYBP_K);
	st->ranked = list_nth_int(ints, SIFT_HYBP_RANKED) != 0;
	st->ties = list_nth_int(ints, SIFT_HYBP_TIES) != 0;
	st->residual = list_nth_int(ints, SIFT_HYBP_RESIDUAL) != 0;
	st->covered = list_nth_int(ints, SIFT_HYBP_COVERED) != 0;
	for (int i = 0; i < SIFT_NHYBE; i++)
		st->est[i] = DatumGetFloat8(castNode(Const, list_nth(estc, i))->constvalue);
	st->query = ExecInitExpr((Expr *) linitial(cs->custom_exprs), ps);
	st->heap = heap;
	st->heap_decide = true;
	st->scan_tlist = cs->custom_scan_tlist;
	st->text_index = index_open(st->text_oid, AccessShareLock);
	if (st->host == SIFT_HYB_HOST_TABLE)
		st->table = sift_f2_store_open(heap);
	else
		st->store_index = index_open(st->store_oid, AccessShareLock);
	st->iinfo = BuildIndexInfo(st->text_index);
	st->qual_econtext = CreateExprContext(estate);
	st->rt_econtext = CreateExprContext(estate);
	st->heapslot = table_slot_create(st->heap, NULL);
	st->cxt = sift_mem_allocset_create(estate->es_query_cxt, "sift hybrid scan");
	init_quals(st, cs);
	if (st->table != NULL)
		sift_f2_store_compile_quals(heap, st->table, st->std_quals, st->cxt, &st->table_quals);
	else
	{
		ExecIndexBuildScanKeys(ps, st->store_index, st->std_quals, false, &st->keys, &st->nkeys, &st->rtkeys, &st->nrt, &arrkeys, &narr);
		SIFT_INVARIANT(narr == 0, "sift: the index of a hybrid scan takes an array qual as one key");
		ExecIndexBuildScanKeys(ps, st->store_index, st->hint_quals, false, &st->hint_keys, &st->nhint, &st->hint_rtkeys, &st->nhrt, &arrkeys, &narr);
		SIFT_INVARIANT(narr == 0, "sift: the index of a hybrid scan takes an array qual as one key");
	}
	st->store = (SiftHybridStore) {.host = st->host,.hot_chains = st->table == NULL,.exact_quals = true,.arg = st,.scan_quals = store_scan_quals,.scan_bridge = store_scan_bridge};
}

void
sift_hybrid_close(HybridState *st)
{
	ExecDropSingleTupleTableSlot(st->heapslot);
	index_close(st->text_index, AccessShareLock);
	if (st->store_index != NULL)
		index_close(st->store_index, AccessShareLock);
}

bool
sift_hybrid_bind(HybridState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext old = MemoryContextSwitchTo(st->cxt);
	bool		isnull;
	Datum		d;
	text	   *q;

	st->started = true;
	PredicateLockRelation(st->heap, estate->es_snapshot);
	st->empty = !eval_quals(st);
	d = ExecEvalExprSwitchContext(st->query, st->rt_econtext, &isnull);
	if (st->empty || isnull)
	{
		st->empty = true;
		MemoryContextSwitchTo(old);
		return false;
	}
	q = DatumGetTextPP(d);
	st->chunk_cxt = sift_mem_allocset_create(st->cxt, "sift hybrid chunk");
	st->ist = sift_state_open(st->text_index, st->cxt);
	st->bound_query = sift_state_query(st->ist, VARDATA_ANY(q), VARSIZE_ANY_EXHDR(q));
	st->plan = sift_plan_bind(st->ist, &st->bound_query, 1, SIFT_BIND_SCORE);
	take_hints(st);
	MemoryContextSwitchTo(old);
	return true;
}

void
sift_hybrid_finish(HybridState *st)
{
	if (!st->started)
		return;
	if (st->rank)
		sift_rank_end(st->rank);
	if (st->vis_open)
		sift_vis_end(&st->vis);
	if (st->ist)
		sift_state_close(st->ist);
	if (st->matches.it)
		sift_iter_end(st->matches.it);
	st->matches = (SiftMatchKeys) {0};
	if (st->or_keys)
		BufFileClose(st->or_keys);
	st->or_keys = NULL;
	if (st->run)
	{
		st->stats = st->run->stats;
		st->stats.granules_skipped = st->granules_skipped;
		sift_hybrid_end(st->run);
	}
	if (BufferIsValid(st->vmbuf))
		ReleaseBuffer(st->vmbuf);
	st->vmbuf = InvalidBuffer;
	st->rank = NULL;
	st->run = NULL;
	st->ist = NULL;
	st->vis_open = false;
	st->started = false;
}
