/*
 * bm25/doctext.c - the sorted docs of a promotion or a merge, and the text of the rows they stand for
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"
#include "utils/timestamp.h"

#include "bm25/analyze/analyze.h"
#include "bm25/doc.h"
#include "bm25/build/build.h"
#include "core/compat.h"
#include "core/lane.h"
#include "core/maint/pacer.h"
#include "core/quarantine/quarantine.h"
#include "core/maint/maint.h"
#include "core/maint/int.h"
#include "bm25/merge/merge.h"
#include "bm25/wbuf.h"
#include "core/varint.h"
#include "core/visibility/visibility.h"
#include "core/memtrack.h"
#include "table/dispatch.h"
#include "bm25/doctext.h"

#define DOCTEXT_SNAPSHOT_DOCS	256
#define DOCTEXT_WINDOW			16384
#define DOCTEXT_WINDOW_FIRST	256

/* The next doc by key; a repeated tid keeps its first record */
bool
sift_doc_next(DocCursor *c)
{
	Datum		val;
	bool		isnull;

	for (;;)
	{
		const char *p;
		const char *text;
		uint64		key;

		if (!tuplesort_getdatum(c->sort, true, false, &val, &isnull, NULL))
			return false;
		p = VARDATA_ANY(DatumGetPointer(val));
		memcpy(&key, p, sizeof(key));
		key = pg_ntoh64(key);
		if (c->any && key == c->key)
			continue;
		c->any = true;
		c->key = key;
		p += sizeof(key) + sizeof(uint32);
		memcpy(&c->dl, p, sizeof(c->dl));
		memcpy(&c->nterms, p += sizeof(c->dl), sizeof(c->nterms));
		p += sizeof(c->nterms);
		if (c->nterms > c->maxterms)
			c->terms = repalloc_huge(c->terms, (c->maxterms = c->nterms) * sizeof(SiftTermFreq));
		text = p + c->nterms * PACK_TERM;
		for (uint32 i = 0; i < c->nterms; i++, p += PACK_TERM)
		{
			uint32		hash;

			memcpy(&hash, p + 2, sizeof(hash));
			c->terms[i] = (SiftTermFreq) {text, (uint8) p[0], (uint8) p[1], hash};
			text += (uint8) p[0];
		}
		return true;
	}
}


/*
 * The text of the docs of a promotion. The write buffer keeps the terms of a doc and their counts, not where in the text they are, and
 * a segment of an index with positions needs the position records of its terms: a merge keeps them only when every segment it merges has
 * them, so one segment without would take them from all.
 */
DocText *
sift_doctext_open(Relation index)
{
	SiftAnalyzerConfig cfg;
	DocText    *d;

	sift_analyzer_config_from_index(index, &cfg);
	if (!sift_index_options(index)->positions || cfg.keyword)
		return NULL;
#ifdef SIFT_TESTING
	if (sift_test_no_heap_positions)
		return NULL;
#endif
	d = palloc0(sizeof(DocText));
	SIFT_TEST_GATE(sift_test_worker_gate && MyBackendType == B_BG_WORKER, 7);
	d->index = index;
	d->heap = table_open(index->rd_index->indrelid, AccessShareLock);
	d->info = BuildIndexInfo(index);
	d->estate = CreateExecutorState();
	d->slot = table_slot_create(d->heap, NULL);
	d->fetch = sift_table_index_fetch_begin(d->heap);
	d->an = sift_analyzer_create(&cfg, CurrentMemoryContext);
	sift_analyzer_set_positions(d->an, true);
	sift_bm25_doc_keys(index, &d->keys, CurrentMemoryContext);
	d->nblocks = sift_vis_nblocks(d->heap);
	if (index->rd_index->indnkeyatts == 1 && sift_tbl_is_table(d->heap) && ((SiftTblFetchHeader *) d->fetch)->ops->fetch_order != NULL)
	{
		d->wcx = sift_mem_allocset_create(CurrentMemoryContext, "sift doctext window");
		d->wkey = palloc(DOCTEXT_WINDOW * sizeof(SiftKey));
		d->wtext = palloc(DOCTEXT_WINDOW * sizeof(text *));
		d->wmax = DOCTEXT_WINDOW_FIRST;
	}
	return d;
}

/* A snapshot for each DOCTEXT_SNAPSHOT_DOCS docs, not one for the whole promotion: what a promotion of a large buffer holds back is the xmin of its latest batch */
static void
doctext_snapshot(DocText *d)
{
	if (d->batch_left > 0)
	{
		d->batch_left--;
		return;
	}
	d->batch_left = DOCTEXT_SNAPSHOT_DOCS - 1;
	if (d->snapshot)
		PopActiveSnapshot();
	PushActiveSnapshot(GetTransactionSnapshot());
	d->snapshot = true;
}

void
sift_doctext_close(DocText *d)
{
	if (d == NULL)
		return;
	if (d->snapshot)
		PopActiveSnapshot();
	table_index_fetch_end(d->fetch);
	if (d->wcx != NULL)
		MemoryContextDelete(d->wcx);
	ExecDropSingleTupleTableSlot(d->slot);
	FreeExecutorState(d->estate);
	table_close(d->heap, AccessShareLock);
}

