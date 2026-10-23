/*
 * col/maint.c - promotion, merge, VACUUM, the maintenance worker and the SQL functions of sift_col indexes (docs/COL.md 2.6, 2.7).
 * The lock, the directory switch, the room, back-pressure, VACUUM and SQL control flow and the sweep are those of bm25/maint.c
 * (sift_mops_*); what this file supplies in col_ops is a column segment: how a sealed one is promoted and how segments are merged.
 *
 * Intentional differences from the BM25 maintenance (bm25/maint.c), kept so the behaviour of sift_col stays what it was:
 * - a step holds the maintenance lock exclusively, not in RowExclusiveLock with claims on its segments: one step of the index at a time;
 * - a step is not run in a subtransaction: an ERROR goes on to the caller (a worker records it, worker_failed, for the backoff of the shared
 *   flow and sift.workers; the other callers see it) and there is no segment another step could have claimed (busy stays 0);
 * - the policy has no goals: background, VACUUM and explicit merges pick alike; VACUUM merges until the policy asks for none or the rewrite limit is reached;
 * - the worker runs in one transaction until nothing is left to do; VACUUM merges to a fixed point only up to sift.vacuum_rewrite_limit.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "col/am.h"
#include "col/seg.h"
#include "bm25/maint.h"
#include "bm25/merge.h"
#include "core/ops.h"

#define corrupt SIFT_CORRUPT

#ifdef SIFT_TESTING
int			sift_test_col_rollup_panic_armed = 0;
bool		sift_test_col_merge_panic = false;
int			sift_test_col_refresh_sleep_armed = 0;
#endif

#define WHY_BUFFER	"write buffer rows"

/* The automatic rewrite (sift.col_auto_merge*, registered in col/am.c) */
bool		sift_col_auto_merge = false;
int			sift_col_auto_merge_dirty_pct = 30;
double		sift_col_auto_merge_fragmentation = 2.0;
double		sift_col_auto_merge_buffer_pct = 1.0;
double		sift_col_auto_merge_min_churn_pct = 0.0;
int			sift_col_auto_merge_max_active = 0;
int			sift_col_auto_merge_partition_kb = 0;
int			sift_col_auto_merge_max_workers = 0;

#define MAX_PARTS		16		/* segments one merge cuts its output into */
#define REWRITE_SLOT_KEY	0x53520000	/* with 0x51F7 in the advisory lock space: slot s of sift.col_auto_merge_max_workers is key + s */

/*
 * A partition is an immutable segment of at least half of sift.col_auto_merge_partition_kb; the smaller ones are the overlays that promotions and
 * rewrites of a partition left over the same superblocks. 0: no partitions, the rewrite is of every segment into one.
 */
static bool
is_partition(const SiftSegEntry *e)
{
	return sift_col_auto_merge_partition_kb > 0 && e->bytes >= (uint64) sift_col_auto_merge_partition_kb * 512;
}

/* The merges of the maintenance worker sleep like vacuum_cost_delay does (sift.maintenance_cost_delay) and keep to sift.maintenance_wal_rate; its promotions, which the writers wait for, and the rollup refresh do not */
static void
throttle(SiftWriteTarget *wt)
{
	if (MyBackendType == B_BG_WORKER && strcmp(MyBgworkerEntry->bgw_type, "sift maintenance") == 0)
	{
		sift_wt_throttle(wt, sift_maintenance_cost_delay);
		sift_wt_wal_rate(wt, sift_maintenance_wal_rate);
	}
}

static void
promote_one(Relation index, const SiftSegEntry *sealed)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col promote", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	const SiftColSchema *schema = sift_col_rel(index)->schema;
	SiftColBatch **batches;
	int			nb;
	SiftWriteTarget *wt = NULL;
	SiftColSegWriter *w = NULL;
	SiftSegEntry neu;
	SiftExtent *pages;
	int			npages;
	TimestampTz start = GetCurrentTimestamp();
	uint64		rows = 0;

	sift_cwbuf_flag_sealed(index, sealed);
	batches = sift_cwbuf_batches(index, sealed, schema, cx, &nb);
	if (nb > 0)
	{
		int			format = sift_col_format_minor;

		/* the rows of a write buffer written before the kinds were kept may lack them: such a segment is not minor 6, 7 or 8 */
		for (int i = 0; i < nb; i++)
			if (!sift_cbatch_kinds_exact(batches[i]))
				format = Min(format, SIFT_COL_FORMAT_TAGS);
		wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
		w = sift_colw_begin(wt, sift_store_new_gen(index), schema, sift_col_rel(index)->policy, format, cx);
		sift_colw_set_preagg(w, &sift_col_rel(index)->pagg);
		for (int i = 0; i < nb; i++)
			sift_colw_add(w, batches[i]);
		rows = sift_colw_rows(w);
		sift_colw_finish(w, &neu);
		sift_wt_finish(wt);
	}
	pages = sift_cwbuf_pages(index, sealed, &npages);
	sift_maint_replace(index, &sealed->gen, 1, nb > 0 ? &neu : NULL, pages, npages, "promoted a sealed column segment of", start, rows);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static const SiftSegEntry *
first_sealed(const SiftDirSnapshot *snap)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_WBUF_SEALED)
			return &snap->segs[i];
	return NULL;
}

