/*
 * col/v1/tomblog.c - the tombstone log of a heap column index with track_changes (docs/COL-TAM.md section 2.4 holds the log; core/v1_col/tomb.h is its code):
 * the entry of a heap address that stopped being live, with the transaction and command that said so. The cache of the log a backend reads
 * is kept apart from the cached state of the index (SiftColRel): a relcache invalidation frees that, and the log is read incrementally.
 */
#include "postgres.h"

#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "lib/ilist.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/v1/maint/maint_int.h"
#include "core/v1_col/tomb.h"
#include "core/v1_col/tomb_dv.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"

typedef struct LogEnt
{
	Oid			relid;			/* hash key */
	uint32		hash;			/* of its pg_class row in the RELOID syscache, which a callback names it by */
	bool		stale;			/* an invalidation of the index's pg_class row: has it been dropped? (reap_stale); the entry is then on log_stale */
	dlist_node	lru;			/* log_lru: the entries in the order of their last read, the oldest first */
	dlist_node	stale_node;		/* log_stale */
	RelFileNumber relnumber;
	bool		has_seg;		/* the index has its tombstone segment (an index does not lose it: VACUUM replaces it, a rebuild is another file) */
	MemoryContext cxt;
	SiftDirSnapshot *dir;
	int			pins;			/* the statements that hold the log: the contexts that were asked for it and are not reset yet */
	MemoryContext pin_cx;		/* the last of them: one pin for each context, not one for each call */
	uint64		tick;
	SiftTombLog log;
} LogEnt;

typedef struct LogPin
{
	MemoryContextCallback cb;
	Oid			relid;
} LogPin;

static HTAB *log_tab;
static dlist_head log_lru = DLIST_STATIC_INIT(log_lru);
static dlist_head log_stale = DLIST_STATIC_INIT(log_stale);
static bool log_inval_all;		/* every entry is stale: reap_stale marks them, so that the callback that says it does no work */

static void
log_unmark(LogEnt *e)
{
	if (e->stale)
		dlist_delete(&e->stale_node);
	e->stale = false;
}

static void
log_mark(LogEnt *e)
{
	if (e->stale)
		return;
	dlist_push_tail(&log_stale, &e->stale_node);
	e->stale = true;
}

static void
log_drop(LogEnt *e)
{
	Oid			relid = e->relid;

	log_unmark(e);
	dlist_delete(&e->lru);
	sift_tomb_forget(&e->log);
	MemoryContextDelete(e->cxt);
	(void) hash_search(log_tab, &relid, HASH_REMOVE, NULL);
}

/* the entries are in the order of their last read: the first that no statement holds is the least recently used */
static LogEnt *
log_victim(void)
{
	dlist_iter	it;

	dlist_foreach(it, &log_lru)
	{
		LogEnt	   *e = dlist_container(LogEnt, lru, it.cur);

		if (e->pins == 0)
			return e;
	}
	return NULL;
}

static uint64
log_oldest(void)
{
	LogEnt	   *e = log_victim();

	return e ? e->tick : UINT64_MAX;
}

static bool
log_evict_one(void)
{
	LogEnt	   *e = log_victim();

	if (e == NULL)
		return false;
	log_drop(e);
	return true;
}

static void
log_evict_all(void)
{
	dlist_mutable_iter it;

	dlist_foreach_modify(it, &log_lru)
	{
		LogEnt	   *e = dlist_container(LogEnt, lru, it.cur);

		if (e->pins == 0)
			log_drop(e);
	}
}

static const SiftCacheKind log_kind = {"col_tomblog", log_oldest, log_evict_one, log_evict_all};

static void
log_unpin(void *arg)
{
	LogPin	   *p = arg;
	LogEnt	   *e = log_tab ? hash_search(log_tab, &p->relid, HASH_FIND, NULL) : NULL;

	if (e != NULL)
	{
		e->pins--;
		e->pin_cx = NULL;
	}
}

