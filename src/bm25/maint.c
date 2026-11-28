/*
 * bm25/maint.c - what the maintenance of a BM25 index adds to the shared one (core/maint/maint.c): the callbacks of its kind, the cost of walking its write buffer, and its
 * background worker
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "bm25/am.h"
#include "core/catname.h"
#include "core/compat.h"
#include "core/limits.h"
#include "core/live/live.h"
#include "core/maint/maint.h"
#include "core/maint/int.h"
#include "core/maint/vacuum.h"
#include "bm25/merge/merge.h"
#include "bm25/wbuf.h"
#include "core/memtrack.h"
#include "core/ops.h"
#include "core/progress.h"
#include "core/quarantine/quarantine.h"
#include "core/reads.h"
#include "core/worker.h"

/* Whatever merge_fanin is, the policy merges (from SIFT_MERGE_ENTRIES entries) before inserters have to make room, and a merge fits a directory */
StaticAssertDecl(SIFT_MERGE_ENTRIES(SIFT_MERGE_MAX_FANIN) + SELF_PROMOTE_SEALED + 1 < ROOM_SEGMENTS, "merge policy acts after the room threshold");
StaticAssertDecl(2 * SIFT_MERGE_MAX_FANIN <= SIFT_DIR_POLICY_MAX, "a merge of the largest merge_fanin overflows a directory");

static bool
is_sift(Relation index)
{
	return index->rd_rel->relam == sift_catalog()->am;
}

static void
bulkdelete_immutable(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	MemoryContext segcx = sift_mem_allocset_create(CurrentMemoryContext, "sift bulkdelete segment");
	SiftSegment *seg = sift_seg_open_once(index, e, segcx);

	sift_vacuum_delay_point();
	sift_seg_bulkdelete(seg, cb, cb_state, stats);
	sift_seg_close(seg);
	MemoryContextDelete(segcx);
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

/* Is the segment still in the directory, immutable, with dead postings to refresh? */
static bool
dirty_segment(Relation index, uint64 gen)
{
	SiftDirSnapshot *snap = sift_dir_read(index);
	bool		dirty = false;

	for (int i = 0; i < snap->nsegs; i++)
		dirty |= snap->segs[i].gen == gen && snap->segs[i].kind == SIFT_SEG_IMMUTABLE && (snap->segs[i].flags & SIFT_SEG_DIRTY);
	pfree(snap);
	return dirty;
}

/* The dead-posting tables of the segments VACUUM cleared docs of and the merges left as they are; a segment is claimed as a merge claims it, so that no step replaces it while it is refreshed */
static void
refresh_dead_tables(Relation index)
{
	SiftDirSnapshot *snap;

	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && (snap->segs[i].flags & SIFT_SEG_DIRTY))
		{
			uint64		gen = snap->segs[i].gen;

			claim(index, gen, true);
			if (dirty_segment(index, gen))
			{
				MemoryContext segcx = sift_mem_allocset_create(CurrentMemoryContext, "sift dead refresh segment");
				SiftSegment *seg = sift_seg_open_once(index, &snap->segs[i], segcx);

				sift_vacuum_delay_point();
				sift_seg_refresh_dead(seg);
				sift_seg_close(seg);
				MemoryContextDelete(segcx);
			}
			unclaim(index, gen);
		}
	pfree(snap);
}

static const SiftMaintOps bm25_ops = {
	.name = "sift",
	.worker = SIFT_WK_BM25_READ_PROMOTE,
	.step_lock = RowExclusiveLock,
	.vacuum_to_fixed_point = true,
	.vacuum_budget = true,
	.is_index = is_sift,
	.seal = sift_wbuf_seal,
	.vacuum_seal = sift_wbuf_seal_vacuum,
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

bool
sift_maint_make_room(Relation index)
{
	return sift_mops_make_room(&bm25_ops, index);
}

IndexBulkDeleteResult *
sift_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	return sift_mops_bulkdelete(&bm25_ops, info, stats, callback, callback_state);
}

static IndexBulkDeleteResult *
bm25_cleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return sift_mops_vacuumcleanup(&bm25_ops, info, stats);
}

