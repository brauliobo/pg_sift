/*
 * col/maint/dvfold.c - the deletion records of the tombstone log (core/tomb_dv.h) in maintenance: folded into the liveness runs when their transaction is older than every snapshot (what the
 * fold of the marks does for the marks, col/maint/fold.c), dropped from the log once every row they name is cleared, and carried to marks when a step replaces the segment they name
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xlog.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"

#include "col/maint/maint_int.h"
#include "core/live.h"
#include "core/membudget.h"
#include "core/ops.h"
#include "core/tomb_dv.h"

struct DvSettle
{
	Relation	index;
	MemoryContext cxt;
	SiftDirSnapshot *dir;
	SiftColSegment *seg;			/* the segment of the generation in hand: the records come in the order of their generations */
	uint64		gen;
	SiftDvRec  *rec;
	uint32	   *rows;
	uint64	   *live;
	uint64	   *mask;
};

DvSettle *
sift_col_dv_settle_begin(Relation index)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift deletion records", ALLOCSET_DEFAULT_SIZES);
	DvSettle   *st = MemoryContextAllocZero(cxt, sizeof(DvSettle));

	st->index = index;
	st->cxt = cxt;
	st->dir = MemoryContextAlloc(cxt, sizeof(SiftDirSnapshot));
	sift_dir_read(index, st->dir);
	st->rec = MemoryContextAlloc(cxt, sizeof(SiftDvRec));
	st->rows = MemoryContextAlloc(cxt, SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	st->live = MemoryContextAlloc(cxt, SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	st->mask = MemoryContextAlloc(cxt, SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	return st;
}

void
sift_col_dv_settle_end(DvSettle *st)
{
	if (st->seg != NULL)
		sift_cseg_close(st->seg);
	MemoryContextDelete(st->cxt);
}

/* The segment of generation gen of the directory in hand, open; NULL when there is none (replaced since) */
static SiftColSegment *
settle_segment(DvSettle *st, uint64 gen)
{
	SIFT_MEM_SCOPE(col_dv_settle, st->cxt);
	if (st->seg != NULL && st->gen == gen)
		return st->seg;
	if (st->seg != NULL)
		sift_cseg_close(st->seg);
	st->seg = NULL;
	st->gen = gen;
	for (int i = 0; i < st->dir->nsegs; i++)
		if (st->dir->segs[i].kind == SIFT_SEG_IMMUTABLE && st->dir->segs[i].gen == gen)
			st->seg = sift_cseg_open(st->index, &st->dir->segs[i], st->cxt);
	SIFT_MEM_CHECKPOINT();
	return st->seg;
}

/* The rows of the record that are still live in its segment, read into st->mask (the record's rows) and st->live */
static bool
record_live(DvSettle *st, SiftColSegment *seg, const SiftDvRec *r)
{
	uint32		nw;

	if (r->desc.part >= seg->nsb || r->desc.row_lo + r->desc.span > seg->sbdir[r->desc.part].nrows)
		return false;
	nw = SIFT_COL_NWORDS(seg->sbdir[r->desc.part].nrows);
	memset(st->mask, 0, nw * sizeof(uint64));
	sift_dv_or_bits(r, st->mask);
	sift_cseg_live(seg, r->desc.part, st->live);
	for (uint32 w = 0; w < nw; w++)
		if (st->mask[w] & st->live[w])
			return true;
	return false;
}

bool
sift_col_dv_settled(DvSettle *st, const SiftTomb *slots, uint32 n)
{
	SiftColSegment *seg;

	sift_dv_decode(slots, n, st->rec);
	if (TransactionIdIsNormal(st->rec->xid) && sift_xid_state(st->rec->xid) == SIFT_XID_ABORTED)
		return true;
	if ((seg = settle_segment(st, st->rec->gen)) == NULL)
		return true;
	return !record_live(st, seg, st->rec);
}

uint32
sift_col_dv_record_keys(DvSettle *st, const SiftTomb *slots, uint32 n, SiftKey **keys)
{
	SiftColSegment *seg;

	sift_dv_decode(slots, n, st->rec);
	if ((seg = settle_segment(st, st->rec->gen)) == NULL || st->rec->desc.part >= seg->nsb || st->rec->desc.row_lo + st->rec->desc.span > seg->sbdir[st->rec->desc.part].nrows)
		return 0;
	sift_dv_rows(st->rec, st->rows);
	*keys = palloc(st->rec->desc.count * sizeof(SiftKey));
	sift_cseg_row_keys(seg, st->rec->desc.part, st->rows, st->rec->desc.count, *keys);
	return st->rec->desc.count;
}

/* Is the record's transaction older than every snapshot and committed: the rows it names are dead for all of them */
static bool
record_foldable(const SiftTomb *head, TransactionId horizon)
{
	return TransactionIdIsNormal(head->xid) && TransactionIdPrecedes(head->xid, horizon) && sift_xid_state(head->xid) == SIFT_XID_COMMITTED;
}

bool
sift_col_dv_foldable(const SiftTomb *head, TransactionId horizon)
{
	return head->kind == SIFT_TOMB_DVEC && record_foldable(head, horizon);
}

/* The newest transaction among the records a fold can clear, none (invalid) when there is no such record */
static TransactionId
newest_foldable(const SiftTombLog *log, TransactionId horizon)
{
	SiftTombEach each;
	const SiftTomb *a;
	uint32		n;
	TransactionId newest = InvalidTransactionId;

	sift_tomb_each_begin(&each, log, CurrentMemoryContext);
	while ((a = sift_tomb_each_next(&each, &n)) != NULL)
		for (uint32 i = 0; i < n; i++)
			if (sift_col_dv_foldable(&a[i], horizon) && (!TransactionIdIsValid(newest) || TransactionIdFollows(a[i].xid, newest)))
				newest = a[i].xid;
	sift_tomb_each_end(&each);
	return newest;
}

uint64
sift_col_dv_fold(Relation index, SiftTombLog *log, TransactionId horizon)
{
	DvSettle   *st;
	SiftTombWalk w;
	SiftTomb	buf[SIFT_TOMB_CAP];
	SiftTomb   *e;
	uint32		n;
	uint64		cleared = 0;
	TransactionId newest;

	if (log->dvecs == 0 || RecoveryInProgress() || !TransactionIdIsValid(newest = newest_foldable(log, horizon)))
		return 0;
	/* a standby without feedback may hold a snapshot that does not see these deletes: it is cancelled before the bits clear, as the prune record of a VACUUM does for the heap */
	(void) sift_wal_snapshot_conflict(index, InvalidBlockNumber, FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), newest));
	st = sift_col_dv_settle_begin(index);
	sift_tomb_walk_begin(&w, log);
	while ((e = sift_tomb_walk_next_rec(&w, buf, &n)) != NULL)
	{
		SiftColSegment *seg;

		if (n == 1 || !record_foldable(e, horizon))
			continue;
		sift_dv_decode(e, n, st->rec);
		if ((seg = settle_segment(st, st->rec->gen)) == NULL || st->rec->desc.part >= seg->nsb || st->rec->desc.row_lo + st->rec->desc.span > seg->sbdir[st->rec->desc.part].nrows)
			continue;
		sift_dv_rows(st->rec, st->rows);
		cleared += (uint64) sift_cseg_clear_rows(seg, st->rec->desc.part, st->rows, st->rec->desc.count);
		SIFT_MEM_CHECKPOINT();
	}
	sift_tomb_walk_end(&w);
	sift_col_dv_settle_end(st);
	/* a scan that began before reads the directory again: the heap marks pages all-visible after a pass the same way (sift_mops_bulkdelete) */
	if (cleared > 0)
		sift_dir_update(index, &(SiftDirChange) {.extra = InvalidBuffer});
	return cleared;
}

