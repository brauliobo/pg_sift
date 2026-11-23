/*
 * bm25/step.c - the maintenance steps of a BM25 index: the claims on the segments a step replaces, a step run in a
 * subtransaction, the promotion of a sealed write buffer and the tiered merge.
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

#include "bm25/analyze.h"
#include "bm25/bm25_doc.h"
#include "bm25/build.h"
#include "core/compat.h"
#include "core/lane.h"
#include "core/maint_pacer.h"
#include "core/quarantine.h"
#include "core/maint.h"
#include "core/maint_int.h"
#include "bm25/merge.h"
#include "bm25/wbuf.h"
#include "core/varint.h"
#include "core/visibility.h"
#include "core/memtrack.h"
#include "table/dispatch.h"

#define CLAIM_TRIES			8		/* picks of a step whose segments others replaced meanwhile */

bool
claim(Relation index, uint64 gen, bool wait)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, RelationGetRelid(index), (uint32) gen, (uint16) (gen >> 32));
	return LockAcquire(&tag, ExclusiveLock, false, !wait) != LOCKACQUIRE_NOT_AVAIL;
}

void
unclaim(Relation index, uint64 gen)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, RelationGetRelid(index), (uint32) gen, (uint16) (gen >> 32));
	LockRelease(&tag, ExclusiveLock, false);
}

/* Whether the directory still has the segment, as a segment of this kind */
static bool
has_segment(const SiftDirSnapshot *snap, uint64 gen, SiftSegKind kind)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].gen == gen)
			return snap->segs[i].kind == kind;
	return false;
}

static SiftScoreRef
score_ref(Relation index)
{
	const SiftOptions *opts = sift_index_options(index);

	return (SiftScoreRef) {(float4) opts->k1, (float4) opts->b, 0};
}

/* Counts the failure for the backoff and keeps what sift.workers shows of it */
static void
stamp_failure(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const ErrorData *err = arg;
	uint32		failures = meta->flags & SIFT_META_FAILURES_MASK;
	TimestampTz now = GetCurrentTimestamp();

	meta->flags = (meta->flags & ~SIFT_META_FAILURES_MASK) | Min(failures + 1, SIFT_META_FAILURES_MASK);
	meta->failures_total = Min(meta->failures_total + 1, PG_UINT32_MAX - 1);
	meta->maint_requested_at = meta->maint_failed_at = now;
	meta->maint_errcode = err->sqlerrcode;
	memset(meta->maint_errmsg, 0, sizeof(meta->maint_errmsg));
	memcpy(meta->maint_errmsg, err->message, pg_mbcliplen(err->message, strlen(err->message), sizeof(meta->maint_errmsg) - 1));
}

/* The WARNING of a maintenance step that failed and is retried: kind is "sift" or "sift_col", index the open index for the hint (NULL: it is gone) */
void
sift_maint_warn_failure(const char *kind, const char *name, Relation index, ErrorData *err)
{
	ereport(WARNING,
			(errcode(err->sqlerrcode),
			 errmsg("%s index \"%s\": maintenance failed: %s", kind, name, err->message),
			 errdetail("It is retried later, after a growing delay."),
			 index ? errhint("Inspect SELECT * FROM sift.health(%s); or retry with SELECT sift.merge(%s);",
							 quote_literal_cstr(sift_index_ident(index)), quote_literal_cstr(sift_index_ident(index))) : 0));
}

/* What a worker that failed says, from a transaction of its own (the one that failed is gone), with the index open for the hint of the warning; the index may be gone as well */
static void
warn_worker_failure(const char *kind, Oid indexoid, const char *name, ErrorData *err)
{
	Relation	index;

	StartTransactionCommand();
	index = sift_try_open_index(indexoid, AccessShareLock);
	if (sift_damage_caught(err))
		ereport(WARNING,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s index \"%s\" is quarantined; maintenance stopped", kind, name),
				 index ? errhint("Queries and maintenance run without it. Rebuild online: REINDEX INDEX CONCURRENTLY %s;", sift_index_ident(index)) : 0));
	else
		sift_maint_warn_failure(kind, name, index, err);
	if (index != NULL)
		index_close(index, AccessShareLock);
	CommitTransactionCommand();
}

