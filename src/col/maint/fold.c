/*
 * col/maint/fold.c - the marks of the tombstone log folded into the liveness runs: the entries whose deleting transaction every snapshot sees as committed are cleared
 * (segments, write buffer records) and their tombstones dropped, which is what the index pass of a VACUUM does for the rows the heap removed, without waiting for it.
 */
#include "postgres.h"
#include "access/transam.h"
#include "access/table.h"
#include "access/xlog.h"
#include "storage/bufmgr.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "col/maint/maint_int.h"
#include "core/ops.h"
#include "col/wbuf.h"
#include "core/tomb.h"

/*
 * What a fold costs, in instructions and WAL in instructions (a MB of WAL weighs 0.6 G instructions, as in the price of a row of the rewrite): the segments are opened and the directory of
 * each unit decoded, 16 instructions for each row of the index (8.7 M for 548,000 rows in 57 units, one mark), a mark costs 55,000 more (the pages of its block bisected in every unit,
 * 19.4 M for 200 marks), and the WAL is the tombstone log written again, the liveness pages and the directory: 1 kB for one mark and 5 kB for 200 within a checkpoint, 90 kB for the
 * first of them after one (the pages go whole), so 64 kB are priced.
 */
#define FOLD_ROW_PRICE		16.0
#define FOLD_MARK_PRICE		55000.0
#define FOLD_WAL_PRICE		(64.0 * 1024 * 600)

typedef struct Fold
{
	SiftKey    *keys;			/* ascending once sorted */
	uint32		n;
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
fold_add(Fold *f, const SiftTomb *ents, uint32 n, TransactionId horizon)
{
	for (uint32 i = 0; i < n; i++)
		if (foldable(f, &ents[i], horizon))
		{
			f->keys[f->n++] = ents[i].key;
			if (TransactionIdIsNormal(ents[i].xid) && (!TransactionIdIsValid(f->newest) || TransactionIdFollows(ents[i].xid, f->newest)))
				f->newest = ents[i].xid;
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

/* The distinct keys of the marks, ascending: a row has one terminal mark, a repeat is a lock that precedes it or a copy */
static void
fold_sort(Fold *f)
{
	uint32		k = 0;

	qsort(f->keys, f->n, sizeof(SiftKey), key_cmp);
	for (uint32 i = 0; i < f->n; i++)
		if (k == 0 || f->keys[k - 1] != f->keys[i])
			f->keys[k++] = f->keys[i];
	f->n = k;
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
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
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
	f.keys = palloc((log->nents + log->nrecent + 1) * sizeof(SiftKey));
	f.heap = table_open(index->rd_index->indrelid, AccessShareLock);
	f.nblocks = RelationGetNumberOfBlocks(f.heap);
	fold_add(&f, log->ents, log->nents, horizon);
	fold_add(&f, log->recent, log->nrecent, horizon);
	if (BufferIsValid(f.buf))
		ReleaseBuffer(f.buf);
	table_close(f.heap, AccessShareLock);
	if (f.n > 0)
	{
		fold_sort(&f);
		sift_dir_read(index, snap);
		/* a standby without feedback may hold a snapshot that does not see these deletes: it is cancelled before the bits clear, as the prune record of a VACUUM does for the heap */
		if (TransactionIdIsValid(f.newest))
			(void) sift_wal_snapshot_conflict(index, InvalidBlockNumber, FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), f.newest));
		for (int i = 0; i < snap->nsegs; i++)
			if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
				removed += sift_cseg_clear_keys(sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext), f.keys, f.n);
			else if (snap->segs[i].kind != SIFT_SEG_TOMB)
				maint_bulkdelete_wbuf(index, &snap->segs[i], fold_dead, &f, &stats);
		for (int i = 0; i < snap->nsegs; i++)
			if (snap->segs[i].kind == SIFT_SEG_TOMB)
				maint_bulkdelete_tomb(index, &snap->segs[i], fold_dead, &f, &stats);
		removed += (uint64) stats.tuples_removed;
		/* a scan that began before reads the directory again: the heap marks pages all-visible after a pass the same way (sift_mops_bulkdelete) */
		if (removed > 0)
			sift_dir_update(index, &(SiftDirChange) {.extra = InvalidBuffer});
	}
	sift_maint_unlock(index);
	pfree(snap);
	pfree(f.keys);
	return removed;
}

/* Have the reads of the server paid for the fold of the marks of the log? What they paid is counted for the log in hand: a fold, or a VACUUM, makes a new one */
bool
sift_col_fold_due(Relation index)
{
	SiftTombLog *log = sift_col_log(index);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	uint64		rows = 0;
	double		cost;

	if (log == NULL)
		return false;
	sift_col_dir_read(index, snap, false);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			rows += snap->segs[i].ndocs;
	pfree(snap);
	cost = FOLD_ROW_PRICE * (double) rows + FOLD_MARK_PRICE * (double) (log->nents + log->nrecent) + FOLD_WAL_PRICE;
	return (double) sift_col_marks_paid(index, log->gen) >= MAINT_HALF * cost;
}

bool
sift_col_fold_tick(Relation index)
{
	if (!sift_col_auto_merge || !sift_async_maintenance || RecoveryInProgress() || index->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		return false;
	return sift_col_fold_due(index) && sift_maint_request_worker(index, "sift_col_maint_worker_main");
}
