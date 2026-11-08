/*
 * col/tomblog.c - the tombstone log of a heap column index with track_changes (docs/COL-TAM.md section 2.4 holds the log; core/tomb.h is its code):
 * the entry of a heap address that stopped being live, with the transaction and command that said so. The cache of the log a backend reads
 * is kept apart from the cached state of the index (SiftColRel): a relcache invalidation frees that, and the log is read incrementally.
 */
#include "postgres.h"

#include "access/table.h"
#include "access/transam.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/maint/maint_int.h"
#include "core/tomb.h"

typedef struct LogEnt
{
	Oid			relid;			/* hash key */
	bool		stale;			/* a relcache invalidation named the index: has it been dropped? (reap_stale) */
	RelFileNumber relnumber;
	bool		has_seg;		/* the index has its tombstone segment (an index does not lose it: VACUUM replaces it, a rebuild is another file) */
	MemoryContext cxt;
	SiftDirSnapshot dir;
	SiftTombLog log;
} LogEnt;

static HTAB *log_tab;

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
log_inval(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS seq;
	LogEnt	   *e;

	if (relid != InvalidOid)
	{
		if ((e = hash_search(log_tab, &relid, HASH_FIND, NULL)) != NULL)
			e->stale = true;
		return;
	}
	hash_seq_init(&seq, log_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
		e->stale = true;
}

/* At the start of a use of the cache, where no log pointer of another index is being relied on across an invalidation of it: frees the marked entries whose index is gone */
static void
reap_stale(Oid keep)
{
	HASH_SEQ_STATUS seq;
	LogEnt	   *e;

#ifdef SIFT_TESTING
	if (sift_test_no_log_reap)
		return;
#endif
	hash_seq_init(&seq, log_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
	{
		if (!e->stale || e->relid == keep)
			continue;
		if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(e->relid)))
		{
			e->stale = false;
			continue;
		}
		MemoryContextDelete(e->cxt);
		(void) hash_search(log_tab, &e->relid, HASH_REMOVE, NULL);
	}
}

/* The cache entry of index, with its directory read now; its log is synced by the caller that needs the entries */
static LogEnt *
log_ent(Relation index)
{
	Oid			relid = RelationGetRelid(index);
	bool		found;
	LogEnt	   *e;

	if (log_tab == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(LogEnt),.hcxt = CacheMemoryContext};

		log_tab = hash_create("sift cache tombstone logs", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		CacheRegisterRelcacheCallback(log_inval, (Datum) 0);
	}
	reap_stale(relid);
	e = hash_search(log_tab, &relid, HASH_ENTER, &found);
	if (found && e->relnumber != index->rd_locator.relNumber)
	{
		MemoryContextDelete(e->cxt);
		found = false;
	}
	if (!found)
	{
		e->relnumber = index->rd_locator.relNumber;
		e->stale = false;
		e->has_seg = false;
		e->cxt = AllocSetContextCreate(CacheMemoryContext, "sift cache tombstone log", ALLOCSET_DEFAULT_SIZES);
		memset(&e->dir, 0, sizeof(e->dir));
		memset(&e->log, 0, sizeof(e->log));
		sift_tomb_init(&e->log, e->cxt, &e->dir);
	}
	sift_dir_read(index, &e->dir);
	return e;
}

SiftTombLog *
sift_col_log(Relation index)
{
	LogEnt	   *e;
	const SiftSegEntry *seg;

	/* the tombstones this backend holds back reach the log before it is read (supersede.c) */
	sift_col_tombs_flush();
	e = log_ent(index);
	seg = sift_tomb_find_seg(&e->dir);

	if (seg == NULL)
		return NULL;
	/*
	 * A table follows a replaced log and keeps the entries it read (its addresses are never given to another row). A heap address is: VACUUM removes the entries of the
	 * rows it frees and then the tombstones of those entries, and the heap hands the address to a new row, which an entry kept here would hide. So the cache is the log
	 * of the directory now, and a scan that needs the entries as of its start takes its own copy.
	 */
	if (e->log.gen != 0 && e->log.gen != seg->gen)
		sift_tomb_forget(&e->log);
	sift_tomb_sync(index, &e->log);
	return &e->log;
}

/*
 * The log to append to: the cache entry of the index with no read of the directory, which the append makes again under the lock that keeps VACUUM from replacing the segment.
 * The segment is looked for (and made) once for the file of the index.
 */
