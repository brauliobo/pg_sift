/*
 * core/horizon.c - the xid horizon of the table of an index (core/horizon.h).
 */
#include "core/host.h"

#include "access/transam.h"
#include "storage/procarray.h"

#include "core/catname.h"
#include "core/horizon.h"
#include "core/part_lock.h"

bool
sift_horizon_removable(Relation index, FullTransactionId xid)
{
	Relation	heap;
	bool		removable;

	if (index->rd_rel->relkind != RELKIND_INDEX)
		return GlobalVisCheckRemovableFullXid(index, xid);
	heap = sift_relation_get(index->rd_index->indrelid);
	removable = GlobalVisCheckRemovableFullXid(heap, xid);
	RelationClose(heap);
	return removable;
}

void
sift_horizon_vacuum_read_begin(Relation index)
{
	LockPage(index, SIFT_F2_VACUUM_READ_LOCK_BLKNO, ShareLock);
}

void
sift_horizon_vacuum_read_end(Relation index)
{
	UnlockPage(index, SIFT_F2_VACUUM_READ_LOCK_BLKNO, ShareLock);
}

bool
sift_horizon_vacuum_reading(Relation index)
{
	LOCKTAG		tag;

	SET_LOCKTAG_PAGE(tag, index->rd_lockInfo.lockRelId.dbId, index->rd_lockInfo.lockRelId.relId, SIFT_F2_VACUUM_READ_LOCK_BLKNO);
	if (LockHeldByMe(&tag, ShareLock, false))
		return true;
	if (!ConditionalLockPage(index, SIFT_F2_VACUUM_READ_LOCK_BLKNO, ExclusiveLock))
		return true;
	UnlockPage(index, SIFT_F2_VACUUM_READ_LOCK_BLKNO, ExclusiveLock);
	return false;
}

FullTransactionId
sift_horizon(Relation index)
{
	Relation	heap = index->rd_rel->relkind == RELKIND_INDEX ? sift_relation_get(index->rd_index->indrelid) : index;
	TransactionId oldest = GetOldestNonRemovableTransactionId(heap);

	if (heap != index)
		RelationClose(heap);
	return FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), oldest);
}
