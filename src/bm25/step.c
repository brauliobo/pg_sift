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
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "commands/vacuum.h"
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
#include "bm25/doctext.h"
#include "core/compat.h"
#include "core/lane.h"
#include "core/maint_pacer.h"
#include "core/quarantine.h"
#include "core/maint.h"
#include "core/maint_int.h"
#include "core/maint_vacuum.h"
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
 * the maintenance memory (the excess spills to temporary files). A doc is packed as a bytea: its key and arrival number
 * first, big-endian, so that byte order is key order and the first record of a repeated tid comes first, then its
 * length, its terms' lengths, frequencies and hashes, then the bytes of the terms (none when the terms come from its row).
 */
#define PACK_HEAD	(VARHDRSZ + sizeof(uint64) + sizeof(uint32) + sizeof(uint16) + sizeof(uint32))

typedef struct DocPacker
{
	char	   *buf;
	Size		cap;
} DocPacker;

/* terms: the doc keeps its terms; a doc whose terms come from its row (DocText) is packed without them */
static bytea *
pack_doc(DocPacker *pk, SiftWbufDoc *d, uint32 seq, bool terms)
{
	Size		bytes = 0,
				size;
	uint32		nterms = terms ? d->nterms : 0;
	uint64		key = pg_hton64(sift_key_from_tid(&d->tid));
	char	   *p;

	for (uint32 i = 0; i < nterms; i++)
		bytes += d->len[i];
	size = PACK_HEAD + nterms * PACK_TERM + bytes;
	if (size > pk->cap)
		pk->buf = repalloc(pk->buf, pk->cap = size * 2);
	seq = pg_hton32(seq);
	p = pk->buf + VARHDRSZ;
	memcpy(p, &key, sizeof(key));
	memcpy(p += sizeof(key), &seq, sizeof(seq));
	memcpy(p += sizeof(seq), &d->dl, sizeof(d->dl));
	memcpy(p += sizeof(d->dl), &nterms, sizeof(nterms));
	p += sizeof(nterms);
	for (uint32 i = 0; i < nterms; i++)
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
sort_docs(Relation index, const SiftSegEntry *seg, const DocText *dt)
{
	SiftWbufReader *r = sift_wbuf_open(index, seg, CurrentMemoryContext, NULL, NULL);
	Tuplesortstate *sort = tuplesort_begin_datum(BYTEAOID, ByteaLessOperator, InvalidOid, false, sift_mem_maint_kb() / 2,
												 NULL, TUPLESORT_RANDOMACCESS);
	DocPacker	pk = {palloc(1024), 1024};
	SiftWbufDoc	d;
	uint32		seq = 0;

	while (sift_wbuf_next(r, &d))
		if (!d.deleted)
			tuplesort_putdatum(sort, PointerGetDatum(pack_doc(&pk, &d, seq++, dt == NULL)), false);
	sift_wbuf_close(r);
	pfree(pk.buf);
	tuplesort_performsort(sort);
	return sort;
}

/* The memory the inverted buffer may take: what the sort left of the maintenance memory when it kept its docs in memory, the half it does not take when it spilled (the sort has half of it) */
static Size
inv_budget(Tuplesortstate *sort)
{
	TuplesortInstrumentation st;
	Size		budget = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1);

	tuplesort_get_stats(sort, &st);
	if (st.sortMethod == SORT_TYPE_QUICKSORT)
		budget -= Min(budget - budget / 4, (Size) st.spaceUsed * 1024);
	else
		budget /= 2;
	return budget;
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

/* An inverted buffer for the docs of the index, which keep the length of each of its fields */
static SiftInvBuf *
inv_create_for(Relation index, Size budget, MemoryContext cx, bool positions)
{
	SiftInvBuf *inv = sift_inv_create(budget, cx, positions);
	SiftAnalyzerConfig cfg;

	sift_analyzer_config_from_index(index, &cfg);
	if (cfg.nfields)
		sift_inv_fields(inv, cfg.nfields);
	return inv;
}

/* Writes the sorted docs as one immutable segment, but for those whose row is gone (dead for every snapshot); false when there are none */
static bool
write_docs(Relation index, SiftWriteTarget *wt, Tuplesortstate *sort, DocText *dt, uint64 gen, SiftSegEntry *entry)
{
	SiftScoreRef ref = score_ref(index);
	SiftInvBuf *inv = inv_create_for(index, inv_budget(sort), CurrentMemoryContext, dt != NULL);
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
			sift_doctext_prime(dt, &c);
		if (!sift_doc_next(&c))
			break;
		CHECK_FOR_INTERRUPTS();
		terms = c.terms;
		nterms = c.nterms;
		dl = c.dl;
		sift_key_to_tid(c.key, &tid);
		if (dt != NULL && !sift_doctext_terms(dt, &tid, &terms, &nterms, &dl, &len))
			continue;
		inv_add_doc(inv, &runs, dt, &tid, terms, nterms, dl, len);
	}
	sift_doctext_close(dt);
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
	DocText    *dt = sift_doctext_open(index);
	SiftSegEntry neu;
	bool		any;
	SiftExtent *pages;
	int			npages;
	TimestampTz start = GetCurrentTimestamp();

	sift_vacuum_delay_point();
	sift_lane_flag_sealed(index, sealed);
	pages = sift_lane_extents(index, sealed, &npages);
	sift_wt_expect(wt, extents_bytes(pages, npages));
	sort = sort_docs(index, sealed, dt);
	sift_mem_assert_within(&mt_bm25_promote);
	any = write_docs(index, wt, sort, dt, sift_store_new_gen(index), &neu);
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
	bool		cut;			/* the pick holds more segments than the merge reads at once: the rest wait for another */
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

/*
 * What one merge may read: at most merge_max_bytes(), and half of temp_file_limit (the live docs, the dictionary and the staged terms it writes are a third of its input at most).
 * A VACUUM also stops at sift.vacuum_rewrite_limit for one merge (it goes on to another until the limit is written), but takes the two smallest segments whatever they are,
 * as it would never merge them otherwise.
 */
static uint64
merge_hard_cap(void)
{
	uint64		cap = merge_max_bytes();

	if (temp_file_limit >= 0)
		cap = Min(cap, Max((uint64) temp_file_limit * 1024 / 2, (uint64) 4 << 20));
	return cap;
}

static uint64
merge_soft_cap(SiftMergeGoal goal)
{
	return goal == SIFT_GOAL_VACUUM ? sift_vacuum_rewrite_budget(false) : PG_UINT64_MAX;
}

/* The segments one merge reads at once: a cursor of SIFT_SEG_CURSOR_MIN each within half the maintenance memory */
static int
merge_fanin_cap(void)
{
	return Min(MAX_MERGE_FANIN, Max(2, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1) / (2 * SIFT_SEG_CURSOR_MIN)));
}

