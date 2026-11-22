/*
 * col/v1/maint/rewrite.c - why an index is worth a rewrite of its segments (sift.col_maintenance_due, sift.col_maintain, sift.col_maintenance_status)
 * the price of the reads that ask for the promotion of the write buffer and the merge of the overlays
 */
#include "postgres.h"
#include <math.h>

#include "col/v1/maint/maint_int.h"
#include "core/worker.h"
#include "core/memtrack.h"

#define WHY_BUFFER	"write buffer rows"
#define WHY_OVERLAY	"overlay units"
#define WHY_SCOPE	"scope segments"

double		sift_col_auto_merge_buffer_pct = 1.0;
uint64		maint_pass_bytes = 0;	/* the bytes of overlapping runs the rewrite pass in hand has rewritten */
uint64		maint_pass_gen = 0;		/* the newest generation the rolling rewrite pass in hand started from (0: not begun): the partitions of it are the ones still to rewrite */

/*
 * The price of a row of the rewrite, instructions and WAL in instructions: 930 M instructions and 2.9 to 3.5 MB of WAL for 300,000 rows (3,100 a row), and a MB of WAL weighs
 * 0.6 G instructions (a G of instructions weighs a third, a MB of WAL a fifth), so about 9,000 a row; a maintenance step counts at half, so a read asks when what the reads owe
 * (sift_col_vacuum_tick) reaches half of it.
 */
#define ROW_PRICE	9000.0

/*
 * What a unit costs a read that need not have read it: 26,000 instructions to open and decode (the per-unit figure of the scan) and the block it reads when its page is cold, in the
 * instructions a row of the rewrite is priced in.
 */
#define UNIT_PRICE	(26000.0 + MAINT_BLOCK_READ)

/* What a row of a unit costs a read of one key in it, which scans the rows of the unit to find the key's (0.15 M instructions for a unit of 7,400 rows: 20 a row, the lowest of the statements measured) */
#define UNIT_ROW_PRICE	20.0

/* What a segment costs a read that lists it, in instructions, even when every unit of it is pruned (15,600 measured) */
#define SEG_PRICE	15600.0

/* A scan of a key-cut index begins: only its overlays are weighed against the scans */
void
sift_col_scan_note(Relation index, const struct SiftColSource *src)
{
	if (sift_col_key_cut(sift_col_rel(index)))
		sift_scans_note(index, sift_col_source_meta(src));
}

/*
 * Is the merge of the small overlays of a key-cut index worth its rows? A request for a key reads, in each overlay whose units cover it, a unit that the merged overlay would hold in one
 * with the others: the units spared for a request of a key the overlays hold are the widths of their key ranges added, less the width of them together, over the width of them together.
 * The requests of an index follow its data: the keys that get new rows (the overlays) are the ones asked for, so that a key outside them is not counted as one that is. The requests are the scans of the index (the aggregate scan counts one too) that its metapage counts
 * (core/reads.c), which neither pg_stat_reset() nor a restart takes, and the merge is due when what they paid comes to half of what the rewrite of the overlays costs (as
 * the reads of sift_col_vacuum_tick do). A unit costs a read to open and, when it holds more rows than the key has, to scan them: the overlays of a promotion are as wide as the units of the index, so that
 * a unit spared is worth its rows (7,400 of them: 0.15 M instructions, not 46,000): an index no one reads, or overlays that hold few keys, are left alone.
 */
static bool
overlay_worth(Relation index, const SiftMetaPageData *meta, const OverlayEv *ev)
{
	double		scans = (double) sift_scans_paid(index, meta),
				spared = (ev->sum - ev->cover) / Max(ev->cover, 1.0),
				unit = UNIT_PRICE + UNIT_ROW_PRICE * (double) ev->rows / Max(ev->units, 1);

	if (ev->forced)
		return scans > 0;
	return spared > 0 && scans * (spared * unit + (ev->n - 1) * SEG_PRICE) >= MAINT_HALF * ROW_PRICE * (double) ev->rows;
}

/*
 * What a row of the write buffer costs the first statement of each backend that reads it (decode, sort, dictionaries and encode of its batches: 115 M instructions for 19,619 rows,
 * 5,900 a row) and what its promotion costs, once, in instructions and WAL (364 M instructions and 714 KB for the same rows: 18,600 a row).
 */
#define WBUF_READ_ROW_PRICE		5900.0
#define WBUF_PROMOTE_ROW_PRICE	18600.0