IndexBulkDeleteResult *
sift_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return sift_vacuum_cleanup(info, stats, bm25_cleanup);
}

/*
 * What a document of the open write buffer costs each query that walks it (its page passed the bloom filter, which a 256-bit bloom on 900 and more terms a page lets nearly all
 * pages do: 170 cycles for an absent term, 440 for a term of 5% of the documents, against 80 once it is in a segment) and what the promotion costs, once (55,000 cycles a
 * document: 295 M cycles for 5,000, 95 ms with the WAL). Cycles, measured on 200,000 documents with a 5,000-document buffer.
 */
#define WBUF_WALK_PRICE			250.0
#define WBUF_PROMOTE_PRICE		55000.0
/* a maintenance step counts at half, and a buffer under this many documents is not worth a segment (a segment costs every later query about 140,000 cycles, the walk of 600 documents) */
#define WBUF_PROMOTE_HALF		0.5
#define WBUF_READ_MIN_DOCS		2000

/* Does an open write buffer hold enough documents for a segment to be worth it, and (reads_paid) have the queries that walked it paid for its promotion? */
static bool
open_buffer_due(Relation index, bool reads_paid)
{
	SiftDirSnapshot *snap;
	bool		due = false;

	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs && !due; i++)
	{
		double		paid;
		uint64		ndocs,
					sum_dl,
					npages;

		if (snap->segs[i].kind != SIFT_SEG_WBUF_OPEN)
			continue;
		paid = (double) sift_cwbuf_decoded(index, &snap->meta, snap->segs[i].gen) * WBUF_WALK_PRICE;
		if (reads_paid && paid < WBUF_PROMOTE_HALF * WBUF_PROMOTE_PRICE * WBUF_READ_MIN_DOCS)
			continue;
		sift_wbuf_summary(index, &snap->segs[i], &ndocs, &sum_dl, &npages);
		due = ndocs >= WBUF_READ_MIN_DOCS && (!reads_paid || paid >= WBUF_PROMOTE_HALF * WBUF_PROMOTE_PRICE * (double) ndocs);
	}
	pfree(snap);
	return due;
}

/*
 * Have the queries that walked the open write buffer paid for its promotion? A buffer nobody reads has no walks noted, so a table that is only written never promotes before
 * its limit, and the sum is per generation of the buffer, so one promotion ends the debt. The sum is the one this backend holds: a read writes nothing, so what other backends paid is not in it.
 */
bool
sift_maint_wbuf_read_due(Relation index)
{
	return open_buffer_due(index, true);
}

