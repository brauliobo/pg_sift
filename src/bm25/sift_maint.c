/*
 * sift_maint.c - promotion of sealed write buffers, tiered merge, VACUUM
 * callbacks, the orphan sweep and the asynchronous maintenance worker
 * (DESIGN.md (c), (i)).
 *
 * Promotion and merge steps hold the maintenance lock (LockPage SIFT_MAINT_LOCK_BLKNO) in
 * RowExclusiveLock, which steps share, and claim the segments they replace, one heavyweight lock per
 * generation, so the steps of the worker and of inserters on other segments run side by side and a
 * long merge stalls nobody. sift.verify() holds it in ShareLock, which keeps the steps out; VACUUM,
 * the orphan sweep and the SQL functions hold it exclusive: nothing else is then at work on the index.
 * A claimed segment can only be changed by its holder. Readers are safe through the store's
 * safexid protocol (sift_store.h P4).
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

#include "sift_am.h"
#include "sift_build.h"
#include "sift_lane.h"
#include "sift_maint.h"
#include "sift_merge.h"
#include "sift_wbuf.h"
#include "sift_ops.h"
#include "sift_sql.h"
#include "sift_visibility.h"

#define BACKOFF_MAX_MS		3600000
/* inputs of one merge: its output (runs grow with a stream, sift_store.c) stays well within the extents a header page lists */
#define MERGE_MAX_BYTES		(UINT64CONST(16) << 30)

#ifdef SIFT_TESTING
int			sift_test_merge_max_kb = 0;
int			sift_test_merge_sleep_ms = 0;
int			sift_test_room_segments = 0;
#define merge_max_bytes()	(sift_test_merge_max_kb > 0 ? (uint64) sift_test_merge_max_kb * 1024 : MERGE_MAX_BYTES)
#define room_segments()		(sift_test_room_segments > 0 ? sift_test_room_segments : ROOM_SEGMENTS)
/* longest wait of an inserter for the maintenance of others (sift._test_maint_wait_us) */
int64		sift_maint_wait_max_us = 0;
#define wait_started()		GetCurrentTimestamp()
#define wait_done(start)	(sift_maint_wait_max_us = Max(sift_maint_wait_max_us, GetCurrentTimestamp() - (start)))
#else
#define merge_max_bytes()	MERGE_MAX_BYTES
#define room_segments()		ROOM_SEGMENTS
#define wait_started()		((TimestampTz) 0)
#define wait_done(start)	((void) (start))
#endif
#define ROOM_SEGMENTS		(SIFT_MAX_SEGMENTS - 16)	/* directory entries from which inserters merge */
#define SELF_PROMOTE_SEALED	4		/* sealed segments from which an inserter promotes them itself */
/* Whatever merge_fanin is, the policy merges (from SIFT_MERGE_ENTRIES entries) before inserters have to make room, and a merge fits a directory */
StaticAssertDecl(SIFT_MERGE_ENTRIES(SIFT_MERGE_MAX_FANIN) + SELF_PROMOTE_SEALED + 1 < ROOM_SEGMENTS, "merge policy acts after the room threshold");
StaticAssertDecl(2 * SIFT_MERGE_MAX_FANIN <= SIFT_MAX_SEGMENTS, "a merge of the largest merge_fanin overflows a directory");
/* VACUUM's truncation of a heap gives back at least this many blocks, or a 16th of the relation */
#define TRUNCATE_MIN_BLOCKS	1000
#define TRUNCATE_FRACTION	16
#define CLAIM_TRIES			8		/* picks of a step whose segments others claimed or replaced meanwhile */

/* ---------- locking and directory helpers ---------- */

static bool
maint_lock_mode(Relation index, bool wait, LOCKMODE mode)
{
	if (wait)
	{
		LockPage(index, SIFT_MAINT_LOCK_BLKNO, mode);
		return true;
	}
	return ConditionalLockPage(index, SIFT_MAINT_LOCK_BLKNO, mode);
}

static void
maint_unlock_mode(Relation index, LOCKMODE mode)
{
	UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, mode);
}

bool
sift_maint_lock(Relation index, bool wait)
{
	return maint_lock_mode(index, wait, ExclusiveLock);
}

void
sift_maint_unlock(Relation index)
{
	maint_unlock_mode(index, ExclusiveLock);
}

/* ---------- claims: the segments a step replaces ---------- */

static bool
claim(Relation index, uint64 gen, bool wait)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, RelationGetRelid(index), (uint32) gen, (uint16) (gen >> 32));
	return LockAcquire(&tag, ExclusiveLock, false, !wait) != LOCKACQUIRE_NOT_AVAIL;
}

