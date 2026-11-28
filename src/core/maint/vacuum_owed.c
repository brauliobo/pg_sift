/*
 * core/maint/vacuum_owed.c - the cleanup of a sift index that VACUUM calls in parallel mode runs at the end of the VACUUM of its table. A table with two indexes or more is vacuumed in parallel
 * mode, where no transaction id can be assigned: the steps that lock the index exclusively (the lock is WAL-logged for a standby) would never run, so the space step would not give the file back.
 * The leader calls the cleanup (the sift access methods leave VACUUM_OPTION_PARALLEL_CLEANUP out) and owes it to the pre-commit of the transaction of the table, where VACUUM has left parallel
 * mode and still holds the locks of the table and its indexes.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/multixact.h"
#include "access/table.h"
#include "access/xact.h"
#include "commands/vacuum.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "core/dispatch.h"
#include "core/maint/vacuum.h"

typedef struct Owed
{
	Oid			index;
	Oid			heap;
	int			message_level;
	bool		estimated_count;
	double		num_heap_tuples;
	IndexBulkDeleteResult *stats;	/* of the bulk deletion, NULL: none ran */
	SiftVacuumCleanup cleanup;
} Owed;

static List *owed = NIL;
static bool hooked = false;

static void
note_relstats(Relation index, const IndexBulkDeleteResult *stats)
{
#if PG_VERSION_NUM >= 180000
	vac_update_relstats(index, stats->num_pages, stats->num_index_tuples, 0, 0, false, InvalidTransactionId, InvalidMultiXactId, NULL, NULL, false);
#else
	vac_update_relstats(index, stats->num_pages, stats->num_index_tuples, 0, false, InvalidTransactionId, InvalidMultiXactId, NULL, NULL, false);
#endif
}

static void
run_owed(const Owed *o)
{
	Relation	heap = table_open(o->heap, ShareUpdateExclusiveLock);
	Relation	index = index_open(o->index, RowExclusiveLock);
	IndexVacuumInfo info = {.index = index,.heaprel = heap,.estimated_count = o->estimated_count,.message_level = o->message_level,.num_heap_tuples = o->num_heap_tuples,
	.strategy = GetAccessStrategy(BAS_VACUUM)};
	IndexBulkDeleteResult *stats;

	PushActiveSnapshot(GetTransactionSnapshot());
	stats = o->cleanup(&info, o->stats);
	PopActiveSnapshot();
	if (stats != NULL && !stats->estimated_count)
		note_relstats(index, stats);
	index_close(index, NoLock);
	table_close(heap, NoLock);
}

static void
owed_xact(XactEvent event, void *arg pg_attribute_unused())
{
	List	   *due = owed;

	owed = NIL;
	if (event == XACT_EVENT_PRE_COMMIT)
		foreach_ptr(Owed, o, due)
			run_owed(o);
}

static void
owe(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, SiftVacuumCleanup cleanup)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	Owed	   *o = palloc(sizeof(Owed));

	*o = (Owed) {RelationGetRelid(info->index), RelationGetRelid(info->heaprel), info->message_level, info->estimated_count, info->num_heap_tuples,
		stats != NULL ? memcpy(palloc(sizeof(IndexBulkDeleteResult)), stats, sizeof(IndexBulkDeleteResult)) : NULL, cleanup};
	owed = lappend(owed, o);
	MemoryContextSwitchTo(old);
	if (!hooked)
		sift_xact_callback(owed_xact, NULL);
	hooked = true;
}

IndexBulkDeleteResult *
sift_vacuum_cleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, SiftVacuumCleanup cleanup)
{
	if (!IsInParallelMode())
		return cleanup(info, stats);
	owe(info, stats, cleanup);
	if (stats != NULL)
		stats->estimated_count = true;
	return stats;
}
