/*
 * col/tomblog.c - the tombstone log of a heap column index with a row key (docs/COL-TAM.md section 2.4 holds the log; core/tomb.h is its code):
 * the entry of a heap address that stopped being live, with the transaction and command that said so. The cache of the log a backend reads
 * is kept apart from the cached state of the index (SiftColRel): a relcache invalidation frees that, and the log is read incrementally.
 */
#include "postgres.h"

#include "access/xact.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/maint/maint_int.h"
#include "core/tomb.h"

typedef struct LogEnt
{
	Oid			relid;			/* hash key */
	RelFileNumber relnumber;
	MemoryContext cxt;
	SiftDirSnapshot dir;
	SiftTombLog log;
} LogEnt;

static HTAB *log_tab;

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

		log_tab = hash_create("sift_col tombstone logs", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(log_tab, &relid, HASH_ENTER, &found);
	if (found && e->relnumber != index->rd_locator.relNumber)
	{
		MemoryContextDelete(e->cxt);
		found = false;
	}
	if (!found)
	{
		e->relnumber = index->rd_locator.relNumber;
		e->cxt = AllocSetContextCreate(CacheMemoryContext, "sift_col tombstone log", ALLOCSET_DEFAULT_SIZES);
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
	LogEnt	   *e = log_ent(index);
	const SiftSegEntry *seg = sift_tomb_find_seg(&e->dir);

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
	IndexBulkDeleteCallback cb;
	void	   *state;
	uint64		dropped;
} DropArg;

/* A tombstone names an entry of the index: when VACUUM removes the entry, the heap may give its address to another row, which the tombstone must not touch */
static bool
drop_keep(SiftTomb *t, void *arg)
{
	DropArg    *d = arg;
	ItemPointerData tid;

	sift_key_to_tid(t->key, &tid);
	if (d->cb(&tid, d->state))
	{
		d->dropped++;
		return false;
	}
	return true;
}

void
maint_bulkdelete_tomb(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	SiftTombLog *log = sift_col_log(index);
	DropArg		d = {cb, cb_state, 0};

	if (log == NULL)
		return;
	/* a log with no entry to drop is not rewritten (the rewrite copies it whole): the first drop found says so */
	for (uint32 i = 0; i < log->nents && d.dropped == 0; i++)
		(void) drop_keep(&log->ents[i], &d);
	for (uint32 i = 0; i < log->nrecent && d.dropped == 0; i++)
		(void) drop_keep(&log->recent[i], &d);
	if (d.dropped == 0)
		return;
	(void) sift_tomb_rewrite(index, log, drop_keep, &d);
}
