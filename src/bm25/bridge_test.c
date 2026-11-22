/*
 * bm25/bridge_test.c - test-only SQL function sift._test_bridge_search, compiled only with SIFT_TESTING and registered by the pg_sift_test extension: the ranked
 * search of one query restricted to a set of TIDs as a core/tidbridge.h bridge, to be compared with the same query and a filter on the TIDs.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/genam.h"
#include "access/relation.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "bm25/eval.h"
#include "core/catname.h"
#include "core/tidbridge.h"
#include "core/memtrack.h"

PG_FUNCTION_INFO_V1(sift_test_bridge_search);

/* The caller's own verification: a TID of the set (a lossy page of the bridge lets others through) */
typedef struct TidSet
{
	ItemPointerData *tid;
	int			n;
} TidSet;

static int
tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

static bool
visible(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	const TidSet *set = arg;

	return bsearch(tid, set->tid, set->n, sizeof(ItemPointerData), tid_cmp) != NULL;
}

/*
 * (index regclass, query text, tids tid[], k int8, budget_kb int4) returns setof (ctid tid, score float4): the bridge holds the TIDs (a lossy page when budget_kb
 * is small: the byte budget of the bridge), the search takes the k best of them, 0 for all
 */
Datum
sift_test_bridge_search(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	text	   *q = PG_GETARG_TEXT_PP(1);
	ArrayType  *tids = PG_GETARG_ARRAYTYPE_P(2);
	int64		k = PG_GETARG_INT64(3);
	Size		budget = (Size) PG_GETARG_INT32(4) * 1024;
	Datum	   *elems;
	int			n;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift bridge test");
	MemoryContext old;
	SiftIndexState *st;
	SiftQuery  *query;
	SiftPlan   *plan;
	SiftRanker *rank;
	SiftTidBridge *br;
	TidSet		set;
	ItemPointerData tid;
	float4		score;
	int64		nret = 0;

	InitMaterializedSRF(fcinfo, 0);
	deconstruct_array(tids, TIDOID, sizeof(ItemPointerData), false, TYPALIGN_SHORT, &elems, NULL, &n);
	old = MemoryContextSwitchTo(cx);
	set.n = n;
	set.tid = palloc(Max(n, 1) * sizeof(ItemPointerData));
	br = sift_tidbridge_create(cx, budget, SIFT_TBR_EXACT_QUALS | SIFT_TBR_EXACT_VIS);
	for (int i = 0; i < n; i++)
	{
		ItemPointer t = DatumGetItemPointer(elems[i]);

		set.tid[i] = *t;
		sift_tidbridge_add_tid(br, (SiftKey) ItemPointerGetBlockNumber(t) << 16 | ItemPointerGetOffsetNumber(t));
	}
	sift_tidbridge_finalize(br);
	qsort(set.tid, n, sizeof(ItemPointerData), tid_cmp);
	st = sift_state_open(index, cx);
	query = sift_state_query(st, VARDATA_ANY(q), VARSIZE_ANY_EXHDR(q));
	plan = sift_plan_bind(st, &query, 1, SIFT_BIND_SCORE);
	sift_plan_set_tidbridge(plan, br);
	rank = sift_rank_begin(plan, k, visible, &set);
	while ((k <= 0 || nret++ < k) && sift_rank_next(rank, &tid, &score))
	{
		Datum		values[2] = {ItemPointerGetDatum(&tid), Float4GetDatum(score)};
		bool		nulls[2] = {false, false};

		MemoryContextSwitchTo(old);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
		MemoryContextSwitchTo(cx);
	}
	sift_rank_end(rank);
	sift_state_close(st);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

#endif