/* the log is held until the context of the caller ends: a scan or a VACUUM reads it across other calls */
static void
log_pin(LogEnt *e)
{
	LogPin	   *p;

	if (e->pin_cx == CurrentMemoryContext)
		return;
	p = MemoryContextAlloc(CurrentMemoryContext, sizeof(LogPin));
	p->cb = (MemoryContextCallback) {log_unpin, p, NULL};
	p->relid = e->relid;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &p->cb);
	e->pins++;
	e->pin_cx = CurrentMemoryContext;
}

#ifdef SIFT_TESTING
bool		sift_test_no_log_reap = false;
bool		sift_test_no_log_hide = false;
uint64		sift_test_col_log_hidden_count = 0;
#endif

/*
 * The cache lives as long as the backend and holds a context for each index whose log the backend read, so it must let go of the indexes that are gone, or a backend
 * that creates and drops indexes grows with every one. The relcache invalidation of a dropped (or rebuilt, or altered) index calls this: it only marks the entry,
 * since a caller holds the pointer of a log across calls that process invalidations (index_open of another relation), and frees nothing.
 */
static void
log_inval(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS seq;
	LogEnt	   *e;

	if (hashvalue == 0)
	{
		log_inval_all = true;
		return;
	}
	hash_seq_init(&seq, log_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (e->hash == hashvalue)
			log_mark(e);
}

/* At the start of a use of the cache, where no log pointer of another index is being relied on across an invalidation of it: frees the marked entries whose index is gone */
static void
reap_stale(Oid keep)
{
	dlist_iter	all;
	dlist_mutable_iter it;

#ifdef SIFT_TESTING
	if (sift_test_no_log_reap)
		return;
#endif
	if (log_inval_all)
	{
		dlist_foreach(all, &log_lru)
			log_mark(dlist_container(LogEnt, lru, all.cur));
		log_inval_all = false;
	}
	dlist_foreach_modify(it, &log_stale)
	{
		LogEnt	   *e = dlist_container(LogEnt, stale_node, it.cur);

		if (e->relid == keep || e->pins > 0)
			continue;
		if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(e->relid)))
			log_unmark(e);
		else
			log_drop(e);
	}
}

/*
 * The cache entry of index, held by the context of the caller before its directory is read: the read asks the cache budget for room, which must not evict the entry it fills.
 * Its log is synced by the caller that needs the entries
 */
static LogEnt *
log_ent(Relation index)
{
	Oid			relid = RelationGetRelid(index);
	bool		found;
	LogEnt	   *e;
	MemoryContext old;

	if (log_tab == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(LogEnt),.hcxt = sift_cache_root_context()};

		log_tab = hash_create("sift cache tombstone logs", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		CacheRegisterSyscacheCallback(RELOID, log_inval, (Datum) 0);
		sift_cache_register(&log_kind);
	}
	reap_stale(relid);
	e = hash_search(log_tab, &relid, HASH_ENTER, &found);
	if (found && e->relnumber != index->rd_locator.relNumber)
	{
		sift_tomb_forget(&e->log);
		MemoryContextDelete(e->cxt);
		log_unmark(e);
		dlist_delete(&e->lru);
		found = false;
	}
	if (!found)
	{
		e->relnumber = index->rd_locator.relNumber;
		e->hash = GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(relid));
		e->stale = false;
		e->tick = sift_cache_tick();
		dlist_push_tail(&log_lru, &e->lru);
		e->has_seg = false;
		e->cxt = sift_cache_create_context("sift cache tombstone log");
		e->pins = 0;
		e->pin_cx = NULL;
		e->dir = NULL;
		memset(&e->log, 0, sizeof(e->log));
		sift_tomb_init(&e->log, e->cxt, &e->dir);
		e->log.spill = true;
	}
	log_pin(e);
	e->tick = sift_cache_tick();
	dlist_move_tail(&log_lru, &e->lru);
	if (e->dir && e->dir->version == sift_dir_version(index))
		return e;
	if (e->dir)
		pfree(e->dir);
	old = MemoryContextSwitchTo(e->cxt);
	e->dir = sift_dir_read_segs(index);
	MemoryContextSwitchTo(old);
	return e;
}

