/*
 * table/vacuum.c - VACUUM, VACUUM FULL / CLUSTER (rewrite), CREATE INDEX CONCURRENTLY validation (docs/COL-TAM.md section 7).
 *
 * VACUUM: write buffer rows whose insertion aborted, or whose deleter is committed and older than OldestXmin, are flagged DEAD (and are
 * the dead TIDs the indexes drop); committed old insertions are frozen; then the tombstone segment is rewritten without the
 * entries that no longer matter. TIDs are never reused, so no ordering between these steps is needed for safety. Column segments
 * left mostly dead are rewritten, and the free run at the end of the file goes back to the operating system.
 *
 * VACUUM FULL / CLUSTER write the kept versions straight into column segments of the new table (sift_tbl_bulk_*).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/multixact.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/bufmgr.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"

#include "col/seg.h"
#include "core/lane.h"
#include "core/ops.h"
#include "bm25/maint.h"
#include "table/table.h"
#include "table/seg_int.h"
#include "core/cmp.h"

/* The cost delay of the pass (autovacuum's, or a worker's sift.table_auto_defrag_delay): a point where the page accesses so far may make it sleep */
#if PG_VERSION_NUM >= 180000
#define COST_DELAY()	vacuum_delay_point(false)
#else
#define COST_DELAY()	vacuum_delay_point()
#endif

/* A worker's limits, from sift.table_auto_defrag_max_active and sift.table_auto_defrag_wal_rate (a VACUUM of a session has none): clients running a statement above which the merges of a pass stop, and the WAL bytes per second it may average since the worker started */
static int	pass_max_active = 0;
static uint64 pass_wal_rate = 0;
static TimestampTz pass_start;
static uint64 pass_wal_start;

/* A VACUUM or a pass is at work in this backend: closing the write buffer it takes must not ask for a worker */
static bool maintaining = false;
/* The bytes the space step may copy under the lock of the table; a worker lowers it */
static uint64 space_cap = PG_UINT64_MAX;
/* The bytes the merge steps of a pass may rewrite (a worker lowers it to sift.table_auto_defrag_budget), and whether the last run of steps stopped at a limit with merges left */
static uint64 merge_cap = PG_UINT64_MAX;
static bool merge_capped = false;

/* A growable array of TIDs: the dead rows VACUUM found, the index entries CREATE INDEX CONCURRENTLY already has (sorted to look up) */
typedef struct TidList
{
	ItemPointerData *tid;
	uint64		n;
	uint64		cap;
} TidList;

