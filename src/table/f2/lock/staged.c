/*
 * table/f2/lock/staged.c - what the entry of a row says of an insert staged in its backend (table/f2/spec.h) and of its last holders: a unique check of another transaction finds the
 * row there until the stage writes it to a part, where the check finds it on disk, so the mark of a row that is written goes.
 */
#include "postgres.h"

#include "col/f2/l0/stage.h"
#include "table/f2/lock/esc.h"
#include "table/f2/lock/int.h"
#include "table/f2/spec.h"

bool
sift_tbl_rowlock_spec_done(Relation rel, uint64 tid, bool succeeded)
{
	int			si;
	SiftRowLockEntry *e = sift_tbl_rowlock_find_locked(rel, tid, LW_EXCLUSIVE, &si);
	bool		need_flush = false;

	if (e == NULL)
		return false;
	if (succeeded)
		need_flush = sift_spec_succeeded(e);
	else
	{
		sift_spec_aborted(e);
		sift_tbl_rowlock_drop_idle(si, e);
	}
	sift_tbl_rowlock_unlock(si);
	return need_flush;
}

void
sift_tbl_rowlock_staged_flushed(Relation rel, uint64 tid)
{
	int			si;
	SiftRowLockEntry *e = sift_tbl_rowlock_find_locked(rel, tid, LW_EXCLUSIVE, &si);

	if (e == NULL)
		return;
	sift_spec_flushed(e);
	sift_tbl_rowlock_unlock(si);
}

/* The mark of a row the stage wrote goes, and the entry with it when it holds nothing else; a speculative insert keeps its token until it completes */
static void
forget_written(Relation rel, uint64 tid)
{
	int			si;
	SiftRowLockEntry *e = sift_tbl_rowlock_find_locked(rel, tid, LW_EXCLUSIVE, &si);

	if (e == NULL)
		return;
	if ((e->flags & (SIFT_ROWLOCK_STAGED_INSERT | SIFT_ROWLOCK_SPECULATIVE)) == SIFT_ROWLOCK_STAGED_INSERT &&
		FullTransactionIdEquals(e->updater_fxid, GetCurrentFullTransactionId()))
	{
		sift_spec_flushed(e);
		sift_tbl_rowlock_drop_idle(si, e);
	}
	sift_tbl_rowlock_unlock(si);
}

void
sift_tbl_rowlock_staged_written(Relation rel)
{
	uint32		n = 0,
				ntouched = sift_touched_count();
	uint64	   *tids;
	bool	   *held;

	if (ntouched == 0)
		return;
	tids = palloc(sizeof(uint64) * ntouched);
	for (uint32 i = 0; i < ntouched; i++)
	{
		const SiftTouched *t = sift_touched_at(i);

		if (t->part_id == SIFT_ESC_PART_ALL && RelFileLocatorEquals(t->key.locator, rel->rd_locator))
			tids[n++] = t->key.tid;
	}
	held = palloc0(sizeof(bool) * Max(n, 1));
	sift_stage_rows_among(rel, tids, (int) n, held);
	for (uint32 i = 0; i < n; i++)
		if (!held[i])
			forget_written(rel, tids[i]);
	pfree(held);
	pfree(tids);
}

uint32
sift_tbl_rowlock_speculative_token(Relation rel, uint64 tid)
{
	int			si;
	SiftRowLockEntry *e = sift_tbl_rowlock_find_locked(rel, tid, LW_SHARED, &si);
	uint32		token;

	if (e == NULL)
		return 0;
	token = (e->flags & SIFT_ROWLOCK_SPECULATIVE) != 0 ? e->spec_token : 0;
	sift_tbl_rowlock_unlock(si);
	return token;
}

SiftRowLockPeek
sift_tbl_rowlock_peek(Relation rel, uint64 tid)
{
	int			si;
	SiftRowLockEntry *e = sift_tbl_rowlock_find_locked(rel, tid, LW_SHARED, &si);
	SiftRowLockPeek p = {0};

	if (e == NULL)
		return p;
	if ((e->flags & SIFT_ROWLOCK_COL_DELTA) != 0 && (e->flags & SIFT_ROWLOCK_COMMITTED_UPD) == 0)
		p = (SiftRowLockPeek) {.delta_xid = XidFromFullTransactionId(e->updater_fxid),.delta_cid = e->updater_cid};
	else if ((e->flags & SIFT_ROWLOCK_COL_DELTA) == 0 && e->nholders > 0)
		p.locker = e->holders[0].xid;
	else if ((e->flags & SIFT_ROWLOCK_LAST_LOCKER) != 0)
		p.locker = XidFromFullTransactionId(e->updater_fxid);
	sift_tbl_rowlock_unlock(si);
	return p;
}
