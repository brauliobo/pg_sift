/*
 * col/maint/fold.c - the marks of the tombstone log folded into the liveness runs: the entries whose deleting transaction every snapshot sees as committed are cleared
 * (segments, write buffer records) and their tombstones dropped, which is what the index pass of a VACUUM does for the rows the heap removed, without waiting for it.
 */
#include "postgres.h"
#include "access/transam.h"
#include "access/table.h"
#include "access/xlog.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "col/maint/maint_int.h"
#include "core/live.h"
#include "core/membudget.h"
#include "core/ops.h"
#include "col/wbuf.h"
#include "core/tomb.h"

/*
 * What a fold costs, in instructions and WAL in instructions (MAINT_WAL_BYTE: a MB of WAL weighs 0.6 G instructions, as in the price of a row of the rewrite): the segments are opened and the directory of
 * each unit decoded, 16 instructions for each row of the index (8.7 M for 548,000 rows in 57 units, one mark), a mark costs 55,000 more (the pages of its block bisected in every unit,
 * 19.4 M for 200 marks), and the WAL is the tombstone log written again, the liveness pages and the directory: 1 kB for one mark and 5 kB for 200 within a checkpoint, 90 kB for the
 * first of them after one (the pages go whole), so 64 kB are priced.
 */
#define FOLD_ROW_PRICE		16.0
#define FOLD_MARK_PRICE		55000.0
#define FOLD_WAL_PRICE		(64.0 * 1024 * MAINT_WAL_BYTE)

/*
 * The keys of the marks come out of the walk of the log in ascending order. They are held a chunk at a time, up to what sift.tomb_log_limit allows (as many keys as the log may keep
 * bytes), and the rest wait in a temp file in the same order: the memory of a fold does not follow the marks.
 */
#define FOLD_KEYS_START		1024

typedef struct Fold
{
	MemoryContext cxt;			/* the keys and the file: the scope col_fold_keys */
	SiftKey    *keys;			/* the chunk in hand, ascending */
	uint32		n;
	uint32		space;
	uint32		cap;			/* the most keys a chunk holds */
	BufFile    *spill;			/* the keys past the first chunk, once the walk is over */
	uint64		left;			/* the keys of the file not read back yet */
	bool		taken;			/* without a file: the chunk was handed out */
	TransactionId newest;		/* the latest deleting transaction among those that are known */
	Relation	heap;
	BlockNumber nblocks;
	Buffer		buf;			/* the heap page of the last frozen mark looked at */
} Fold;

/*
 * Has the heap taken the version at key off its page (a line pointer VACUUM or pruning left dead, or a page truncated away)? What the prune record of the page logged for a standby
 * (the recovery conflict with the snapshots that can still see the version) precedes the fold in the WAL.
 */