/* The cost delay, then the sleep that brings the WAL the worker wrote back to its rate */
static void
pass_delay(void)
{
	COST_DELAY();
	while (pass_wal_rate > 0)
	{
		int64		ahead = (int64) ((pgWalUsage.wal_bytes - pass_wal_start) * 1000 / pass_wal_rate);

		ahead -= TimestampDifferenceMilliseconds(pass_start, GetCurrentTimestamp());
		if (ahead <= 0)
			break;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, Min(ahead, 1000), PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/* The next free slot */
static ItemPointer
tid_push(TidList *d)
{
	if (d->n == d->cap)
	{
		d->cap = Max(1024, d->cap * 2);
		d->tid = d->tid ? repalloc_huge(d->tid, d->cap * sizeof(ItemPointerData)) :
			palloc_extended(d->cap * sizeof(ItemPointerData), MCXT_ALLOC_HUGE);
	}
	return &d->tid[d->n++];
}

static int
cmp_tid(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

static void
tid_sort(TidList *d)
{
	qsort(d->tid, d->n, sizeof(ItemPointerData), cmp_tid);
}

/* In the sorted list? */
static bool
tid_has(ItemPointer itemptr, void *state)
{
	TidList    *d = state;

	return d->n > 0 && bsearch(itemptr, d->tid, d->n, sizeof(ItemPointerData), cmp_tid) != NULL;
}

typedef struct VacState
{
	TransactionId oldest;
	TransactionId newest_kept;	/* oldest xid still stored: relfrozenxid */
	double		live;			/* as a heap counts them: in-progress insertions left out, in-progress deletions in */
	double		dead;			/* recently dead: deleter committed but not removable yet */
	double		group_dead;		/* of those, rows of the column groups (their liveness bit is still set) */
	TidList	dead_tids;
	SiftTblKeyRuns dead_runs;	/* sorted: the rows of the groups with unfrozen rows whose inserter aborted */
} VacState;

static void
note_xid(VacState *v, TransactionId xid)
{
	if (TransactionIdIsNormal(xid) && TransactionIdPrecedes(xid, v->newest_kept))
		v->newest_kept = xid;
}

/* Is the tombstone entry removable now: aborted, a lock of a finished transaction, or a delete whose row VACUUM has just flagged */
static bool
tomb_keep(const SiftTblTomb *e, void *arg)
{
	VacState   *v = arg;

	if (sift_tbl_xid_aborted(e->xid))
		return false;
	if (e->kind == SIFT_TBL_TOMB_LOCK)
	{
		if (TransactionIdIsInProgress(e->xid) || TransactionIdIsCurrentTransactionId(e->xid))
		{
			note_xid(v, e->xid);
			return true;
		}
		return false;
	}
	if (TransactionIdDidCommit(e->xid) && TransactionIdPrecedes(e->xid, v->oldest))
		return false;
	note_xid(v, e->xid);
	return true;
}

/* Vacuums one write buffer page: flags dead rows, freezes old insertions; one WAL record */
static void
vacuum_page(Relation rel, SiftTblRel *st, const SiftTblSeg *seg, uint32 idx, VacState *v)
{
	Buffer		buf = sift_tbl_lock_page(rel, seg, idx);
	GenericXLogState *state = GenericXLogStart(rel);
	Page		page = GenericXLogRegisterBuffer(state, buf, 0);
	SiftTblPageHdr *ph = TBL_PAGE(page);
	bool		changed = false;

	if (!(ph->flags & SIFT_TBL_PAGE_OVFDATA))
		for (uint32 off = 1; off <= ph->nrows; off++)
		{
			SiftTblRec *rec;
			SiftTblDel	del;
			uint64		key = ((uint64) ph->vblock << 16) | off;
			bool		dead = false;

			if (ph->slot[off - 1] == 0)
				continue;
			rec = (SiftTblRec *) (SiftPageGetData(page) + ph->slot[off - 1]);
			if (rec->flags & SIFT_TBL_REC_DEAD)
				continue;
			if (TransactionIdIsNormal(rec->xmin) && sift_tbl_xid_aborted(rec->xmin))
				dead = true;
			else
			{
				sift_tbl_del_state(st, key, &del);
				if ((del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED) &&
					TransactionIdPrecedes(del.xid, v->oldest))
					dead = true;
				else if (del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED)
					v->dead += 1;
				else if (!TransactionIdIsNormal(rec->xmin) || !TransactionIdIsInProgress(rec->xmin))
					v->live += 1;
			}
			if (dead)
			{
				rec->flags |= SIFT_TBL_REC_DEAD;
				changed = true;
				ItemPointerSet(tid_push(&v->dead_tids), ph->vblock, off);
				continue;
			}
			if (TransactionIdIsNormal(rec->xmin) && !TransactionIdIsCurrentTransactionId(rec->xmin) &&
				!TransactionIdIsInProgress(rec->xmin) && TransactionIdPrecedes(rec->xmin, v->oldest))
			{
				rec->xmin = FrozenTransactionId;
				changed = true;
			}
			note_xid(v, rec->xmin);
		}
	if (changed)
		GenericXLogFinish(state);
	else
		GenericXLogAbort(state);
	UnlockReleaseBuffer(buf);
}

/* Is the vblock in a write buffer (whose rows vacuum_page judges) */
static bool
in_wbuf(const SiftTblRel *st, uint64 vblock)
{
	for (int i = 0; i < st->nwbuf; i++)
		if (vblock >= st->wbuf[i].vbase && vblock < st->wbuf[i].vbase + st->wbuf[i].vcap)
			return true;
	return false;
}

/*
 * The dead TIDs the tombstones say for rows of the column groups: committed deletions older than OldestXmin; [*lo, *hi] their
 * superblocks. Committed deletions not old enough yet are counted as recently dead. A row has one committed deletion at most.
 */
static void
group_dead(const SiftTblRel *st, VacState *v, const SiftTblTomb *a, uint32 n, uint32 *lo, uint32 *hi)
{
	for (uint32 i = 0; i < n; i++)
	{
		const SiftTblTomb *e = &a[i];
		uint32		sb = (e->key >> 16) / SIFT_SB_PAGES;

		if (e->kind == SIFT_TBL_TOMB_LOCK || in_wbuf(st, e->key >> 16) || !TransactionIdDidCommit(e->xid))
			continue;
		if (!TransactionIdPrecedes(e->xid, v->oldest))
		{
			v->dead += 1;
			v->group_dead += 1;
			continue;
		}
		sift_key_to_tid(e->key, tid_push(&v->dead_tids));
		*lo = Min(*lo, sb);
		*hi = Max(*hi, sb);
	}
}

static int
cmp_run(const void *a, const void *b)
{
	SiftKey		x = ((const SiftKey *) a)[0],
				y = ((const SiftKey *) b)[0];

	return SIFT_CMP3(x, y);
}

/* Is the row dead for the indexes: a dead TID, or a row of an aborted writer in a group (a key in one of the runs) */
static bool
dead_has(ItemPointer itemptr, void *state)
{
	VacState   *v = state;
	SiftKey		key = sift_key_from_tid(itemptr);
	int			lo = 0,
				hi = v->dead_runs.n;

	if (tid_has(itemptr, &v->dead_tids))
		return true;
	SIFT_BISECT(int, mid, lo, hi, v->dead_runs.run[mid][0] <= key);
	return lo > 0 && key <= v->dead_runs.run[lo - 1][1];
}

/* The writers of the groups with unfrozen rows hold the oldest xid back */
static void
note_groups(const SiftTblRel *st, VacState *v)
{
	for (int i = 0; i < st->dir.nsegs; i++)
		if (sift_tbl_seg_unfrozen(&st->dir.segs[i]))
			note_xid(v, st->dir.segs[i].reserved);
}

/* Steps 1 and 2: flags the dead write buffer rows, clears the liveness of the dead group rows, counts the rows */
static void
vacuum_rows(Relation rel, SiftTblRel *st, VacState *v)
{
	uint32		sb_lo = PG_UINT32_MAX,
				sb_hi = 0;

	sift_tbl_tomb_sync(rel, st);
	for (int i = 0; i < st->nwbuf; i++)
	{
		SiftTblSeg	seg = st->wbuf[i];

		sift_tbl_seg_load(rel, &seg);
		for (uint32 p = 0; p < seg.npages; p++)
		{
			CHECK_FOR_INTERRUPTS();
			vacuum_page(rel, st, &seg, p, v);
			pass_delay();
		}
	}
	group_dead(st, v, st->ents, st->nents, &sb_lo, &sb_hi);
	group_dead(st, v, st->recent, st->nrecent, &sb_lo, &sb_hi);
	v->dead_runs.mcxt = CurrentMemoryContext;
	sift_tbl_aborted_runs(rel, st, &v->dead_runs, &sb_lo, &sb_hi);
	tid_sort(&v->dead_tids);
	if (v->dead_runs.n > 0)
		qsort(v->dead_runs.run, v->dead_runs.n, sizeof(SiftKey[2]), cmp_run);
	if (sb_lo <= sb_hi)
		(void) sift_tbl_group_bulkdelete(rel, st, dead_has, v, sb_lo, sb_hi);
	/* the bulk delete flagged the segments dirty: their liveness is read from entries that say so */
	sift_tbl_refresh(rel, st);
	v->live += sift_tbl_group_live_rows(rel, st) - v->group_dead;
}

/* Step 3; returns the number of indexes */
static int
vacuum_indexes(Relation rel, VacState *v, BufferAccessStrategy bstrategy)
{
	Relation   *irel;
	int			nindexes;

	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &irel);
	for (int i = 0; i < nindexes; i++)
	{
		IndexVacuumInfo ivinfo = {.index = irel[i],.heaprel = rel,.analyze_only = false,.report_progress = false,
		.estimated_count = false,.message_level = DEBUG2,.num_heap_tuples = v->live + v->dead,.strategy = bstrategy};
		IndexBulkDeleteResult *res = NULL;

		if (v->dead_tids.n > 0 || v->dead_runs.n > 0)
			res = index_bulk_delete(&ivinfo, NULL, dead_has, v);
		res = index_vacuum_cleanup(&ivinfo, res);
		if (res)
			pfree(res);
	}
	vac_close_indexes(nindexes, irel, NoLock);
	return nindexes;
}

/* Would the tombstone rewrite drop an entry? tomb_keep notes the xids of the entries kept either way */
static bool
tomb_drops(SiftTblRel *st, VacState *v)
{
	bool		drops = false;

	for (uint32 i = 0; i < st->nents; i++)
		drops |= !tomb_keep(&st->ents[i], v);
	for (uint32 i = 0; i < st->nrecent; i++)
		drops |= !tomb_keep(&st->recent[i], v);
	return drops;
}

/* The table buffer adds a virtual-to-physical page vector and overflow links to the common lane layout. */
static void
relocate_page(Page page, const SiftExtent *old, const SiftExtent *fresh, int n)
{
	if (SiftPageGetOpaque(page)->type == SIFT_PT_WBUFHEADER)
	{
		SiftTblWbufHdr *h = TBL_WBUF_HDR(page);

		for (uint32 i = 0; i < h->npages; i++)
			h->page[i] = sift_store_relocated_block(h->page[i], old, fresh, n);
	}
	else if (SiftPageGetOpaque(page)->type == SIFT_PT_WBUFDATA)
	{
		SiftTblPageHdr *h = TBL_PAGE(page);

		if (h->flags & SIFT_TBL_PAGE_OVFDATA)
			h->ovf_next = sift_store_relocated_block(h->ovf_next, old, fresh, n);
		else
			for (uint32 i = 1; i <= h->nrows; i++)
			{
				SiftTblRec *rec = (SiftTblRec *) sift_tbl_page_rec(page, SiftPageGetOpaque(page)->gen, i);

				if (rec && (rec->flags & SIFT_TBL_REC_OVF))
					rec->aux = sift_store_relocated_block(rec->aux, old, fresh, n);
			}
	}
}

/* Closes the open write buffer segment once it is worth a column segment (pages) */
#define VACUUM_CLOSE_PAGES	8

/* Merge steps while they find something, up to merge_cap bytes (a worker's budget; a VACUUM has none), a cost delay after each; the steps made */
static int
merge_steps(Relation rel, SiftTblRel *st, TransactionId oldest)
{
	uint64		bytes = 0,
				done;
	int			steps = 0;

	while (bytes < merge_cap && !sift_maint_busy(pass_max_active) && (done = sift_tbl_merge(rel, st, oldest)) > 0)
	{
		bytes += done;
		steps++;
		pass_delay();
	}
	merge_capped = bytes >= merge_cap;
	return steps;
}

/*
 * Steps 4 and 5, then the space: the blocks released before (by earlier VACUUMs and writers) are recycled, and the end of the file is
 * given back (sift_maint_space): under AccessExclusiveLock, which keeps every reader out, what this VACUUM released is reusable at once
 * (a lazy VACUUM's own xid is in no snapshot or horizon, so outside that lock it is reused once another transaction has ended), so the
 * segments that end the file are written again into the free blocks below them and the cut finds the end free.
 */
static void
vacuum_space(Relation rel, SiftTblRel *st, VacState *v, bool truncate, int elevel)
{
	(void) sift_tbl_drop_dead_groups(rel, st);
	sift_tbl_close_wbuf(rel, st, VACUUM_CLOSE_PAGES);
	(void) sift_tbl_promote(rel, st, v->oldest, true);
	(void) sift_tbl_freeze_groups(rel, st, v->oldest);
	(void) merge_steps(rel, st, v->oldest);
	if (tomb_drops(st, v))
		(void) sift_tbl_tomb_rewrite(rel, st, tomb_keep, v);
	(void) sift_recycle(rel);
	(void) sift_maint_orphan_sweep(rel);
	if (truncate)
		sift_maint_space_budget(rel, relocate_page, elevel, Min((uint64) sift_vacuum_rewrite_limit << 20, space_cap));
}

/* Step 6, with the counts of a heap: reltuples and the live rows, the recently dead ones for autovacuum */
static void
vacuum_report(Relation rel, const VacState *v, const struct VacuumCutoffs *cutoffs, bool hasindex, TimestampTz start, int elevel)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	bool		fz,
				mz;

#if PG_VERSION_NUM >= 180000
	vac_update_relstats(rel, nblocks, v->live, 0, 0, hasindex, v->newest_kept, cutoffs->OldestMxact, &fz, &mz, false);
#else
	vac_update_relstats(rel, nblocks, v->live, 0, hasindex, v->newest_kept, cutoffs->OldestMxact, &fz, &mz, false);
#endif
	sift_pgstat_report_vacuum(rel, (PgStat_Counter) v->live, (PgStat_Counter) v->dead, start);
	ereport(elevel, (errmsg("table \"%s\": rows: %llu removed, %.0f remain, %.0f are dead but not yet removable",
							RelationGetRelationName(rel), (unsigned long long) v->dead_tids.n, v->live, v->dead)));
}

void
sift_tbl_relation_vacuum(Relation rel, SIFT_TAM_VACUUM_PARAMS, BufferAccessStrategy bstrategy)
{
	VacState	v = {0};
	struct VacuumCutoffs cutoffs;
	SiftTblRel *st = sift_tbl_rel(rel);
	TimestampTz start = GetCurrentTimestamp();
	int			elevel = (params->options & VACOPT_VERBOSE) ? INFO : DEBUG2;
	int			nindexes;

	(void) vacuum_get_cutoffs(rel, params, &cutoffs);
	v.oldest = cutoffs.OldestXmin;
	v.newest_kept = cutoffs.OldestXmin;
	sift_tbl_refresh(rel, st);
	sift_maint_lock(rel, true);
	maintaining = true;
	PG_TRY();
	{
		if (st->initialized)
			vacuum_rows(rel, st, &v);
		nindexes = vacuum_indexes(rel, &v, bstrategy);
		if (st->initialized)
		{
			vacuum_space(rel, st, &v, params->truncate == VACOPTVALUE_ENABLED, elevel);
			note_groups(st, &v);
		}
	}
	PG_FINALLY();
	{
		maintaining = false;
	}
	PG_END_TRY();
	sift_maint_unlock(rel);
	vacuum_report(rel, &v, &cutoffs, nindexes > 0, start, elevel);
}

bool		sift_tbl_auto_defrag = false;
int			sift_tbl_auto_defrag_free_pct = 25;
int			sift_tbl_auto_defrag_delay = 10;
int			sift_tbl_auto_defrag_budget = 128;
int			sift_tbl_auto_defrag_dead_pct = 20;
int			sift_tbl_auto_defrag_max_active = 0;
int			sift_tbl_auto_defrag_wal_rate = 0;
bool		sift_tbl_auto_defrag_coalesce = true;
bool		sift_tbl_auto_defrag_indexes = true;

/* The bytes a worker copies under AccessExclusiveLock at most: the window in which the table is closed to readers stays short */
#define DEFRAG_RELOCATE_BYTES	((uint64) 32 << 20)

/*
 * The heap reloption vacuum_truncate (a table access method has no options of its own, so the heap ones apply), read from the catalog row
 * because the struct that holds it changes between releases
 */
static bool
tbl_truncates(Relation rel)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
	bool		truncates = true;

	if (HeapTupleIsValid(tup))
	{
		bool		isnull;
		Datum		opts = SysCacheGetAttr(RELOID, tup, Anum_pg_class_reloptions, &isnull);

		if (!isnull)
			foreach_ptr(DefElem, e, untransformRelOptions(opts))
			{
				if (e->defnamespace == NULL && strcmp(e->defname, "vacuum_truncate") == 0)
					truncates = defGetBoolean(e);
			}
		ReleaseSysCache(tup);
	}
	return truncates;
}

