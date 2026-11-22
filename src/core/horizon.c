/*
 * core/horizon.c - the xid horizon of the table of an index (core/horizon.h).
 */
#include "core/host.h"

#include "access/transam.h"
#include "storage/procarray.h"

#include "core/catname.h"
#include "core/horizon.h"

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

FullTransactionId
sift_horizon(Relation index)
{
	Relation	heap = index->rd_rel->relkind == RELKIND_INDEX ? sift_relation_get(index->rd_index->indrelid) : index;
	TransactionId oldest = GetOldestNonRemovableTransactionId(heap);

	if (heap != index)
		RelationClose(heap);
	return FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), oldest);
}
