/*
 * sift_maint.c - promotion of sealed mutable segments, tiered merge, VACUUM
 * callbacks, the orphan sweep and the asynchronous maintenance worker
 *
 *
 * All of it runs under the maintenance lock (LockPage SIFT_MAINT_LOCK_BLKNO),
 * so there is one promoter/merger/vacuumer per index and the segments it
 * reads can only be changed by it. Readers are safe through the store's
 * safexid protocol (sift_store.h P4).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "common/int.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "sift_am.h"
#include "sift_build.h"
#include "sift_maint.h"
#include "sift_mutable.h"
#include "sift_sql.h"

#define MERGE_FANIN			8		/* segments of a tier that trigger a merge */
#define MERGE_MAX_SEGMENTS	32		/* more segments than this trigger a merge */
#define REQUEST_INTERVAL_MS	2000

/* ---------- locking and directory helpers ---------- */

static bool
maint_lock(Relation index, bool wait)
{
	if (wait)
	{
		LockPage(index, SIFT_MAINT_LOCK_BLKNO, ExclusiveLock);
		return true;
	}
	return ConditionalLockPage(index, SIFT_MAINT_LOCK_BLKNO, ExclusiveLock);
}

static void
maint_unlock(Relation index)
{
	UnlockPage(index, SIFT_MAINT_LOCK_BLKNO, ExclusiveLock);
}

static SiftScoreRef
score_ref(Relation index)
{
	const SiftOptions *opts = sift_index_options(index);

	return (SiftScoreRef) {(float4) opts->k1, (float4) opts->b, 0};
}

/* ---------- switching segments in the directory ---------- */

typedef struct Switch
{
	const uint64 *old;			/* generations replaced */
	int			nold;
	const SiftSegEntry *neu;	/* the replacement or NULL */
} Switch;

static void
switch_segments(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	Switch	   *sw = arg;
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
		meta->ndocs += sw->neu->ndocs;
		meta->sum_dl += sw->neu->sum_dl;
	}
	dir->nsegs = n;
}

/* ---------- promotion ---------- */

typedef struct PromoDoc
{
	SiftKey		key;
	uint16		dl;
	int			nterms;
	SiftTermFreq *terms;
} PromoDoc;

static int
promo_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(((const PromoDoc *) a)->key, ((const PromoDoc *) b)->key);
}

/* Copies the live docs of a mutable segment, sorted by key; a repeated tid keeps its first record */
static PromoDoc *
read_docs(Relation index, const SiftSegEntry *seg, int64 *ndocs)
{
	SiftMutReader *r = sift_mut_open(index, seg, CurrentMemoryContext, NULL, NULL);
	SiftMutDoc	d;
	int64		cap = 256,
				n = 0,
				m = 0;
	PromoDoc   *docs = palloc(cap * sizeof(PromoDoc));

	while (sift_mut_next(r, &d))
	{
		const char *t = d.terms;
		SiftTermFreq *terms;

		if (d.deleted)
			continue;
		if (n == cap)
			docs = repalloc_huge(docs, (cap *= 2) * sizeof(PromoDoc));
		terms = palloc(Max(d.nterms, 1) * sizeof(SiftTermFreq));
		for (int i = 0; i < d.nterms; t += d.len[i++])
		{
			char	   *copy = palloc(Max(d.len[i], 1));

			memcpy(copy, t, d.len[i]);
			terms[i] = (SiftTermFreq) {copy, d.len[i], d.tf[i], d.hash[i]};
		}
		docs[n++] = (PromoDoc) {sift_key_from_tid(&d.tid), d.dl, d.nterms, terms};
	}
	sift_mut_close(r);
	qsort(docs, n, sizeof(PromoDoc), promo_cmp);
	for (int64 i = 0; i < n; i++)
		if (m == 0 || docs[i].key != docs[m - 1].key)
			docs[m++] = docs[i];
	*ndocs = m;
	return docs;
}