static void
unclaim(Relation index, uint64 gen)
{
	LOCKTAG		tag;

	SET_LOCKTAG_OBJECT(tag, MyDatabaseId, RelationGetRelid(index), (uint32) gen, (uint16) (gen >> 32));
	LockRelease(&tag, ExclusiveLock, false);
}

/* Waits until the holder of the claim is done, which is how an inserter waits for a full directory to shrink */
static void
wait_claim(Relation index, uint64 gen)
{
	TimestampTz start = wait_started();

	claim(index, gen, true);
	unclaim(index, gen);
	wait_done(start);
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

/* ---------- switching segments in the directory ---------- */

void
sift_maint_switch_segments(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	SiftMaintSwitch *sw = arg;
	int			n = 0;
	int			at = -1;

	for (int i = 0; i < dir->nsegs; i++)
	{
		bool		replaced = false;

		for (int k = 0; k < sw->nold; k++)
			replaced |= dir->segs[i].gen == sw->old[k];
		if (replaced && at < 0)
			at = n;
		if (replaced && dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			meta->ndocs -= dir->segs[i].ndocs;
			meta->sum_dl -= dir->segs[i].sum_dl;
		}
		if (!replaced)
			dir->segs[n++] = dir->segs[i];
	}
	if (sw->neu)
	{
		memmove(&dir->segs[at + 1], &dir->segs[at], (n - at) * sizeof(SiftSegEntry));
		dir->segs[at] = *sw->neu;
		n++;
		if (sw->neu->kind == SIFT_SEG_IMMUTABLE)
		{
			meta->ndocs += sw->neu->ndocs;
			meta->sum_dl += sw->neu->sum_dl;
		}
	}
	dir->nsegs = n;
	meta->flags &= ~SIFT_META_FAILURES_MASK;
}

/* The generations and extents of in[0..n), allocated in the current memory context */
SiftMaintInputs
sift_maint_inputs(Relation index, const SiftSegEntry *in, int n)
{
	SiftMaintInputs mi = {.gens = palloc(n * sizeof(uint64)),.release = palloc(0)};

	for (int i = 0; i < n; i++)
	{
		int			k;
		const SiftExtent *ext = sift_store_immutable_extents(index, &in[i], &k);

		mi.release = repalloc(mi.release, (mi.nrelease + k) * sizeof(SiftExtent));
		memcpy(mi.release + mi.nrelease, ext, k * sizeof(SiftExtent));
		mi.nrelease += k;
		mi.gens[i] = in[i].gen;
		mi.bytes += in[i].bytes;
	}
	return mi;
}

/* The bytes of the segments the steps of this backend wrote (VACUUM budgets its merges by what they add to it) */
static uint64 maint_written = 0;

void
sift_maint_replace(Relation index, const uint64 *old, int nold, const SiftSegEntry *neu, SiftExtent *release, int nrelease,
				   const char *what, TimestampTz start, int64 docs)
{
	SiftMaintSwitch sw = {.old = old,.nold = nold,.neu = neu};

	if (neu)
		maint_written += neu->bytes;

	sift_dir_update(index, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw,.release = release,
			.nrelease = nrelease,.extra = InvalidBuffer});
	sift_log_maint(index, what, start, docs, neu ? neu->bytes : 0);
}

/* ---------- steps ---------- */

static int64
backoff_ms(const SiftMetaPageData *meta)
{
	uint32		failures = meta->flags & SIFT_META_FAILURES_MASK;

	return Min((int64) sift_maintenance_naptime << Min(failures, 20), BACKOFF_MAX_MS);
}

/* Failed steps double the time before the next one is attempted in the background, up to an hour */
static bool
in_backoff(const SiftMetaPageData *meta)
{
	return (meta->flags & SIFT_META_FAILURES_MASK) != 0 &&
		GetCurrentTimestamp() - meta->maint_requested_at < backoff_ms(meta) * INT64CONST(1000);
}

static void
stamp_failure(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	uint32		failures = meta->flags & SIFT_META_FAILURES_MASK;

	meta->flags = (meta->flags & ~SIFT_META_FAILURES_MASK) | Min(failures + 1, SIFT_META_FAILURES_MASK);
	meta->maint_requested_at = *(TimestampTz *) arg;
}

typedef void (*StepFn) (Relation index, SiftWriteTarget *wt, const void *arg);