/*
 * Step 2 of VACUUM alone: the liveness bits of the group rows whose deletion is committed and older than the horizon are cleared. The
 * tombstone entries stay, and with them the dead set of the next VACUUM, which still hands every one of these TIDs to the indexes (this one
 * does not touch them; it finds the entries as the rows of a VACUUM that stopped before its index cleanup). The rows of an aborted writer
 * are not cleared: only the live bits say which they are, so the dead set would be lost.
 */
static int64
clear_removable(Relation rel, SiftTblRel *st, VacState *v)
{
	uint32		sb_lo = PG_UINT32_MAX,
				sb_hi = 0;
	int64		removed = 0;

	sift_tbl_tomb_sync(rel, st);
	group_dead(st, v, st->ents, st->nents, &sb_lo, &sb_hi);
	group_dead(st, v, st->recent, st->nrecent, &sb_lo, &sb_hi);
	tid_sort(&v->dead_tids);
	if (sb_lo <= sb_hi)
	{
		removed = sift_tbl_group_bulkdelete(rel, st, dead_has, v, sb_lo, sb_hi);
		sift_tbl_refresh(rel, st);
	}
	return removed;
}

void
sift_tbl_maintain(Relation rel, bool background, bool clear, SiftTblMaintain *r)
{
	VacState	v = {0};
	SiftTblRel *st = sift_tbl_rel(rel);
	TransactionId oldest = GetOldestNonRemovableTransactionId(rel);
	uint64		budget = (uint64) sift_vacuum_rewrite_limit << 20;

	*r = (SiftTblMaintain) {.blocks_before = RelationGetNumberOfBlocks(rel)};
	r->blocks_after = r->blocks_before;
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
	for (uint32 i = 0; i < st->nents; i++)
		n += TransactionIdPrecedes(st->ents[i].xid, horizon);
	for (uint32 i = 0; i < st->nrecent; i++)
		n += TransactionIdPrecedes(st->recent[i].xid, horizon);
	return n;
}

