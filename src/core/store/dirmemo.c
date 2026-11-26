/*
 * core/store/dirmemo.c - the directories a backend read last (SiftDirMemo), for the statements that open an index again and again (the lookup of a key of a UNIQUE index, a search of a
 * small table): the directory of an index changes with its version (sift_dir_update), which a pin of block 1 shows without a lock, so a backend that finds the version it has takes its
 * copy, and neither of the pages 0 and 1 is locked by it. A directory that is chained is not kept, nor one that is out of service (sift_index_known_out): its flag is in the metapage, which the copy may
 * predate. No relcache callback: the entry is told by the file of the index (its number changes when the index is rewritten) and by its version.
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/cachebudget.h"
#include "core/quarantine/quarantine.h"
#include "core/store/store.h"

#define DIR_MEMO_SIZE	8

typedef struct SiftDirMemo
{
	Oid			index;
	RelFileNumber file;
	uint64		used;			/* sift_cache_tick() of its last use */
	SiftDirSnapshot *snap;		/* NULL: the slot is free */
} SiftDirMemo;

static SiftDirMemo memo[DIR_MEMO_SIZE];
static MemoryContext memo_cx;

static void
memo_drop(SiftDirMemo *m)
{
	pfree(m->snap);
	m->snap = NULL;
}

static SiftDirMemo *
memo_oldest(void)
{
	SiftDirMemo *oldest = NULL;

	for (int i = 0; i < DIR_MEMO_SIZE; i++)
		if (memo[i].snap != NULL && (oldest == NULL || memo[i].used < oldest->used))
			oldest = &memo[i];
	return oldest;
}

static uint64
memo_tick(void)
{
	SiftDirMemo *m = memo_oldest();

	return m ? m->used : UINT64_MAX;
}

static bool
memo_evict_one(void)
{
	SiftDirMemo *m = memo_oldest();

	if (m == NULL)
		return false;
	memo_drop(m);
	return true;
}

static void
memo_evict_all(void)
{
	while (memo_evict_one())
		;
}

static const SiftCacheKind memo_kind = {"core_dir_memo", memo_tick, memo_evict_one, memo_evict_all};

static SiftDirMemo *
memo_find(Relation index)
{
	for (int i = 0; i < DIR_MEMO_SIZE; i++)
		if (memo[i].snap != NULL && memo[i].index == RelationGetRelid(index) && memo[i].file == index->rd_locator.relNumber)
			return &memo[i];
	return NULL;
}

/* Is the directory of the index at the version of the copy? Block 1 is pinned, not locked */
static bool
memo_current(Relation index, const SiftDirMemo *m)
{
	Buffer		dirbuf = sift_dir_pin(index);
	bool		same = sift_dir_version_peek(dirbuf) == m->snap->version;

	ReleaseBuffer(dirbuf);
	return same;
}

/* The slot of the index (its older copy goes), else a free one, else the one used longest ago */
static SiftDirMemo *
memo_slot(Relation index)
{
	SiftDirMemo *m = memo_find(index);

	for (int i = 0; m == NULL && i < DIR_MEMO_SIZE; i++)
		if (memo[i].snap == NULL)
			m = &memo[i];
	if (m == NULL)
		m = memo_oldest();
	if (m->snap != NULL)
		memo_drop(m);
	return m;
}

static void
memo_put(Relation index, const SiftDirSnapshot *snap)
{
	SiftDirMemo *m;

	if (memo_cx == NULL)
	{
		memo_cx = sift_cache_create_context("sift cache directories");
		sift_cache_register(&memo_kind);
	}
	if (!sift_cache_room(sift_dir_bytes(snap)))
		return;
	m = memo_slot(index);
	m->index = RelationGetRelid(index);
	m->file = index->rd_locator.relNumber;
	m->used = sift_cache_tick();
	m->snap = MemoryContextAlloc(memo_cx, sift_dir_bytes(snap));
	memcpy(m->snap, snap, sift_dir_bytes(snap));
	m->snap->chain = NULL;
	m->snap->nchain = 0;
	sift_cache_commit();
}

SiftDirSnapshot *
sift_dir_read_memo(Relation index)
{
	SiftDirMemo *m = memo_find(index);
	SiftDirSnapshot *snap;

	if (m != NULL && !sift_index_known_out(index) && memo_current(index, m))
	{
		m->used = sift_cache_tick();
		return sift_dir_copy(m->snap);
	}
	snap = sift_dir_read(index);
	if (snap->nchain == 0 && !snap->partial)
		memo_put(index, snap);
	return snap;
}
