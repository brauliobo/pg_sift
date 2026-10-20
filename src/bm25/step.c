/*
 * bm25/step.c - the maintenance steps of a BM25 index: the claims on the segments a step replaces, a step run in a
 * subtransaction, the promotion of a sealed write buffer and the tiered merge.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "common/int.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"
#include "utils/tuplesort.h"
#include "utils/timestamp.h"

#include "bm25/am.h"
#include "bm25/build.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/maint_int.h"
#include "bm25/merge.h"
#include "bm25/wbuf.h"
#include "core/ops.h"
#include "core/sql.h"
#include "core/visibility.h"

#define CLAIM_TRIES			8		/* picks of a step whose segments others claimed or replaced meanwhile */

/* ---------- claims: the segments a step replaces ---------- */

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

/* ---------- steps ---------- */

static void
stamp_failure(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	uint32		failures = meta->flags & SIFT_META_FAILURES_MASK;

	meta->flags = (meta->flags & ~SIFT_META_FAILURES_MASK) | Min(failures + 1, SIFT_META_FAILURES_MASK);
	meta->maint_requested_at = *(TimestampTz *) arg;
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
	ereport(WARNING,
			(errcode(err->sqlerrcode),
			 errmsg("maintenance of sift index \"%s\" failed: %s", RelationGetRelationName(index), err->message),
			 errdetail("It is retried later, after a growing delay.")));
	FreeErrorData(err);
	sift_dir_update(index, &(SiftDirChange) {.fn = stamp_failure,.arg = &(TimestampTz) {GetCurrentTimestamp()},
			.extra = InvalidBuffer});
	return SIFT_STEP_FAILED;
}

/* ---------- promotion ---------- */

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
	Tuplesortstate *sort = tuplesort_begin_datum(BYTEAOID, ByteaLessOperator, InvalidOid, false, maintenance_work_mem,
												 NULL, TUPLESORT_NONE);
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

/* The memory the inverted buffer may take: what the sort left of maintenance_work_mem when it kept its docs in memory */
static Size
inv_budget(Tuplesortstate *sort)
{
	TuplesortInstrumentation st;
	Size		budget = (Size) maintenance_work_mem * 1024;

	tuplesort_get_stats(sort, &st);
	if (st.sortMethod == SORT_TYPE_QUICKSORT)
		budget -= Min(budget - budget / 4, (Size) st.spaceUsed * 1024);
	return budget;
}