/* The tuple a key stands for, in d->slot, through the table access method (a sift_table has no heap pages to read); false when it is gone (dead for every snapshot) */
static bool
doctext_fetch(DocText *d, const ItemPointerData *tid)
{
	ItemPointerData t = *tid;
	bool		again = false,
				all_dead = false;

	if (ItemPointerGetBlockNumber(tid) >= d->nblocks)
		return false;
	return table_index_fetch_tuple(d->fetch, &t, SnapshotAny, d->slot, &again, &all_dead);
}

/* The text of the doc at tid, detoasted, valid until the next call; NULL when its row or its text is gone */
static text *
doctext_read(DocText *d, const ItemPointerData *tid)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	text	   *t;
	MemoryContext old;

	doctext_snapshot(d);
	if (!doctext_fetch(d, tid))
		return NULL;
	ResetPerTupleExprContext(d->estate);
	GetPerTupleExprContext(d->estate)->ecxt_scantuple = d->slot;
	FormIndexDatum(d->info, d->slot, d->estate, values, isnull);
	old = MemoryContextSwitchTo(GetPerTupleMemoryContext(d->estate));
	t = isnull[0] ? NULL : DatumGetTextPP(values[0]);
	MemoryContextSwitchTo(old);
	return t;
}

typedef struct DocLoc
{
	uint64		loc;
	uint32		doc;
} DocLoc;

static int
docloc_cmp(const void *a, const void *b)
{
	const DocLoc *x = a,
			   *y = b;

	return x->loc != y->loc ? (x->loc > y->loc) - (x->loc < y->loc) : (x->doc > y->doc) - (x->doc < y->doc);
}

/*
 * The texts of the next docs of the cursor, read in the order the rows lie in the table and kept for the docs in their own order: a table whose rows are laid out by a key and not by their
 * address (a sift_table) reads a granule for each doc when the docs come by address, and each granule once when they come by where the rows are.
 */
void
sift_doctext_prime(DocText *d, DocCursor *c)
{
	const SiftKey key = c->key;
	const bool	any = c->any;
	const Size	limit = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1) / 4;
	Size		bytes = 0;
	DocLoc	   *order;
	uint32		n = 0;

	MemoryContextReset(d->wcx);
	tuplesort_markpos(c->sort);
	while (n < d->wmax && sift_doc_next(c))
		d->wkey[n++] = c->key;
	tuplesort_restorepos(c->sort);
	c->key = key;
	c->any = any;
	d->nwin = n;
	d->wpos = 0;
	order = MemoryContextAlloc(d->wcx, Max(n, 1) * sizeof(DocLoc));
	for (uint32 i = 0; i < n; i++)
	{
		ItemPointerData tid;

		sift_key_to_tid(d->wkey[i], &tid);
		order[i] = (DocLoc) {((SiftTblFetchHeader *) d->fetch)->ops->fetch_order(d->fetch, &tid), i};
	}
	qsort(order, n, sizeof(DocLoc), docloc_cmp);
	for (uint32 i = 0; i < n; i++)
	{
		ItemPointerData tid;
		text	   *t;

		CHECK_FOR_INTERRUPTS();
		sift_key_to_tid(d->wkey[order[i].doc], &tid);
		t = doctext_read(d, &tid);
		d->wtext[order[i].doc] = t == NULL ? NULL : MemoryContextAlloc(d->wcx, VARSIZE_ANY(t));
		if (t != NULL)
		{
			memcpy(d->wtext[order[i].doc], t, VARSIZE_ANY(t));
			bytes += VARSIZE_ANY(t);
		}
	}
	d->wmax = bytes > limit ? Max(DOCTEXT_WINDOW_FIRST / 4, d->wmax / 2) : bytes < limit / 2 ? Min(DOCTEXT_WINDOW, d->wmax * 2) : d->wmax;
}

/* The terms of the doc at tid with their position records, as a build analyzes it; false when its row or its text is gone */
bool
sift_doctext_terms(DocText *d, const ItemPointerData *tid, SiftTermFreq **terms, int *nterms, uint32 *dl, int *len)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	text	   *t;
	MemoryContext old;
	bool		found;

	if (d->wcx != NULL)
	{
		t = d->wtext[d->wpos++];
		if (t == NULL)
			return false;
		*len = VARSIZE_ANY_EXHDR(t);
		*nterms = sift_analyze_doc(d->an, VARDATA_ANY(t), *len, terms, dl);
		return true;
	}
	doctext_snapshot(d);
	if (!doctext_fetch(d, tid))
		return false;
	ResetPerTupleExprContext(d->estate);
	GetPerTupleExprContext(d->estate)->ecxt_scantuple = d->slot;
	FormIndexDatum(d->info, d->slot, d->estate, values, isnull);
	old = MemoryContextSwitchTo(GetPerTupleMemoryContext(d->estate));
	found = sift_bm25_row_terms(d->an, &d->keys, values, isnull, terms, nterms, dl, len);
	MemoryContextSwitchTo(old);
	return found;
}