static SiftStepResult
promote_next(Relation index, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftSegEntry *s;

	sift_dir_read(index, snap);
	if ((s = first_sealed(snap)) != NULL)
		promote_one(index, s);
	pfree(snap);
	return s ? SIFT_STEP_DONE : SIFT_STEP_IDLE;
}

/* A segment worth a rewrite alone: more dead rows than dead_rewrite_pct, or an overflow flag that only dead rows account for */
bool
sift_col_mostly_dead(Relation index, const SiftSegEntry *e)
{
	SiftColSegment *seg;
	bool		dead;

	if (!(e->flags & SIFT_SEG_DIRTY))
		return false;
	seg = sift_cseg_open(index, e, CurrentMemoryContext);
	dead = sift_cseg_live_rows(seg) * 100 < e->ndocs * (100 - sift_col_options(index)->dead_rewrite_pct) || sift_cseg_stale_ovf(seg);
	sift_cseg_close(seg);
	return dead;
}

static int
pick_merge(Relation index, const SiftDirSnapshot *snap, bool force, SiftSegEntry *sel)
{
	SiftMergePolicy pol = {.tier_base = (uint64) sift_col_options(index)->write_buffer_limit * 1024, .fanin = SIFT_MERGE_FANIN, .mostly_dead = sift_col_mostly_dead};
	SiftDirSnapshot *overlays;
	int			n,
				k = 0;

	if (force || sift_col_auto_merge_partition_kb == 0)
		return sift_merge_pick(index, &pol, snap, force, sel);
	/* the tiers merge the overlays: the partitions of the same size would go back into one segment, and wait for their rewrite (or for the dead rows to fill them) */
	overlays = palloc(sizeof(SiftDirSnapshot));
	*overlays = *snap;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE || !is_partition(&snap->segs[i]))
			overlays->segs[k++] = snap->segs[i];
	overlays->nsegs = k;
	n = sift_merge_pick(index, &pol, overlays, false, sel);
	pfree(overlays);
	return n > 0 ? n : sift_merge_pick_dead(index, &pol, snap, sel);
}

/* Units of the segment that hold a dead row */
static uint32
dirty_units(SiftColSegment *seg)
{
	uint32		d = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
		d += sift_cseg_dirty(seg, u);
	return d;
}

/* Do the two segments hold a unit of the same superblock? */
static bool
share_superblock(const SiftColSegment *a, const SiftColSegment *b)
{
	for (uint32 i = 0, j = 0; i < a->nsb && j < b->nsb;)
	{
		uint32		x = a->sbdir[i].sb,
					y = b->sbdir[j].sb;

		if (x == y)
			return true;
		if (x < y)
			i++;
		else
			j++;
	}
	return false;
}

/*
 * The immutable segments of a directory, open, and which of them are overlays: those under half of sift.col_auto_merge_partition_kb, and any that holds a unit
 * of a superblock another holds with more rows per unit (a promotion of rows into the holes of old pages is small in rows but has a unit in each superblock it
 * reaches, and its fixed bytes can make it as large as a partition). Without partitions every segment is an overlay.
 */
typedef struct RewriteView
{
	int			n;
	SiftSegEntry imm[SIFT_MAX_SEGMENTS];
	SiftColSegment *seg[SIFT_MAX_SEGMENTS];
	bool		over[SIFT_MAX_SEGMENTS];
} RewriteView;

static RewriteView *
view_open(Relation index, const SiftDirSnapshot *snap, MemoryContext cx)
{
	RewriteView *v = MemoryContextAllocZero(cx, sizeof(RewriteView));

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			v->imm[v->n] = snap->segs[i];
			v->seg[v->n] = sift_cseg_open(index, &v->imm[v->n], cx);
			v->over[v->n] = !is_partition(&v->imm[v->n]);
			v->n++;
		}
	for (int i = 0; i < v->n; i++)
		for (int j = 0; j < v->n && !v->over[i]; j++)
		{
			/* rows per unit, compared as products; the older of two alike is the one to keep */
			uint64		di = v->imm[i].ndocs * v->seg[j]->nsb,
						dj = v->imm[j].ndocs * v->seg[i]->nsb;

			v->over[i] = j != i && (dj > di || (dj == di && v->imm[j].gen > v->imm[i].gen)) && share_superblock(v->seg[i], v->seg[j]);
		}
	return v;
}

/*
 * The segments one step of the rewrite (SIFT_GOAL_REWRITE) replaces, in sel. With no partitions (sift.col_auto_merge_partition_kb 0) all of them. Else, in
 * this order: the overlays together into one segment, when there are several or one has a dead row (cheap, and cut into partitions they would be overlays
 * again); when the units listed per superblock covered ask for it and one overlay is left, that overlay, the segments that hold a unit of a superblock
 * it holds and the dirty partitions, cut into partitions (an overlay over every partition is the rewrite of the index, once); else the partition the most of
 * whose units hold a dead row (over sift.col_auto_merge_dirty_pct), alone, so that the step costs a partition and the space it needs is one. Each step
 * leaves nothing of what it picked for the next one, so the steps end. cut: the output is cut into partitions.
 */