/* A background worker whose transaction failed (sift_col steps run outside a subtransaction): its own transaction keeps the failure like run_step does */
void
sift_maint_record_failure(Oid indexoid, ErrorData *err)
{
	Relation	index;

	StartTransactionCommand();
	index = sift_try_open_index(indexoid, RowExclusiveLock);
	if (index != NULL)
	{
		sift_dir_update(index, &(SiftDirChange) {.fn = stamp_failure,.arg = err,.extra = InvalidBuffer});
		index_close(index, RowExclusiveLock);
	}
	CommitTransactionCommand();
}

/*
 * A worker whose transaction failed (called from its PG_CATCH, in the memory context it was running in): the error is kept where sift.workers and the backoff of the requests find it,
 * warned as a failed BM25 step is, and the worker goes on or ends as its caller says. Cancellation and shutdown go on as they were.
 */
void
sift_maint_worker_failed(const char *kind, Oid indexoid, const char *name, MemoryContext cx)
{
	ErrorData  *err;

	MemoryContextSwitchTo(cx);
	err = CopyErrorData();
	FlushErrorState();
	AbortCurrentTransaction();
	if (ERRCODE_TO_CATEGORY(err->sqlerrcode) == ERRCODE_OPERATOR_INTERVENTION)
		ReThrowError(err);
	warn_worker_failure(kind, indexoid, name, err);
	if (!sift_damage_caught(err))
		sift_maint_record_failure(indexoid, err);
	FreeErrorData(err);
}

/*
 * Runs fn in a subtransaction, so an ERROR rolls back every buffer lock and pin it held and leaves
 * the blocks fn took in wt unreferenced, which are then returned to the free list. propagate: the
 * ERROR goes on to the caller (an explicit sift.promote() or sift.merge()); otherwise it is logged
 * as a WARNING, counted for the backoff and the step reports SIFT_STEP_FAILED. Cancellation and shutdown
 * always go on. Frees recycled extents first, so the step finds contiguous space.
 */
SiftStepResult
run_step(Relation index, bool propagate, StepFn fn, const void *arg)
{
	MemoryContext oldcx = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	SiftWriteTarget *wt;
	ErrorData  *err = NULL;

	sift_recycle(index);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		SIFT_TEST_GATE(sift_test_step_gate && MyBackendType == B_BG_WORKER, 17);
		fn(index, wt, arg);
		ReleaseCurrentSubTransaction();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcx);
		err = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
	}
	PG_END_TRY();
	MemoryContextSwitchTo(oldcx);
	CurrentResourceOwner = oldowner;
	if (err == NULL)
		return SIFT_STEP_DONE;
	sift_wt_abort(wt);
	if (propagate || ERRCODE_TO_CATEGORY(err->sqlerrcode) == ERRCODE_OPERATOR_INTERVENTION)
		ReThrowError(err);
	if (sift_damage_caught(err))
	{
		sift_quarantine_stopped(index, SIFT_SKIP_VACUUM);
		FreeErrorData(err);
		return SIFT_STEP_FAILED;
	}
	sift_maint_warn_failure("sift", RelationGetRelationName(index), index, err);
	sift_dir_update(index, &(SiftDirChange) {.fn = stamp_failure,.arg = err,.extra = InvalidBuffer});
	FreeErrorData(err);
	return SIFT_STEP_FAILED;
}

/*
 * The live docs of a write buffer go through a tuplesort, so that promoting a segment of any size stays within
 * maintenance_work_mem (the excess spills to temporary files). A doc is packed as a bytea: its key and arrival number
 * first, big-endian, so that byte order is key order and the first record of a repeated tid comes first, then its
 * length, its terms' lengths, frequencies and hashes, then the bytes of the terms.
 */
#define PACK_HEAD	(VARHDRSZ + sizeof(uint64) + sizeof(uint32) + sizeof(uint16) + sizeof(uint32))
#define PACK_TERM	(2 + sizeof(uint32))

typedef struct DocPacker
{
	char	   *buf;
	Size		cap;
} DocPacker;

