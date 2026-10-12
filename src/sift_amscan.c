/*
 * sift_amscan.c - index scan callbacks. Only bitmap scans exist: amgettuple
 * is NULL (DESIGN.md (f) 1); ranked and ordered access go through the
 * planner's custom scan (M3).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "nodes/tidbitmap.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "sift_am.h"
#include "sift_eval.h"

typedef struct SiftScanOpaque
{
	MemoryContext cxt;			/* state, plan and iterator of one rescan */
} SiftScanOpaque;

IndexScanDesc
sift_ambeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	SiftScanOpaque *so = palloc(sizeof(SiftScanOpaque));

	so->cxt = AllocSetContextCreate(CurrentMemoryContext, "sift scan", ALLOCSET_DEFAULT_SIZES);
	scan->opaque = so;
	return scan;
}

void
sift_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	SiftScanOpaque *so = scan->opaque;

	if (keys && scan->numberOfKeys > 0)
		memcpy(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
	MemoryContextReset(so->cxt);
}

/* Adds the matches of one heap page to the bitmap; returns their number */
static int
add_page(TIDBitmap *tbm, const SiftPageMatch *pm)
{
	ItemPointerData tids[MaxHeapTuplesPerPage];
	int			n = 0;

	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		for (uint64 m = pm->offmask[wd]; m; m &= m - 1)
			ItemPointerSet(&tids[n++], pm->blk, wd * 64 + pg_rightmost_one_pos64(m) + 1);
	tbm_add_tuples(tbm, tids, n, pm->recheck);
	return n;
}

/* Analyzes the keys' queries; false when a key is NULL (the operator is strict) */
static bool
scan_queries(IndexScanDesc scan, SiftIndexState *st, SiftQuery **queries)
{
	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		key = &scan->keyData[i];
		text	   *q;

		if (key->sk_flags & SK_ISNULL)
			return false;
		q = DatumGetTextPP(key->sk_argument);
		queries[i] = sift_state_query(st, VARDATA_ANY(q), VARSIZE_ANY_EXHDR(q));
	}
	return true;
}

int64
sift_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	SiftScanOpaque *so = scan->opaque;
	MemoryContext old = MemoryContextSwitchTo(so->cxt);
	SiftIndexState *st = sift_state_open(scan->indexRelation, so->cxt);
	SiftQuery **queries = palloc(scan->numberOfKeys * sizeof(SiftQuery *));
	int64		ntids = 0;

	if (scan->instrument)
		scan->instrument->nsearches++;
	if (scan_queries(scan, st, queries))
	{
		SiftMatchIter *it = sift_iter_begin(sift_plan_bind(st, queries, scan->numberOfKeys, 0));
		SiftPageMatch pm;

		while (sift_iter_next(it, &pm))
			ntids += add_page(tbm, &pm);
		sift_iter_end(it);
	}
	sift_state_close(st);
	MemoryContextSwitchTo(old);
	MemoryContextReset(so->cxt);
	return ntids;
}

void
sift_amendscan(IndexScanDesc scan)
{
	MemoryContextDelete(((SiftScanOpaque *) scan->opaque)->cxt);
}