/* A search walked docs of the ndocs documents of the open buffer of generation gen: a buffer under WBUF_READ_MIN_DOCS is not worth a segment, so its reads cost nothing (no sum); else note them in the list of the backend (core/reads.c: it never reaches the metapage, a read logs nothing), and now and then ask whether the reads have paid for the promotion (at most once per sift.maintenance_naptime for a backend) */
void
sift_maint_wbuf_walked(Relation index, uint64 gen, uint64 docs, uint64 ndocs)
{
	static struct
	{
		Oid			index;
		TimestampTz at;
	}			looked[8];
	static int	next_slot = 0;
	TimestampTz now;
	int			slot;

	if (docs == 0 || ndocs < WBUF_READ_MIN_DOCS)
		return;
	sift_cwbuf_decoded_note(index, gen, docs);
	if (!sift_async_maintenance || !IsUnderPostmaster || IsParallelWorker() || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
	now = GetCurrentTimestamp();
	for (slot = 0; slot < lengthof(looked) && looked[slot].index != RelationGetRelid(index); slot++)
		;
	if (slot < lengthof(looked) && now - looked[slot].at < (int64) sift_maintenance_naptime * 1000)
		return;
	if (slot == lengthof(looked))
	{
		slot = next_slot;
		next_slot = (next_slot + 1) % lengthof(looked);
	}
	looked[slot].index = RelationGetRelid(index);
	looked[slot].at = now;
	if (sift_maint_wbuf_read_due(index) && !sift_index_known_out(index))
		sift_maint_register_worker(index, SIFT_WK_BM25_READ_PROMOTE);
}

/*
 * One promotion or merge in a transaction of its own, holding no snapshot: the extents it frees are
 * reusable by the next step, because neither its xid nor its xmin hold the horizon back once it
 * committed. The promotion worker and the merge worker of an index each hold a lock of their own (lockblk): a promotion never waits for
 * a merge, and each makes it the only worker of its job at work on the index; the others leave. A promotion asks for the merge worker, which the segments it adds may need.
 */
static SiftStepResult
worker_step(Oid indexoid, TimestampTz start, BlockNumber lockblk, SiftJob job)
{
	Relation	index;
	SiftStepResult	r = SIFT_STEP_IDLE;
	MemoryContext cx;
	MemoryContext old;

	StartTransactionCommand();
	cx = sift_mem_allocset_create(CurrentMemoryContext, "sift maintenance step");
	old = MemoryContextSwitchTo(cx);
	SiftMemTracker mt_maint_worker_step;

	sift_mem_init_sub(&mt_maint_worker_step, "maint_worker_step", cx, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	index = sift_try_open_index(indexoid, RowExclusiveLock);
	if (index != NULL)
	{
		/* the lock is busy when a VACUUM is at it, which then does the work */
		if (is_sift(index) && ConditionalLockPage(index, lockblk, ExclusiveLock) &&
			maint_lock_mode(index, false, RowExclusiveLock))
		{
			uint64		busy = 0;
			TimestampTz began = GetCurrentTimestamp();

			r = job == SIFT_JOB_PROMOTE ? promote_next(index, false, &busy) : merge_next(index, false, SIFT_GOAL_BACKGROUND, false, &busy);
			maint_unlock_mode(index, RowExclusiveLock);
			if (r == SIFT_STEP_DONE)
			{
				sift_worker_ok(index, job, began);
				if (job == SIFT_JOB_PROMOTE)
					sift_maint_request_worker(index, SIFT_WK_BM25_MAINT);
			}
			else if (r == SIFT_STEP_IDLE && busy != 0)
				r = SIFT_STEP_BUSY;
		}
		else if (is_sift(index))
			r = SIFT_STEP_BUSY;
		if (r == SIFT_STEP_FAILED)
			sift_log_maint(index, "maintenance worker finished", start, -1, -1);
		index_close(index, RowExclusiveLock);
	}
	sift_mem_assert_within(&mt_maint_worker_step);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	CommitTransactionCommand();
	return r;
}

static SiftStepResult
promote_step(Oid indexoid, TimestampTz start, bool last)
{
	return worker_step(indexoid, start, SIFT_WORKER_LOCK_BLKNO, SIFT_JOB_PROMOTE);
}

static SiftStepResult
merge_step(Oid indexoid, TimestampTz start, bool last)
{
	return worker_step(indexoid, start, SIFT_MERGE_WORKER_LOCK_BLKNO, SIFT_JOB_MERGE);
}

static void
worker_main(Datum main_arg, bool promote)
{
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);

	if (!sift_maint_stay_begin(indexoid, promote ? SIFT_WK_BM25_READ_PROMOTE : SIFT_WK_BM25_MAINT))
		return;
	if (promote)
	{
		Relation	index;

		StartTransactionCommand();
		index = sift_try_open_index(indexoid, RowExclusiveLock);
		if (index != NULL)
		{
			/* the reads that asked are in the memory of their backend: judged again is whether the buffer is still worth a segment */
			if (is_sift(index) && open_buffer_due(index, false))
				sift_wbuf_seal(index, true);
			index_close(index, RowExclusiveLock);
		}
		CommitTransactionCommand();
	}
	sift_maint_worker_stay(indexoid, start, promote ? promote_step : merge_step);
}

/* The merge worker of the index */
void
sift_maint_worker_main(Datum main_arg)
{
	worker_main(main_arg, false);
}

/* The promotion worker: it seals the open write buffer first when the reads that walked it asked */
void
sift_read_promote_worker_main(Datum main_arg)
{
	worker_main(main_arg, true);
}

void
sift_bm25_maint_init(void)
{
	sift_mops_register(&bm25_ops);
	sift_wbuf_stage_init();
}
