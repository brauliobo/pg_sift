/*
 * table/vacuum_maint.c - the maintenance of a sift_table: sift_tbl_maintain, the automatic defragmentation tick, the background VACUUM and the workers (settle, maintenance).
 */
#include "vacuum_int.h"

void
sift_tbl_maintain(Relation rel, bool background, bool clear, SiftTblMaintain *r)
{
	VacState	v = {0};
	SiftTblRel *st = sift_tbl_rel(rel);
	TransactionId oldest = GetOldestNonRemovableTransactionId(rel);
	uint64		budget = (uint64) sift_vacuum_rewrite_limit << 20;

	*r = (SiftTblMaintain) {.blocks_before = RelationGetNumberOfBlocks(rel)};
	r->blocks_after = r->blocks_before;
	v.st = st;
	v.oldest = v.newest_kept = oldest;
	sift_tbl_refresh(rel, st);
	if (!st->initialized)
		return;
	if (!sift_maint_lock(rel, false))
	{
		r->busy = true;
		return;
	}
	maintaining = true;
	PG_TRY();
	{
		/*
		 * The rows of the write buffers are promoted first: their dead ones are not flagged here as VACUUM does (the flag is what its
		 * promotion drops and also what keeps the row out of the dead set of a later VACUUM), so they are cleared as rows of a group
		 */
		sift_tbl_close_wbuf(rel, st, VACUUM_CLOSE_PAGES);
		r->promoted = sift_tbl_promote(rel, st, oldest, false);
		if (clear)
			r->dead_rows = clear_removable(rel, st, &v);
		(void) sift_tbl_drop_dead_groups(rel, st);
		(void) sift_tbl_freeze_groups(rel, st, oldest);
		r->merged = merge_steps(rel, st, oldest);
		(void) sift_recycle(rel);
		(void) sift_maint_orphan_sweep(rel);
		if (tbl_truncates(rel))
			sift_maint_space_budget(rel, relocate_page, DEBUG2, background ? Min(budget, DEFRAG_RELOCATE_BYTES) : budget);
	}
	PG_FINALLY();
	{
		maintaining = false;
		sift_maint_unlock(rel);
	}
	PG_END_TRY();
	r->blocks_after = RelationGetNumberOfBlocks(rel);
	if (v.dead_tids.tid)
		pfree(v.dead_tids.tid);
	if (v.redirects.tid)
		pfree(v.redirects.tid);
}

/*
 * Is the table worth a pass now, and which: called by the backend that closed a write buffer (every few megabytes of writes) and, through
 * sift_tbl_auto_defrag_tick, by one that deletes, updates or reads, from what it has at hand. The dead rows are told by the tombstones
 * against the rows of the column segments (the exact percent of a segment is known after its bits are cleared, which the pass does), the
 * space by the free blocks of the file. Only the tombstones older than every snapshot count: a long transaction holds the rest, and a pass
 * would read the table to remove nothing, so it is not asked for until the transaction ends. The tombstones outlive a pass until a VACUUM
 * has been through the indexes, so what the backend saw when it asked last decides whether they grew since; likewise the free blocks.
 */
static uint64
removable_tombs(Relation rel, SiftTblRel *st)
{
	TransactionId horizon = GetOldestNonRemovableTransactionId(rel);
	uint64		n = 0;

	sift_tbl_tomb_sync(rel, st);
	/* (the frozen entries are the roots of chains that go on: a pass keeps them) */
	for (uint32 i = 0; i < st->nents; i++)
		n += st->ents[i].xid != FrozenTransactionId && TransactionIdPrecedes(st->ents[i].xid, horizon);
	for (uint32 i = 0; i < st->nrecent; i++)
		n += st->recent[i].xid != FrozenTransactionId && TransactionIdPrecedes(st->recent[i].xid, horizon);
	return n;
}

