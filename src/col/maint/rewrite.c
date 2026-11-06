/*
 * col/maint/rewrite.c - the rewrite of segments that hold dead rows
 * when a rewrite is due, requested and done in the background
 * the tick of the rewrite worker
 */
#include "postgres.h"
#include "col/maint/maint_int.h"
#include "pgstat.h"

#define WHY_BUFFER	"write buffer rows"

double		sift_col_auto_merge_buffer_pct = 1.0;
int			sift_col_auto_merge_pass_mb = 1024;
uint64		maint_pass_bytes = 0;	/* the bytes of overlapping runs the rewrite pass in hand has rewritten */

/* The worker was started because the reads of a backend owe the rewrite (maint_read_due) */
bool		maint_read_asked = false;

/*
 * The price of a row of the rewrite, instructions and WAL in instructions: 930 M instructions and 2.9 to 3.5 MB of WAL for 300,000 rows (3,100 a row), and a MB of WAL weighs
 * 0.6 G instructions (a G of instructions weighs a third, a MB of WAL a fifth), so about 9,000 a row; a maintenance step counts at half, so a read asks when what the reads owe
 * (sift_col_vacuum_tick) reaches half of it.
 */
#define ROW_PRICE	9000.0
#define MAINT_HALF	0.5

/*
 * What a unit costs a read that need not have read it: 26,000 instructions to open and decode (the per-unit figure of the scan) and the block it reads when its page is cold, in the
 * instructions a row of the rewrite is priced in.
 */
#define UNIT_PRICE	46000.0

/* What a row of a unit costs a read of one key in it, which scans the rows of the unit to find the key's (0.15 M instructions for a unit of 7,400 rows: 20 a row, the lowest of the statements measured) */
#define UNIT_ROW_PRICE	20.0

/* What a segment costs a read that lists it, in instructions, even when every unit of it is pruned (15,600 measured) */
#define SEG_PRICE	15600.0

static double
index_scans(Relation index)
{
	const PgStat_StatTabEntry *tab = pgstat_fetch_stat_tabentry(RelationGetRelid(index));

	return tab ? (double) tab->numscans : 0.0;
}

/*
 * Is the merge of the small overlays of a key-cut index worth its rows? A request for a key reads, in each overlay whose units cover it, a unit that the merged overlay would hold in one
 * with the others: the units spared for a request of a key the overlays hold are the widths of their key ranges added, less the width of them together, over the width of them together.
 * The requests of an index follow its data: the keys that get new rows (the overlays) are the ones asked for, so that a key outside them is not counted as one that is. The requests are the scans the statistics count
 * for the index (the aggregate scan counts one too) since they were reset, and the merge is due when what they paid comes to half of what the rewrite of the overlays costs (as
 * the reads of sift_col_vacuum_tick do). A unit costs a read to open and, when it holds more rows than the key has, to scan them: the overlays of a promotion are as wide as the units of the index, so that
 * a unit spared is worth its rows (7,400 of them: 0.15 M instructions, not 46,000): an index no one reads, or overlays that hold few keys, are left alone.
 */
static bool
overlay_worth(Relation index, const OverlayEv *ev)
{
	double		spared = (ev->sum - ev->cover) / Max(ev->cover, 1.0),
				unit = UNIT_PRICE + UNIT_ROW_PRICE * (double) ev->rows / Max(ev->units, 1);

	return spared > 0 && index_scans(index) * (spared * unit + (ev->n - 1) * SEG_PRICE) >= MAINT_HALF * ROW_PRICE * (double) ev->rows;
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
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const double least = SIFT_COL_UNIT_MAX_ROWS / 4;
	bool		due = false;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs && !due; i++)
	{
		double		paid;
		uint64		nrows,
					npages;

		if (snap->segs[i].kind != SIFT_SEG_WBUF_OPEN)
			continue;
		paid = (double) sift_cwbuf_decoded(index, snap->segs[i].gen) * WBUF_READ_ROW_PRICE;
		if (paid < MAINT_HALF * WBUF_PROMOTE_ROW_PRICE * least)
			continue;
		sift_cwbuf_summary(index, &snap->segs[i], &nrows, &npages);
		due = (double) nrows >= least && paid >= MAINT_HALF * WBUF_PROMOTE_ROW_PRICE * (double) nrows;
	}
	pfree(snap);
	return due;
}

