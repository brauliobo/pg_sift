/*
 * table/f2/staged.c - the lookup of an address by a dirty snapshot (a check of a unique index) when no part holds it: a running insert may have staged the row without writing it (table/f2/spec.h).
 */
#include "postgres.h"

#include "executor/tuptable.h"
#include "utils/snapshot.h"

#include "table/f2/gcache.h"
#include "table/f2/spec.h"
#include "table/f2/tbl.h"

/*
 * The address no part holds may be a row an insert of a running transaction has staged and not written (f2_spec.h): the check of a unique index finds its entry before the row exists anywhere a
 * scan can read. A dirty snapshot learns who to wait for, and the slot is the row with no values (the check looks at whether the row is there, not at what it holds). Locate again when the inserter
 * ended in the meantime: a committed one wrote the row at its pre-commit flush.
 */
bool
sift_tbl_f2_locate_or_wait(Relation rel, SiftF2Store *store, uint64 tid, Snapshot snapshot, SiftF2Loc *loc, bool *waiting)
{
	TransactionId xmin = InvalidTransactionId;
	uint32		token = 0;

	*waiting = false;
	if (sift_f2_locate_cached_read(rel, store, tid, loc))
		return true;
	if (snapshot->snapshot_type != SNAPSHOT_DIRTY)
		return false;
	switch (sift_tbl_rowlock_probe_staged(rel, tid, &xmin, &token))
	{
		case SIFT_SPEC_ACTIVE:
			snapshot->xmin = xmin;
			snapshot->xmax = InvalidTransactionId;
			snapshot->speculativeToken = token;
			*waiting = true;
			return false;
		case SIFT_SPEC_ENDED:
			return sift_f2_locate_cached_read(rel, store, tid, loc);
		case SIFT_SPEC_NONE:
			break;
	}
	return false;
}

void
sift_tbl_f2_staged_stub(TupleTableSlot *slot, uint64 tid)
{
	TupleDesc	desc = slot->tts_tupleDescriptor;

	ExecClearTuple(slot);
	memset(slot->tts_isnull, true, sizeof(bool) * desc->natts);
	sift_tbl_tid_unpack(&slot->tts_tid, tid);
	ExecStoreVirtualTuple(slot);
}