static bytea *
pack_doc(DocPacker *pk, SiftWbufDoc *d, uint32 seq)
{
	Size		bytes = 0,
				size;
	uint64		key = pg_hton64(sift_key_from_tid(&d->tid));
	char	   *p;

	for (uint32 i = 0; i < d->nterms; i++)
		bytes += d->len[i];
	size = PACK_HEAD + d->nterms * PACK_TERM + bytes;
	if (size > pk->cap)
		pk->buf = repalloc(pk->buf, pk->cap = size * 2);
	seq = pg_hton32(seq);
	p = pk->buf + VARHDRSZ;
	memcpy(p, &key, sizeof(key));
	memcpy(p += sizeof(key), &seq, sizeof(seq));
	memcpy(p += sizeof(seq), &d->dl, sizeof(d->dl));
	memcpy(p += sizeof(d->dl), &d->nterms, sizeof(d->nterms));
	p += sizeof(d->nterms);
	for (uint32 i = 0; i < d->nterms; i++)
	{
		*p++ = d->len[i];
		*p++ = d->tf[i];
		memcpy(p, &d->hash[i], sizeof(uint32));
		p += sizeof(uint32);
	}
	memcpy(p, d->terms, bytes);
	SET_VARSIZE(pk->buf, size);
	return (bytea *) pk->buf;
}

/* Sorts the live docs of a write buffer by key */
static Tuplesortstate *
sort_docs(Relation index, const SiftSegEntry *seg)
{
	SiftWbufReader *r = sift_wbuf_open(index, seg, CurrentMemoryContext, NULL, NULL);
	Tuplesortstate *sort = tuplesort_begin_datum(BYTEAOID, ByteaLessOperator, InvalidOid, false, maintenance_work_mem / 2,
												 NULL, TUPLESORT_RANDOMACCESS);
	DocPacker	pk = {palloc(1024), 1024};
	SiftWbufDoc	d;
	uint32		seq = 0;

	while (sift_wbuf_next(r, &d))
		if (!d.deleted)
			tuplesort_putdatum(sort, PointerGetDatum(pack_doc(&pk, &d, seq++)), false);
	sift_wbuf_close(r);
	pfree(pk.buf);
	tuplesort_performsort(sort);
	return sort;
}

typedef struct DocCursor
{
	Tuplesortstate *sort;
	SiftKey		key;			/* of the doc last returned */
	uint16		dl;
	uint32		nterms;
	SiftTermFreq *terms;		/* point into the sorted doc, valid until the next call */
	uint32		maxterms;
	bool		any;
} DocCursor;

/* The next doc by key; a repeated tid keeps its first record */
static bool
next_doc(DocCursor *c)
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
			c->terms = repalloc(c->terms, (c->maxterms = c->nterms) * sizeof(SiftTermFreq));
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

/* The memory the inverted buffer may take: what the sort left of maintenance_work_mem when it kept its docs in memory, the half it does not take when it spilled (the sort has half of it) */
static Size
inv_budget(Tuplesortstate *sort)
{
	TuplesortInstrumentation st;
	Size		budget = (Size) maintenance_work_mem * 1024;

	tuplesort_get_stats(sort, &st);
	if (st.sortMethod == SORT_TYPE_QUICKSORT)
		budget -= Min(budget - budget / 4, (Size) st.spaceUsed * 1024);
	else
		budget /= 2;
	return budget;
}

/*
 * The text of the docs of a promotion. The write buffer keeps the terms of a doc and their counts, not where in the text they are, and
 * a segment of an index with positions needs the position records of its terms: a merge keeps them only when every segment it merges has
 * them, so one segment without would take them from all.
 */
#define DOCTEXT_SNAPSHOT_DOCS	256
#define DOCTEXT_WINDOW			16384
#define DOCTEXT_WINDOW_FIRST	256

typedef struct DocText
{
	bool		snapshot;		/* a snapshot is pushed: the text of the docs of a batch may be out of line, and a worker has none */
	uint32		batch_left;		/* the docs the snapshot serves still */
	Relation	index;
	Relation	heap;
	IndexInfo  *info;
	EState	   *estate;
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;
	SiftAnalyzer *an;
	BlockNumber nblocks;
	/* a table whose rows lie in an order of their own: the texts of the next docs read in that order (doctext_prime), and handed out in the order of the docs */
	MemoryContext wcx;
	SiftKey    *wkey;
	text	  **wtext;
	uint32		nwin;
	uint32		wpos;
	uint32		wmax;			/* the docs of the next window: it doubles while the texts of a window take less than half of what they may, and halves when they take more */
} DocText;

static DocText *
doctext_open(Relation index)
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
	d->nblocks = RelationGetNumberOfBlocks(d->heap);
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