/* The worker that seals and promotes the open write buffer, when the reads of the server have paid for it (a table that opted out of autovacuum is promoted too: no VACUUM is asked) */
bool
sift_col_promote_tick(Relation index)
{
	if (!sift_col_auto_merge || !sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return false;
	return maint_wbuf_read_due(index) && sift_maint_request_worker(index, "sift_col_read_promote_worker_main");
}

/* Do the reads of this backend owe the rewrite of the index? The rows are the directory's: no unit is read */
bool
maint_read_due(Relation index, double owed)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	uint64		rows = 0;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			rows += snap->segs[i].ndocs;
	pfree(snap);
	return owed >= MAINT_HALF * ROW_PRICE * (double) rows;
}

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
 * sift.col_auto_merge_buffer_pct percent of the rows. Why, or NULL. The automatic rewrite also stays within sift.col_auto_merge_wal_pct of the WAL the server writes (wal_budget_spent), which defers the rewrite of the segments and not the promotion of the write buffer. It waits too while the blocks the last rewrite freed are held back by a snapshot.
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
				overlay_due,
				take[SIFT_MAX_SEGMENTS] = {false},
				part_dirty = false,
				buffer_due,
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
	int			nseg,
				key_overlap;
	OverlayEv	ev;

	maint_overlay_asked = false;
	maint_overlay_steps = automatic ? 1 : SIFT_MAX_SEGMENTS;
	sift_dir_read(index, snap);
	maint_use_partitions(snap);
	partitioned = maint_part_kb > 0;
	v = maint_view_open(index, snap, cx);
	nseg = v->n;
	key_overlap = v->nkover;
	build_peers = peers_overlap(v) || key_overlap >= 2;
	overlay_due = maint_overlay_set(index, v, take, &ev) && overlay_worth(index, &ev);
	for (int i = 0; i < v->n; i++)
	{
		const SiftSegEntry *e = &v->imm[i];
		SiftColSegment *seg = v->seg[i];
		uint32		d = 0;

		rows += e->ndocs;
		if (e->flags & SIFT_SEG_UNIT)
		{
			sbytes[i] = 0;
			lsn[i] = InvalidXLogRecPtr;
			sift_cseg_close(seg);
			continue;
		}
		bytes += e->bytes;
		sbytes[i] = e->bytes;
		lsn[i] = automatic ? maint_header_lsn(index, e) : InvalidXLogRecPtr;
		listed += seg->nsb;
		if (e->flags & SIFT_SEG_DIRTY)
		{
			d = maint_dirty_units(seg);
			dirty += d;
		}
		if (!v->over[i] && !v->kover[i])
			part_dirty |= (uint64) d * 100 > (uint64) sift_col_auto_merge_dirty_pct * seg->nsb;
		else
		{
			over_listed += seg->nsb;
			over_dirty += d;
		}
		sift_cseg_close(seg);
	}
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_WBUF_OPEN || snap->segs[i].kind == SIFT_SEG_WBUF_SEALED)
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
	/*
	 * Runs of a build that are as large as partitions but each hold rows of every key (a lookup of one key reads a unit in each of them) are neither overlays nor dirty,
	 * and the units listed twice may be too few to call them fragmented. The rewrite cuts them by key, once; it is the build's, not the churn the WAL budget limits.
	 */
	else if (key_overlap >= 2)
		why = "key ranges overlap";
	else if (covered > 0 && (!partitioned || over_listed > 0) && (double) listed >= sift_col_auto_merge_fragmentation * covered)
		why = "fragmented units";
	else if (overlay_due)
	{
		why = "overlay units";
		maint_overlay_asked = true;
	}
	else if (maint_read_asked && dirty > 0)
		why = "dirty units";
	buffer_due = buffered > 0 && (double) buffered * 100 > sift_col_auto_merge_buffer_pct * (double) (rows + buffered);
	if (why == NULL && buffer_due)
		why = WHY_BUFFER;
	/* the budget is of the rewrite of the segments that churn calls for, not of the first one, which the peers of a build call for; the promotion of a write buffer costs its rows */
	if (why != NULL && automatic && build_peers && gates != NULL && maint_rewrite_all(why))
		*gates |= SIFT_COL_PEERS;
	/* the merge of the overlays is paced by the bytes of a pass (maint_overlay_set), not by the WAL others write: a quiet server is when it costs least */
	if (why != NULL && automatic && maint_rewrite_all(why) && !build_peers && !maint_read_asked && !maint_overlay_asked && maint_wal_budget_spent(lsn, sbytes, nseg, bytes))
	{
		if (gates != NULL)
			*gates |= SIFT_COL_GATE_WAL;
		/* the budget defers the rewrite of the segments, not the promotion of the write buffer, which costs its rows: reads would walk them until the budget is back */
		if (!buffer_due)
			return NULL;
		why = WHY_BUFFER;
	}
	/*
	 * A snapshot older than the last rewrite keeps the blocks it freed from being reused, and its rows out of the visibility map: another rewrite
	 * answers no more units and leaves as much again behind (the file grew 1.6 times faster, for 5% more WAL, under a snapshot held through 16 rounds
	 * of churn). The rewrite waits for the horizon when the blocks held back are half the index.
	 */
	if (why != NULL && automatic && sift_store_held_blocks(index) * BLCKSZ * 2 >= bytes)
	{
		maint_overlay_asked = false;
		if (gates != NULL)
			*gates |= SIFT_COL_GATE_HELD;
		return NULL;
	}
	return why;
}