#ifdef SIFT_TESTING
int			sift_test_col_log_max_read = 0;
#endif

/* The newest column format whose tombstone log this build appends to: an older build would append tombstones without the records a newer one keeps in the log */
static int
log_max_read(void)
{
#ifdef SIFT_TESTING
	if (sift_test_col_log_max_read > 0)
		return sift_test_col_log_max_read;
#endif
	return SIFT_COL_FORMAT_MAX_READ;
}

static void
check_format(Relation index, const SiftSegEntry *seg)
{
	if (seg->format > log_max_read())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" has a tombstone log of column format 1.%d, this build reads up to 1.%d", RelationGetRelationName(index), seg->format, log_max_read()),
				 errhint("Use a newer pg_sift, or REINDEX.")));
}

static SiftTombLog *
log_synced(Relation index, bool entries)
{
	LogEnt	   *e;
	const SiftSegEntry *seg;

	/* the tombstones this backend holds back reach the log before it is read (supersede.c) */
	sift_col_tombs_flush();
	e = log_ent(index);
	SiftMemTracker mt_col_tomb_log;

	sift_mem_init_sub(&mt_col_tomb_log, "col_tomb_log", e->cxt, false, (Size) sift_backend_cache_kb * 1024 + SIFT_MEM_MIN_QUERY_BYTES);
	seg = sift_tomb_find_seg(e->dir);

	if (seg == NULL)
		return NULL;
	check_format(index, seg);
	/*
	 * A table follows a replaced log and keeps the entries it read (its addresses are never given to another row). A heap address is: VACUUM removes the entries of the
	 * rows it frees and then the tombstones of those entries, and the heap hands the address to a new row, which an entry kept here would hide. So the cache is the log
	 * of the directory now, and a scan that needs the entries as of its start takes its own copy.
	 */
	if (e->log.gen != 0 && e->log.gen != seg->gen)
		sift_tomb_forget(&e->log);
	if (entries)
		sift_tomb_sync(index, &e->log);
	else
		sift_tomb_sync_header(index, &e->log);
	sift_mem_assert_within(&mt_col_tomb_log);
	sift_cache_commit();
	return &e->log;
}

SiftTombLog *
sift_col_log(Relation index)
{
	return log_synced(index, true);
}

SiftTombLog *
sift_col_log_header(Relation index)
{
	return log_synced(index, false);
}

/*
 * The log to append to: the cache entry of the index, whose directory copy the append reads again under the lock that keeps VACUUM from replacing the segment. A writer reads
 * no entry of the log, so it caches none: the segment is looked for (and made) once for the file of the index.
 */
SiftTombLog *
sift_col_log_appender(Relation index)
{
	Oid			relid = RelationGetRelid(index);
	LogEnt	   *e = log_tab != NULL ? hash_search(log_tab, &relid, HASH_FIND, NULL) : NULL;

	if (e != NULL && e->has_seg && !log_inval_all && !e->stale && e->relnumber == index->rd_locator.relNumber)
		return &e->log;
	e = log_ent(index);
	if (sift_tomb_find_seg(e->dir) == NULL)
	{
		sift_col_log_create(index);
		e = log_ent(index);
	}
	check_format(index, sift_tomb_find_seg(e->dir));
	e->has_seg = true;
	return &e->log;
}

static void
add_log_segment(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	dir->segs[dir->nsegs++] = *(SiftSegEntry *) arg;
}

void
sift_col_log_create(Relation index)
{
	LogEnt	   *e = log_ent(index);
	SiftSegEntry seg;

	if (sift_tomb_find_seg(e->dir) != NULL)
		return;
	seg = sift_tomb_new_segment(index, SIFT_SEG_TOMB, SIFT_COL_FORMAT_ROWKEY);
	sift_dir_update(index, &(SiftDirChange) {.fn = add_log_segment,.arg = &seg,.nadd = 1,.extra = InvalidBuffer});
}