/*
 * Runs fn in a subtransaction, so an ERROR rolls back every buffer lock and pin it held and leaves
 * the blocks fn took in wt unreferenced, which are then returned to the free list. propagate: the
 * ERROR goes on to the caller (an explicit sift.promote() or sift.merge()); otherwise it is logged
 * as a WARNING, counted for the backoff and the step reports SIFT_STEP_FAILED. Cancellation and shutdown
 * always go on. Frees recycled extents first, so the step finds contiguous space.
 */
static SiftStepResult
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
static SiftStepResult
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
 * The segments to merge, by the policy shared with the other hosts (sift_merge.h); tiers start at the write buffer limit.
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
static SiftStepResult
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

/* ---------- the steps of an index kind: the control flow shared by BM25 and sift_col ---------- */

static bool make_room_locked(const SiftMaintOps *ops, Relation index);

/* Promotes every sealed segment, oldest first, until one fails; returns the number promoted */
static int
promote_locked(const SiftMaintOps *ops, Relation index, bool seal_active, bool propagate)
{
	uint64		busy = 0;
	int			n = 0;

	if (seal_active)
	{
		make_room_locked(ops, index);
		ops->seal(index, true);
	}
	while (ops->promote_next(index, propagate, &busy) == SIFT_STEP_DONE)
	{
		n++;
		CHECK_FOR_INTERRUPTS();
	}
	return n;
}

/*
 * Promotes and merges until the directory is below ROOM_SEGMENTS entries, so a seal always finds a
 * free one; false when it cannot (nothing to merge, or a step failed).
 */
static bool
make_room_locked(const SiftMaintOps *ops, Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		room;

	for (;;)
	{
		SiftStepResult	r;
		uint64		busy = 0;

		sift_dir_read(index, snap);
		if (snap->nsegs < room_segments())
			break;
		if ((r = ops->promote_next(index, false, &busy)) == SIFT_STEP_IDLE)
			r = ops->merge_next(index, false, SIFT_GOAL_ANY, false, &busy);
		if (r == SIFT_STEP_IDLE && busy != 0)
		{
			/* what is left to merge is claimed by others: their step frees the entries */
			wait_claim(index, busy);
			continue;
		}
		if (r != SIFT_STEP_DONE)
			break;
		CHECK_FOR_INTERRUPTS();
	}
	room = snap->nsegs < room_segments();
	pfree(snap);
	return room;
}

int
sift_mops_promote(const SiftMaintOps *ops, Relation index, bool wait)
{
	int			n;

	if (!maint_lock_mode(index, wait, ops->step_lock))
		return 0;
	n = promote_locked(ops, index, false, false);
	maint_unlock_mode(index, ops->step_lock);
	return n;
}

bool
sift_mops_make_room(const SiftMaintOps *ops, Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		room = false;

	sift_dir_read(index, snap);
	if (!in_backoff(&snap->meta))
	{
		TimestampTz start = GetCurrentTimestamp();

		maint_lock_mode(index, true, ops->step_lock);
		wait_done(start);
		room = make_room_locked(ops, index);
		maint_unlock_mode(index, ops->step_lock);
		sift_log_stall(index, start);
	}
	pfree(snap);
	return room;
}

bool
sift_mops_merge_step(const SiftMaintOps *ops, Relation index, bool wait)
{
	SiftStepResult	r;
	uint64		busy = 0;

	if (!maint_lock_mode(index, wait, ops->step_lock))
		return false;
	r = ops->merge_next(index, false, SIFT_GOAL_BACKGROUND, false, &busy);
	maint_unlock_mode(index, ops->step_lock);
	return r == SIFT_STEP_DONE;
}

/*
 * Promotion is asynchronous when allowed. Back-pressure: with many sealed
 * segments waiting, the inserter promotes itself unless maintenance is busy,
 * and with the directory nearly full it merges itself, waiting for the
 * maintenance lock (inserts slow down instead of failing).
 */
void
sift_mops_after_seal(const SiftMaintOps *ops, Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int			nsealed = 0;

	sift_dir_read(index, snap);
	if (snap->nsegs >= room_segments())
		sift_mops_make_room(ops, index);
	if (ops->seal(index, false) == 0)
	{
		pfree(snap);
		return;
	}
	if (sift_async_maintenance)
		sift_maint_request_worker(index, ops->worker);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		nsealed += snap->segs[i].kind == SIFT_SEG_WBUF_SEALED;
	if (nsealed >= SELF_PROMOTE_SEALED && !in_backoff(&snap->meta))
		sift_mops_promote(ops, index, false);
	pfree(snap);
}