/* The policy's choice (smallest first) cut to the smallest segments whose bytes fit one merge; a merge of fewer than two is none */
static int
pick_merge(Relation index, const SiftDirSnapshot *snap, bool force, SiftMergeGoal goal, MergeSel *sel)
{
	int			excess = snap->nsegs - sift_index_options(index)->vacuum_segments;
	bool		counted = goal != SIFT_GOAL_ANY && !force;
	int			n = pick_policy(index, snap, force, counted && excess <= 0, sel);
	uint64		total = 0,
				hard = merge_hard_cap(),
				soft = merge_soft_cap(goal);
	int			k = 0,
				fanin = merge_fanin_cap();

	if (counted)
	{
		/* VACUUM merges down to the target and no further, and takes the smallest segments the policy left over */
		if (goal == SIFT_GOAL_VACUUM && n > 1)
			n = Min(n, excess + 1);
		if (goal == SIFT_GOAL_VACUUM && n == 0 && excess > 0)
			n = pick_smallest(snap, excess + 1, sift_index_options(index)->merge_fanin, sel);
	}

	while (k < n && k < fanin && total + sel->seg[k].bytes <= hard && (k < 2 || total + sel->seg[k].bytes <= soft))
		total += sel->seg[k++].bytes;
	sel->cut = k < n;
	return k < Min(n, 2) ? 0 : k;
}