typedef struct DropArg
{
	IndexBulkDeleteCallback cb;	/* NULL: only the transactions are looked at */
	void	   *state;
	TransactionId frozen;		/* a tombstone of a transaction that committed before this one holds the same for every snapshot: it is written as frozen */
	TransactionId aged;			/* a journal record of a transaction before this one is dropped (log_vacuum has raised the trim of the journal to it first) */
	DvSettle   *settle;			/* the deletion records: which of them the log is better without */
	uint64		changed;
} DropArg;

/*
 * A tombstone names an entry of the index: when VACUUM removes the entry, the heap may give its address to another row, which the tombstone must not touch. One whose
 * transaction ended without committing deletes nothing and never will: it would stay as long as its entry does. A committed one older than every snapshot becomes a
 * frozen one: the commit log is truncated up to the oldest xid the tables are frozen up to, and a tombstone is not counted in that (an entry may outlive its tuple's
 * line pointer, when a VACUUM skipped the vacuuming of the indexes), so its xid would one day name a transaction the log no longer knows. One that is already beyond the
 * log stays (nothing is known of it: the reads go to the heap for its page) until its entry goes.
 */
static bool
drop_keep(SiftTomb *t, void *arg)
{
	DropArg    *d = arg;
	ItemPointerData tid;

	/* the slots of a deletion record are decided whole, by drop_dv */
	if (t->kind == SIFT_TOMB_DVEC || t->kind == SIFT_TOMB_DVEC_BODY)
		return true;
	/*
	 * A journal record (col/v1/conn.h) names no heap address and is never frozen: a stamp older than the oldest transaction any snapshot can have still compares its horizon with
	 * the record's transaction. It goes when its transaction aborted, and when it is so old that the comparison would wrap (the journal then vouches from the horizon on).
	 */
	if (SIFT_TOMB_IS_JOURNAL(t->kind))
	{
		if (!TransactionIdPrecedes(t->xid, d->aged) && (!TransactionIdIsNormal(t->xid) || sift_xid_state(t->xid) != SIFT_XID_ABORTED))
			return true;
		d->changed++;
		return false;
	}
	sift_key_to_tid(t->key, &tid);
	if (d->cb != NULL && d->cb(&tid, d->state))
		goto drop;
	if (!TransactionIdIsNormal(t->xid))
		return true;
	switch (sift_xid_state(t->xid))
	{
		case SIFT_XID_ABORTED:
			goto drop;
		case SIFT_XID_COMMITTED:
			if (t->kind != SIFT_TOMB_LOCK && TransactionIdPrecedes(t->xid, d->frozen))
			{
				t->xid = FrozenTransactionId;
				d->changed++;
			}
			break;
		default:
			break;
	}
	return true;
drop:
	d->changed++;
	return false;
}

/* A deletion record leaves the log when its transaction aborted, its segment was replaced, or every row it names is cleared (a fold or an index pass of VACUUM did): nothing is lost with it */
static bool
drop_dv(SiftTomb *slots, uint32 n, void *arg)
{
	DropArg    *d = arg;
	bool		settled = sift_col_dv_settled(d->settle, slots, n);

	d->changed += settled;
	return !settled;
}

/* Records of the journal older than the age limit go with this VACUUM: the journal stops vouching for the horizons before the limit first, so that no reader takes the missing records for none */
/* STOR[STOR-COL-12]: V1 tomblog full rewrite on vacuum delta (full-log WAL on 1 change) */
static void
trim_journal(Relation index, SiftTombLog *log, TransactionId aged)
{
	SiftTombKeyWalk w;
	const SiftTomb *rec;
	bool		old = false;

	if (!log->keys_cover || log->keys.lineage == 0)
		return;
	sift_tomb_key_begin(&w, log, SIFT_TOMB_KEYS_KEY, CurrentMemoryContext);
	while (!old && (rec = sift_tomb_key_next(&w)) != NULL)
		old = SIFT_TOMB_IS_JOURNAL(rec->kind) && TransactionIdPrecedes(rec->xid, aged);
	sift_tomb_key_end(&w);
	if (old)
	{
		SiftTombKeys keys = log->keys;

		if (!TransactionIdIsNormal(keys.trim) || TransactionIdPrecedes(keys.trim, aged))
			keys.trim = aged;
		sift_tomb_set_keys(index, log, &keys);
	}
}