static uint64
column_rows(const SiftTblRel *st)
{
	uint64		rows = 0;

	for (int i = 0; i < st->dir.nsegs; i++)
		if (st->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			rows += st->dir.segs[i].ndocs;
	return rows;
}

/* Could that many tombstones be enough? The header counts every entry, the removable ones are fewer, and counting them reads the log */
static bool
dead_over(const SiftTblRel *st, uint64 tombs)
{
	uint64		rows = column_rows(st);

	return sift_tbl_auto_defrag_dead_pct < 100 && rows > 0 && tombs * 100 >= rows * sift_tbl_auto_defrag_dead_pct;
}

static bool
dead_wanted(Relation rel, SiftTblRel *st, uint64 tombs, uint64 seen)
{
	return dead_over(st, tombs) && tombs >= seen && (tombs - seen) * 200 >= column_rows(st) * sift_tbl_auto_defrag_dead_pct;
}

/*
 * Closed write buffers are waiting and the oldest xmin moved since they were asked for: a worker that found rows of a transaction still open left
 * without them, and nobody asks again until another buffer closes, which a table that is read, or written in small transactions, may not do for long
 */
static bool
settle_due(Relation rel, SiftTblRel *st)
{
	for (int i = 0; i < st->dir.nsegs; i++)
		if (st->dir.segs[i].kind == SIFT_SEG_WBUF_SEALED && (st->dir.segs[i].flags & SIFT_TBL_SEG_WBUF))
			return st->settle_horizon != GetOldestNonRemovableTransactionId(rel);
	return false;
}

static void
auto_defrag_check(Relation rel, SiftTblRel *st)
{
	BlockNumber nblocks;
	SiftFreeStats fs;
	uint64		tombs;
	bool		dead,
				space,
				writing;

	if (!sift_tbl_auto_defrag || !sift_async_maintenance || maintaining || !IsUnderPostmaster || RecoveryInProgress())
		return;
	sift_tbl_refresh(rel, st);
	if (!st->initialized)
		return;
	/* the log is read, by this backend, only when its length could cross the threshold: a ticking backend that does nothing else does not sync it */
	if (dead_over(st, sift_tbl_tomb_entries(rel, st)))
		tombs = removable_tombs(rel, st);
	else
		tombs = st->defrag_tombs = 0;
	nblocks = RelationGetNumberOfBlocks(rel);
	fs = sift_store_free_stats(rel);
	/*
	 * The free blocks of a table that is being written are the write buffers of the promoted rows, about to be taken again by the next
	 * buffers: an append flood has half its file free at any moment, and a pass would cut it for the writers to extend it. The space is
	 * asked for when no write buffer was opened since this backend last looked (the first look sees none to compare with).
	 */
	writing = SIFT_TBL_NEXT_VBLOCK(&st->dir.meta) != st->defrag_vblock;
	st->defrag_vblock = SIFT_TBL_NEXT_VBLOCK(&st->dir.meta);
	/* a VACUUM rewrote the log, or a pass or a truncation used the free blocks: what is asked next is relative to what is left */
	st->defrag_tombs = Min(st->defrag_tombs, tombs);
	st->defrag_free = Min(st->defrag_free, fs.blocks);
	dead = dead_wanted(rel, st, tombs, st->defrag_tombs);
	space = !writing && fs.blocks * 100 >= (int64) nblocks * sift_tbl_auto_defrag_free_pct && sift_maint_space_worth(fs.blocks, nblocks) &&
		fs.blocks >= st->defrag_free + (int64) nblocks / 32;
	if (!dead && !space)
	{
		if (settle_due(rel, st))
			(void) sift_tbl_settle_async(rel, st);
		return;
	}
	if (sift_maint_request_worker(rel, "sift_tbl_maint_worker_main"))
	{
		st->defrag_tombs = tombs;
		st->defrag_free = fs.blocks;
		sift_tbl_refresh(rel, st);	/* the request moved the directory version */
	}
}

/* A table whose writes are small, or only deletes, closes no write buffer for a long time: its deletes and its readers look as well, each backend once per naptime */
void
sift_tbl_auto_defrag_tick(Relation rel, SiftTblRel *st)
{
	int64		now;

	if (!sift_tbl_auto_defrag || IsInParallelMode())
		return;
	now = GetCurrentTimestamp();
	if (now - st->defrag_at < (int64) sift_maintenance_naptime * 1000)
		return;
	st->defrag_at = now;
	auto_defrag_check(rel, st);
}

/*
 * The whole of VACUUM for a table whose dead rows cross the threshold, from the worker: without the index cleanup the tombstone log, which
 * every scan reads and which holds the file above the live rows, cannot be shortened (the 8-round benchmark of the pull request). The
 * page accesses are charged to the cost delay (vacuum_cost_limit) as autovacuum's are, and the exclusive window of the space step is capped.
 */
static void
background_vacuum(Relation rel, SiftTblMaintain *r)
{
	struct VacuumParams params = {0};

	params.freeze_min_age = params.freeze_table_age = params.multixact_freeze_min_age = params.multixact_freeze_table_age = -1;
#if PG_VERSION_NUM >= 190000
	params.log_vacuum_min_duration = -1;	/* renamed in 19 */
#else
	params.log_min_duration = -1;
#endif
	params.index_cleanup = VACOPTVALUE_ENABLED;
	params.truncate = tbl_truncates(rel) ? VACOPTVALUE_ENABLED : VACOPTVALUE_DISABLED;
	*r = (SiftTblMaintain) {.blocks_before = RelationGetNumberOfBlocks(rel)};
	space_cap = DEFRAG_RELOCATE_BYTES;
	sift_tbl_relation_vacuum(rel, &params, NULL);
	r->blocks_after = RelationGetNumberOfBlocks(rel);
}

/* The passes a worker makes after the one it was asked for, while they find something to do, and the pause between them (a fourth of sift.maintenance_naptime, 100 ms at least) */
#define DEFRAG_TRIES		20
#define DEFRAG_IDLE_TRIES	3

/* The tombstones the last pass of this worker left: the next one waits for them to grow, not for the rows it could not remove */
static uint64 worker_tombs = 0;
/* The last pass cleared dead rows (a VACUUM or the pass without the indexes): it found work even when it merged nothing and gave no block back */
static bool pass_cleared = false;

typedef enum WorkerPass
{
	PASS_SPACE,					/* the pass without the indexes, a whole VACUUM when the dead rows ask for it */
	PASS_SETTLE					/* what a writer does when it closes a write buffer (sift.table_async_promote) */
} WorkerPass;

/*
 * One pass in a transaction of its own, holding no snapshot, as the table VACUUM runs: the table and the worker lock only if free. A whole
 * VACUUM when the dead rows cross the threshold (and sift.table_auto_defrag_indexes), else the pass without the indexes.
 */
static bool
worker_pass(Oid relid, WorkerPass pass, SiftTblMaintain *r)
{
	Relation	rel;
	bool		ran = false;

	*r = (SiftTblMaintain) {.busy = true};
	pass_cleared = false;
	merge_cap = sift_tbl_auto_defrag_budget > 0 ? (uint64) sift_tbl_auto_defrag_budget << 20 : PG_UINT64_MAX;
	StartTransactionCommand();
	if (ConditionalLockRelationOid(relid, ShareUpdateExclusiveLock) && (rel = try_table_open(relid, NoLock)) != NULL)
	{
		if (sift_tbl_is_table(rel) && ConditionalLockPage(rel, SIFT_WORKER_LOCK_BLKNO, ExclusiveLock))
		{
			SiftTblRel *st = sift_tbl_rel(rel);

			sift_tbl_refresh(rel, st);
			if (pass == PASS_SETTLE)
			{
				r->promoted = sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), false, SIFT_TBL_SETTLE_STEPS);
				r->blocks_after = RelationGetNumberOfBlocks(rel);
			}
			else if (dead_wanted(rel, st, removable_tombs(rel, st), worker_tombs))
			{
				if (sift_tbl_auto_defrag_indexes)
					background_vacuum(rel, r);
				else
					sift_tbl_maintain(rel, true, true, r);
				pass_cleared = true;
				sift_tbl_refresh(rel, st);	/* the pass moved the log: the directory read before it points past the cut */
				sift_tbl_tomb_sync(rel, st);
				worker_tombs = removable_tombs(rel, st);
			}
			else
				sift_tbl_maintain(rel, true, false, r);
			ran = true;
			UnlockPage(rel, SIFT_WORKER_LOCK_BLKNO, ExclusiveLock);
		}
		table_close(rel, NoLock);
	}
	CommitTransactionCommand();
	return ran;
}