/* Writes the docs (ascending keys) as one immutable segment; false when there are none */
static bool
write_docs(Relation index, SiftWriteTarget *wt, const PromoDoc *docs, int64 ndocs, uint64 gen,
		   SiftSegEntry *entry)
{
	SiftScoreRef ref = score_ref(index);
	Size		budget = (Size) maintenance_work_mem * 1024;
	SiftInvBuf *inv = sift_inv_create(budget, CurrentMemoryContext);
	SiftRun   **runs = palloc(sizeof(SiftRun *));
	int			nruns = 0,
				maxruns = 1;

	for (int64 i = 0; i < ndocs; i++)
	{
		ItemPointerData tid;

		sift_key_to_tid(docs[i].key, &tid);
		sift_inv_add(inv, &tid, docs[i].terms, docs[i].nterms, docs[i].dl);
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

static void
promote_one(Relation index, const SiftSegEntry *sealed)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift promote", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	int64		ndocs;
	PromoDoc   *docs;
	SiftWriteTarget *wt;
	SiftSegEntry neu;
	bool		any;
	SiftExtent *pages;
	int			npages;
	Switch		sw = {.old = &sealed->gen,.nold = 1};

	sift_mut_flag_sealed(index, sealed);
	docs = read_docs(index, sealed, &ndocs);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	any = write_docs(index, wt, docs, ndocs, sift_store_new_gen(index), &neu);
	sift_wt_finish(wt);
	pages = sift_mut_pages(index, sealed, &npages);
	sw.neu = any ? &neu : NULL;
	sift_dir_update(index, &(SiftDirChange) {.fn = switch_segments,.arg = &sw,.release = pages,
			.nrelease = npages,.extra = InvalidBuffer});
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static const SiftSegEntry *
first_sealed(const SiftDirSnapshot *snap)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_MUTABLE_SEALED)
			return &snap->segs[i];
	return NULL;
}

static int
promote_locked(Relation index, bool seal_active)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftSegEntry *s;
	int			n = 0;

	if (seal_active)
		sift_mut_seal(index, true);
	for (;;)
	{
		sift_dir_read(index, snap);
		if ((s = first_sealed(snap)) == NULL)
			break;
		promote_one(index, s);
		n++;
		CHECK_FOR_INTERRUPTS();
	}
	pfree(snap);
	return n;
}

int
sift_maint_promote(Relation index, bool wait)
{
	int			n;

	if (!maint_lock(index, wait))
		return 0;
	n = promote_locked(index, false);
	maint_unlock(index);
	return n;
}

/* ---------- merge ---------- */

static int
tier_of(const SiftSegEntry *s, uint64 base)
{
	uint64		r = s->bytes / Max(base, 1);
	int			t = 0;

	for (; r >= MERGE_FANIN; r /= MERGE_FANIN)
		t++;
	return t;
}

static int
bytes_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(((const SiftSegEntry *) a)->bytes, ((const SiftSegEntry *) b)->bytes);
}

/*
 * The segments to merge: MERGE_FANIN or more of one tier, else the smallest
 * MERGE_FANIN when the index has too many, else a lone segment that is more
 * than half dead. force: all immutable segments, a lone one only if some of
 * its docs are dead (the rewrite drops them and their terms). Returns their
 * number in sel.
 */
