/*
 * core/quarantine_sql.c - the SQL functions over the registry of core/quarantine.c: the rows of sift.quarantine, the metapage flag of an index and the names of the indexes out of service.
 */
#include "postgres.h"

#include "access/relation.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "core/inspect.h"
#include "core/meta_peek.h"
#include "core/quarantine.h"
#include "core/quarantine_int.h"
#include "core/sift.h"
#include "core/sql.h"

PG_FUNCTION_INFO_V1(sift_quarantine_rows);
PG_FUNCTION_INFO_V1(sift_meta_quarantined);

/* The indexes of this database that the registry holds out of service, as a list of names for a hint; NULL when there is none */
const char *
sift_quarantine_names(void)
{
	QuarShm    *q = sift_quar_table();
	Oid			ids[QUAR_ENTRIES];
	int			n = 0;
	StringInfoData names;

	if (q == NULL || pg_atomic_read_u32(&q->active_count) == 0 || !IsTransactionState())
		return NULL;
	SpinLockAcquire(&q->mutex);
	for (int i = 0; i < sift_quar_entries(); i++)
		if (q->entry[i].used && q->entry[i].confirmed && q->entry[i].file.db == MyDatabaseId)
			ids[n++] = q->entry[i].indexoid;
	SpinLockRelease(&q->mutex);
	initStringInfo(&names);
	for (int i = 0; i < n; i++)
	{
		char	   *name = get_rel_name(ids[i]);

		if (name != NULL)
			appendStringInfo(&names, "%s\"%s\"", names.len ? ", " : "", name);
	}
	return names.len ? names.data : NULL;
}


/* sift._quarantine(): the files of this database the registry knows, one row each (the view sift.quarantine joins them to their indexes) */
Datum
sift_quarantine_rows(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	QuarEntry  *copy = palloc(sizeof(QuarEntry) * QUAR_ENTRIES);
	QuarShm    *q;
	int			n = 0;

	InitMaterializedSRF(fcinfo, 0);
	sift_quar_publish(sift_quar_table());
	if ((q = sift_quar_table()) != NULL)
	{
		SpinLockAcquire(&q->mutex);
		for (int i = 0; i < sift_quar_entries(); i++)
			if (q->entry[i].used && q->entry[i].file.db == MyDatabaseId)
				copy[n++] = q->entry[i];
		SpinLockRelease(&q->mutex);
	}
	else
		for (int i = 0; i < QUAR_LOCAL; i++)
			if (sift_quar_local[i].used && sift_quar_local[i].file.db == MyDatabaseId)
				copy[n++] = sift_quar_local[i];
	for (int i = 0; i < n; i++)
	{
		Datum		v[11];
		bool		nulls[11] = {false};

		if (!sift_may_inspect_rel(copy[i].indexoid))
			continue;
		v[0] = ObjectIdGetDatum(copy[i].indexoid);
		v[1] = ObjectIdGetDatum(copy[i].file.rel);
		v[2] = TimestampTzGetDatum(copy[i].detected_at);
		v[3] = BoolGetDatum(copy[i].confirmed);
		v[4] = BoolGetDatum(copy[i].persisted);
		v[5] = CStringGetTextDatum(unpack_sql_state(copy[i].sqlerrcode));
		v[6] = CStringGetTextDatum(copy[i].reason);
		v[7] = Int64GetDatum(copy[i].detections);
		v[8] = Int64GetDatum((int64) copy[i].skipped_writes);
		v[9] = Int64GetDatum((int64) copy[i].skipped_vacuums);
		v[10] = Int64GetDatum((int64) copy[i].refused_scans);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	return (Datum) 0;
}

/* Whether the metapage of the index flags it out of service, whatever the registry says (the rows of sift.quarantine that no backend has met since the server started); false while a REINDEX holds the index, as the inspectors answer NULL then */
Datum
sift_meta_quarantined(PG_FUNCTION_ARGS)
{
	Oid			indexoid = PG_GETARG_OID(0);
	Relation	rel;
	bool		flagged = false;

	if (!sift_may_inspect_rel(indexoid) || !ConditionalLockRelationOid(indexoid, AccessShareLock))
		PG_RETURN_BOOL(false);
	rel = try_relation_open(indexoid, NoLock);
	if (rel != NULL && sift_quar_is_sift_index(rel))
		flagged = (sift_meta_peek_format(rel).flags & SIFT_META_QUARANTINED) != 0;
	if (rel != NULL)
		relation_close(rel, NoLock);
	UnlockRelationOid(indexoid, AccessShareLock);
	PG_RETURN_BOOL(flagged);
}

