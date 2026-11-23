/*
 * bm25/amscan.c - index scan callbacks. Only bitmap scans exist: amgettuple
 * is NULL; ranked and ordered access go through the
 * planner's custom scan (M3).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/table.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "bm25/am.h"
#include "bm25/eval.h"
#include "core/errctx.h"
#include "core/maint_pacer.h"
#include "core/quarantine.h"
#include "core/memtrack.h"

typedef struct SiftScanOpaque
{
	MemoryContext cxt;			/* state, plan and iterator of one rescan */
} SiftScanOpaque;

IndexScanDesc
sift_ambeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	SiftScanOpaque *so = palloc(sizeof(SiftScanOpaque));

	so->cxt = sift_mem_allocset_create(CurrentMemoryContext, "sift scan");
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
add_page(TIDBitmap *tbm, const SiftPageMatch *pm, bool recheck)
{
	ItemPointerData tids[MaxHeapTuplesPerPage];
	int			n = 0;

	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		for (uint64 m = pm->offmask[wd]; m; m &= m - 1)
			ItemPointerSet(&tids[n++], pm->blk, wd * 64 + pg_rightmost_one_pos64(m) + 1);
	tbm_add_tuples(tbm, tids, n, pm->recheck || recheck);
	return n;
}

/* The key columns of an index of several are sections of its text; a clause on one searches that section (bm25/analyze_config.c names them) */
static bool
scope_to_field(SiftQNode *n, int field)
{
	switch (n->kind)
	{
		case SQ_TERM:
		case SQ_PREFIX:
		case SQ_WILDCARD:
		case SQ_FUZZY:
			n->field = field;
			return true;
		case SQ_REGEX:
			return false;		/* a pattern over the terms of a section is not a term of the index: the whole table is the candidates */
		case SQ_AND:
		case SQ_OR:
		case SQ_NOT:
			for (int i = 0; i < n->nchild; i++)
				if (!scope_to_field(n->child[i], field))
					return false;
			return true;
		default:
			return true;		/* a phrase is its terms in any section, which the recheck of the clause narrows */
	}
}

/* The query of a clause on key column attno of an index of several, as `column @@> query` means it (no field names, a colon is text), searching the column's section */
static SiftQuery *
column_query(SiftIndexState *st, int attno, const char *q, int len)
{
	SiftAnalyzerConfig cfg;
	SiftQuery  *query;

	sift_analyzer_config_from_index(st->index, &cfg);
	sift_analyzer_config_set_synthetic(&cfg, cfg.prefix_len, "");
	query = sift_query_analyze(sift_query_parse(q, len), sift_analyzer_create(&cfg, CurrentMemoryContext));
	return scope_to_field(query->root, attno) ? query : NULL;
}

typedef enum KeyQueries
{
	KEYS_NULL,					/* a key is NULL (the operator is strict): no match */
	KEYS_ANALYZED,
	KEYS_ANY_ROW				/* a query the index cannot narrow */
} KeyQueries;

/* Analyzes the keys' queries */
static KeyQueries
scan_queries(IndexScanDesc scan, SiftIndexState *st, SiftQuery **queries)
{
	bool		columns = IndexRelationGetNumberOfKeyAttributes(scan->indexRelation) > 1;

	for (int i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		key = &scan->keyData[i];
		text	   *q;

		if (key->sk_flags & SK_ISNULL)
			return KEYS_NULL;
		q = DatumGetTextPP(key->sk_argument);
		queries[i] = columns ? column_query(st, key->sk_attno, VARDATA_ANY(q), VARSIZE_ANY_EXHDR(q))
			: sift_state_query(st, VARDATA_ANY(q), VARSIZE_ANY_EXHDR(q));
		if (queries[i] == NULL)
			return KEYS_ANY_ROW;
	}
	return KEYS_ANALYZED;
}

/* Every page of the table, to be rechecked */
static int64
add_all_pages(Relation index, TIDBitmap *tbm)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	BlockNumber nblocks = sift_vis_nblocks(heap);

	for (BlockNumber blk = 0; blk < nblocks; blk++)
		tbm_add_page(tbm, blk);
	table_close(heap, AccessShareLock);
	return nblocks;
}

int64
sift_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	SiftScanOpaque *so = scan->opaque;
	TimestampTz began = GetCurrentTimestamp();
	MemoryContext old = MemoryContextSwitchTo(so->cxt);
	SiftIndexState *st = sift_state_open(scan->indexRelation, so->cxt);
	SiftQuery **queries = palloc(scan->numberOfKeys * sizeof(SiftQuery *));
	SiftErrCtx	ctx;
	int64		ntids = 0;

	sift_quarantine_net(scan->indexRelation, st->dir->meta.flags);
	sift_errctx_push(&ctx, RelationGetRelid(scan->indexRelation));
	sift_count_search(scan);
	pgstat_count_index_scan(scan->indexRelation);
	switch (scan_queries(scan, st, queries))
	{
		case KEYS_ANALYZED:
			{
				SiftMatchIter *it = sift_iter_begin(sift_plan_bind(st, queries, scan->numberOfKeys, 0));
				SiftPageMatch pm;
				bool		columns = IndexRelationGetNumberOfKeyAttributes(scan->indexRelation) > 1;

				while (sift_iter_next(it, &pm))
					ntids += add_page(tbm, &pm, columns);
				sift_iter_end(it);
			}
			break;
		case KEYS_ANY_ROW:
			ntids = add_all_pages(scan->indexRelation, tbm);
			break;
		case KEYS_NULL:
			break;
	}
	sift_state_close(st);
	sift_errctx_pop(&ctx);
	MemoryContextSwitchTo(old);
	MemoryContextReset(so->cxt);
	sift_pacer_note_foreground(GetCurrentTimestamp() - began);
	return ntids;
}

void
sift_amendscan(IndexScanDesc scan)
{
	MemoryContextDelete(((SiftScanOpaque *) scan->opaque)->cxt);
}