static bool
pass_found_work(const SiftTblMaintain *r, BlockNumber before)
{
	return r->promoted + r->merged > 0 || merge_capped || r->blocks_after < before || pass_cleared;
}

/*
 * The request comes while the writer that made it is at work: its rows are not final yet and the lock of the end of the file is not free. The
 * worker stays a few seconds and passes again until two passes in a row find nothing to do.
 */
static void
maint_worker(Datum main_arg, WorkerPass first)
{
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();
	Oid			relid = sift_maint_worker_connect(main_arg, &errctx);
	SiftTblMaintain r;
	int			idle;

	/* the page accesses of every pass are charged as autovacuum's are (vacuum_cost_limit), and the steps sleep for them */
	SetConfigOption("vacuum_cost_delay", psprintf("%d", sift_tbl_auto_defrag_delay), PGC_SUSET, PGC_S_OVERRIDE);
	VacuumUpdateCosts();
	/* the settling of write buffers is what keeps the directory from filling: only the pass for dead rows and space gives way to the load */
	if (first != PASS_SETTLE)
	{
		pass_max_active = sift_tbl_auto_defrag_max_active;
		pass_wal_rate = (uint64) sift_tbl_auto_defrag_wal_rate << 10;
		sift_tbl_coalesce = sift_tbl_auto_defrag_coalesce;
	}

	if (!sift_maint_wait_quiet(pass_max_active))
		return;
	/* the rate counts from here: the time spent waiting for a quiet moment is no credit */
	pass_start = GetCurrentTimestamp();
	pass_wal_start = pgWalUsage.wal_bytes;
	if (!worker_pass(relid, first, &r))
		return;
	/* a pass that found nothing leaves at once: the writer that asked is not waited for (only the settling of write buffers stays for the rows still coming) */
	idle = first == PASS_SPACE && !pass_found_work(&r, r.blocks_before) ? DEFRAG_IDLE_TRIES : 0;
	for (int i = 0; i < DEFRAG_TRIES && idle < DEFRAG_IDLE_TRIES; i++)
	{
		BlockNumber before = r.blocks_after;

		sift_nap(Max(sift_maintenance_naptime / 4, 100));
		if (!sift_maint_wait_quiet(pass_max_active))
			break;
		if (!worker_pass(relid, first == PASS_SETTLE ? PASS_SETTLE : PASS_SPACE, &r))
			break;
		idle = pass_found_work(&r, before) ? 0 : idle + 1;
	}
	StartTransactionCommand();
	{
		Relation	rel = try_table_open(relid, AccessShareLock);

		if (rel != NULL)
		{
			sift_log_maint(rel, "table maintenance worker finished", start, -1, (int64) RelationGetNumberOfBlocks(rel) * BLCKSZ);
			table_close(rel, AccessShareLock);
		}
	}
	CommitTransactionCommand();
}

void
sift_tbl_maint_worker_main(Datum main_arg)
{
	maint_worker(main_arg, PASS_SPACE);
}

void
sift_tbl_settle_worker_main(Datum main_arg)
{
	maint_worker(main_arg, PASS_SETTLE);
}

bool		sift_tbl_async_promote = false;

bool
sift_tbl_settle_async(Relation rel, SiftTblRel *st)
{
	/* under pressure the writer settles itself: it never depends on the worker to stay inside the directory */
	if (!sift_tbl_async_promote || !sift_async_maintenance || maintaining || !IsUnderPostmaster || RecoveryInProgress() || dir_pressure(st))
		return false;
	if (sift_maint_request_worker(rel, "sift_tbl_settle_worker_main"))
		st->settle_horizon = GetOldestNonRemovableTransactionId(rel);
	return true;
}

