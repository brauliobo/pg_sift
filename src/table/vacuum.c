/*
 * table/vacuum.c - VACUUM of a sift_table: dead rows, tombstones, indexes, the space step and the report
 * (docs/COL-TAM.md). The passes of the maintenance worker are in vacuum_maint.c, VACUUM FULL / CLUSTER in vacuum_copy.c.
 */
#include "vacuum_int.h"
#include "utils/timestamp.h"

/* A worker's limits, from sift.table_auto_defrag_max_active and sift.table_auto_defrag_wal_rate (a VACUUM of a session has none): clients running a statement above which the merges of a pass stop, and the WAL bytes per second it may average since the worker started */
int	pass_max_active = 0;
uint64 pass_wal_rate = 0;
TimestampTz pass_start;
uint64 pass_wal_start;

/* A VACUUM or a pass is at work in this backend: closing the write buffer it takes must not ask for a worker */
bool maintaining = false;
/* The bytes the space step may copy under the lock of the table; a worker lowers it */
uint64 space_cap = PG_UINT64_MAX;
/* The bytes the merge steps of a pass may rewrite (a worker lowers it to sift.table_auto_defrag_budget), and whether the last run of steps stopped at a limit with merges left */
uint64 merge_cap = PG_UINT64_MAX;
bool merge_capped = false;
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
		sift_nap(Min(ahead, 1000));
	}
}

/* The next free slot */
ItemPointer
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
bool
tid_has(ItemPointer itemptr, void *state)
{
	TidList    *d = state;

	return d->n > 0 && bsearch(itemptr, d->tid, d->n, sizeof(ItemPointerData), cmp_tid) != NULL;
}

static void
note_xid(VacState *v, TransactionId xid)
{
	if (TransactionIdIsNormal(xid) && TransactionIdPrecedes(xid, v->newest_kept))
		v->newest_kept = xid;
}

/* Is the deletion or update del tells of committed and older than every snapshot */
static bool
del_removable(const VacState *v, const SiftTblDel *del)
{
	return (del->state == SIFT_TBL_DEL_DELETED || del->state == SIFT_TBL_DEL_UPDATED) && TransactionIdPrecedes(del->xid, v->oldest);
}

/* The chain that goes on at key (the new version of a HOT update) is dead: its end is deleted or updated otherwise, for good. *key: the first link that is not removable */
static bool
chain_dead(const SiftTblRel *st, const VacState *v, uint64 *key)
{
	for (;;)
	{
		SiftTblDel	del;

		sift_tbl_del_state(st, *key, &del);
		if (!del_removable(v, &del))
			return false;
		if (!del.hot)
			return true;
		*key = del.succ;
	}
}

/*
 * Is the tombstone entry removable now: aborted, a lock of a finished transaction, or a delete whose row VACUUM has just flagged. The HOT update of
 * a chain member goes with the row (the entry of its root skips it); the one of a root whose chain lives stays, moved past the removable links, with
 * a frozen xid (a log entry is read as the update of a transaction every snapshot sees, and holds no xid back)
 */
static bool
tomb_keep(SiftTblTomb *e, void *arg)
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
	{
		uint64		next = e->succ;

		if (e->kind != SIFT_TBL_TOMB_UPDATE || !(e->flags & SIFT_TBL_TOMB_HOT) || sift_tbl_hot_member(v->st, e->key) || chain_dead(v->st, v, &next))
			return false;
		e->succ = next;
		e->xid = FrozenTransactionId;
		e->cid = 0;
		return true;
	}
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
				if (del_removable(v, &del))
				{
					uint64		next = del.succ;

					dead = true;
					if (del.hot && !chain_dead(st, v, &next))
						ItemPointerSet(tid_push(&v->redirects), ph->vblock, off);
				}
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
		if (e->kind == SIFT_TBL_TOMB_UPDATE && (e->flags & SIFT_TBL_TOMB_HOT))
		{
			uint64		next = e->succ;

			if (!chain_dead(st, v, &next))
				sift_key_to_tid(e->key, tid_push(&v->redirects));
		}
		*lo = Min(*lo, sb);
		*hi = Max(*hi, sb);
	}
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

/* A dead TID that VACUUM may tell the indexes of: not the root of a chain that goes on */
static bool
index_dead_has(ItemPointer itemptr, void *state)
{
	VacState   *v = state;

	return dead_has(itemptr, v) && !tid_has(itemptr, &v->redirects);
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
	tid_sort(&v->redirects);
	if (v->dead_runs.n > 0)
		qsort(v->dead_runs.run, v->dead_runs.n, sizeof(SiftKey[2]), sift_cmp_u64);
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
			res = index_bulk_delete(&ivinfo, NULL, index_dead_has, v);
		res = index_vacuum_cleanup(&ivinfo, res);
		if (res)
			pfree(res);
	}
	vac_close_indexes(nindexes, irel, NoLock);
	return nindexes;
}

/* Would the tombstone rewrite change the log, by dropping an entry or freezing a root? tomb_keep notes the xids of the entries kept either way */
static bool
tomb_changes(SiftTblRel *st, VacState *v)
{
	bool		changes = false;

	for (uint32 i = 0; i < st->nents; i++)
	{
		TransactionId xid = st->ents[i].xid;

		changes |= !tomb_keep(&st->ents[i], v) || st->ents[i].xid != xid;
	}
	for (uint32 i = 0; i < st->nrecent; i++)
	{
		TransactionId xid = st->recent[i].xid;

		changes |= !tomb_keep(&st->recent[i], v) || st->recent[i].xid != xid;
	}
	return changes;
}

/* The table buffer adds a virtual-to-physical page vector and overflow links to the common lane layout. */
void
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

/* Merge steps while they find something, up to merge_cap bytes (a worker's budget; a VACUUM has none), a cost delay after each; the steps made */
int
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
	if (tomb_changes(st, v))
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
	v.st = st;
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

bool		sift_tbl_auto_defrag = true;
int			sift_tbl_auto_defrag_free_pct = 25;
int			sift_tbl_auto_defrag_delay = 10;
int			sift_tbl_auto_defrag_budget = 128;
int			sift_tbl_auto_defrag_dead_pct = 5;
int			sift_tbl_auto_defrag_max_active = 0;
int			sift_tbl_auto_defrag_wal_rate = 0;
bool		sift_tbl_auto_defrag_coalesce = true;
bool		sift_tbl_auto_defrag_indexes = true;

/*
 * The heap reloption vacuum_truncate (a table access method has no options of its own, so the heap ones apply), read from the catalog row
 * because the struct that holds it changes between releases
 */
bool
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
int64
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