static int
pick_merge(Relation index, const SiftDirSnapshot *snap, bool force, SiftSegEntry *sel)
{
	uint64		base = (uint64) sift_index_options(index)->mutable_limit * 1024;
	SiftSegEntry imm[SIFT_MAX_SEGMENTS];
	int			n = 0;

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			imm[n++] = snap->segs[i];
	qsort(imm, n, sizeof(SiftSegEntry), bytes_cmp);
	if (force)
	{
		memcpy(sel, imm, n * sizeof(SiftSegEntry));
		return n > 1 || (n == 1 && (imm[0].flags & SIFT_SEG_DIRTY)) ? n : 0;
	}
	for (int i = 0; i + MERGE_FANIN <= n; i++)
		if (tier_of(&imm[i], base) == tier_of(&imm[i + MERGE_FANIN - 1], base))
		{
			int			k = 0;

			for (int j = i; j < n && tier_of(&imm[j], base) == tier_of(&imm[i], base) && k < 2 * MERGE_FANIN; j++)
				sel[k++] = imm[j];
			return k;
		}
	if (snap->nsegs > MERGE_MAX_SEGMENTS && n >= 2)
	{
		memcpy(sel, imm, Min(n, MERGE_FANIN) * sizeof(SiftSegEntry));
		return Min(n, MERGE_FANIN);
	}
	for (int i = 0; i < n; i++)
	{
		SiftSegment *seg;
		bool		mostly_dead;

		if (!(imm[i].flags & SIFT_SEG_DIRTY))
			continue;
		seg = sift_seg_open(index, &imm[i], CurrentMemoryContext);
		mostly_dead = sift_seg_live_docs(seg) * 2 < imm[i].ndocs;
		sift_seg_close(seg);
		if (mostly_dead)
		{
			sel[0] = imm[i];
			return 1;
		}
	}
	return 0;
}

static void
merge_segments(Relation index, const SiftSegEntry *in, int n)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftScoreRef ref = score_ref(index);
	SiftRun   **runs = palloc(n * sizeof(SiftRun *));
	uint64	   *gens = palloc(n * sizeof(uint64));
	SiftExtent *rel = palloc(0);
	int			nrel = 0,
				nruns = 0;
	SiftSegEntry neu;
	SiftWriteTarget *wt;
	Switch		sw = {.old = gens,.nold = n};

	for (int i = 0; i < n; i++)
	{
		SiftSegment *seg = sift_seg_open(index, &in[i], cx);
		SiftRun    *run = sift_run_from_segment(seg, &ref, cx);
		int			k;
		SiftExtent *ext = sift_store_immutable_extents(index, &in[i], &k);

		sift_seg_close(seg);
		if (run)
			runs[nruns++] = run;
		gens[i] = in[i].gen;
		rel = repalloc(rel, (nrel + k) * sizeof(SiftExtent));
		memcpy(rel + nrel, ext, k * sizeof(SiftExtent));
		nrel += k;
	}
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	if (nruns > 0)
	{
		sift_runs_write(runs, nruns, wt, sift_store_new_gen(index), &ref, &neu);
		sw.neu = &neu;
	}
	sift_wt_finish(wt);
	sift_dir_update(index, &(SiftDirChange) {.fn = switch_segments,.arg = &sw,.release = rel,
			.nrelease = nrel,.extra = InvalidBuffer});
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static bool
merge_locked(Relation index, bool force)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	int			n;

	sift_dir_read(index, snap);
	n = pick_merge(index, snap, force, sel);
	pfree(snap);
	if (n > 0)
		merge_segments(index, sel, n);
	return n > 0;
}

bool
sift_maint_merge_step(Relation index, bool wait)
{
	bool		merged;

	if (!maint_lock(index, wait))
		return false;
	merged = merge_locked(index, false);
	maint_unlock(index);
	return merged;
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

/* ---------- VACUUM ---------- */

IndexBulkDeleteResult *
sift_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback,
				  void *callback_state)
{
	Relation	index = info->index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	maint_lock(index, true);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftSegment *seg = sift_seg_open(index, &snap->segs[i], CurrentMemoryContext);

			sift_seg_bulkdelete(seg, callback, callback_state, stats);
			sift_seg_close(seg);
		}
		else
			sift_mut_bulkdelete(index, &snap->segs[i], callback, callback_state, stats);
	}
	maint_unlock(index);
	pfree(snap);
	return stats;
}