static void
doctext_close(DocText *d)
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

	doctext_snapshot(d);
	if (!doctext_fetch(d, tid))
		return NULL;
	ResetPerTupleExprContext(d->estate);
	GetPerTupleExprContext(d->estate)->ecxt_scantuple = d->slot;
	FormIndexDatum(d->info, d->slot, d->estate, values, isnull);
	return isnull[0] ? NULL : DatumGetTextPP(values[0]);
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
static void
doctext_prime(DocText *d, DocCursor *c)
{
	const SiftKey key = c->key;
	const bool	any = c->any;
	const Size	limit = (Size) maintenance_work_mem * 1024 / 4;
	Size		bytes = 0;
	DocLoc	   *order;
	uint32		n = 0;

	MemoryContextReset(d->wcx);
	tuplesort_markpos(c->sort);
	while (n < d->wmax && next_doc(c))
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
static bool
doctext_terms(DocText *d, const ItemPointerData *tid, SiftTermFreq **terms, int *nterms, uint32 *dl, int *len)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	text	   *t;

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
	return sift_bm25_row_terms(d->an, d->index, values, isnull, terms, nterms, dl, len);
}

typedef struct RunList
{
	SiftRun   **run;
	int			n;
	int			max;
} RunList;

static void
runlist_push(RunList *l, SiftRun *r)
{
	if (l->n == l->max)
		l->run = repalloc(l->run, (l->max *= 2) * sizeof(SiftRun *));
	l->run[l->n++] = r;
}

/* Adds a doc to the buffer, which spills to a run of the list when the doc does not fit or the buffer is full */
static void
inv_add_doc(SiftInvBuf *inv, RunList *runs, DocText *dt, ItemPointerData *tid, SiftTermFreq *terms, int nterms, uint32 dl, int len)
{
	if (dt != NULL && len >= sift_inv_small_doc(inv) && !sift_inv_empty(inv) && !sift_inv_fits(inv, nterms, len))
		runlist_push(runs, sift_inv_spill(inv));
	sift_inv_add(inv, tid, terms, nterms, dl);
	if (dt != NULL)
		sift_analyzer_reset(dt->an);
	if (sift_inv_full(inv))
		runlist_push(runs, sift_inv_spill(inv));
}

/* Writes the sorted docs as one immutable segment, but for those whose row is gone (dead for every snapshot); false when there are none */
static bool
write_docs(Relation index, SiftWriteTarget *wt, Tuplesortstate *sort, uint64 gen, SiftSegEntry *entry)
{
	SiftScoreRef ref = score_ref(index);
	DocText    *dt = doctext_open(index);
	SiftInvBuf *inv = sift_inv_create(inv_budget(sort), CurrentMemoryContext, dt != NULL);
	RunList		runs = {.run = palloc(sizeof(SiftRun *)),.max = 1};
	DocCursor	c = {.sort = sort,.terms = palloc(sizeof(SiftTermFreq)),.maxterms = 1};

	for (;;)
	{
		ItemPointerData tid;
		SiftTermFreq *terms;
		int			nterms,
					len = 0;
		uint32		dl;

		if (dt != NULL && dt->wcx != NULL && dt->wpos == dt->nwin)
			doctext_prime(dt, &c);
		if (!next_doc(&c))
			break;
		CHECK_FOR_INTERRUPTS();
		terms = c.terms;
		nterms = c.nterms;
		dl = c.dl;
		sift_key_to_tid(c.key, &tid);
		if (dt != NULL && !doctext_terms(dt, &tid, &terms, &nterms, &dl, &len))
			continue;
		inv_add_doc(inv, &runs, dt, &tid, terms, nterms, dl, len);
	}
	doctext_close(dt);
	if (runs.n == 0 && sift_inv_empty(inv))
		return false;
	if (runs.n == 0)
		sift_inv_write(inv, wt, gen, &ref, entry);
	else
	{
		if (!sift_inv_empty(inv))
			runlist_push(&runs, sift_inv_spill(inv));
		sift_runs_write(runs.run, runs.n, wt, gen, &ref, entry);
	}
	return true;
}

static uint64
extents_bytes(const SiftExtent *ext, int n)
{
	uint64		blocks = 0;

	for (int i = 0; i < n; i++)
		blocks += ext[i].nblocks;
	return blocks * BLCKSZ;
}