static int
pick_rewrite(Relation index, const SiftDirSnapshot *snap, SiftSegEntry *sel, bool *cut)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col rewrite pick", ALLOCSET_SMALL_SIZES);
	RewriteView *v = view_open(index, snap, cx);
	bool		take[SIFT_MAX_SEGMENTS] = {false},
				dirty_part[SIFT_MAX_SEGMENTS] = {false};
	int			nsel = 0,
				nover = 0,
				over = 0,
				worst = -1;
	uint64		worst_share = 0,
				listed = 0;
	bool		dirty_over = false;

	*cut = false;
	for (int i = 0; i < v->n; i++)
	{
		uint32		d = (v->imm[i].flags & SIFT_SEG_DIRTY) ? dirty_units(v->seg[i]) : 0;

		listed += v->seg[i]->nsb;
		if (sift_col_auto_merge_partition_kb == 0)
			take[i] = true;
		else if (v->over[i])
		{
			nover++;
			over = i;
			dirty_over |= d > 0;
		}
		else if ((uint64) d * 100 > (uint64) sift_col_auto_merge_dirty_pct * v->seg[i]->nsb)
		{
			dirty_part[i] = true;
			if ((uint64) d * 1000 / v->seg[i]->nsb > worst_share)
			{
				worst = i;
				worst_share = (uint64) d * 1000 / v->seg[i]->nsb;
			}
		}
	}
	if (nover >= 2 || dirty_over)
		memcpy(take, v->over, v->n);
	else if (nover == 1 && (double) listed >= sift_col_auto_merge_fragmentation * (double) sift_cseg_dir_units(snap))
	{
		for (int i = 0; i < v->n; i++)
			take[i] = i == over || dirty_part[i] || share_superblock(v->seg[i], v->seg[over]);
		*cut = true;
	}
	else if (worst >= 0)
		take[worst] = *cut = true;
	for (int i = 0; i < v->n; i++)
		if (take[i])
			sel[nsel++] = v->imm[i];
	for (int i = 0; i < v->n; i++)
		sift_cseg_close(v->seg[i]);
	MemoryContextDelete(cx);
	/* one clean segment is the end state */
	return nsel > 1 || (nsel == 1 && (sel[0].flags & SIFT_SEG_DIRTY)) ? nsel : 0;
}

/*
 * Merges the segments in[0..n) into one, or into partitions when part_bytes is set: the live bytes they hold are cut into as many segments of about
 * part_bytes each as fit max_parts (the new ones take the place of the first of the old ones, in one switch of the directory).
 */
static void
merge_segments(Relation index, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftCPaggSpec *pagg, const SiftSegEntry *in, int n,
			   int format, SiftColFreezeFn freeze, void *freeze_arg, uint64 part_bytes, int max_parts)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	MemoryContext pcx = NULL;
	SiftColSegment **seg = palloc(n * sizeof(SiftColSegment *));
	SiftColBatch **part = palloc(n * sizeof(SiftColBatch *));
	SiftColBatch **use = palloc(n * sizeof(SiftColBatch *));
	uint32	   *cur = palloc0(n * sizeof(uint32));
	SiftMaintInputs mi = sift_maint_inputs(index, in, n);
	SiftColBatch *out = sift_cbatch_create(cx, schema);
	SiftWriteTarget *wt = NULL;
	SiftColSegWriter *w = NULL;
	SiftSegEntry neu[MAX_PARTS];
	int			nneu = 0,
				parts = 1;
	uint64		cut = 0,
				live_bytes = 0,
				live_units = 0;
	TimestampTz start = GetCurrentTimestamp();
	bool		exact = true;

	for (int i = 0; i < n; i++)
	{
		double		live;
		uint64		units = 0;

		seg[i] = sift_cseg_open(index, &in[i], cx);
		if (seg[i]->schema->hash != schema->hash)
			corrupt(index, "Segment %llu was written for another column layout.", (unsigned long long) in[i].gen);
		part[i] = sift_cbatch_create(cx, schema);
		exact &= sift_cseg_kinds_exact(seg[i]);
		live = in[i].ndocs > 0 ? (double) sift_cseg_live_rows(seg[i]) / in[i].ndocs : 0;
		for (uint32 u = 0; u < seg[i]->nsb; u++)
			units += seg[i]->sbdir[u].len;
		live_bytes += (uint64) (in[i].bytes * live);
		live_units += (uint64) (units * live);
	}
	/*
	 * Equal partitions (a short last one would be an overlay of every later rewrite): as many as the live bytes of the inputs make of part_bytes, cut by
	 * the bytes of units written, which are the part of a segment that grows with its rows (the dictionaries and the like grow with them)
	 */
	if (part_bytes > 0)
	{
		parts = Max(1, Min(Min((live_bytes + part_bytes / 2) / part_bytes, MAX_PARTS), max_parts));
		cut = live_units / parts;
	}
	/* the NULL slots and astext values of a segment that did not keep their kinds come out of the merge without them: its minor says so */
	if (!exact)
		format = Min(format, SIFT_COL_FORMAT_TAGS);
	for (;;)
	{
		uint32		sb = PG_UINT32_MAX;
		int			nuse = 0;

		for (int i = 0; i < n; i++)
			if (cur[i] < seg[i]->nsb)
				sb = Min(sb, seg[i]->sbdir[cur[i]].sb);
		if (sb == PG_UINT32_MAX)
			break;
		for (int i = 0; i < n; i++)
		{
			if (cur[i] >= seg[i]->nsb || seg[i]->sbdir[cur[i]].sb != sb)
				continue;
			sift_cseg_decode_unit_freeze(seg[i], cur[i]++, part[i], freeze, freeze_arg);
			if (part[i]->nrows > 0)
				use[nuse++] = part[i];
		}
		if (nuse == 0)
			continue;
		sift_cbatch_reset(out, sb);
		sift_cbatch_merge(out, use, nuse);
		if (w == NULL)
		{
			/* a context of its own for each partition, so that the dictionaries and zones of those written do not pile up */
			pcx = AllocSetContextCreate(cx, "sift_col merge partition", ALLOCSET_DEFAULT_SIZES);
			wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
			sift_wt_expect(wt, mi.bytes / parts);
			throttle(wt);
			w = sift_colw_begin(wt, sift_store_new_gen(index), schema, policy, format, pcx);
			if (pagg)
				sift_colw_set_preagg(w, pagg);
		}
		sift_colw_add(w, out);
		if (nneu + 1 < parts && sift_colw_bytes(w) >= cut)
		{
			sift_colw_finish(w, &neu[nneu++]);
			sift_wt_finish(wt);
			w = NULL;
			MemoryContextDelete(pcx);
#ifdef SIFT_TESTING
			if (sift_test_col_merge_panic)
				elog(PANIC, "sift test: the merge stops after its first partition, before the segments replace the old ones");
#endif
		}
		CHECK_FOR_INTERRUPTS();
	}
	for (int i = 0; i < n; i++)
		sift_cseg_close(seg[i]);
	if (w)
	{
		sift_colw_finish(w, &neu[nneu++]);
		sift_wt_finish(wt);
	}
