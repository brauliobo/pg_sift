/*
 * core/maint/vacuum_owed.c - the space step of a sift index whose cleanup VACUUM calls in parallel mode runs at the end of the VACUUM of its table. A table with two indexes or more is vacuumed in
 * parallel mode, where no transaction id can be assigned: the steps that lock the index exclusively (the lock is WAL-logged for a standby) would never run, so the file would keep its free end.
 * The leader calls the cleanup (the sift access methods leave VACUUM_OPTION_PARALLEL_CLEANUP out), which runs as VACUUM calls it, its merge included, and owes the space step to the pre-commit of
 * the transaction of the table, where VACUUM has left parallel mode and still holds the locks of the table and its indexes.
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
#include "core/quarantine/quarantine.h"
#include "core/restrict.h"

typedef struct Owed
{
	Oid			index;
	Oid			heap;
	int			message_level;
	SiftVacuumSpace space;
	const void *arg;
} Owed;

typedef struct OwedRun
{
	const Owed *owed;
	Relation	index;
} OwedRun;

static List *owed = NIL;
static bool hooked = false;

static void
note_size(Relation index)
{
	const BlockNumber pages = RelationGetNumberOfBlocks(index);

#if PG_VERSION_NUM >= 180000
	vac_update_relstats(index, pages, index->rd_rel->reltuples, 0, 0, false, InvalidTransactionId, InvalidMultiXactId, NULL, NULL, false);
#else
	vac_update_relstats(index, pages, index->rd_rel->reltuples, 0, false, InvalidTransactionId, InvalidMultiXactId, NULL, NULL, false);
#endif
}

static void
space_step(void *arg)
{
	const OwedRun *r = arg;

	r->owed->space(r->index, r->owed->arg, r->owed->message_level);
}

static void
run_owed(const Owed *o)
{
	Relation	heap = table_open(o->heap, ShareUpdateExclusiveLock);
	OwedRun		r = {o, index_open(o->index, RowExclusiveLock)};

	PushActiveSnapshot(GetTransactionSnapshot());
	if (sift_quarantine_run(r.index, SIFT_SKIP_VACUUM, space_step, &r))
		note_size(r.index);
	PopActiveSnapshot();
	index_close(r.index, NoLock);
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

bool
sift_vacuum_owes(IndexVacuumInfo *info, SiftVacuumSpace space, const void *arg)
{
	MemoryContext old;

	if (!sift_xid_unassignable())
		return false;
	old = MemoryContextSwitchTo(TopTransactionContext);
	owed = lappend(owed, memcpy(palloc(sizeof(Owed)), &(Owed) {RelationGetRelid(info->index), RelationGetRelid(info->heaprel), info->message_level, space, arg}, sizeof(Owed)));
	MemoryContextSwitchTo(old);
	if (!hooked)
		sift_xact_callback(owed_xact, NULL);
	hooked = true;
	return true;
}