IndexBulkDeleteResult *
sift_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	index = info->index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	if (info->analyze_only)
		return stats;
	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	maint_lock(index, true);
	promote_locked(index, false);
	merge_locked(index, false);
	sift_recycle(index);
	sift_maint_orphan_sweep(index);
	maint_unlock(index);
	sift_dir_read(index, snap);
	stats->num_pages = RelationGetNumberOfBlocks(index);
	stats->estimated_count = false;
	if (stats->num_index_tuples == 0)
		for (int i = 0; i < snap->nsegs; i++)
			stats->num_index_tuples += snap->segs[i].ndocs;
	pfree(snap);
	return stats;
}

/* ---------- asynchronous maintenance ---------- */

static void
stamp_request(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->maint_requested_at = *(TimestampTz *) arg;
}

bool
sift_maint_request_async(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TimestampTz now = GetCurrentTimestamp();
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;
	bool		started;

	sift_dir_read(index, snap);
	started = now - snap->meta.maint_requested_at >= REQUEST_INTERVAL_MS * INT64CONST(1000);
	pfree(snap);
	if (!started)
		return false;
	sift_dir_update(index, &(SiftDirChange) {.fn = stamp_request,.arg = &now,.extra = InvalidBuffer});
	bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "sift_maint_worker_main");
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift maintenance (index %u)", RelationGetRelid(index));
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift maintenance");
	bw.bgw_main_arg = ObjectIdGetDatum(RelationGetRelid(index));
	memcpy(bw.bgw_extra, &MyDatabaseId, sizeof(Oid));
	return RegisterDynamicBackgroundWorker(&bw, &handle);
}

/*
 * Promotion is asynchronous when allowed. Back-pressure: with many sealed
 * segments waiting, the inserter promotes itself unless maintenance is busy
 * (writes never block).
 */
void
sift_maint_after_seal(Relation index)
{
	SiftDirSnapshot *snap;
	int			nsealed = 0;

	if (sift_mut_seal(index, false) == 0)
		return;
	if (sift_async_maintenance)
		sift_maint_request_async(index);
	snap = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		nsealed += snap->segs[i].kind == SIFT_SEG_MUTABLE_SEALED;
	pfree(snap);
	if (nsealed >= 4)
		sift_maint_promote(index, false);
}

void
sift_maint_worker_main(Datum main_arg)
{
	Oid			indexoid = DatumGetObjectId(main_arg);
	Oid			dboid;
	Relation	index;

	memcpy(&dboid, MyBgworkerEntry->bgw_extra, sizeof(Oid));
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	index = try_index_open(indexoid, RowExclusiveLock);
	if (index != NULL && index->rd_rel->relam == sift_catalog()->am)
	{
		/* the lock is busy when another worker or a VACUUM is at it, which then does the work */
		while (sift_maint_promote(index, false) > 0 || sift_maint_merge_step(index, false))
			CHECK_FOR_INTERRUPTS();
		index_close(index, RowExclusiveLock);
	}
	else if (index != NULL)
		index_close(index, RowExclusiveLock);
	PopActiveSnapshot();
	CommitTransactionCommand();
}

/* ---------- SQL ---------- */

static Relation
open_sift_index(Oid indexoid)
{
	Relation	index = index_open(indexoid, RowExclusiveLock);

	if (index->rd_rel->relam != sift_catalog()->am)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sift index", RelationGetRelationName(index))));
	return index;
}

/* Seals the active segment and promotes every sealed one; returns the number promoted */
Datum
sift_promote(PG_FUNCTION_ARGS)
{
	Relation	index = open_sift_index(PG_GETARG_OID(0));
	int			n;

	maint_lock(index, true);
	n = promote_locked(index, true);
	maint_unlock(index);
	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

/* Runs merge steps until the policy asks for none; force merges all immutable segments into one */
Datum
sift_merge(PG_FUNCTION_ARGS)
{
	Relation	index = open_sift_index(PG_GETARG_OID(0));
	bool		force = PG_NARGS() > 1 && PG_GETARG_BOOL(1);
	int			n = 0;

	maint_lock(index, true);
	promote_locked(index, false);
	while (merge_locked(index, force && n == 0))
	{
		n++;
		CHECK_FOR_INTERRUPTS();
	}
	maint_unlock(index);
	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}
