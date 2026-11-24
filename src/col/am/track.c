/*
 * col/am/track.c - sift.col_track_old_rows(), the trigger of the option track_changes, and the triggers following the option after ALTER INDEX, by the format of the index
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_index.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "col/am.h"

PG_FUNCTION_INFO_V1(sift_col_track_old_rows);

/* The index a trigger serves, and its format: asked again when the cache of pg_class says the index changed (REINDEX CONCURRENTLY moves the trigger to the new index) */
static struct
{
	Oid			trigger;
	uint32		index_hash;
	const SiftColFormatOps *ops;
	bool		hooked;
} served;

static void
served_inval(Datum arg, int cacheid, uint32 hashvalue)
{
	if (hashvalue == 0 || hashvalue == served.index_hash)
		served.trigger = InvalidOid;
}

static const SiftColFormatOps *
ops_serving(Trigger *trig)
{
	Oid			index;

	if (!served.hooked)
	{
		CacheRegisterSyscacheCallback(RELOID, served_inval, (Datum) 0);
		served.hooked = true;
	}
	if (served.trigger != trig->tgoid)
	{
		index = sift_col_track_trigger_index(trig->tgoid);
		served.index_hash = GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(index));
		served.ops = OidIsValid(index) && sift_col_index_oid_format(index) == SIFT_FORMAT2_MAJOR ? &sift_col_v2_ops : &sift_col_v1_ops;
		served.trigger = trig->tgoid;
	}
	return served.ops;
}

/*
 * The format of the trigger is the format of the index it serves, not the shape it was made in: an index that REINDEX CONCURRENTLY moved from format major 1 to 2 keeps the triggers
 * of format major 1, whose rows the format 2 reads as it does its own. Its statement triggers have no rows to read: a delete they would have staged is the heap's to tell.
 */
Datum
sift_col_track_old_rows(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	const SiftColFormatOps *ops = CALLED_AS_TRIGGER(fcinfo) ? ops_serving(td->tg_trigger) : &sift_col_v1_ops;

	if (ops == &sift_col_v2_ops && !TRIGGER_FIRED_FOR_ROW(td->tg_event))
		return PointerGetDatum(NULL);
	return ops->track_trigger(fcinfo);
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