static bool
heap_pruned(Fold *f, SiftKey key)
{
	BlockNumber blk = (BlockNumber) (key >> 16);
	OffsetNumber off = (OffsetNumber) (key & 0xFFFF);
	Page		page;
	bool		dead;

	if (blk >= f->nblocks)
		return true;
	if (!BufferIsValid(f->buf) || BufferGetBlockNumber(f->buf) != blk)
	{
		if (BufferIsValid(f->buf))
			ReleaseBuffer(f->buf);
		f->buf = ReadBuffer(f->heap, blk);
	}
	LockBuffer(f->buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(f->buf);
	dead = off <= PageGetMaxOffsetNumber(page) && ItemIdIsDead(PageGetItemId(page, off));
	LockBuffer(f->buf, BUFFER_LOCK_UNLOCK);
	return dead;
}

/*
 * The marks the entry of a row is as dead as VACUUM would find it for: a delete or an update of the version by a transaction that committed before every snapshot, whose
 * transaction a standby is told of (the conflict record before the fold), or a frozen one (its transaction is not known: VACUUM froze it as older than every snapshot) whose heap
 * version is gone, which the prune record told the standby of.
 */
static bool
foldable(Fold *f, const SiftTomb *t, TransactionId horizon)
{
	if (t->kind != SIFT_TOMB_DELETE && t->kind != SIFT_TOMB_UPDATE)
		return false;
	if (t->xid == FrozenTransactionId)
		return heap_pruned(f, t->key);
	return TransactionIdIsNormal(t->xid) && TransactionIdPrecedes(t->xid, horizon) && sift_xid_state(t->xid) == SIFT_XID_COMMITTED;
}

static void
fold_spill(Fold *f)
{
	if (f->spill == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(f->cxt);

		f->spill = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	BufFileWrite(f->spill, f->keys, (size_t) f->n * sizeof(SiftKey));
	f->left += f->n;
	f->n = 0;
}

static void
fold_push(Fold *f, SiftKey key)
{
	if (f->n == f->space && f->space < f->cap)
	{
		f->space = (uint32) Min((uint64) f->cap, (uint64) f->space * 2);
		f->keys = repalloc_huge(f->keys, (Size) f->space * sizeof(SiftKey));
		SIFT_MEM_CHECKPOINT();
	}
	if (f->n == f->space)
		fold_spill(f);
	f->keys[f->n++] = key;
}

static void
fold_add(Fold *f, SiftTombLog *log, TransactionId horizon)
{
	SiftTombWalk w;
	const SiftTomb *t;

	sift_tomb_walk_begin(&w, log);
	while ((t = sift_tomb_walk_next(&w)) != NULL)
		if (foldable(f, t, horizon))
		{
			fold_push(f, t->key);
			if (TransactionIdIsNormal(t->xid) && (!TransactionIdIsValid(f->newest) || TransactionIdFollows(t->xid, f->newest)))
				f->newest = t->xid;
		}
	sift_tomb_walk_end(&w);
	if (f->spill != NULL)
	{
		fold_spill(f);
		BufFileSeek(f->spill, 0, 0, SEEK_SET);
	}
}

static int
key_cmp(const void *a, const void *b)
{
	SiftKey		x = *(const SiftKey *) a,
				y = *(const SiftKey *) b;

	return x < y ? -1 : x > y;
}

static bool
fold_dead(ItemPointer tid, void *arg)
{
	Fold	   *f = arg;
	SiftKey		key = sift_key_from_tid(tid);

	return bsearch(&key, f->keys, f->n, sizeof(SiftKey), key_cmp) != NULL;
}

/* The next chunk of the keys of the marks, without repeats (a row has one terminal mark, a repeat is a lock that precedes it or a copy): false past the last */
static bool
fold_next(Fold *f)
{
	uint32		k = 0;

	if (f->spill == NULL)
	{
		if (f->taken)
			return false;
		f->taken = true;
	}
	else
	{
		f->n = (uint32) Min((uint64) f->cap, f->left);
		f->left -= f->n;
		BufFileReadExact(f->spill, f->keys, (size_t) f->n * sizeof(SiftKey));
	}
	for (uint32 i = 0; i < f->n; i++)
		if (k == 0 || f->keys[k - 1] != f->keys[i])
			f->keys[k++] = f->keys[i];
	f->n = k;
	return k > 0;
}

/* The chunk in hand cleared from the segments and the write buffers, then its tombstones dropped: a reader that took the log and then the liveness of an entry sees the tombstone or the clear bit */
static uint64
fold_clear(Relation index, Fold *f, IndexBulkDeleteResult *stats)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int			order[SIFT_MAX_SEGMENTS];
	uint64		removed = 0;

	sift_dir_read(index, snap);
	for (int k = 0, n = sift_dir_clear_order(snap, order); k < n; k++)
	{
		if (snap->segs[order[k]].kind == SIFT_SEG_IMMUTABLE)
			removed += sift_cseg_clear_keys(sift_cseg_open(index, &snap->segs[order[k]], CurrentMemoryContext), f->keys, f->n);
		else
			maint_bulkdelete_wbuf(index, &snap->segs[order[k]], fold_dead, f, stats);
		SIFT_CLEAR_TEST_POINT();
	}
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_TOMB)
			maint_bulkdelete_tomb(index, &snap->segs[i], fold_dead, f, stats);
	pfree(snap);
	return removed;
}

/*
 * The marks of the tombstone log folded into the liveness runs, in the order of a VACUUM's index pass (the tombstones go last, so a reader that took the log and then the
 * liveness of an entry now cleared sees the tombstone or the clear bit). Returns the rows cleared.
 */