/* ---------- VACUUM ---------- */

IndexBulkDeleteResult *
sift_mops_bulkdelete(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
					 void *callback_state)
{
	Relation	index = info->index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int64		removed;

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	removed = stats->tuples_removed;
	sift_maint_lock(index, true);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			ops->bulkdelete_immutable(index, &snap->segs[i], callback, callback_state, stats);
		else
			ops->bulkdelete_wbuf(index, &snap->segs[i], callback, callback_state, stats);
	/* The heap marks pages all-visible only after this returns: a scan that reads the map after that finds the version moved (sift_vis_begin) */
	if (stats->tuples_removed > removed)
		sift_dir_update(index, &(SiftDirChange) {.extra = InvalidBuffer});
	sift_maint_unlock(index);
	pfree(snap);
	return stats;
}

/* The merges of VACUUM: until the policy asks for none, as a build that left many segments needs, or just one; stops at a failed step */
static void
vacuum_merge(const SiftMaintOps *ops, Relation index)
{
	uint64		busy = 0;
	uint64		start = maint_written;
	uint64		limit = (uint64) sift_vacuum_rewrite_limit << 20;

	while (ops->merge_next(index, false, SIFT_GOAL_VACUUM, false, &busy) == SIFT_STEP_DONE && ops->vacuum_to_fixed_point)
	{
		CHECK_FOR_INTERRUPTS();
		if (ops->vacuum_budget && maint_written - start >= limit)
			break;
	}
}

IndexBulkDeleteResult *
sift_mops_vacuumcleanup(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	index = info->index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		skipped = stats == NULL;

	if (info->analyze_only)
		return stats;
	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	sift_maint_lock(index, true);
	if (ops->vacuum_seal)
		ops->vacuum_seal(index);
	promote_locked(ops, index, false, false);
	if (skipped && ops->vacuum_skipped)
		ops->vacuum_skipped(index, info->heaprel);
	vacuum_merge(ops, index);
	SIFT_TEST_GATE(sift_test_cleanup_gate, 2);
	if (ops->vacuum_refresh)
		ops->vacuum_refresh(index);
	sift_recycle(index);
	sift_maint_orphan_sweep(index);
	if (ops->vacuum_truncate && ops->vacuum_truncate(index))
		sift_maint_space(index, NULL, info->message_level);
	sift_maint_unlock(index);
	sift_dir_read(index, snap);
	stats->num_pages = RelationGetNumberOfBlocks(index);
	stats->estimated_count = false;
	if (stats->num_index_tuples == 0)
		for (int i = 0; i < snap->nsegs; i++)
			stats->num_index_tuples += snap->segs[i].ndocs;
	pfree(snap);
	return stats;
}

/* ---------- SQL ---------- */

Relation
sift_mops_open(const SiftMaintOps *ops, Oid indexoid, LOCKMODE lock)
{
	Relation	index = index_open(indexoid, lock);

	if (!ops->is_index(index))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a %s index", RelationGetRelationName(index), ops->name)));
	return index;
}

Relation
sift_mops_open_maintain(const SiftMaintOps *ops, Oid indexoid)
{
	Relation	index = sift_mops_open(ops, indexoid, RowExclusiveLock);

	sift_check_maintain(index);
	return index;
}

/* Seals the active segment and promotes every sealed one; returns the number promoted */
int
sift_mops_sql_promote(const SiftMaintOps *ops, Relation index)
{
	int			n;

	sift_maint_lock(index, true);
	n = promote_locked(ops, index, true, true);
	sift_maint_unlock(index);
	return n;
}

/* Runs merge steps until the policy asks for none; force merges all immutable segments into one */
int
sift_mops_sql_merge(const SiftMaintOps *ops, Relation index, bool force)
{
	uint64		busy = 0;
	int			n = 0;

	sift_maint_lock(index, true);
	promote_locked(ops, index, false, true);
	while (ops->merge_next(index, force && n == 0, SIFT_GOAL_ANY, true, &busy) == SIFT_STEP_DONE)
	{
		n++;
		CHECK_FOR_INTERRUPTS();
	}
	sift_maint_unlock(index);
	return n;
}

/* ---------- BM25 ---------- */

static bool
is_sift(Relation index)
{
	return index->rd_rel->relam == sift_catalog()->am;
}

static void
bulkdelete_immutable(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	SiftSegment *seg = sift_seg_open(index, e, CurrentMemoryContext);

	sift_seg_bulkdelete(seg, cb, cb_state, stats);
	sift_seg_close(seg);
}