/*
 * The records of the segments old[0..nold) carried to marks, for a step that is about to replace them: a mark names a row by its heap address, which the new segments keep, where a record
 * names the unit of a segment that will be gone. Holds the lock that the writers of records take shared, from here to sift_col_dv_switched, so that no record is written for a segment after
 * this has looked, and the switch of the directory is the step's own. The records stay in the log, as nothing names their segments any more, and the next rewrite of the log drops them.
 */
void
sift_col_dv_switching(Relation index, const uint64 *old, int nold)
{
	SiftTombLog *log;
	DvSettle   *st;
	SiftTombWalk w;
	SiftTomb	buf[SIFT_TOMB_CAP];
	SiftTomb   *e;
	uint32		n;

	LockPage(index, SIFT_TOMB_LOCK_BLKNO, ExclusiveLock);
	if ((log = sift_col_log(index)) == NULL || log->dvecs == 0)
		return;
	st = sift_col_dv_settle_begin(index);
	sift_tomb_walk_begin(&w, log);
	while ((e = sift_tomb_walk_next_rec(&w, buf, &n)) != NULL)
	{
		SiftColSegment *seg;
		SiftTomb   *marks;
		SiftKey    *at;
		uint32		nmarks = 0;
		bool		named = false;

		if (n == 1)
			continue;
		sift_dv_decode(e, n, st->rec);
		for (int i = 0; i < nold; i++)
			named |= old[i] == st->rec->gen;
		if (!named || (TransactionIdIsNormal(st->rec->xid) && sift_xid_state(st->rec->xid) == SIFT_XID_ABORTED))
			continue;
		if ((seg = settle_segment(st, st->rec->gen)) == NULL || !record_live(st, seg, st->rec))
			continue;
		sift_dv_rows(st->rec, st->rows);
		at = palloc(st->rec->desc.count * sizeof(SiftKey));
		marks = palloc(st->rec->desc.count * sizeof(SiftTomb));
		for (uint32 i = 0; i < st->rec->desc.count; i++)
			if (st->live[st->rows[i] / 64] >> (st->rows[i] % 64) & 1)
				st->rows[nmarks++] = st->rows[i];
		sift_cseg_row_keys(seg, st->rec->desc.part, st->rows, nmarks, at);
		for (uint32 i = 0; i < nmarks; i++)
			marks[i] = (SiftTomb) {.key = at[i],.xid = st->rec->xid,.cid = st->rec->cid,.kind = SIFT_TOMB_DELETE};
		sift_tomb_append_many(index, log, marks, (int) nmarks);
		pfree(at);
		pfree(marks);
		SIFT_MEM_CHECKPOINT();
	}
	sift_tomb_walk_end(&w);
	sift_col_dv_settle_end(st);
}

void
sift_col_dv_switched(Relation index)
{
	UnlockPage(index, SIFT_TOMB_LOCK_BLKNO, ExclusiveLock);
}
