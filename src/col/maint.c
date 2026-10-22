/*
 * col/maint.c - promotion, merge, VACUUM, the maintenance worker and the SQL functions of sift_col indexes.
 * The lock, the directory switch, the room, back-pressure, VACUUM and SQL control flow and the sweep are those of bm25/maint.c
 * (sift_mops_*); what this file supplies in col_ops is a column segment: how a sealed one is promoted and how segments are merged.
 *
 * Intentional differences from the BM25 maintenance (bm25/maint.c), kept so the behaviour of sift_col stays what it was:
 * - a step holds the maintenance lock exclusively, not in RowExclusiveLock with claims on its segments: one step of the index at a time;
 * - a step is not run in a subtransaction: an ERROR goes on to the caller, nothing is counted for the backoff (the failure flags are never
 *   set, so the backoff of the shared flow never applies) and there is no segment another step could have claimed (busy stays 0);
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
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
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

/* The merges of the maintenance worker sleep like vacuum_cost_delay does (sift.maintenance_cost_delay); its promotions, which the writers wait for, and the rollup refresh do not */
static void
throttle(SiftWriteTarget *wt)
{
	if (MyBackendType == B_BG_WORKER && strcmp(MyBgworkerEntry->bgw_type, "sift maintenance") == 0)
		sift_wt_throttle(wt, sift_maintenance_cost_delay);
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

		/* the rows of a write buffer written before the kinds were kept may lack them: such a segment is not minor 6 or 7 */
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

	return sift_merge_pick(index, &pol, snap, force, sel);
}

void
sift_col_merge_segments(Relation index, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftCPaggSpec *pagg, const SiftSegEntry *in, int n,
						int format, SiftColFreezeFn freeze, void *freeze_arg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColSegment **seg = palloc(n * sizeof(SiftColSegment *));
	SiftColBatch **part = palloc(n * sizeof(SiftColBatch *));
	SiftColBatch **use = palloc(n * sizeof(SiftColBatch *));
	uint32	   *cur = palloc0(n * sizeof(uint32));
	SiftMaintInputs mi = sift_maint_inputs(index, in, n);
	SiftColBatch *out = sift_cbatch_create(cx, schema);
	SiftWriteTarget *wt = NULL;
	SiftColSegWriter *w = NULL;
	SiftSegEntry neu;
	TimestampTz start = GetCurrentTimestamp();
	bool		exact = true;

	for (int i = 0; i < n; i++)
	{
		seg[i] = sift_cseg_open(index, &in[i], cx);
		if (seg[i]->schema->hash != schema->hash)
			corrupt(index, "Segment %llu was written for another column layout.", (unsigned long long) in[i].gen);
		part[i] = sift_cbatch_create(cx, schema);
		exact &= sift_cseg_kinds_exact(seg[i]);
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
			wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
			sift_wt_expect(wt, mi.bytes);
			throttle(wt);
			w = sift_colw_begin(wt, sift_store_new_gen(index), schema, policy, format, cx);
			if (pagg)
				sift_colw_set_preagg(w, pagg);
		}
		sift_colw_add(w, out);
		CHECK_FOR_INTERRUPTS();
	}
	for (int i = 0; i < n; i++)
		sift_cseg_close(seg[i]);
	if (w)
	{
		sift_colw_finish(w, &neu);
		sift_wt_finish(wt);
	}
#ifdef SIFT_TESTING
	if (sift_test_col_rollup_panic_armed || sift_test_col_merge_panic)
		elog(PANIC, "sift test: the merge stops before the new segment replaces the old ones");
	if (sift_test_col_refresh_sleep_armed > 0)
		pg_usleep(sift_test_col_refresh_sleep_armed * 1000L);
#endif
	sift_maint_replace(index, mi.gens, n, w ? &neu : NULL, mi.release, mi.nrelease,
					   psprintf("merged %d column segments of", n), start, w ? neu.ndocs : 0);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static SiftStepResult
merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	int			n;

	sift_dir_read(index, snap);
	n = pick_merge(index, snap, force, sel);
	pfree(snap);
	if (n > 0)
		sift_col_merge_segments(index, sift_col_rel(index)->schema, sift_col_rel(index)->policy, &sift_col_rel(index)->pagg, sel, n, sift_col_format_minor, NULL, NULL);
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
 * Is the index worth a rewrite of all its segments? A DELETE or UPDATE spread over the table puts a dead row in most units while
 * the share of dead rows stays under dead_rewrite_pct, and each promotion of the write buffer adds a segment over the same superblocks, so the
 * index slowly loses the units that answer from their pre-aggregates and gains units to read. The rule is on what a rewrite changes, measured on
 * the directory and the liveness (no heap page is read): more than sift.col_auto_merge_dirty_pct percent of the units hold a dead row, or the
 * units listed are sift.col_auto_merge_fragmentation times the superblocks they cover, or the write buffer holds more than
 * sift.col_auto_merge_buffer_pct percent of the rows. Why, or NULL. The automatic rewrite leaves alone an index over sift.vacuum_rewrite_limit, and
 * waits until sift.col_auto_merge_min_churn_pct percent of the rows changed since the last rewrite: the dead ones, and those of its write buffer and of
 * its segments but the largest (the one a rewrite leaves).
 */
static const char *
rewrite_due(Relation index, bool automatic)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	uint64		listed = 0,
				dirty = 0,
				rows = 0,
				bytes = 0,
				buffered = 0,
				changed = 0,
				largest = 0,
				covered;

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];

		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, e, CurrentMemoryContext);

			rows += e->ndocs;
			bytes += e->bytes;
			listed += seg->nsb;
			largest = Max(largest, e->ndocs);
			if (e->flags & SIFT_SEG_DIRTY)
			{
				for (uint32 u = 0; u < seg->nsb; u++)
					dirty += sift_cseg_dirty(seg, u);
				changed += e->ndocs - sift_cseg_live_rows(seg);
			}
			sift_cseg_close(seg);
		}
		else
		{
			uint64		nrows,
						npages;

			sift_cwbuf_summary(index, e, &nrows, &npages);
			buffered += nrows;
		}
	}
	covered = sift_cseg_dir_units(snap);
	pfree(snap);
	changed += rows - largest + buffered;
	/* the size and the churn bound what a rewrite of all the segments costs, not the promotion of a write buffer */
	if (!automatic || (bytes <= ((uint64) sift_vacuum_rewrite_limit << 20) &&
					   (double) changed * 100 >= sift_col_auto_merge_min_churn_pct * (double) (rows + buffered)))
	{
		if (listed > 0 && dirty * 100 > (uint64) sift_col_auto_merge_dirty_pct * listed)
			return "dirty units";
		if (covered > 0 && (double) listed >= sift_col_auto_merge_fragmentation * covered)
			return "fragmented units";
	}
	if (buffered > 0 && (double) buffered * 100 > sift_col_auto_merge_buffer_pct * (double) (rows + buffered))
		return WHY_BUFFER;
	return NULL;
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

void
sift_col_maint_worker_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
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

/* Client backends of this database that are running a statement */
static int
active_backends(void)
{
	int			n = 0;

	pgstat_clear_backend_activity_snapshot();
	for (int i = 1, max = pgstat_fetch_stat_numbackends(); i <= max; i++)
	{
		const PgBackendStatus *s = &pgstat_get_local_beentry_by_index(i)->backendStatus;

		n += s->st_backendType == B_BACKEND && s->st_databaseid == MyDatabaseId && s->st_state == STATE_RUNNING;
	}
	return n;
}

/*
 * sift.col_auto_merge_max_active: waits, with no transaction (and so no snapshot that holds back the horizon), until at most that many client backends
 * of the database are running a statement: looks again after sift.maintenance_naptime, then twice as late each time, 8 times. False: it stayed busy,
 * and the next VACUUM asks again.
 */
static bool
wait_quiet(void)
{
	long		delay = sift_maintenance_naptime;

	for (int tries = 0; sift_col_auto_merge_max_active > 0 && active_backends() > sift_col_auto_merge_max_active; tries++)
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

/* The rewrite VACUUM asked for: the rule is judged again (the index may have changed since), and a held maintenance lock leaves it for the next VACUUM */
void
sift_col_rewrite_worker_main(Datum main_arg)
{
	ErrorContextCallback errctx;
	Oid			indexoid = sift_maint_worker_connect(main_arg, &errctx);
	Relation	index;

	if (!wait_quiet())
		return;
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

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
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