#ifdef SIFT_TESTING
	if (sift_test_col_rollup_panic_armed || sift_test_col_merge_panic)
		elog(PANIC, "sift test: the merge stops before the new segment replaces the old ones");
	if (sift_test_col_refresh_sleep_armed > 0)
		pg_usleep(sift_test_col_refresh_sleep_armed * 1000L);
#endif
	{
		int64		docs = 0;

		for (int i = 0; i < nneu; i++)
			docs += neu[i].ndocs;
		sift_maint_replace_n(index, mi.gens, n, neu, nneu, mi.release, mi.nrelease,
							 psprintf("merged %d column segments into %d of", n, nneu), start, docs);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

void
sift_col_merge_segments(Relation index, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftCPaggSpec *pagg, const SiftSegEntry *in, int n,
						int format, SiftColFreezeFn freeze, void *freeze_arg)
{
	merge_segments(index, schema, policy, pagg, in, n, format, freeze, freeze_arg, 0, 1);
}

static SiftStepResult
merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	int			n;
	int			room;
	bool		cut = false;

	sift_dir_read(index, snap);
	n = goal == SIFT_GOAL_REWRITE ? pick_rewrite(index, snap, sel, &cut) : pick_merge(index, snap, force, sel);
	/* the new segments must fit the directory beside the others, with the room the inserters keep */
	room = SIFT_MAX_SEGMENTS - 16 - (snap->nsegs - n);
	pfree(snap);
	if (n > 0)
		merge_segments(index, sift_col_rel(index)->schema, sift_col_rel(index)->policy, &sift_col_rel(index)->pagg, sel, n, sift_col_format_minor, NULL, NULL,
					   cut ? (uint64) sift_col_auto_merge_partition_kb * 1024 : 0, Max(room, 1));
	return n > 0 ? SIFT_STEP_DONE : SIFT_STEP_IDLE;
}

static bool
is_col(Relation index)
{
	return sift_col_is_index(index);
}

static void
bulkdelete_immutable(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	SiftColSegment *seg = sift_cseg_open(index, e, CurrentMemoryContext);

	sift_cseg_bulkdelete(seg, cb, cb_state, stats);
	sift_cseg_close(seg);
}

static void
bulkdelete_wbuf(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	sift_cwbuf_bulkdelete(index, e, sift_col_rel(index)->schema, cb, cb_state, stats);
}

typedef struct HeapDead
{
	IndexFetchTableData *scan;
	TupleTableSlot *slot;
	BlockNumber nblocks;
} HeapDead;

/* The heap row of a TID is dead to every snapshot (or its page is gone): what an index entry may be dropped for */
static bool
heap_row_dead(const ItemPointerData *tid, void *arg)
{
	HeapDead   *h = arg;
	ItemPointerData t = *tid;
	bool		again = false,
				all_dead = false;

	if (ItemPointerGetBlockNumber(tid) >= h->nblocks)
		return true;
	table_index_fetch_tuple(h->scan, &t, SnapshotAny, h->slot, &again, &all_dead);
	return all_dead;
}

/*
 * VACUUM that skipped the vacuuming of the indexes (dead rows on under 2% of the heap pages) never called ambulkdelete, so the rows it
 * dropped are live in the index, and a unit that flags an overflow value keeps the flag after the rows that held it are repaired. The
 * rows that hold such a value are the only ones looked up in the heap here; the merge that follows rewrites the segments the flag only
 * dead rows account for (sift_cseg_stale_ovf).
 */
static void
reap_overflow(Relation index, Relation heap)
{
	const SiftColLeafStats *ls = sift_col_leaf_stats(index);
	SiftDirSnapshot *snap;
	HeapDead	h;
	bool		flagged = false;

	for (int c = 0; c < sift_col_index_defs(index)->ncols; c++)
		flagged |= ls[c].has_ovf;
	if (!flagged)
		return;
	snap = palloc(sizeof(SiftDirSnapshot));
	h = (HeapDead) {.scan = sift_table_index_fetch_begin(heap),.slot = table_slot_create(heap, NULL),.nblocks = RelationGetNumberOfBlocks(heap)};
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);

			sift_cseg_reap_overflow(seg, heap_row_dead, &h);
			sift_cseg_close(seg);
		}
	ExecDropSingleTupleTableSlot(h.slot);
	table_index_fetch_end(h.scan);
	pfree(snap);
}

