/*
 * col/maint/rewrite.c - the rewrite of segments that hold dead rows
 * when a rewrite is due, requested and done in the background
 * the tick of the rewrite worker
 */
#include "postgres.h"
#include "col/maint/maint_int.h"

#define WHY_BUFFER	"write buffer rows"

double		sift_col_auto_merge_buffer_pct = 1.0;

/*
 * Do the segments the budget counts from (those of at least half the bytes of the largest) overlap each other? A build in parallel, or one that
 * outgrew the memory of a segment writer, leaves peers that all cover the same superblocks, and the rewrite that merges them is the first, not
 * a repeat of the churn that the budget limits. A rewrite leaves peers that do not overlap, and the segments a promotion adds are small.
 */
static bool
peers_overlap(const RewriteView *v)
{
	SiftDirSnapshot *peers = palloc(sizeof(SiftDirSnapshot));
	uint64		large = 0,
				listed = 0;
	bool		overlap;

	for (int i = 0; i < v->n; i++)
		large = Max(large, v->imm[i].bytes);
	peers->nsegs = 0;
	for (int i = 0; i < v->n; i++)
		if (v->imm[i].bytes * 2 >= large)
		{
			peers->segs[peers->nsegs++] = v->imm[i];
			listed += v->seg[i]->nsb;
		}
	overlap = peers->nsegs > 1 && (double) listed >= sift_col_auto_merge_fragmentation * (double) sift_cseg_dir_units(peers);
	pfree(peers);
	return overlap;
}

/*
 * Is the index worth a rewrite of all its segments? A DELETE or UPDATE spread over the table puts a dead row in most units while
 * the share of dead rows stays under dead_rewrite_pct, and each promotion of the write buffer adds a segment over the same superblocks, so the
 * index slowly loses the units that answer from their pre-aggregates and gains units to read. The rule is on what a rewrite changes, measured on
 * the directory and the liveness (no heap page is read): more than sift.col_auto_merge_dirty_pct percent of the units hold a dead row, or the
 * units listed are sift.col_auto_merge_fragmentation times the superblocks they cover, or the write buffer holds more than
 * sift.col_auto_merge_buffer_pct percent of the rows. Why, or NULL. The automatic rewrite also stays within sift.col_auto_merge_wal_pct of the WAL the server writes (wal_budget_spent). It waits too while the blocks the last rewrite freed are held back by a snapshot.
 * With sift.col_auto_merge_partition_kb the rewrite is of one partition at a time: a partition with dirty_pct of its units dirty is due, whatever the
 * share of the whole index, units listed twice are those of the overlays over the partitions, the size of the index does not matter and the segments
 * a rewrite leaves are the partitions.
 */
