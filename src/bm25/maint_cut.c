/*
 * bm25/maint_cut.c - the cut of the free end of an index file that asks for no AccessExclusiveLock: the online shrink and the fallback of a VACUUM or a step of the worker whose
 * lock stayed refused (the cut of the file is in space.c, the callers in maint.c)
 */
#include "postgres.h"

#include "storage/lmgr.h"
#include "access/xlog.h"
#include "utils/rel.h"

#include "bm25/maint.h"
#include "bm25/maint_int.h"

/*
 * The online shrink (no AccessExclusiveLock; only where sift_maint_shrink may run): a write buffer above the end the file can have is sealed and promoted first, as appenders
 * keep the pages of its lanes and it cannot move, then the segments that lie above that end go down into the holes below them
 */
uint64
online_shrink(const SiftMaintOps *ops, Relation index, int elevel)
{
	if (ops->vacuum_rotate && sift_maint_buffer_above_shrink(index))
	{
		ops->vacuum_rotate(index);
		promote_locked(ops, index, false, false);
	}
	return sift_maint_shrink(index, elevel);
}

/*
 * The AccessExclusiveLock of the cut stayed refused for the wait (a reader or a writer holds the index all the time): the online cut and shrink need no such lock, and
 * whoever could not take it gives the free blocks back the way a VACUUM with a standby does. A no-op when the last cut got its lock.
 */
void
online_fallback(const SiftMaintOps *ops, Relation index, int elevel)
{
	uint64		moved = 0;
	BlockNumber freed = 0;

	if (!sift_maint_cut_lock_busy())
		return;
	sift_maint_online_fallback(true);
	PG_TRY();
	{
		moved = online_shrink(ops, index, elevel);
		freed = sift_maint_space_budget(index, NULL, elevel, 0);
		sift_maint_report_fallback(index, elevel, moved, freed);
	}
	PG_FINALLY();
	{
		sift_maint_online_fallback(false);
	}
	PG_END_TRY();
}

/* The free end of the file that a step of the worker leaves, cut when it is worth the lock (released: the step freed it itself, which its own snapshot holds back from the screen) */
void
cut_free_end(const SiftMaintOps *ops, Relation index, bool released)
{
	if (!ops->vacuum_truncate || !ops->vacuum_truncate(index) || RecoveryInProgress())
		return;
	sift_maint_lock(index, true);
	if (released)
		(void) sift_maint_cut_released(index, DEBUG1);
	else
		(void) sift_maint_space_budget(index, NULL, DEBUG1, 0);
	online_fallback(ops, index, DEBUG1);
	sift_maint_unlock(index);
}