static bool
vacuum_truncates(Relation index)
{
	return sift_col_options(index)->vacuum_truncate;
}

/*
 * Is the index worth a rewrite of all its segments (docs/COL.md 2.6)? A DELETE or UPDATE spread over the table puts a dead row in most units while
 * the share of dead rows stays under dead_rewrite_pct, and each promotion of the write buffer adds a segment over the same superblocks, so the
 * index slowly loses the units that answer from their pre-aggregates and gains units to read. The rule is on what a rewrite changes, measured on
 * the directory and the liveness (no heap page is read): more than sift.col_auto_merge_dirty_pct percent of the units hold a dead row, or the
 * units listed are sift.col_auto_merge_fragmentation times the superblocks they cover, or the write buffer holds more than
 * sift.col_auto_merge_buffer_pct percent of the rows. Why, or NULL. The automatic rewrite leaves alone an index over sift.vacuum_rewrite_limit, and
 * waits until sift.col_auto_merge_min_churn_pct percent of the rows changed since the last rewrite: the dead ones, and those of its write buffer and of
 * its segments but the largest (the one a rewrite leaves). It waits too while the blocks the last rewrite freed are held back by a snapshot.
 * With sift.col_auto_merge_partition_kb the rewrite is of one partition at a time: a partition with dirty_pct of its units dirty is due, whatever the
 * share of the whole index, units listed twice are those of the overlays over the partitions, the size of the index does not matter and the segments
 * a rewrite leaves are the partitions.
 */
static const char *
rewrite_check(Relation index, bool automatic, int *gates)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col rewrite due", ALLOCSET_SMALL_SIZES);
	RewriteView *v;
	const char *why = NULL;
	bool		partitioned = sift_col_auto_merge_partition_kb > 0,
				part_dirty = false;
	uint64		listed = 0,
				dirty = 0,
				rows = 0,
				bytes = 0,
				buffered = 0,
				changed = 0,
				largest = 0,
				part_rows = 0,
				over_listed = 0,
				over_dirty = 0,
				covered;

	sift_dir_read(index, snap);
	v = view_open(index, snap, cx);
	for (int i = 0; i < v->n; i++)
	{
		const SiftSegEntry *e = &v->imm[i];
		SiftColSegment *seg = v->seg[i];
		uint32		d = 0;

		rows += e->ndocs;
		bytes += e->bytes;
		listed += seg->nsb;
		largest = Max(largest, e->ndocs);
		if (e->flags & SIFT_SEG_DIRTY)
		{
			d = dirty_units(seg);
			dirty += d;
			changed += e->ndocs - sift_cseg_live_rows(seg);
		}
		if (!v->over[i])
		{
			part_rows += e->ndocs;
			part_dirty |= (uint64) d * 100 > (uint64) sift_col_auto_merge_dirty_pct * seg->nsb;
		}
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
	changed += rows - (partitioned ? part_rows : largest) + buffered;
	if (gates != NULL && automatic && !partitioned && bytes > ((uint64) sift_vacuum_rewrite_limit << 20))
		*gates |= SIFT_COL_GATE_SIZE;
	/* the size and the churn bound what a rewrite of all the segments costs, not the promotion of a write buffer */
	if (!automatic || ((partitioned || bytes <= ((uint64) sift_vacuum_rewrite_limit << 20)) &&
					   (double) changed * 100 >= sift_col_auto_merge_min_churn_pct * (double) (rows + buffered)))
	{
		if (partitioned ? part_dirty || over_dirty * 100 > (uint64) sift_col_auto_merge_dirty_pct * over_listed :
			listed > 0 && dirty * 100 > (uint64) sift_col_auto_merge_dirty_pct * listed)
			why = "dirty units";
		else if (covered > 0 && (!partitioned || over_listed > 0) && (double) listed >= sift_col_auto_merge_fragmentation * covered)
			why = "fragmented units";
	}
	if (why == NULL && buffered > 0 && (double) buffered * 100 > sift_col_auto_merge_buffer_pct * (double) (rows + buffered))
		why = WHY_BUFFER;
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

static const char *
rewrite_due(Relation index, bool automatic)
{
	return rewrite_check(index, automatic, NULL);
}

/* Only the write buffer is large: promoting it is the cost of its rows, a rewrite is the cost of the index */
static bool
rewrite_all(const char *why)
{
	return strcmp(why, WHY_BUFFER) != 0;
}

/* VACUUM leaves the rewrite to a worker of its own: it is not VACUUM's to hold the table for, and the worker sleeps as sift.maintenance_cost_delay says */
static void
rewrite_request(Relation index)
{
	if (!sift_col_auto_merge || !sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return;
	if (rewrite_due(index, true) != NULL)
		sift_maint_register_worker(index, "sift_col_rewrite_worker_main");
}

static const SiftMaintOps col_ops = {
	.name = "sift_col",
	.worker = "sift_col_maint_worker_main",
	.step_lock = ExclusiveLock,
	.vacuum_to_fixed_point = true,
	.vacuum_budget = true,
	.force_seals = true,
	.vacuum_truncate = vacuum_truncates,
	.vacuum_seal = sift_cwbuf_seal_vacuum,
	.is_index = is_col,
	.seal = sift_cwbuf_seal,
	.promote_next = promote_next,
	.merge_next = merge_next,
	.bulkdelete_immutable = bulkdelete_immutable,
	.bulkdelete_wbuf = bulkdelete_wbuf,
	.vacuum_skipped = reap_overflow,
	.rewrite_due = rewrite_due,
	.rewrite_all = rewrite_all,
	.vacuum_done = rewrite_request,
};

void
sift_col_maint_after_seal(Relation index)
{
	sift_mops_after_seal(&col_ops, index);
}

bool
sift_col_maint_make_room(Relation index)
{
	return sift_mops_make_room(&col_ops, index);
}

int
sift_col_maint_promote(Relation index, bool wait)
{
	return sift_mops_promote(&col_ops, index, wait);
}

bool
sift_col_maint_merge_step(Relation index, bool wait)
{
	return sift_mops_merge_step(&col_ops, index, wait);
}

IndexBulkDeleteResult *
sift_col_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	return sift_mops_bulkdelete(&col_ops, info, stats, callback, callback_state);
}

IndexBulkDeleteResult *
sift_col_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return sift_mops_vacuumcleanup(&col_ops, info, stats);
}