static void
promote_one(Relation index, SiftWriteTarget *wt, const void *arg)
{
	const SiftSegEntry *sealed = arg;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift promote");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftMemTracker mt_bm25_promote;

	sift_mem_init_sub(&mt_bm25_promote, "bm25_promote", cx, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	Tuplesortstate *sort;
	SiftSegEntry neu;
	bool		any;
	SiftExtent *pages;
	int			npages;
	TimestampTz start = GetCurrentTimestamp();

	sift_lane_flag_sealed(index, sealed);
	pages = sift_lane_extents(index, sealed, &npages);
	sift_wt_expect(wt, extents_bytes(pages, npages));
	sort = sort_docs(index, sealed);
	sift_mem_assert_within(&mt_bm25_promote);
	any = write_docs(index, wt, sort, sift_store_new_gen(index), &neu);
	sift_mem_assert_within(&mt_bm25_promote);
	tuplesort_end(sort);
	sift_wt_finish(wt);
	sift_maint_replace(index, wt, &sealed->gen, 1, any ? &neu : NULL, pages, npages, "promoted a sealed segment of", start, sealed->ndocs);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* Re-reads the directory, as the segments claimed from an older snapshot may have been replaced meanwhile */
static bool
still_there(Relation index, uint64 gen, SiftSegKind kind)
{
	SiftDirSnapshot *snap;
	bool		found;

	snap = sift_dir_read(index);
	found = has_segment(snap, gen, kind);
	pfree(snap);
	return found;
}

/* Promotes the oldest sealed segment nobody else is at; busy: the generation of one that is */
SiftStepResult
promote_next(Relation index, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap;
	SiftStepResult	r = SIFT_STEP_IDLE;

	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs && r == SIFT_STEP_IDLE; i++)
	{
		SiftSegEntry s = snap->segs[i];

		if (s.kind != SIFT_SEG_WBUF_SEALED)
			continue;
		if (!claim(index, s.gen, false))
			*busy = s.gen;
		else
		{
			if (still_there(index, s.gen, SIFT_SEG_WBUF_SEALED))
				r = run_step(index, propagate, promote_one, &s);
			unclaim(index, s.gen);
		}
	}
	pfree(snap);
	return r;
}

typedef struct MergeSel
{
	int			n;
	int			cap;			/* the entries seg and busy have room for: those of the directory last read */
	SiftSegEntry *seg;
	int			nbusy;			/* generations others claimed: the policy leaves them out */
	uint64	   *busy;
} MergeSel;

static void
merge_sel_room(MergeSel *sel, int nsegs)
{
	if (nsegs <= sel->cap)
		return;
	sel->cap = Max(nsegs, 2 * sel->cap);
	sel->seg = sel->seg ? repalloc(sel->seg, sel->cap * sizeof(SiftSegEntry)) : palloc(sel->cap * sizeof(SiftSegEntry));
	sel->busy = sel->busy ? repalloc(sel->busy, sel->cap * sizeof(uint64)) : palloc(sel->cap * sizeof(uint64));
}

static bool
is_busy(const MergeSel *sel, uint64 gen)
{
	for (int i = 0; i < sel->nbusy; i++)
		if (sel->busy[i] == gen)
			return true;
	return false;
}

/*
 * The segments to merge, by the policy shared with the other hosts (bm25/merge.h); tiers start at the write buffer limit.
 * dead_only: just a segment mostly dead, as an index with no entries over vacuum_segments has no merge for the count to make.
 */
static int
pick_policy(Relation index, const SiftDirSnapshot *snap, bool force, bool dead_only, MergeSel *msel)
{
	SiftMergePolicy pol = {.tier_base = (uint64) sift_index_options(index)->write_buffer_limit * 1024, .fanin = sift_index_options(index)->merge_fanin,
						   .dead_pct = sift_index_options(index)->dead_rewrite_pct};
	SiftDirSnapshot *view = NULL;
	int			n;

	if (msel->nbusy > 0)
	{
		/* a segment another step claimed is still an entry of the directory but is not offered */
		view = sift_dir_copy(snap);
		for (int i = 0; i < view->nsegs; i++)
			if (is_busy(msel, view->segs[i].gen))
				view->segs[i].kind = SIFT_SEG_WBUF_SEALED;
	}
	if (dead_only)
		n = sift_merge_pick_dead(index, &pol, view ? view : snap, msel->seg);
	else
		n = sift_merge_pick(index, &pol, view ? view : snap, force, msel->seg);
	if (view)
		pfree(view);
	return n;
}

/*
 * The smallest segments, up to want of them, that are worth merging for the count: each after the first is at most
 * merge_fanin times the size of the ones before it, so a segment past the tier of the others stays (its rewrite would
 * cost more than the count gains: a 0.1 MB segment next to one of 11 MB would rewrite the 11 MB at every VACUUM).
 */
static int
pick_smallest(const SiftDirSnapshot *snap, int want, int fanin, MergeSel *sel)
{
	SiftSegEntry *imm = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));
	uint64		sum = 0;
	int			n = 0,
				k = 0;

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && !is_busy(sel, snap->segs[i].gen))
			imm[n++] = snap->segs[i];
	qsort(imm, n, sizeof(SiftSegEntry), sift_seg_bytes_cmp);
	for (; k < Min(n, want) && (k < 1 || imm[k].bytes <= fanin * sum); k++)
	{
		sel->seg[k] = imm[k];
		sum += imm[k].bytes;
	}
	pfree(imm);
	return k < 2 ? 0 : k;
}

