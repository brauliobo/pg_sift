/*
 * table/f2/lock/2pc.c - the row locks of a prepared transaction in the lock manager of the server (f2_lock_2pc.h).
 */
#include "postgres.h"

#include "access/twophase.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"

#include "table/f2/lock/2pc.h"

static LOCKMODE
lock_mode(LockTupleMode mode)
{
	static const LOCKMODE map[4] = {AccessShareLock, RowShareLock, ExclusiveLock, AccessExclusiveLock};

	return map[mode];
}

static LOCKTAG
tuple_tag(Oid dbid, Oid relid, uint64 tid)
{
	LOCKTAG		tag;

	SET_LOCKTAG_TUPLE(tag, dbid, relid, (BlockNumber) (tid >> 16), (OffsetNumber) (tid & 0xFFFF));
	return tag;
}

static LOCKTAG
marker_tag(Oid dbid, Oid relid)
{
	LOCKTAG		tag;

	SET_LOCKTAG_PAGE(tag, dbid, relid, SIFT_F2_ROW_2PC_LOCK_BLKNO);
	return tag;
}

void
sift_lock_2pc_hold(Oid dbid, Oid relid, uint64 tid, LockTupleMode mode, bool first_of_relation)
{
	LOCKTAG		tuple = tuple_tag(dbid, relid, tid);
	LOCKTAG		marker = marker_tag(dbid, relid);

	(void) LockAcquire(&tuple, lock_mode(mode), false, false);
	if (first_of_relation)
		(void) LockAcquire(&marker, RowShareLock, false, false);
}

bool
sift_lock_2pc_present(Oid dbid, Oid relid)
{
	LOCKTAG		marker = marker_tag(dbid, relid);
	VirtualTransactionId *holders;
	int			n;

	holders = GetLockConflicts(&marker, ExclusiveLock, &n);
	pfree(holders);
	return n > 0;
}

TM_Result
sift_lock_2pc_probe(Oid dbid, Oid relid, uint64 tid, LockTupleMode mode, TM_FailureData *tmfd)
{
	LOCKTAG		tuple = tuple_tag(dbid, relid, tid);
	VirtualTransactionId *holders;
	TransactionId prepared = InvalidTransactionId;
	int			n;

	holders = GetLockConflicts(&tuple, lock_mode(mode), &n);
	for (int i = 0; i < n && !TransactionIdIsValid(prepared); i++)
	{
		bool		more;

		prepared = TwoPhaseGetXidByVirtualXID(holders[i], &more);
	}
	pfree(holders);
	tmfd->xmax = prepared;
	return TransactionIdIsValid(prepared) ? TM_BeingModified : TM_Ok;
}