/*
 * A worker whose transaction fails keeps the error where sift.workers and the backoff of the requests find it (the steps of a sift_col index run
 * outside a subtransaction, so the whole transaction of the worker is the unit), warns as a failed BM25 step does, and ends. Cancellation and
 * shutdown go on as they were.
 */
static void
worker_failed(Oid indexoid, MemoryContext cx)
{
	ErrorData  *err;

	MemoryContextSwitchTo(cx);
	err = CopyErrorData();
	FlushErrorState();
	AbortCurrentTransaction();
	if (ERRCODE_TO_CATEGORY(err->sqlerrcode) == ERRCODE_OPERATOR_INTERVENTION)
		ReThrowError(err);
	ereport(WARNING,
			(errcode(err->sqlerrcode),
			 errmsg("maintenance of sift_col index %u failed: %s", indexoid, err->message),
			 errdetail("It is retried later, after a growing delay.")));
	sift_maint_record_failure(indexoid, err);
	FreeErrorData(err);
}

void
sift_col_maint_worker_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	MemoryContext cx = CurrentMemoryContext;

	PG_TRY();
	{
		Relation	index;

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
		index = try_index_open(indexoid, RowExclusiveLock);
		if (index != NULL && sift_col_is_index(index))
		{
			/* the lock is busy when another worker or a VACUUM is at it, which then does the work */
			while (sift_col_maint_promote(index, false) > 0 || sift_col_maint_merge_step(index, false))
				CHECK_FOR_INTERRUPTS();
			sift_log_maint(index, "column maintenance worker finished", start, -1, -1);
		}
		if (index != NULL)
			index_close(index, RowExclusiveLock);
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		worker_failed(indexoid, cx);
	}
	PG_END_TRY();
}

/* Waits as sift_maint_wait_quiet does, until ready() says so; False: it stayed not ready, and the next VACUUM asks again */
static bool
wait_until(bool (*ready) (void))
{
	long		delay = sift_maintenance_naptime;

	for (int tries = 0; !ready(); tries++)
	{
		if (tries == 8)
			return false;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, delay, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		delay *= 2;
	}
	return true;
}

/*
 * sift.col_auto_merge_max_workers: a worker rewrites while it holds one of that many session locks of the database (advisory locks of the keys
 * REWRITE_SLOT_KEY + slot, which the server drops when the worker ends, whatever the way), so no more than that many rewrite at a time, whichever
 * indexes they are for.
 */
static bool
take_slot(void)
{
	for (int s = 0; s < sift_col_auto_merge_max_workers; s++)
	{
		LOCKTAG		tag;

		SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F7, REWRITE_SLOT_KEY + s, 2);
		if (LockAcquire(&tag, ExclusiveLock, true, true) != LOCKACQUIRE_NOT_AVAIL)
			return true;
	}
	return sift_col_auto_merge_max_workers == 0;
}

/* The rewrite VACUUM asked for: the rule is judged again (the index may have changed since), and a held maintenance lock leaves it for the next VACUUM */
void
sift_col_rewrite_worker_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	MemoryContext cx = CurrentMemoryContext;

	/* sift.col_auto_merge_max_active, then a slot of sift.col_auto_merge_max_workers; False: it stayed busy, and the next VACUUM asks again */
	if (!sift_maint_wait_quiet(sift_col_auto_merge_max_active) || !wait_until(take_slot))
		return;
	PG_TRY();
	{
		Relation	index;

		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());
		index = try_index_open(indexoid, RowExclusiveLock);
		if (index != NULL)
		{
			if (sift_col_is_index(index) && sift_col_auto_merge && !RecoveryInProgress())
			{
				TimestampTz start = GetCurrentTimestamp();

				if (sift_mops_rewrite(&col_ops, index, true) >= 0)
					sift_log_maint(index, "column rewrite worker finished", start, -1, -1);
			}
			index_close(index, RowExclusiveLock);
		}
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		worker_failed(indexoid, cx);
	}
	PG_END_TRY();
}