/* The policy's choice (smallest first) cut to the smallest segments whose bytes fit one merge; a merge of fewer than two is none */
static int
pick_merge(Relation index, const SiftDirSnapshot *snap, bool force, SiftMergeGoal goal, MergeSel *sel)
{
	int			excess = snap->nsegs - sift_index_options(index)->vacuum_segments;
	bool		counted = goal != SIFT_GOAL_ANY && !force;
	int			n = pick_policy(index, snap, force, counted && excess <= 0, sel);
	uint64		total = 0;
	int			k = 0;

	if (counted)
	{
		/* VACUUM merges down to the target and no further, and takes the smallest segments the policy left over */
		if (goal == SIFT_GOAL_VACUUM && n > 1)
			n = Min(n, excess + 1);
		if (goal == SIFT_GOAL_VACUUM && n == 0 && excess > 0)
			n = pick_smallest(snap, excess + 1, sift_index_options(index)->merge_fanin, sel);
	}

	while (k < n && total + sel->seg[k].bytes <= merge_max_bytes())
		total += sel->seg[k++].bytes;
	return k < Min(n, 2) ? 0 : k;
}

typedef struct Heal
{
	DocText    *dt;
	SiftInvBuf *inv;
	RunList    *runs;
} Heal;

static void
heal_docs(void *arg, const SiftKey *keys, const uint16 *dl, int n)
{
	Heal	   *h = arg;

	for (int i = 0; i < n; i++)
	{
		ItemPointerData tid;
		SiftTermFreq *terms;
		int			nterms,
					len;
		uint32		doclen;

		sift_key_to_tid(keys[i], &tid);
		/* a row that is gone is dead for every snapshot: its doc is left out, as a vacuum would */
		if (doctext_terms(h->dt, &tid, &terms, &nterms, &doclen, &len))
			inv_add_doc(h->inv, h->runs, h->dt, &tid, terms, nterms, doclen, len);
	}
}

/*
 * The runs of a segment without positions (written before promotions had them), analyzed again from the heap: one segment without
 * would take the positions from every segment it merges with, and the index would keep paying the rechecks of its phrases.
 */
static void
heal_runs(DocText *dt, SiftInvBuf *inv, SiftSegment *seg, RunList *runs)
{
	Heal		h = {.dt = dt,.inv = inv,.runs = runs};

	sift_seg_stream(seg, (Size) maintenance_work_mem * 1024 / 2, &(SiftSegSink) {.docs = heal_docs}, &h);
	if (!sift_inv_empty(h.inv))
		runlist_push(runs, sift_inv_spill(h.inv));
}