typedef struct Heal
{
	DocText    *dt;
	SiftInvBuf *inv;
	RunList    *runs;
} Heal;

static void
heal_docs(void *arg, const SiftKey *keys, const uint16 *dl, const uint16 *fdl, int n)
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
		if (sift_doctext_terms(h->dt, &tid, &terms, &nterms, &doclen, &len))
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

	sift_seg_stream(seg, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1) / 2, &(SiftSegSink) {.docs = heal_docs}, &h);
	if (!sift_inv_empty(h.inv))
		runlist_push(runs, sift_inv_spill(h.inv));
}

/* The merge that re-analyzes the segments that lack positions: the others are dumped to runs beside the docs of the heap, as the inverted buffer's */
static bool
merge_healing(Relation index, SiftWriteTarget *wt, const MergeSel *in, DocText *dt, MemoryContext cx, const SiftScoreRef *ref, SiftSegEntry *neu)
{
	RunList		runs = {.run = palloc(in->n * sizeof(SiftRun *)),.max = in->n};
	SiftInvBuf *inv = inv_create_for(index, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1) / 2, cx, true);
	bool		written;

	for (int i = 0; i < in->n; i++)
	{
		SiftSegment *seg = sift_seg_open(index, &in->seg[i], cx);
		SiftRun    *run;

		if (!(in->seg[i].flags & SIFT_SEG_POSITIONS))
			heal_runs(dt, inv, seg, &runs);
		else if ((run = sift_run_from_segment(seg, ref, cx)) != NULL)
			runlist_push(&runs, run);
		sift_seg_close(seg);
	}
	sift_doctext_close(dt);
	if ((written = runs.n > 0))
		sift_runs_write(runs.run, runs.n, wt, sift_store_new_gen(index), ref, neu);
	return written;
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
	SiftMaintInputs mi = sift_maint_inputs(index, in->seg, in->n);
	bool		written;
	DocText    *dt = NULL;
	SiftSegEntry neu;
	TimestampTz start = GetCurrentTimestamp();

	sift_wt_expect(wt, mi.bytes);
	sift_wt_pace_worker(wt);
	/* a long merge of the worker, the segments claimed */
	SIFT_TEST_GATE(sift_test_merge_gate && MyBackendType == B_BG_WORKER, 4);
	for (int i = 0; i < in->n; i++)
	{
		sift_vacuum_delay_point();
		sift_wt_charge(wt, in->seg[i].bytes / BLCKSZ);	/* the read of each segment */
	}
	for (int i = 0; i < in->n && dt == NULL; i++)
		if (!(in->seg[i].flags & SIFT_SEG_POSITIONS))
			dt = sift_doctext_open(index);
	/* the segments that get their positions from the heap are merged by this process */
	if (dt == NULL)
		written = sift_merge_ranges(index, wt, in->seg, in->n, &ref, &neu) > 0;
	else
		written = merge_healing(index, wt, in, dt, cx, &ref, &neu);
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

/* One merge of segments nobody else is at; busy: the generation of one that is, when that left nothing to merge; cut: the merge left segments of the pick for another */
static SiftStepResult
merge_once(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy, bool *cut)
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
	*cut = sel->cut;
	if (sel->seg)
	{
		pfree(sel->seg);
		pfree(sel->busy);
	}
	pfree(sel);
	pfree(snap);
	return r;
}

/* One merge, or for force every merge it takes to bring all the immutable segments into one when the memory reads fewer at once */
SiftStepResult
merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy)
{
	bool		cut;
	SiftStepResult r = merge_once(index, force, goal, propagate, busy, &cut);

	while (force && cut && r == SIFT_STEP_DONE && merge_once(index, true, goal, propagate, busy, &cut) == SIFT_STEP_DONE)
		CHECK_FOR_INTERRUPTS();
	return r;
}
