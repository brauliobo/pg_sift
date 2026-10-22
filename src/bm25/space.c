/*
 * bm25/space.c - giving space back to the file: the orphan sweep, the truncation of the free end and the VACUUM space step
 * that moves the segments at the end of the file down (core/store.h P7).
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
#include "core/cmp.h"

/* VACUUM's truncation of a heap gives back at least this many blocks, or a 16th of the relation */
#define TRUNCATE_MIN_BLOCKS	1000
#define TRUNCATE_FRACTION	16

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
 * is cut (core/store.h P7). The caller holds the maintenance lock exclusively.
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
		SIFT_BISECT(BlockNumber, mid, lo, hi, sift_store_free_below(rel, mid, SPACE_MIN_RUN) < (int64) moved);
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

/* Is a free run of blocks worth the exclusive lock of the space step (the screen of sift_maint_space) */
bool
sift_maint_space_worth(int64 blocks, BlockNumber nblocks)
{
	return space_worth(blocks, nblocks);
}

void
sift_maint_space(Relation rel, SiftRelocatePage relocate_page, int elevel)
{
	sift_maint_space_budget(rel, relocate_page, elevel, (uint64) sift_vacuum_rewrite_limit << 20);
}

/* The space step with a cap on the bytes it may copy under the lock (0: only the free tail is cut) */
void
sift_maint_space_budget(Relation rel, SiftRelocatePage relocate_page, int elevel, uint64 budget)
{
	const char *what = rel->rd_rel->relkind == RELKIND_INDEX ? "index" : "table";
	BlockNumber nblocks;
	BlockNumber freed = 0;

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