static void
bulkdelete_wbuf(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	sift_wbuf_bulkdelete(index, e, cb, cb_state, stats);
}

static bool
vacuum_truncates(Relation index)
{
	return sift_index_options(index)->vacuum_truncate;
}

/* The dead-posting tables of the segments VACUUM cleared docs of and the merges left as they are */
static void
refresh_dead_tables(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && (snap->segs[i].flags & SIFT_SEG_DIRTY))
		{
			SiftSegment *seg = sift_seg_open(index, &snap->segs[i], CurrentMemoryContext);

			sift_seg_refresh_dead(seg);
			sift_seg_close(seg);
		}
	pfree(snap);
}

static const SiftMaintOps bm25_ops = {
	.name = "sift",
	.worker = "sift_maint_worker_main",
	.step_lock = RowExclusiveLock,
	.vacuum_to_fixed_point = true,
	.is_index = is_sift,
	.seal = sift_wbuf_seal,
	.promote_next = promote_next,
	.merge_next = merge_next,
	.bulkdelete_immutable = bulkdelete_immutable,
	.bulkdelete_wbuf = bulkdelete_wbuf,
	.vacuum_truncate = vacuum_truncates,
	.vacuum_refresh = refresh_dead_tables,
};

void
sift_maint_after_seal(Relation index)
{
	sift_mops_after_seal(&bm25_ops, index);
}

int
sift_maint_promote(Relation index, bool wait)
{
	return sift_mops_promote(&bm25_ops, index, wait);
}

bool
sift_maint_make_room(Relation index)
{
	return sift_mops_make_room(&bm25_ops, index);
}

bool
sift_maint_merge_step(Relation index, bool wait)
{
	return sift_mops_merge_step(&bm25_ops, index, wait);
}

IndexBulkDeleteResult *
sift_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	return sift_mops_bulkdelete(&bm25_ops, info, stats, callback, callback_state);
}

IndexBulkDeleteResult *
sift_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return sift_mops_vacuumcleanup(&bm25_ops, info, stats);
}

/* ---------- orphan sweep ---------- */

static SiftExtent *
bitmap_extents(Bitmapset *set, int *n)
{
	SiftExtent *ext = palloc((bms_num_members(set) + 1) * sizeof(SiftExtent));
	int			x = -1;

	*n = 0;
	while ((x = bms_next_member(set, x)) >= 0)
	{
		if (*n > 0 && ext[*n - 1].start + ext[*n - 1].nblocks == (BlockNumber) x)
			ext[*n - 1].nblocks++;
		else
			ext[(*n)++] = (SiftExtent) {x, 1};
	}
	return ext;
}

/*
 * The scan for candidates runs without blocking appenders. A segment created
 * meanwhile (allocated before it is in the directory) is excluded by marking
 * the references again under the lock that serializes creations.
 */
int64
sift_maint_orphan_sweep(Relation index)
{
	Bitmapset  *cand = sift_store_orphans(index);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	Bitmapset  *ref;
	SiftExtent *ext;
	int			n;
	int64		freed;

	if (bms_is_empty(cand))
		return 0;
	LockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(index, snap);
	ref = sift_store_referenced_blocks(index, snap);
	cand = bms_del_members(cand, ref);
	ext = bitmap_extents(cand, &n);
	freed = bms_num_members(cand);
	if (n > 0)
		sift_dir_update(index, &(SiftDirChange) {.release = ext,.nrelease = n,.extra = InvalidBuffer});
	UnlockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	return freed;
}

/* ---------- truncation ---------- */

/*
 * Gives the free blocks at the end of the file back to the operating system, like VACUUM does for a
 * heap: when they are worth an exclusive lock, and only if AccessExclusiveLock on the index is free
 * right now (any scan, insert or step of the worker holds a lock on it), released as soon as the file
 * is cut (sift_store.h P7). The caller holds the maintenance lock exclusively.
 */
void
sift_maint_truncate(Relation index, int elevel)
{
	BlockNumber tail;
	BlockNumber freed;

	/* The lock is WAL-logged for a standby, which takes a transaction id; the leader of a parallel VACUUM runs the cleanup in parallel mode and cannot assign one */
	if (IsInParallelMode() && !FullTransactionIdIsValid(GetTopFullTransactionIdIfAny()))
		return;
	tail = sift_store_tail_blocks(index);
	if (tail == 0 || (tail < TRUNCATE_MIN_BLOCKS && tail < RelationGetNumberOfBlocks(index) / TRUNCATE_FRACTION))
		return;
	if (!ConditionalLockRelation(index, AccessExclusiveLock))
	{
		ereport(elevel, (errmsg("index \"%s\": stopping truncate due to conflicting lock request",
								RelationGetRelationName(index))));
		return;
	}
	freed = sift_store_truncate(index);
	UnlockRelation(index, AccessExclusiveLock);
	if (freed > 0)
		ereport(elevel, (errmsg("index \"%s\": truncated %u to %u pages", RelationGetRelationName(index),
								RelationGetNumberOfBlocks(index) + freed, RelationGetNumberOfBlocks(index))));
}