/*
 * Have the backends that read the open write buffer paid for its promotion? The rows they decoded cost WBUF_READ_ROW_PRICE each; the promotion costs its rows once, and a
 * maintenance step counts at half. A buffer under a quarter of a unit is not worth a segment, which costs every later read UNIT_PRICE + SEG_PRICE (the rule of
 * sift_cwbuf_seal_vacuum). A buffer no one reads has no rows decoded, so a table that is only written never promotes before its limit.
 */
bool
maint_wbuf_read_due(Relation index)
{
	SiftDirSnapshot *snap;
	const double least = SIFT_COL_UNIT_MAX_ROWS / 4;
	bool		due = false;

	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs && !due; i++)
	{
		double		paid;
		uint64		nrows,
					npages;

		if (snap->segs[i].kind != SIFT_SEG_WBUF_OPEN)
			continue;
		paid = (double) sift_cwbuf_decoded(index, &snap->meta, snap->segs[i].gen) * WBUF_READ_ROW_PRICE;
		if (paid < MAINT_HALF * WBUF_PROMOTE_ROW_PRICE * least)
			continue;
		sift_cwbuf_summary(index, &snap->segs[i], &nrows, &npages);
		due = (double) nrows >= least && paid >= MAINT_HALF * WBUF_PROMOTE_ROW_PRICE * (double) nrows;
	}
	pfree(snap);
	return due;
}

/*
 * Does the write buffer hold more than sift.col_auto_merge_buffer_pct percent of the rows of the index (the reason 'write buffer rows' of sift.col_maintenance_due)? Its rows are read as one
 * in-memory unit per superblock they touch on every query until a promotion, which costs the rows once: the VACUUM that ends asks for it (maint_buffer_request)
 */
static bool
buffer_share_due(Relation index, const SiftDirSnapshot *snap, uint64 rows)
{
	uint64		buffered = 0;

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_WBUF_OPEN || snap->segs[i].kind == SIFT_SEG_WBUF_SEALED)
		{
			uint64		nrows,
						npages;

			sift_cwbuf_summary(index, &snap->segs[i], &nrows, &npages);
			buffered += nrows;
		}
	return buffered > 0 && (double) buffered * 100 > sift_col_auto_merge_buffer_pct * (double) (rows + buffered);
}

bool
maint_wbuf_share_due(Relation index)
{
	SiftDirSnapshot *snap;
	uint64		rows = 0;
	bool		due;

	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			rows += snap->segs[i].ndocs;
	due = buffer_share_due(index, snap, rows);
	pfree(snap);
	return due;
}