static bool
dead_wanted(Relation rel, SiftTblRel *st, uint64 tombs, uint64 seen)
{
	uint64		rows = 0;

	for (int i = 0; i < st->dir.nsegs; i++)
		if (st->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			rows += st->dir.segs[i].ndocs;
	return sift_tbl_auto_defrag_dead_pct < 100 && rows > 0 && tombs * 100 >= rows * sift_tbl_auto_defrag_dead_pct &&
		tombs >= seen && (tombs - seen) * 200 >= rows * sift_tbl_auto_defrag_dead_pct;
}

static void
auto_defrag_check(Relation rel, SiftTblRel *st)
{
	BlockNumber nblocks;
	SiftFreeStats fs;
	uint64		tombs;
	bool		dead,
				space;

	if (!sift_tbl_auto_defrag || !sift_async_maintenance || maintaining || !IsUnderPostmaster || RecoveryInProgress())
		return;
	sift_tbl_refresh(rel, st);
	if (!st->initialized)
		return;
	tombs = removable_tombs(rel, st);
	nblocks = RelationGetNumberOfBlocks(rel);
	fs = sift_store_free_stats(rel);
	/* a VACUUM rewrote the log, or a pass or a truncation used the free blocks: what is asked next is relative to what is left */
	st->defrag_tombs = Min(st->defrag_tombs, tombs);
	st->defrag_free = Min(st->defrag_free, fs.blocks);
	dead = dead_wanted(rel, st, tombs, st->defrag_tombs);
	space = fs.blocks * 100 >= (int64) nblocks * sift_tbl_auto_defrag_free_pct && sift_maint_space_worth(fs.blocks, nblocks) &&
		fs.blocks >= st->defrag_free + (int64) nblocks / 32;
	if (!dead && !space)
		return;
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
	int			idle = 0;

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
	for (int i = 0; i < DEFRAG_TRIES && idle < DEFRAG_IDLE_TRIES; i++)
	{
		BlockNumber before = r.blocks_after;

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, Max(sift_maintenance_naptime / 4, 100), PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
		if (!sift_maint_wait_quiet(pass_max_active))
			break;
		if (!worker_pass(relid, first == PASS_SETTLE ? PASS_SETTLE : PASS_SPACE, &r))
			break;
		idle = (r.promoted + r.merged > 0 || merge_capped || r.blocks_after < before) ? 0 : idle + 1;
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
	(void) sift_maint_request_worker(rel, "sift_tbl_settle_worker_main");
	return true;
}

typedef struct CopyState
{
	SiftTblRel *st;
	TransactionId oldest;
	double	   *num_tuples;
	double	   *tups_vacuumed;
	double	   *tups_recently_dead;
} CopyState;

/*
 * Does the rewrite keep the version: inserted (committed, or by this transaction) and not deleted. A deletion by this transaction is
 * final for the new table: the rewrite commits or aborts with it, and the table is locked against every other.
 */
static bool
copy_keep(const SiftTblRowVer *rv, void *arg)
{
	CopyState  *c = arg;
	SiftTblDel	del;

	if ((rv->flags & SIFT_TBL_REC_DEAD) || (TransactionIdIsNormal(rv->xmin) && sift_tbl_xid_aborted(rv->xmin)))
		*c->tups_vacuumed += 1;
	else if (sift_tbl_del_state(c->st, rv->key, &del), del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED)
		*(TransactionIdPrecedes(del.xid, c->oldest) ? c->tups_vacuumed : c->tups_recently_dead) += 1;
	else if (del.state == SIFT_TBL_DEL_SELF)
		*c->tups_recently_dead += 1;
	else
	{
		*c->num_tuples += 1;
		return true;
	}
	return false;
}

/* CLUSTER by an index scan (an index that cannot order a sort): the versions in the order of OldIndex, each fetched once */
static void
copy_index_order(Relation OldTable, Relation OldIndex, SiftTblBulk *bulk, CopyState *c)
{
	IndexScanDesc iscan = sift_index_beginscan(OldTable, OldIndex, SnapshotAny, 0);
	ItemPointer tid;
	SiftTblRowVer rv;

	index_rescan(iscan, NULL, 0, NULL, 0);
	while ((tid = index_getnext_tid(iscan, ForwardScanDirection)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		if (!sift_tbl_fetch_row(OldTable, c->st, tid, &rv))
			continue;
		if (copy_keep(&rv, c))
			sift_tbl_bulk_tuple(bulk, rv.tuple);
		sift_tbl_free_row(&rv);
	}
	index_endscan(iscan);
}

/* The versions as a scan returns them, as tuples, into the new table or into sort: column segments of another layout are read by attribute */
static void
copy_scan(Relation OldTable, SiftTblBulk *bulk, Tuplesortstate *sort, CopyState *c)
{
	TableScanDesc scan = sift_table_beginscan(OldTable, SnapshotAny, 0, NULL);
	SiftTblRowVer rv;

	while (sift_tbl_scan_next_stored(scan, &rv))
	{
		CHECK_FOR_INTERRUPTS();
		if (copy_keep(&rv, c))
		{
			if (sort)
				tuplesort_putheaptuple(sort, rv.tuple);
			else
				sift_tbl_bulk_tuple(bulk, rv.tuple);
		}
		heap_freetuple(rv.tuple);
	}
	table_endscan(scan);
}

/* CLUSTER by a sort in the order of a btree index */
static void
copy_sorted(Relation OldTable, Relation OldIndex, SiftTblBulk *bulk, CopyState *c)
{
	Tuplesortstate *sort = tuplesort_begin_cluster(RelationGetDescr(OldTable), OldIndex, maintenance_work_mem, NULL, TUPLESORT_NONE);
	HeapTuple	tup;

	copy_scan(OldTable, NULL, sort, c);
	tuplesort_performsort(sort);
	while ((tup = tuplesort_getheaptuple(sort, true)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		sift_tbl_bulk_tuple(bulk, tup);
	}
	tuplesort_end(sort);
}

/*
 * The kept versions are written into the new table as frozen column segments with dense TIDs: in the order of the index for CLUSTER,
 * else the units of the column groups re-keyed as they decode and then the write buffer rows. Recently dead versions are dropped (no snapshot
 * of the new table can want them). A TID fetch decodes the unit of its superblock, so CLUSTER sorts whenever the index can order a
 * sort (btree), whatever the planner's heap costs chose (use_sort): an index scan in random order would decode a unit per row.
 */
void
sift_tbl_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort,
								   TransactionId OldestXmin, SIFT_TAM_CLUSTER_SNAPSHOT TransactionId *xid_cutoff, MultiXactId *multi_cutoff,
								   double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
	SiftTblRel *new_st = sift_tbl_rel(NewTable);
	CopyState	c = {sift_tbl_rel(OldTable), OldestXmin, num_tuples, tups_vacuumed, tups_recently_dead};
	SiftTblBulk *bulk;

	*num_tuples = *tups_vacuumed = *tups_recently_dead = 0;
	new_st->fresh = true;
	sift_tbl_layout_adopt(NewTable, new_st, OldTable);
	sift_tbl_ensure_init(NewTable);
	sift_tbl_refresh(OldTable, c.st);
	sift_tbl_tomb_sync(OldTable, c.st);
	bulk = sift_tbl_bulk_begin(NewTable, new_st);
	if (OldIndex != NULL && OldIndex->rd_rel->relam == BTREE_AM_OID)
		copy_sorted(OldTable, OldIndex, bulk, &c);
	else if (OldIndex != NULL)
		copy_index_order(OldTable, OldIndex, bulk, &c);
	else if (!sift_tbl_bulk_copy(bulk, OldTable, c.st, copy_keep, &c))
		copy_scan(OldTable, bulk, NULL, &c);
	sift_tbl_bulk_finish(bulk);
	*xid_cutoff = OldestXmin;
	*multi_cutoff = GetOldestMultiXactId();
}

void
sift_tbl_index_validate_scan(Relation table_rel, Relation index_rel, IndexInfo *indexInfo, Snapshot snapshot,
							 ValidateIndexState *state)
{
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = table_slot_create(table_rel, NULL);
	TableScanDesc scan;
	Datum		ts_val;
	bool		ts_isnull;
	TidList		have = {0};
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	econtext->ecxt_scantuple = slot;
	indexInfo->ii_PredicateState = ExecPrepareQual(indexInfo->ii_Predicate, estate);
	/* the TIDs the first pass found in the index, sorted (every one of them is looked up below) */
	while (tuplesort_getdatum(state->tuplesort, true, false, &ts_val, &ts_isnull, NULL))
	{
		itemptr_decode(tid_push(&have), DatumGetInt64(ts_val));
		state->itups += 1;
	}
	scan = sift_table_beginscan(table_rel, snapshot, 0, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		ItemPointerData tid = slot->tts_tid;

		CHECK_FOR_INTERRUPTS();
		state->htups += 1;
		if (tid_has(&tid, &have))
			continue;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (indexInfo->ii_PredicateState != NULL && !ExecQual(indexInfo->ii_PredicateState, econtext))
			continue;
		FormIndexDatum(indexInfo, slot, estate, values, isnull);
		index_insert(index_rel, values, isnull, &tid, table_rel,
					 indexInfo->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO, false, indexInfo);
		state->tups_inserted += 1;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	indexInfo->ii_PredicateState = NULL;
	if (have.tid)
		pfree(have.tid);
}