/* ---------- space: the end of the file moved down, then cut ---------- */

/* A coarse candidate screen only; sift_store_relocate plans exact extent reservations with the allocator before accepting a move. */
#define SPACE_MIN_RUN		16

static bool
space_worth(int64 blocks, BlockNumber nblocks)
{
	return blocks > 0 && (blocks >= TRUNCATE_MIN_BLOCKS || blocks >= nblocks / TRUNCATE_FRACTION);
}

/* A segment of the directory, as the space step sees it: where its blocks end and how many it takes */
typedef struct SpaceSeg
{
	uint64		gen;
	uint64		blocks;
	BlockNumber end;
} SpaceSeg;

static int
cmp_space_end_desc(const void *a, const void *b)
{
	return pg_cmp_u32(((const SpaceSeg *) b)->end, ((const SpaceSeg *) a)->end);
}

/*
 * Rank tail prefixes by the expected file shrinkage within the remaining copy allowance. The destination boundary already includes
 * the free capacity occupied by the copies: subtracting their size again would reject useful moves in interleaved layouts.
 * sift_store_relocate proves each exact reservation fits below that boundary before accepting the candidate.
 */
static int
space_plan(Relation rel, const SiftDirSnapshot *snap, uint64 budget, SpaceSeg *seg, int64 *gain, BlockNumber *limit)
{
	int			n = 0;
	int			best = 0;
	uint64		moved = 0;
	BlockNumber hwm = snap->meta.high_water;
	BlockNumber top;

	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		int			k;
		SiftExtent *ext = e->kind == SIFT_SEG_IMMUTABLE ? sift_store_immutable_extents(rel, e, &k) : sift_lane_extents(rel, e, &k);

		seg[n] = (SpaceSeg) {.gen = e->gen};
		for (int x = 0; x < k; x++)
		{
			seg[n].blocks += ext[x].nblocks;
			seg[n].end = Max(seg[n].end, ext[x].start + ext[x].nblocks);
		}
		pfree(ext);
		n++;
	}
	qsort(seg, n, sizeof(SpaceSeg), cmp_space_end_desc);
	top = n > 0 ? seg[0].end : 0;	/* the free blocks above it go with the cut anyway */
	*gain = 0;
	for (int j = 1; j <= n; j++)
	{
		BlockNumber end = j < n ? seg[j].end : 0;
		BlockNumber lo = 0;
		BlockNumber hi = hwm;
		int64		net;

		moved += seg[j - 1].blocks;
		if (moved * BLCKSZ > budget || sift_store_free_below(rel, hwm, SPACE_MIN_RUN) < (int64) moved)
			break;
		/* the lowest end that holds them: the free blocks are taken from the start of the file up */
		while (lo < hi)
		{
			BlockNumber mid = lo + (hi - lo) / 2;

			if (sift_store_free_below(rel, mid, SPACE_MIN_RUN) >= (int64) moved)
				hi = mid;
			else
				lo = mid + 1;
		}
		end = Max(end, lo);
		net = (int64) top - (int64) end;
		if (net > *gain)
		{
			*gain = net;
			best = j;
			*limit = end;
		}
	}
	return space_worth(*gain, hwm) ? best : 0;
}

/* The directory entry of a generation, or NULL when a step replaced it */
static const SiftSegEntry *
space_entry(const SiftDirSnapshot *snap, uint64 gen)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].gen == gen)
			return &snap->segs[i];
	return NULL;
}

/*
 * Writes the segments that end the file again, into the free blocks below them (space_plan chooses), so that the cut finds its end free:
 * a load leaves its write buffers below the column segment promoted from them, a merge leaves its output above the segments it replaced,
 * and a few live segments among free blocks would hold the file at its size for good. A second look follows each round, as the blocks
 * a segment was copied into are reserved by exact extent length, which can reject a coarse candidate. Every completed copy consumes the remaining allowance.
 */