static void
merge_segments(Relation index, SiftWriteTarget *wt, const void *arg)
{
	const MergeSel *in = arg;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift merge");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftMemTracker mt_bm25_merge;

	sift_mem_init_sub(&mt_bm25_merge, "bm25_merge", cx, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	SiftScoreRef ref = score_ref(index);
	RunList		runs = {.run = palloc(in->n * sizeof(SiftRun *)),.max = in->n};
	SiftMaintInputs mi = sift_maint_inputs(index, in->seg, in->n);
	int			written = -1;
	DocText    *dt = NULL;
	SiftInvBuf *inv = NULL;
	SiftSegEntry neu;
	TimestampTz start = GetCurrentTimestamp();

	sift_wt_expect(wt, mi.bytes);
	sift_wt_pace_worker(wt);
	/* a long merge of the worker, the segments claimed */
	SIFT_TEST_GATE(sift_test_merge_gate && MyBackendType == B_BG_WORKER, 4);
	for (int i = 0; i < in->n; i++)
		sift_wt_charge(wt, in->seg[i].bytes / BLCKSZ);	/* the read of each segment */
	for (int i = 0; i < in->n && dt == NULL; i++)
		if (!(in->seg[i].flags & SIFT_SEG_POSITIONS))
			dt = doctext_open(index);
	/* the segments that get their positions from the heap are merged by this process */
	if (dt == NULL)
		written = sift_merge_ranges(index, wt, in->seg, in->n, &ref, &neu);
	if (written < 0)
	{
		if (dt != NULL)
			inv = sift_inv_create((Size) maintenance_work_mem * 1024 / 2, cx, true);
		for (int i = 0; i < in->n; i++)
		{
			SiftSegment *seg = sift_seg_open(index, &in->seg[i], cx);
			SiftRun    *run;

			if (dt != NULL && !(in->seg[i].flags & SIFT_SEG_POSITIONS))
				heal_runs(dt, inv, seg, &runs);
			else if ((run = sift_run_from_segment(seg, &ref, cx)) != NULL)
				runlist_push(&runs, run);
			sift_seg_close(seg);
			sift_mem_assert_within(&mt_bm25_merge);
		}
		doctext_close(dt);
		if ((written = runs.n > 0) != 0)
			sift_runs_write(runs.run, runs.n, wt, sift_store_new_gen(index), &ref, &neu);
	}
	sift_mem_assert_within(&mt_bm25_merge);
	sift_wt_finish(wt);
	sift_maint_replace(index, wt, mi.gens, in->n, written ? &neu : NULL, mi.release, mi.nrelease,
					   psprintf("merged %d segments of", in->n), start, written ? neu.ndocs : 0);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* Claims the selected segments; false when another step has some, which the next pick then leaves out */
static bool
claim_all(Relation index, MergeSel *sel)
{
	int			nbusy = sel->nbusy;

	for (int i = 0; i < sel->n; i++)
		if (!claim(index, sel->seg[i].gen, false))
			sel->busy[sel->nbusy++] = sel->seg[i].gen;
	if (sel->nbusy == nbusy)
		return true;
	for (int i = 0; i < sel->n; i++)
		if (!is_busy(sel, sel->seg[i].gen))
			unclaim(index, sel->seg[i].gen);
	return false;
}

static bool
all_there(Relation index, const MergeSel *sel)
{
	SiftDirSnapshot *snap;
	bool		all = true;

	snap = sift_dir_read(index);
	for (int i = 0; i < sel->n; i++)
		all &= has_segment(snap, sel->seg[i].gen, SIFT_SEG_IMMUTABLE);
	pfree(snap);
	return all;
}

/* One merge of segments nobody else is at; busy: the generation of one that is, when that left nothing to merge */
SiftStepResult
merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap = NULL;
	MergeSel   *sel = palloc0(sizeof(MergeSel));
	SiftStepResult	r = SIFT_STEP_IDLE;

	for (int tries = 0; tries < CLAIM_TRIES && r == SIFT_STEP_IDLE; tries++)
	{
		if (snap != NULL)
			pfree(snap);
		snap = sift_dir_read(index);
		merge_sel_room(sel, snap->nsegs);
		if ((sel->n = pick_merge(index, snap, force, goal, sel)) == 0)
			break;
		/* a pick that met claimed segments learned them (at least one more), so it is not a try: the policy offers the rest next, and a step that leaves the claimed ones to their owners reaches a merge of its own */
		if (!claim_all(index, sel))
		{
			tries--;
			continue;
		}
		if (all_there(index, sel))
			r = run_step(index, propagate, merge_segments, sel);
		/* STOR[STOR-BM25-13]: post-merge freelist recycle + tail truncate check (bounds 2.5x peak merge high_water) */
		if (r == SIFT_STEP_DONE)
			sift_recycle(index);
		for (int i = 0; i < sel->n; i++)
			unclaim(index, sel->seg[i].gen);
	}
	if (r == SIFT_STEP_IDLE && sel->nbusy > 0)
		*busy = sel->busy[0];
	if (sel->seg)
	{
		pfree(sel->seg);
		pfree(sel->busy);
	}
	pfree(sel);
	pfree(snap);
	return r;
}