/* Does the log hold an entry drop_keep changes (in memory or in a run): it works on a copy of each, as the rewrite does on the entries it keeps */
static void
log_changes(SiftTombLog *log, DropArg *d)
{
	SiftTombEach each;
	const SiftTomb *a;
	uint32		n;

	sift_tomb_each_begin(&each, log, CurrentMemoryContext);
	while (d->changed == 0 && (a = sift_tomb_each_next(&each, &n)) != NULL)
		for (uint32 i = 0; i < n && d->changed == 0; i++)
		{
			SiftTomb	t = a[i];

			(void) drop_keep(&t, d);
		}
	sift_tomb_each_end(&each);
	if (d->changed == 0 && log->dvecs > 0)
	{
		SiftTombWalk w;
		SiftTomb	buf[SIFT_TOMB_CAP];
		SiftTomb   *e;
		uint32		nslots;

		sift_tomb_walk_begin(&w, log);
		while (d->changed == 0 && (e = sift_tomb_walk_next_rec(&w, buf, &nslots)) != NULL)
			if (nslots > 1)
				(void) drop_dv(e, nslots, d);
		sift_tomb_walk_end(&w);
	}
}

static void
log_vacuum(Relation index, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	SiftTombLog *log = sift_col_log(index);
	DropArg		d = {.cb = cb,.state = cb_state,.frozen = GetOldestNonRemovableTransactionId(NULL),.aged = ReadNextTransactionId() - (1U << 30)};
	SiftTombChan chan;
	uint64		cleared;

	if (log == NULL)
		return;
	trim_journal(index, log, d.aged);
	/* the records of transactions older than every snapshot are cleared from the units before anything is dropped, as the marks are: a reader that took the log and then the liveness sees the record or the clear bit */
	cleared = sift_col_dv_fold(index, log, d.frozen);
	if (stats != NULL)
		stats->tuples_removed += (double) cleared;
	d.settle = log->dvecs > 0 ? sift_col_dv_settle_begin(index) : NULL;
	/* a log with nothing to drop or freeze is not rewritten (the rewrite copies it whole): the first change found says so */
	log_changes(log, &d);
	if (d.changed != 0)
		(void) sift_tomb_rewrite_dv(index, log, drop_keep, drop_dv, &d);
	if (d.settle != NULL)
		sift_col_dv_settle_end(d.settle);
	chan = log->chan;
	if ((chan.flags & SIFT_TOMB_COMPLETE) && TransactionIdIsNormal(chan.base_xid) && TransactionIdPrecedes(chan.base_xid, d.frozen) &&
		sift_xid_state(chan.base_xid) == SIFT_XID_COMMITTED)
	{
		chan.base_xid = FrozenTransactionId;
		sift_tomb_set_chan(index, log, &chan);
	}
}

void
maint_bulkdelete_tomb(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	log_vacuum(index, cb, cb_state, stats);
}

/* The cleanup of every VACUUM, also one that skipped the vacuuming of the indexes */
void
maint_vacuum_log(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	/*
	 * the addresses this VACUUM left dead (it skipped the indexes) get their tombstones here, and the entries it freed lose theirs below; a VACUUM with parallel workers for the b-trees
	 * runs this in the leader under parallel mode, where no transaction id can be assigned: the baseline waits for a VACUUM without them, and the reads check the heap until then
	 */
	if (!IsInParallelMode())
		(void) sift_col_track_rebaseline(index, heap);
	table_close(heap, NoLock);
	log_vacuum(index, NULL, NULL, NULL);
}

/* Test only: the entries the cache holds and the bytes of their contexts */
void
sift_col_log_cache_stats(int64 *entries, int64 *bytes)
{
	HASH_SEQ_STATUS seq;
	LogEnt	   *e;

	*entries = 0;
	*bytes = 0;
	if (log_tab == NULL)
		return;
	hash_seq_init(&seq, log_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
	{
		(*entries)++;
		*bytes += MemoryContextMemAllocated(e->cxt, true);
	}
}