typedef struct SpaceMove
{
	const SiftSegEntry *entry;
	SiftExtent *release;
	int			nrelease;
	BlockNumber limit;
	uint64		budget;
	uint64		written;
	BlockNumber header;
	SiftRelocatePage relocate_page;
} SpaceMove;

static void
space_move(Relation rel, SiftWriteTarget *wt, const void *arg)
{
	SpaceMove  *move = (SpaceMove *) arg;
	SiftSegEntry fresh;
	SiftMaintSwitch sw = {.old = &move->entry->gen,.nold = 1,.neu = &fresh};

	move->written = sift_store_relocate(rel, wt, move->entry, move->release, move->nrelease,
									 move->limit, move->budget, &fresh, move->relocate_page);
	if (move->written == 0)
		return;
	move->header = fresh.header_blk;
	sift_wt_publish(wt, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw,
					.release = move->release,.nrelease = move->nrelease,.extra = InvalidBuffer});
	sift_log_maint(rel, "relocated", GetCurrentTimestamp(), move->entry->ndocs, fresh.bytes);
}

static void
space_relocate(Relation rel, uint64 budget, SiftRelocatePage relocate_page)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SpaceSeg   *seg = palloc(SIFT_MAX_SEGMENTS * sizeof(SpaceSeg));

	while (budget > 0)
	{
		int64		gain;
		BlockNumber limit = 0;
		int			n;

		sift_dir_read(rel, snap);
		/* Relocation follows the same failure backoff as other automatic writes. Tail truncation remains available. */
		if (in_backoff(&snap->meta))
			break;
		n = space_plan(rel, snap, budget, seg, &gain, &limit);
		if (n == 0)
			break;
		for (int i = 0; i < n; i++)
		{
			const SiftSegEntry *e;
			int nrelease;
			SiftExtent *release;
			SpaceMove move;

			sift_dir_read(rel, snap);
			if ((e = space_entry(snap, seg[i].gen)) == NULL)
				continue;
			release = e->kind == SIFT_SEG_IMMUTABLE ? sift_store_immutable_extents(rel, e, &nrelease) : sift_lane_extents(rel, e, &nrelease);
			move = (SpaceMove) {.entry = e,.release = release,.nrelease = nrelease,.limit = limit,.budget = budget,.relocate_page = relocate_page};
			if (run_step(rel, false, space_move, &move) != SIFT_STEP_DONE || move.written == 0)
			{
				pfree(release);
				goto done;
			}
#ifdef SIFT_TESTING
			if (sift_test_space_error && e->kind == SIFT_SEG_WBUF_OPEN && !(e->flags & SIFT_TBL_SEG_TOMB))
				elog(ERROR, "sift test: error after open buffer relocation publication (header %u to %u)", e->header_blk, move.header);
#endif
			SIFT_SPACE_TEST_POINT(2);
			budget -= move.written;
			pfree(release);
			CHECK_FOR_INTERRUPTS();
		}
	}
done:
	pfree(seg);
	pfree(snap);
}

void
sift_maint_space(Relation rel, SiftRelocatePage relocate_page, int elevel)
{
	const char *what = rel->rd_rel->relkind == RELKIND_INDEX ? "index" : "table";
	BlockNumber nblocks;
	BlockNumber freed = 0;
	uint64		budget = (uint64) sift_vacuum_rewrite_limit << 20;

	/* The lock is WAL-logged for a standby, which takes a transaction id; the leader of a parallel VACUUM runs the cleanup in parallel mode and cannot assign one */
	if (IsInParallelMode() && !FullTransactionIdIsValid(GetTopFullTransactionIdIfAny()))
		return;
	nblocks = RelationGetNumberOfBlocks(rel);
	/*
	 * nothing to cut and not enough free blocks to be worth gathering (what this VACUUM released is not reusable yet, but is once the
	 * relation is locked): do not stop the relation for it
	 */
	if (!space_worth(sift_store_tail_blocks(rel), nblocks) &&
		!(budget > 0 && space_worth(sift_store_free_stats(rel).blocks, nblocks)))
		return;
	if (!ConditionalLockRelation(rel, AccessExclusiveLock))
	{
		ereport(elevel, (errmsg("%s \"%s\": stopping truncate due to conflicting lock request", what, RelationGetRelationName(rel))));
		return;
	}
	PG_TRY();
	{
		sift_store_space_exclusive(rel, true);
		if (budget > 0)
		{
			(void) sift_recycle(rel);
			SIFT_SPACE_TEST_POINT(1);
			space_relocate(rel, budget, relocate_page);
		}
		freed = sift_store_truncate(rel);
	}
	PG_FINALLY();
	{
		sift_store_space_exclusive(rel, false);
	}
	PG_END_TRY();
	UnlockRelation(rel, AccessExclusiveLock);
	if (freed > 0)
		ereport(elevel, (errmsg("%s \"%s\": truncated %u to %u pages", what, RelationGetRelationName(rel),
								RelationGetNumberOfBlocks(rel) + freed, RelationGetNumberOfBlocks(rel))));
}