/* Writes the sorted docs as one immutable segment; false when there are none */
static bool
write_docs(Relation index, SiftWriteTarget *wt, Tuplesortstate *sort, uint64 gen, SiftSegEntry *entry)
{
	SiftScoreRef ref = score_ref(index);
	SiftInvBuf *inv = sift_inv_create(inv_budget(sort), CurrentMemoryContext, false);
	SiftRun   **runs = palloc(sizeof(SiftRun *));
	DocCursor	c = {.sort = sort,.terms = palloc(sizeof(SiftTermFreq)),.maxterms = 1};
	int			nruns = 0,
				maxruns = 1;

	while (next_doc(&c))
	{
		ItemPointerData tid;

		sift_key_to_tid(c.key, &tid);
		sift_inv_add(inv, &tid, c.terms, c.nterms, c.dl);
		if (sift_inv_full(inv))
		{
			if (nruns == maxruns)
				runs = repalloc(runs, (maxruns *= 2) * sizeof(SiftRun *));
			runs[nruns++] = sift_inv_spill(inv);
		}
	}
	if (nruns == 0 && sift_inv_empty(inv))
		return false;
	if (nruns == 0)
		sift_inv_write(inv, wt, gen, &ref, entry);
	else
	{
		if (!sift_inv_empty(inv))
		{
			if (nruns == maxruns)
				runs = repalloc(runs, (maxruns *= 2) * sizeof(SiftRun *));
			runs[nruns++] = sift_inv_spill(inv);
		}
		sift_runs_write(runs, nruns, wt, gen, &ref, entry);
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
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift promote", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	Tuplesortstate *sort;
	SiftSegEntry neu;
	bool		any;
	SiftExtent *pages;
	int			npages;
	TimestampTz start = GetCurrentTimestamp();

	sift_wbuf_flag_sealed(index, sealed);
	pages = sift_wbuf_pages(index, sealed, &npages);
	sift_wt_expect(wt, extents_bytes(pages, npages));
	sort = sort_docs(index, sealed);
	any = write_docs(index, wt, sort, sift_store_new_gen(index), &neu);
	tuplesort_end(sort);
	sift_wt_finish(wt);
	sift_maint_replace(index, &sealed->gen, 1, any ? &neu : NULL, pages, npages, "promoted a sealed segment of", start, sealed->ndocs);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* Re-reads the directory, as the segments claimed from an older snapshot may have been replaced meanwhile */
static bool
still_there(Relation index, uint64 gen, SiftSegKind kind)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		found;

	sift_dir_read(index, snap);
	found = has_segment(snap, gen, kind);
	pfree(snap);
	return found;
}

/* Promotes the oldest sealed segment nobody else is at; busy: the generation of one that is */
SiftStepResult
promote_next(Relation index, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftStepResult	r = SIFT_STEP_IDLE;

	sift_dir_read(index, snap);
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

/* ---------- merge ---------- */

typedef struct MergeSel
{
	int			n;
	SiftSegEntry seg[SIFT_MAX_SEGMENTS];
	int			nbusy;			/* generations others claimed: the policy leaves them out */
	uint64		busy[SIFT_MAX_SEGMENTS];
} MergeSel;

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
		view = palloc(sizeof(SiftDirSnapshot));
		*view = *snap;
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
	SiftSegEntry imm[SIFT_MAX_SEGMENTS];
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

static void
merge_segments(Relation index, SiftWriteTarget *wt, const void *arg)
{
	const MergeSel *in = arg;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftScoreRef ref = score_ref(index);
	SiftRun   **runs = palloc(in->n * sizeof(SiftRun *));
	SiftMaintInputs mi = sift_maint_inputs(index, in->seg, in->n);
	int			nruns = 0;
	SiftSegEntry neu;
	TimestampTz start = GetCurrentTimestamp();

	sift_wt_expect(wt, mi.bytes);
	if (MyBackendType == B_BG_WORKER)
		sift_wt_throttle(wt, sift_maintenance_cost_delay);
#ifdef SIFT_TESTING
	/* a long merge of the worker, the segments claimed */
	for (int ms = 0; MyBackendType == B_BG_WORKER && ms < sift_test_merge_sleep_ms; ms += 50)
	{
		pg_usleep(50000);
		CHECK_FOR_INTERRUPTS();
	}
#endif
	for (int i = 0; i < in->n; i++)
	{
		SiftSegment *seg;
		SiftRun    *run;

		sift_wt_charge(wt, in->seg[i].bytes / BLCKSZ);	/* the read of this segment */
		seg = sift_seg_open(index, &in->seg[i], cx);
		run = sift_run_from_segment(seg, &ref, cx);
		sift_seg_close(seg);
		if (run)
			runs[nruns++] = run;
	}
	if (nruns > 0)
		sift_runs_write(runs, nruns, wt, sift_store_new_gen(index), &ref, &neu);
	sift_wt_finish(wt);
	sift_maint_replace(index, mi.gens, in->n, nruns > 0 ? &neu : NULL, mi.release, mi.nrelease,
					   psprintf("merged %d segments of", in->n), start, nruns > 0 ? neu.ndocs : 0);
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
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		all = true;

	sift_dir_read(index, snap);
	for (int i = 0; i < sel->n; i++)
		all &= has_segment(snap, sel->seg[i].gen, SIFT_SEG_IMMUTABLE);
	pfree(snap);
	return all;
}

/* One merge of segments nobody else is at; busy: the generation of one that is, when that left nothing to merge */
SiftStepResult
merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MergeSel   *sel = palloc0(sizeof(MergeSel));
	SiftStepResult	r = SIFT_STEP_IDLE;

	for (int tries = 0; tries < CLAIM_TRIES && r == SIFT_STEP_IDLE; tries++)
	{
		sift_dir_read(index, snap);
		if ((sel->n = pick_merge(index, snap, force, goal, sel)) == 0)
			break;
		if (!claim_all(index, sel))
			continue;
		if (all_there(index, sel))
			r = run_step(index, propagate, merge_segments, sel);
		for (int i = 0; i < sel->n; i++)
			unclaim(index, sel->seg[i].gen);
	}
	if (r == SIFT_STEP_IDLE && sel->nbusy > 0)
		*busy = sel->busy[0];
	pfree(sel);
	pfree(snap);
	return r;
}
