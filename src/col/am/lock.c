/*
 * col/am/lock.c - the lock a CREATE INDEX of a sift_col index takes first
 *
 * The build installs the triggers of the change log of its index when it has read the table (col/track.c), which takes ShareRowExclusiveLock on the table, and CREATE INDEX holds ShareLock
 * on it all the while: two builds of indexes of one table that run at the same time (a parallel restore) each wait for the other's ShareLock. The event trigger before the statement takes the
 * stronger lock, that conflicts with itself, before the statement takes its own, so the second build waits for the first at the start and holds nothing.
 */
#include "postgres.h"
#include "commands/event_trigger.h"
#include "catalog/namespace.h"
#include "commands/tablecmds.h"
#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "storage/lockdefs.h"

PG_FUNCTION_INFO_V1(sift_col_build_lock);

Datum
sift_col_build_lock(PG_FUNCTION_ARGS)
{
	Node	   *stmt;

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		ereport(ERROR, (errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED), errmsg("not fired by the event trigger manager")));
	stmt = ((EventTriggerData *) fcinfo->context)->parsetree;
	if (IsA(stmt, IndexStmt) && !((IndexStmt *) stmt)->concurrent && strcmp(((IndexStmt *) stmt)->accessMethod, "sift_col") == 0)
		(void) RangeVarGetRelidExtended(((IndexStmt *) stmt)->relation, ShareRowExclusiveLock, RVR_MISSING_OK, RangeVarCallbackOwnsRelation, NULL);
	PG_RETURN_NULL();
}