/* ---------- asynchronous maintenance ---------- */

static void
stamp_request(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->maint_requested_at = *(TimestampTz *) arg;
}

bool
sift_maint_request_worker(Relation index, const char *function)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TimestampTz now = GetCurrentTimestamp();
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;
	bool		started;

	sift_dir_read(index, snap);
	started = now - snap->meta.maint_requested_at >= backoff_ms(&snap->meta) * INT64CONST(1000);
	pfree(snap);
	if (!started)
		return false;
	sift_dir_update(index, &(SiftDirChange) {.fn = stamp_request,.arg = &now,.extra = InvalidBuffer});
	bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "%s", function);
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift maintenance (index %u)", RelationGetRelid(index));
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift maintenance");
	bw.bgw_main_arg = ObjectIdGetDatum(RelationGetRelid(index));
	memcpy(bw.bgw_extra, &MyDatabaseId, sizeof(Oid));
	if (RegisterDynamicBackgroundWorker(&bw, &handle))
		return true;
	sift_log_refused(index);
	return false;
}

Oid
sift_maint_worker_connect(Datum main_arg, ErrorContextCallback *errctx)
{
	Oid			indexoid = DatumGetObjectId(main_arg);
	Oid			dboid;

	sift_maint_context(errctx, indexoid);
	memcpy(&dboid, MyBgworkerEntry->bgw_extra, sizeof(Oid));
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);
	return indexoid;
}

/*
 * One promotion or merge in a transaction of its own, holding no snapshot: the extents it frees are
 * reusable by the next step, because neither its xid nor its xmin hold the horizon back once it
 * committed. The worker lock makes it the only worker of the index at work; the others leave.
 */
static SiftStepResult
worker_step(Oid indexoid, TimestampTz start, bool last)
{
	Relation	index;
	SiftStepResult	r = SIFT_STEP_IDLE;

	StartTransactionCommand();
	index = try_index_open(indexoid, RowExclusiveLock);
	if (index != NULL)
	{
		/* the lock is busy when a VACUUM is at it, which then does the work */
		if (is_sift(index) && ConditionalLockPage(index, SIFT_WORKER_LOCK_BLKNO, ExclusiveLock) &&
			maint_lock_mode(index, false, RowExclusiveLock))
		{
			uint64		busy = 0;

			if ((r = promote_next(index, false, &busy)) == SIFT_STEP_IDLE)
				r = merge_next(index, false, SIFT_GOAL_BACKGROUND, false, &busy);
			maint_unlock_mode(index, RowExclusiveLock);
		}
		if (last || r == SIFT_STEP_FAILED)
			sift_log_maint(index, "maintenance worker finished", start, -1, -1);
		index_close(index, RowExclusiveLock);
	}
	CommitTransactionCommand();
	return r;
}

/*
 * The worker stays until sift.maintenance_naptime after it started, looking for work every so often: a seal inside that window
 * of the request that started it asks for no worker of its own, and its segments would wait for the next seal or a VACUUM.
 * The last look is made after the window ended, so every such seal is before it.
 */
void
sift_maint_worker_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);

	for (;;)
	{
		long		left = sift_maintenance_naptime - TimestampDifferenceMilliseconds(start, GetCurrentTimestamp());
		SiftStepResult	r;

		while ((r = worker_step(indexoid, start, left <= 0)) == SIFT_STEP_DONE)
			CHECK_FOR_INTERRUPTS();
		if (r == SIFT_STEP_FAILED || left <= 0)
			break;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 Min(left, Min(Max(sift_maintenance_naptime / 20, 100), 1000)), PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/* ---------- SQL ---------- */

Datum
sift_promote(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(&bm25_ops, PG_GETARG_OID(0));
	int			n = sift_mops_sql_promote(&bm25_ops, index);

	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

Datum
sift_merge(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(&bm25_ops, PG_GETARG_OID(0));
	int			n = sift_mops_sql_merge(&bm25_ops, index, PG_NARGS() > 1 && PG_GETARG_BOOL(1));

	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}