uint64
sift_col_fold_marks(Relation index)
{
	SiftTombLog *log;
	TransactionId horizon;
	IndexBulkDeleteResult stats = {0};
	Fold		f = {0};
	uint64		removed = 0;

	if (RecoveryInProgress())
		return 0;
	/*
	 * The marks are read under the lock of the index pass of a VACUUM: one that ran before has dropped the tombstones of the entries it cleared, so that no mark names an address the
	 * heap has given to another row since. A merge or a VACUUM that holds it is not waited for: the next read that pays asks again.
	 */
	if (!sift_maint_lock(index, false))
		return 0;
	log = sift_col_log(index);
	if (log == NULL)
	{
		sift_maint_unlock(index);
		return 0;
	}
	sift_tomb_sync_all(index, log);
	horizon = GetOldestNonRemovableTransactionId(NULL);
	f.cxt = AllocSetContextCreate(CurrentMemoryContext, "sift fold keys", ALLOCSET_DEFAULT_SIZES);
	SIFT_MEM_SCOPE(col_fold_keys, f.cxt);
	f.cap = (uint32) Min((uint64) sift_tomb_log_limit_kb * 1024 / sizeof(SiftKey), PG_INT32_MAX);
	f.space = Min(FOLD_KEYS_START, f.cap);
	f.keys = MemoryContextAllocHuge(f.cxt, (Size) f.space * sizeof(SiftKey));
	f.heap = table_open(index->rd_index->indrelid, AccessShareLock);
	f.nblocks = RelationGetNumberOfBlocks(f.heap);
	fold_add(&f, log, horizon);
	if (BufferIsValid(f.buf))
		ReleaseBuffer(f.buf);
	table_close(f.heap, AccessShareLock);
	/* a standby without feedback may hold a snapshot that does not see these deletes: it is cancelled before the bits clear, as the prune record of a VACUUM does for the heap */
	if (TransactionIdIsValid(f.newest))
		(void) sift_wal_snapshot_conflict(index, InvalidBlockNumber, FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), f.newest));
	while (fold_next(&f))
	{
		SIFT_MEM_CHECKPOINT();
		removed += fold_clear(index, &f, &stats);
	}
	removed += (uint64) stats.tuples_removed;
	/* a scan that began before reads the directory again: the heap marks pages all-visible after a pass the same way (sift_mops_bulkdelete) */
	if (removed > 0)
		sift_dir_update(index, &(SiftDirChange) {.extra = InvalidBuffer});
	if (f.spill != NULL)
		BufFileClose(f.spill);
	MemoryContextDelete(f.cxt);
	sift_maint_unlock(index);
	return removed;
}

/* The marks a fold could clear now: those of a delete or update that is frozen or older than the horizon (the rest wait for the snapshots that hold it, and a fold of them clears nothing) */
/* The marks a fold could clear now: those of a delete or update that is frozen or older than the horizon (the rest wait for the snapshots that hold it, and a fold of them clears nothing) */
static uint64
fold_ready(SiftTombLog *log, TransactionId horizon)
{
	SiftTombEach each;
	const SiftTomb *a;
	uint32		n;
	uint64		ready = 0;

	sift_tomb_each_begin(&each, log, CurrentMemoryContext);
	while ((a = sift_tomb_each_next(&each, &n)) != NULL)
		for (uint32 i = 0; i < n; i++)
			ready += (a[i].kind == SIFT_TOMB_DELETE || a[i].kind == SIFT_TOMB_UPDATE) &&
				(a[i].xid == FrozenTransactionId || (TransactionIdIsNormal(a[i].xid) && TransactionIdPrecedes(a[i].xid, horizon)));
	sift_tomb_each_end(&each);
	return ready;
}

/* Have the reads of the server paid for the fold of the marks of the log? What they paid is counted for the log in hand: a fold, or a VACUUM, makes a new one */
bool
sift_col_fold_due(Relation index)
{
	SiftTombLog *log = sift_col_log(index);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	uint64		rows = 0,
				paid,
				ready;
	double		cost;

	if (log == NULL)
		return false;
	ready = fold_ready(log, GetOldestNonRemovableTransactionId(NULL));
	if (ready == 0)
		return false;
	sift_col_dir_read(index, snap, false);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			rows += snap->segs[i].ndocs;
	paid = sift_col_marks_paid(index, &snap->meta, log->gen);
	pfree(snap);
	cost = FOLD_ROW_PRICE * (double) rows + FOLD_MARK_PRICE * (double) ready + FOLD_WAL_PRICE;
	return (double) paid >= MAINT_HALF * cost;
}

bool
sift_col_fold_tick(Relation index)
{
	if (!sift_col_auto_merge || !sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return false;
	return sift_col_fold_due(index) && sift_maint_request_worker(index, "sift_col_maint_worker_main");
}