SiftTombLog *
sift_col_log_appender(Relation index)
{
	Oid			relid = RelationGetRelid(index);
	LogEnt	   *e = log_tab != NULL ? hash_search(log_tab, &relid, HASH_FIND, NULL) : NULL;

	if (e != NULL && e->has_seg && !e->stale && e->relnumber == index->rd_locator.relNumber)
		return &e->log;
	if (sift_col_log(index) == NULL)
	{
		sift_col_log_create(index);
		(void) sift_col_log(index);
	}
	e = hash_search(log_tab, &relid, HASH_FIND, NULL);
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

	if (sift_tomb_find_seg(&e->dir) != NULL)
		return;
	seg = sift_tomb_new_segment(index, SIFT_SEG_TOMB, SIFT_COL_FORMAT_ROWKEY);
	sift_dir_update(index, &(SiftDirChange) {.fn = add_log_segment,.arg = &seg,.extra = InvalidBuffer});
}

typedef struct DropArg
{
	IndexBulkDeleteCallback cb;	/* NULL: only the transactions are looked at */
	void	   *state;
	TransactionId frozen;		/* a tombstone of a transaction that committed before this one holds the same for every snapshot: it is written as frozen */
	TransactionId aged;			/* a journal record of a transaction before this one is dropped (log_vacuum has raised the trim of the journal to it first) */
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

	/*
	 * A journal record (col/conn.h) names no heap address and is never frozen: a stamp older than the oldest transaction any snapshot can have still compares its horizon with
	 * the record's transaction. It goes when its transaction aborted, and when it is so old that the comparison would wrap (the journal then vouches from the horizon on).
	 */
	if (t->kind == SIFT_TOMB_KEYCHG)
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

/* Records of the journal older than the age limit go with this VACUUM: the journal stops vouching for the horizons before the limit first, so that no reader takes the missing records for none */
static void
trim_journal(Relation index, SiftTombLog *log, TransactionId aged)
{
	int			n;
	const SiftTomb *rec = log->keys_cover && log->keys.lineage != 0 ? sift_tomb_find(log, SIFT_TOMB_KEYS_KEY, &n) : NULL;

	for (int i = 0; rec != NULL && i < n; i++)
		if (rec[i].kind == SIFT_TOMB_KEYCHG && TransactionIdPrecedes(rec[i].xid, aged))
		{
			SiftTombKeys keys = log->keys;

			if (!TransactionIdIsNormal(keys.trim) || TransactionIdPrecedes(keys.trim, aged))
				keys.trim = aged;
			sift_tomb_set_keys(index, log, &keys);
			return;
		}
}

static void
log_vacuum(Relation index, IndexBulkDeleteCallback cb, void *cb_state)
{
	SiftTombLog *log = sift_col_log(index);
	DropArg		d = {.cb = cb,.state = cb_state,.frozen = GetOldestNonRemovableTransactionId(NULL),.aged = ReadNextTransactionId() - (1U << 30)};
	SiftTombChan chan;

	if (log == NULL)
		return;
	trim_journal(index, log, d.aged);
	/* a log with nothing to drop or freeze is not rewritten (the rewrite copies it whole): the first change found says so. A changing scan rewrites the copy it made, so it works on one */
	for (uint32 i = 0; i < log->nents && d.changed == 0; i++)
	{
		SiftTomb	t = log->ents[i];

		(void) drop_keep(&t, &d);
	}
	for (uint32 i = 0; i < log->nrecent && d.changed == 0; i++)
	{
		SiftTomb	t = log->recent[i];

		(void) drop_keep(&t, &d);
	}
	if (d.changed != 0)
		(void) sift_tomb_rewrite(index, log, drop_keep, &d);
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
	log_vacuum(index, cb, cb_state);
}

/* The cleanup of every VACUUM, also one that skipped the vacuuming of the indexes */
void
maint_vacuum_log(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, NoLock);	/* VACUUM holds ShareUpdateExclusiveLock on it */

	/* the addresses this VACUUM left dead (it skipped the indexes) get their tombstones here, and the entries it freed lose theirs below */
	(void) sift_col_track_rebaseline(index, heap);
	table_close(heap, NoLock);
	log_vacuum(index, NULL, NULL);
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