/* sift.col_maintenance_due(index, automatic): why the rewrite is due now (dirty units, fragmented units, write buffer rows), NULL if it is not; automatic: as the server's own */
Datum
sift_col_maintenance_due(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open(&col_ops, PG_GETARG_OID(0), AccessShareLock);
	const char *why = rewrite_due(index, PG_GETARG_BOOL(1));

	index_close(index, AccessShareLock);
	if (why == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(why));
}

/*
 * sift.col_maintenance_status(index): where the chain stands (the table is vacuumed when the rows changed since its last VACUUM reach vacuum_at, the
 * index is rewritten after that VACUUM), and blocked_by: what keeps a worker from vacuuming it or rewriting the index, empty when nothing does.
 */
Datum
sift_col_maintenance_status(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open(&col_ops, PG_GETARG_OID(0), AccessShareLock);
	Relation	heap = relation_open(index->rd_index->indrelid, AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TupleDesc	tupdesc;
	Datum		v[7],
				blocked[8];
	bool		nulls[7] = {0};
	int64		changed = 0,
				threshold = 0;
	TimestampTz last = 0;
	const char *why;
	int			gates = 0,
				nblocked = 0;

	SIFT_INVARIANT(get_call_result_type(fcinfo, NULL, &tupdesc) == TYPEFUNC_COMPOSITE, "return type must be a row type");
	sift_dir_read(index, snap);
	why = rewrite_check(index, true, &gates);
	if (!sift_col_auto_merge)
		blocked[nblocked++] = CStringGetTextDatum("sift.col_auto_merge is off");
	if (!sift_async_maintenance)
		blocked[nblocked++] = CStringGetTextDatum("sift.async_maintenance is off");
	if (RecoveryInProgress())
		blocked[nblocked++] = CStringGetTextDatum("standby");
	if (index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		blocked[nblocked++] = CStringGetTextDatum("temporary table");
	if (!sift_col_autovacuum_enabled(heap))
		blocked[nblocked++] = CStringGetTextDatum("autovacuum_enabled is false");
	if (gates & SIFT_COL_GATE_SIZE)
		blocked[nblocked++] = CStringGetTextDatum("index over sift.vacuum_rewrite_limit");
	if (gates & SIFT_COL_GATE_HELD)
		blocked[nblocked++] = CStringGetTextDatum("a snapshot holds back the blocks of the last rewrite");
	v[0] = ObjectIdGetDatum(RelationGetRelid(heap));
	nulls[1] = nulls[2] = nulls[3] = !sift_col_table_churn(RelationGetRelid(heap), &changed, &threshold, &last);
	nulls[1] |= last == 0;
	v[1] = TimestampTzGetDatum(last);
	v[2] = Int64GetDatum(changed);
	v[3] = Int64GetDatum(threshold);
	nulls[4] = why == NULL;
	if (why != NULL)
		v[4] = CStringGetTextDatum(why);
	nulls[5] = snap->meta.maint_requested_at == 0;
	v[5] = TimestampTzGetDatum(snap->meta.maint_requested_at);
	v[6] = PointerGetDatum(construct_array(blocked, nblocked, TEXTOID, -1, false, TYPALIGN_INT));
	relation_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, v, nulls)));
}

/* sift.col_maintain(index): the rewrite of sift.col_auto_merge, now and in this session, whatever that setting says; false when nothing is due or the maintenance lock is held */
Datum
sift_col_maintain(PG_FUNCTION_ARGS)
{
	Relation	index;
	int			n;

	PreventCommandIfReadOnly("sift.col_maintain()");
	PreventCommandDuringRecovery("sift.col_maintain()");
	index = sift_mops_open_maintain(&col_ops, PG_GETARG_OID(0));
	n = sift_mops_rewrite(&col_ops, index, false);
	index_close(index, RowExclusiveLock);
	PG_RETURN_BOOL(n >= 0);
}