const char *
maint_rewrite_due(Relation index, bool automatic)
{
	maint_pass_bytes = 0;
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
 * A look at the directory before the rule opens the segments: the segments under half the largest, as many as the requests of the index could pay for if every superblock
 * of them were a unit a request opens (the most the rule can find).
 */
static bool
overlay_maybe(Relation index, const SiftDirSnapshot *snap)
{
	uint64		large = 0,
				rows = 0,
				units = 0;
	int			n = 0;

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			large = Max(large, snap->segs[i].bytes);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE && snap->segs[i].bytes * 2 < large)
		{
			n++;
			rows += snap->segs[i].ndocs;
			units += (uint64) snap->segs[i].sb_hi + 1 - snap->segs[i].sb_lo;
		}
	return n >= 2 && index_scans(index) * ((double) units * UNIT_PRICE + n * SEG_PRICE) >= MAINT_HALF * ROW_PRICE * (double) rows;
}

/*
 * The peers a build left are rewritten without a VACUUM to ask for it: a table nothing changes is never vacuumed, and its scans would walk the units of every
 * peer for good. A look at the directory (the ranges of the largest segments overlap) comes before the rule, which opens the segments; the request is
 * the one of VACUUM, so that its backoff and the stamp in the index keep the backends that look from starting a worker each.
 */
bool
sift_col_rewrite_tick(Relation index, double owed)
{
	SiftDirSnapshot *snap;
	SiftDirSnapshot *peers;
	uint64		large = 0,
				ranges = 0;
	int			gates = 0;
	bool		asked = false;

	if (!sift_col_auto_merge || !sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return false;
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
		asked = sift_maint_request_worker(index, "sift_col_rewrite_worker_main");
	if (!asked && overlay_maybe(index, snap) && maint_rewrite_check(index, true, NULL) != NULL && maint_overlay_asked)
		asked = sift_maint_request_worker(index, "sift_col_rewrite_worker_main");
	pfree(peers);
	pfree(snap);
	return asked || (owed > 0 && maint_read_due(index, owed) && sift_maint_request_worker(index, "sift_col_read_rewrite_worker_main"));
}