/* The end of a VACUUM: a write buffer that holds more than its share of the rows is sealed and promoted by the worker that stays for promotions (the rest of what it left is the merges' of the policy) */
void
maint_buffer_request(Relation index)
{
	if (!sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
	if (maint_wbuf_share_due(index))
		sift_maint_register_worker(index, SIFT_WK_COL_READ_PROMOTE);
}

/* The end of a statement: what it decoded of the buffer and paid for the marks of the log goes to the metapage, unless the sums meet the rules already */
void
sift_col_reads_flush(Relation index)
{
	sift_reads_flush(index, maint_wbuf_read_due, sift_col_fold_due);
}

/* The worker that seals and promotes the open write buffer, when the reads of the server have paid for it (a table that opted out of autovacuum is promoted too: no VACUUM is asked) */
bool
sift_col_promote_tick(Relation index)
{
	if (!sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return false;
	return maint_wbuf_read_due(index) && sift_maint_request_worker(index, SIFT_WK_COL_READ_PROMOTE);
}

/*
 * Is the index worth a rewrite of all its segments? A DELETE or UPDATE spread over the table puts a dead row in most units while
 * the share of dead rows stays under dead_rewrite_pct, and each promotion of the write buffer adds a segment over the same superblocks, so the
 * index slowly loses the units that answer from their pre-aggregates and gains units to read. The rule is on what a rewrite changes, measured on
 * the directory and the liveness (no heap page is read): a unit holds a dead row, or the
 * units listed are sift.col_auto_merge_fragmentation times the superblocks they cover, or the write buffer holds more than
 * sift.col_auto_merge_buffer_pct percent of the rows. Why, or NULL. Nothing starts a rewrite from it: sift.col_maintenance_status tells which reasons ask for a REINDEX,
 * and sift.col_maintain(index) runs the rewrite when it is asked for.
 * With partitions the rewrite is of one partition at a time: a partition with a dirty unit is due, whatever the
 * share of the whole index, units listed twice are those of the overlays over the partitions, the size of the index does not matter and the segments
 * a rewrite leaves are the partitions.
 */
const char *
maint_rewrite_due(Relation index)
{
	SiftDirSnapshot *snap;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col rewrite due", ALLOCSET_SMALL_SIZES);
	SiftMemTracker mt_col_rewrite;

	sift_mem_init_sub(&mt_col_rewrite, "col_rewrite", cx, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	RewriteView *v;
	const char *why = NULL;
	bool		partitioned,
				overlay_due,
				*take,
				conn_stale,
				buffer_due,
				stale = false;
	uint64		listed = 0,
				dirty = 0,
				rows = 0,
				over_listed = 0,
				covered,
				shared,
				shared_listed;
	int			key_overlap;
	OverlayEv	ev;

	maint_pass_bytes = 0;
	maint_pass_gen = 0;
	maint_overlay_asked = false;
	maint_redecided = false;
	snap = sift_col_dir_read(index, false);
	maint_use_partitions(index, snap);
	partitioned = maint_part_kb > 0;
	v = maint_view_open(index, snap, cx);
	take = MemoryContextAllocZero(cx, Max(v->n, 1) * sizeof(bool));
	conn_stale = v->conn_stale;
	key_overlap = v->nkover;
	overlay_due = maint_overlay_set(index, v, take, &ev) && overlay_worth(index, &snap->meta, &ev);
	for (int i = 0; i < v->n; i++)
	{
		const SiftSegEntry *e = &v->imm[i];
		SiftColSegment *seg = v->seg[i];

		rows += e->ndocs;
		if (e->flags & SIFT_SEG_UNIT)
		{
			sift_cseg_close(seg);
			continue;
		}
		listed += seg->nsb;
		stale |= v->stale[i];
		if (e->flags & SIFT_SEG_DIRTY)
			dirty += maint_dirty_units(seg);
		/* the partitions of a key-cut index are told by size: what a rolling pass writes has superblocks in common with its neighbours, and the density does not say it is no partition */
		if (!(partitioned && sift_col_key_cut(sift_col_rel(index)) ? maint_is_partition(e) : !v->over[i] && !v->kover[i]))
			over_listed += seg->nsb;
		sift_cseg_close(seg);
	}
	buffer_due = buffer_share_due(index, snap, rows);
	covered = sift_cseg_dir_units(snap);
	shared = sift_cseg_dir_shared(snap, &shared_listed);
	MemoryContextDelete(cx);
	pfree(snap);
	if (dirty > 0)
		why = "dirty units";
	/*
	 * Runs of a build that are as large as partitions but each hold rows of every key (a lookup of one key reads a unit in each of them) are neither overlays nor dirty,
	 * and the units listed twice may be too few to call them fragmented. The rewrite cuts them by key, once.
	 */
	else if (key_overlap >= 2)
		why = "key ranges overlap";
	else if (covered > 0 && (!partitioned || over_listed > 0) && (double) listed >= sift_col_auto_merge_fragmentation * covered)
		why = "fragmented units";
	/* the peers of a key_cut build (each holds every key) have ranges of different lengths: they list the superblocks they share as many times as they overlap, not as often as the covered ones */
	else if (maint_after_build && sift_col_key_cut(sift_col_rel(index)) && shared > 1 && (double) shared_listed >= sift_col_auto_merge_fragmentation * shared)
		why = "fragmented units";
	else if (overlay_due)
	{
		why = WHY_OVERLAY;
		maint_overlay_asked = true;
	}
	else if (conn_stale)
		why = "stale connection";
	else if (stale || maint_scope_refill_due(index))
		why = "stale scope";
	else if (sift_col_rel(index)->nscope > 0 && maint_scope_merge_due(index, maint_after_build))
		why = WHY_SCOPE;
	if (why == NULL && buffer_due)
		why = WHY_BUFFER;
	sift_mem_assert_within(&mt_col_rewrite);
	return why;
}

/* Only the write buffer is large: promoting it is the cost of its rows, a rewrite is the cost of the index */
bool
maint_rewrite_all(const char *why)
{
	return strcmp(why, WHY_BUFFER) != 0;
}

/* The promotion of the write buffer and the merges of the overlays and of the scope segments are the incremental work of the workers and of VACUUM; the other reasons are met by writing the index again */
bool
maint_reindex_wanted(const char *why)
{
	return maint_rewrite_all(why) && strcmp(why, WHY_OVERLAY) != 0 && strcmp(why, WHY_SCOPE) != 0;
}
