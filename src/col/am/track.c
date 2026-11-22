/*
 * col/am/track.c - sift.col_track_old_rows(), the trigger of the option track_changes, and the triggers following the option after ALTER INDEX, by the format of the index
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_index.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "utils/syscache.h"

#include "col/am.h"

PG_FUNCTION_INFO_V1(sift_col_track_old_rows);

/* The one row trigger on both events is that of an index of format major 2; the others fire for one event, or for a statement */
Datum
sift_col_track_old_rows(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;

	if (CALLED_AS_TRIGGER(fcinfo) && TRIGGER_FIRED_AFTER(td->tg_event) && TRIGGER_FIRED_FOR_ROW(td->tg_event) && TRIGGER_FOR_UPDATE(td->tg_trigger->tgtype) &&
		TRIGGER_FOR_DELETE(td->tg_trigger->tgtype))
		return sift_col_v2_ops.track_trigger(fcinfo);
	return sift_col_v1_ops.track_trigger(fcinfo);
}

void
sift_col_track_reconcile(Relation index)
{
	sift_col_ops(index)->track_reconcile(index);
}

/*
 * An index in service: DROP INDEX CONCURRENTLY marks it dead (indislive) before it waits for the transactions that could still use it, then deletes it holding its lock until it
 * commits. A writer that gets past that mark skips it (it is not read, and a lock on it would wait for the drop, which waits for the writer's lock on the table). An index that is
 * deleted (InvalidOid: the trigger's index is gone) is not in service either.
 */
bool
sift_col_track_index_live(Oid indexoid)
{
	HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
	bool		live = HeapTupleIsValid(tup) && ((Form_pg_index) GETSTRUCT(tup))->indislive;

	if (HeapTupleIsValid(tup))
		ReleaseSysCache(tup);
	return live;
}
