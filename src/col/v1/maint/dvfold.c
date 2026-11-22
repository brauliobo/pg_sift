/*
 * col/v1/maint/dvfold.c - the deletion records of the tombstone log (core/v1_col/tomb_dv.h) in maintenance: folded into the liveness runs when their transaction is older than every snapshot (what the
 * fold of the marks does for the marks, col/v1/maint/fold.c), dropped from the log once every row they name is cleared, and carried to marks when a step replaces the segment they name
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xlog.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"

#include "col/v1/maint/maint_int.h"
#include "core/wal_conflict.h"
#include "core/live.h"
#include "core/memtrack.h"
#include "core/ops.h"
#include "core/v1_col/tomb_dv.h"

struct DvSettle
{
	Relation	index;
	MemoryContext cxt;
	MemoryContext segcx;			/* the segment in hand, reset when it is closed */
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
	MemoryContext old;

	st->index = index;
	st->cxt = cxt;
	st->segcx = sift_mem_allocset_create(cxt, "sift deletion records segment");
	old = MemoryContextSwitchTo(cxt);
	st->dir = sift_dir_read(index);
	MemoryContextSwitchTo(old);
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
	SiftMemTracker mt_col_dv_settle;

	sift_mem_init_sub(&mt_col_dv_settle, "col_dv_settle", st->cxt, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	if (st->seg != NULL && st->gen == gen)
		return st->seg;
	if (st->seg != NULL)
		sift_cseg_close(st->seg);
	MemoryContextReset(st->segcx);
	st->seg = NULL;
	st->gen = gen;
	for (int i = 0; i < st->dir->nsegs; i++)
		if (st->dir->segs[i].kind == SIFT_SEG_IMMUTABLE && st->dir->segs[i].gen == gen)
			st->seg = sift_cseg_open(st->index, &st->dir->segs[i], st->segcx);
	sift_mem_assert_within(&mt_col_dv_settle);
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
	}
	sift_tomb_walk_end(&w);
	sift_col_dv_settle_end(st);
	/* a scan that began before reads the directory again: the heap marks pages all-visible after a pass the same way (sift_mops_bulkdelete) */
	if (cleared > 0)
		sift_dir_update(index, &(SiftDirChange) {.extra = InvalidBuffer});
	return cleared;
}

/* The unit the moves send (gen, unit) to: false when no move holds it */
static bool
rekey_target(const SiftDvMove *moves, int nmoves, uint64 gen, uint32 unit, uint64 *gen2, uint32 *unit2)
{
	for (int i = 0; i < nmoves; i++)
		if (moves[i].gen == gen && unit >= moves[i].unit && unit - moves[i].unit < moves[i].nunits)
		{
			*gen2 = moves[i].gen2;
			*unit2 = moves[i].unit2 + (unit - moves[i].unit);
			return true;
		}
	return false;
}

/* The record in hand written again for the unit that holds its rows now: the old one stays in the log, as nothing names its segment after the switch, and the next rewrite of the log drops it */
static void
rekey_record(Relation index, SiftTombLog *log, const SiftTomb *slots, uint32 n, uint64 gen2, uint32 unit2)
{
	SiftTomb   *copy = palloc(n * sizeof(SiftTomb));

	memcpy(copy, slots, n * sizeof(SiftTomb));
	sift_dv_rekey_slots(copy, n, gen2, unit2);
	sift_tomb_append_many(index, log, copy, (int) n);
	pfree(copy);
}

/* The live rows of the record in hand as marks, for a unit that the new segments cut again */
static void
carry_record(DvSettle *st, SiftColSegment *seg, Relation index, SiftTombLog *log)
{
	SiftTomb   *marks;
	SiftKey    *at;
	uint32		nmarks = 0;

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
}

/*
 * The records of the segments old[0..nold) settled for a step that is about to replace them: those of the units that moves name are written again under the names of the units that hold their
 * rows (the new segments hold them as they were), the rest carried to marks (a mark names a row by its heap address, which the new segments keep, where a record names the unit of a segment that
 * will be gone). Holds the lock that the writers of records take shared, from here to sift_col_dv_switched, so that no record is written for a segment after this has looked, and the switch of the
 * directory is the step's own: a reader that holds the old directory finds the old records, one that holds the new finds the new, and nothing between the two is lost. A record of a transaction that
 * aborted, or whose rows are cleared, is left alone. The step that re-cuts units may fold the records of committed-and-old transactions into the liveness first (sift_col_fold_marks, before it reads
 * the liveness of the units it re-cuts), which spares their marks; the carry is correct without it.
 */
void
sift_col_dv_rekey(Relation index, const uint64 *old, int nold, const SiftDvMove *moves, int nmoves)
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
		uint64		gen2;
		uint32		unit2;
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
		if (rekey_target(moves, nmoves, st->rec->gen, st->rec->desc.part, &gen2, &unit2))
			rekey_record(index, log, e, n, gen2, unit2);
		else
			carry_record(st, seg, index, log);
	}
	sift_tomb_walk_end(&w);
	sift_col_dv_settle_end(st);
}

void
sift_col_dv_switching(Relation index, const uint64 *old, int nold)
{
	sift_col_dv_rekey(index, old, nold, NULL, 0);
}

void
sift_col_dv_switched(Relation index)
{
	UnlockPage(index, SIFT_TOMB_LOCK_BLKNO, ExclusiveLock);
}