Datum
sift_col_promote(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(&col_ops, PG_GETARG_OID(0));
	int			n = sift_mops_sql_promote(&col_ops, index);

	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

Datum
sift_col_merge(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open_maintain(&col_ops, PG_GETARG_OID(0));
	int			n = sift_mops_sql_merge(&col_ops, index, PG_NARGS() > 1 && PG_GETARG_BOOL(1));

	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

Datum
sift_col_index_info(PG_FUNCTION_ARGS)
{
	Relation	index = sift_mops_open(&col_ops, PG_GETARG_OID(0), AccessShareLock);
	const SiftColIndexMeta *defs = sift_col_index_defs(index);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TupleDesc	tupdesc;
	SiftFreeStats fs;
	Datum		v[16];
	bool		nulls[16] = {0};
	int64		rows = 0,
				bytes = 0,
				units = 0,
				pagg_bytes = 0,
				pagg_units = 0;
	int			nwbuf = 0;

	SIFT_INVARIANT(get_call_result_type(fcinfo, NULL, &tupdesc) == TYPEFUNC_COMPOSITE, "return type must be a row type");
	sift_dir_read(index, snap);
	fs = sift_store_free_stats(index);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);

			rows += snap->segs[i].ndocs;
			bytes += snap->segs[i].bytes;
			pagg_bytes += sift_cseg_preagg_bytes(seg);
			pagg_units += sift_cseg_preagg_units(seg);
			sift_cseg_close(seg);
		}
		else
			nwbuf++;
	units = sift_cseg_dir_units(snap);
	v[0] = Int64GetDatum(RelationGetNumberOfBlocks(index));
	v[1] = Int64GetDatum(snap->meta.high_water);
	v[2] = Int64GetDatum(fs.blocks);
	v[3] = Int64GetDatum(fs.reusable);
	v[4] = Int64GetDatum(fs.records);
	v[5] = Int32GetDatum(snap->nsegs);
	v[6] = Int32GetDatum(SIFT_MAX_SEGMENTS);
	v[7] = Int64GetDatum(rows);
	v[8] = Int64GetDatum(bytes);
	v[9] = Int64GetDatum(units);
	v[10] = Int32GetDatum(nwbuf);
	v[11] = Int32GetDatum(defs->ncols);
	nulls[12] = RecoveryInProgress();
	v[12] = BoolGetDatum(!nulls[12] && sift_maintenance_busy(index));
	v[13] = CStringGetTextDatum(sift_simd_level_name(sift_cpu.active));
	v[14] = Int64GetDatum(pagg_bytes);
	v[15] = Int64GetDatum(pagg_units);
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

Datum
sift_col_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_mops_open(&col_ops, PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		Datum		v[12];
		bool		nulls[12] = {0};

		v[0] = Int64GetDatum(e->gen);
		v[1] = CStringGetTextDatum(e->kind == SIFT_SEG_IMMUTABLE ? "immutable" :
								   e->kind == SIFT_SEG_WBUF_OPEN ? "buffer" : "sealed buffer");
		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, e, CurrentMemoryContext);

			v[2] = Int64GetDatum(e->ndocs);
			v[3] = Int64GetDatum(e->bytes);
			v[4] = Int32GetDatum(e->sb_lo);
			v[5] = Int32GetDatum(e->sb_hi);
			v[6] = Int32GetDatum(seg->nsb);
			v[7] = BoolGetDatum((e->flags & SIFT_SEG_DIRTY) != 0);
			v[8] = Int64GetDatum(e->ndocs - sift_cseg_live_rows(seg));
			v[9] = Int32GetDatum(seg->hdr->nextents);
			v[10] = Int32GetDatum(e->format);
			v[11] = Int64GetDatum(sift_cseg_preagg_bytes(seg));
			sift_cseg_close(seg);
		}
		else
		{
			uint64		nrows,
						npages;

			sift_cwbuf_summary(index, e, &nrows, &npages);
			v[2] = Int64GetDatum(nrows);
			v[3] = Int64GetDatum(npages * BLCKSZ);
			nulls[4] = nulls[5] = nulls[6] = nulls[7] = nulls[8] = nulls[9] = nulls[10] = nulls[11] = true;
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* One row per leaf: the chunks of all immutable segments by encoding, their bytes, and the dictionaries */
Datum
sift_col_leaves(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_mops_open(&col_ops, PG_GETARG_OID(0), AccessShareLock);
	const SiftColSchema *sc = sift_col_rel(index)->schema;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int64	   *chunks = palloc0(sc->ncols * 5 * sizeof(int64)),
			   *bytes = palloc0(sc->ncols * sizeof(int64)),
			   *dict_n = palloc0(sc->ncols * sizeof(int64)),
			   *dict_bytes = palloc0(sc->ncols * sizeof(int64));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftCUnitView v;
			uint32		len;
			const uint8 *rec = sift_cseg_unit(seg, u, &len);

			if (!sift_cunit_parse(rec, len, sc, &v))
				corrupt(index, "Unit %u of segment %llu does not parse.", u, (unsigned long long) snap->segs[i].gen);
			for (int c = 0; c < sc->ncols; c++)
			{
				SiftCChunk	ch;

				if (!sift_cunit_chunk(&v, c, &ch))
					continue;
				chunks[c * 5 + (ch.enc == SIFT_COLENC_ISTR ? 4 : Min(ch.enc, 3))]++;
				bytes[c] += v.chunk_len[c];
			}
		}
		for (int c = 0; c < sc->ncols; c++)
		{
			SiftCDict  *d = sift_cseg_dict(seg, c);

			if (d)
			{
				dict_n[c] += d->n;
				dict_bytes[c] += d->nbytes + (int64) (d->n + 1) * sizeof(uint32);
			}
		}
		sift_cseg_close(seg);
	}
	for (int c = 0; c < sc->ncols; c++)
	{
		Datum		v[11];
		bool		nulls[11] = {false};

		v[0] = Int32GetDatum(c);
		v[1] = CStringGetTextDatum(sc->cols[c].path[0] ? sc->cols[c].path : NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[c].attno - 1)->attname));
		v[2] = CStringGetTextDatum(sc->cols[c].kind == SIFT_COLK_GROUP ? "group" : "scalar");
		v[3] = CStringGetTextDatum(sc->cols[c].lane > 0 ? psprintf("%s (lane %d)", format_type_be(sift_col_def_typid(RelationGetDescr(index), &sc->cols[c])), sc->cols[c].lane) :
								   format_type_be(sift_col_def_typid(RelationGetDescr(index), &sc->cols[c])));
		for (int e = 0; e < 5; e++)
			v[4 + e] = Int64GetDatum(chunks[c * 5 + e]);
		v[9] = Int64GetDatum(bytes[c]);
		v[10] = Int64GetDatum(dict_bytes[c]);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