const char *
maint_rewrite_check(Relation index, bool automatic, int *gates)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col rewrite due", ALLOCSET_SMALL_SIZES);
	RewriteView *v;
	const char *why = NULL;
	bool		partitioned,
				part_dirty = false,
				build_peers;
	uint64		listed = 0,
				dirty = 0,
				rows = 0,
				bytes = 0,
				buffered = 0,
				over_listed = 0,
				over_dirty = 0,
				covered;
	XLogRecPtr	lsn[SIFT_MAX_SEGMENTS];
	uint64		sbytes[SIFT_MAX_SEGMENTS];
	int			nseg;

	sift_dir_read(index, snap);
	maint_use_partitions(snap);
	partitioned = maint_part_kb > 0;
	v = maint_view_open(index, snap, cx);
	nseg = v->n;
	build_peers = peers_overlap(v);
	for (int i = 0; i < v->n; i++)
	{
		const SiftSegEntry *e = &v->imm[i];
		SiftColSegment *seg = v->seg[i];
		uint32		d = 0;

		rows += e->ndocs;
		bytes += e->bytes;
		sbytes[i] = e->bytes;
		lsn[i] = automatic ? maint_header_lsn(index, e) : InvalidXLogRecPtr;
		listed += seg->nsb;
		if (e->flags & SIFT_SEG_DIRTY)
		{
			d = maint_dirty_units(seg);
			dirty += d;
		}
		if (!v->over[i])
			part_dirty |= (uint64) d * 100 > (uint64) sift_col_auto_merge_dirty_pct * seg->nsb;
		else
		{
			over_listed += seg->nsb;
			over_dirty += d;
		}
		sift_cseg_close(seg);
	}
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
		{
			uint64		nrows,
						npages;

			sift_cwbuf_summary(index, &snap->segs[i], &nrows, &npages);
			buffered += nrows;
		}
	covered = sift_cseg_dir_units(snap);
	MemoryContextDelete(cx);
	pfree(snap);
	if (partitioned ? part_dirty || over_dirty * 100 > (uint64) sift_col_auto_merge_dirty_pct * over_listed :
		listed > 0 && dirty * 100 > (uint64) sift_col_auto_merge_dirty_pct * listed)
		why = "dirty units";
	else if (covered > 0 && (!partitioned || over_listed > 0) && (double) listed >= sift_col_auto_merge_fragmentation * covered)
		why = "fragmented units";
	if (why == NULL && buffered > 0 && (double) buffered * 100 > sift_col_auto_merge_buffer_pct * (double) (rows + buffered))
		why = WHY_BUFFER;
	/* the budget is of the rewrite of the segments that churn calls for, not of the first one, which the peers of a build call for; the promotion of a write buffer costs its rows */
	if (why != NULL && automatic && build_peers && gates != NULL && maint_rewrite_all(why))
		*gates |= SIFT_COL_PEERS;
	if (why != NULL && automatic && maint_rewrite_all(why) && !build_peers && maint_wal_budget_spent(lsn, sbytes, nseg, bytes))
	{
		if (gates != NULL)
			*gates |= SIFT_COL_GATE_WAL;
		return NULL;
	}
	/*
	 * A snapshot older than the last rewrite keeps the blocks it freed from being reused, and its rows out of the visibility map: another rewrite
	 * answers no more units and leaves as much again behind (the file grew 1.6 times faster, for 5% more WAL, under a snapshot held through 16 rounds
	 * of churn). The rewrite waits for the horizon when the blocks held back are half the index.
	 */
	if (why != NULL && automatic && sift_store_held_blocks(index) * BLCKSZ * 2 >= bytes)
	{
		if (gates != NULL)
			*gates |= SIFT_COL_GATE_HELD;
		return NULL;
	}
	return why;
}

const char *
maint_rewrite_due(Relation index, bool automatic)
{
	return maint_rewrite_check(index, automatic, NULL);
}

/* Only the write buffer is large: promoting it is the cost of its rows, a rewrite is the cost of the index */
bool
maint_rewrite_all(const char *why)
{
	return strcmp(why, WHY_BUFFER) != 0;
}

/* VACUUM leaves the rewrite to a worker of its own: it is not VACUUM's to hold the table for, and the worker sleeps as sift.maintenance_cost_delay says */
void
maint_rewrite_request(Relation index)
{
	if (!sift_col_auto_merge || !sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
	if (maint_rewrite_due(index, true) != NULL)
		sift_maint_register_worker(index, "sift_col_rewrite_worker_main");
}

/*
 * The peers a build left are rewritten without a VACUUM to ask for it: a table nothing changes is never vacuumed, and its scans would walk the units of every
 * peer for good. A look at the directory (the ranges of the largest segments overlap) comes before the rule, which opens the segments; the request is
 * the one of VACUUM, so that its backoff and the stamp in the index keep the backends that look from starting a worker each.
 */
void
sift_col_rewrite_tick(Relation index)
{
	SiftDirSnapshot *snap;
	SiftDirSnapshot *peers;
	uint64		large = 0,
				ranges = 0;
	int			gates = 0;

	if (!sift_col_auto_merge || !sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
	snap = palloc(sizeof(SiftDirSnapshot));
	peers = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			large = Max(large, snap->segs[i].bytes);
	peers->nsegs = 0;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && snap->segs[i].bytes * 2 >= large)
		{
			peers->segs[peers->nsegs++] = snap->segs[i];
			ranges += (uint64) snap->segs[i].sb_hi + 1 - snap->segs[i].sb_lo;
		}
	if (peers->nsegs > 1 && sift_cseg_dir_units(peers) < ranges && maint_rewrite_check(index, true, &gates) != NULL && (gates & SIFT_COL_PEERS))
		(void) sift_maint_request_worker(index, "sift_col_rewrite_worker_main");
	pfree(peers);
	pfree(snap);
}
