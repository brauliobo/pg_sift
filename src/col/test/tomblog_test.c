/*
 * col/test/tomblog_test.c - test hooks for the tombstone log of a heap column index (col/tomblog.c)
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "access/xact.h"
#include "funcapi.h"
#include "utils/rel.h"
#include "col/am.h"
#include "core/tomb.h"

PG_FUNCTION_INFO_V1(sift_test_col_log_create);
PG_FUNCTION_INFO_V1(sift_test_col_log_append);
PG_FUNCTION_INFO_V1(sift_test_col_log_entries);

/* sift._test_col_log_create(index): adds the tombstone log to an index that has none */
Datum
sift_test_col_log_create(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), ShareUpdateExclusiveLock);

	sift_col_log_create(index);
	index_close(index, NoLock);
	PG_RETURN_VOID();
}

/* sift._test_col_log_append(index, tid): one entry for the heap address, by the current transaction and command */
Datum
sift_test_col_log_append(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), RowExclusiveLock);
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	SiftTombLog *log = sift_col_log(index);

	if (log == NULL)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("index \"%s\" has no tombstone log", RelationGetRelationName(index))));
	sift_tomb_append(index, log, sift_key_from_tid(tid), GetCurrentTransactionId(), GetCurrentCommandId(true), SIFT_TOMB_DELETE, 0, 0);
	index_close(index, NoLock);
	PG_RETURN_VOID();
}

/* sift._test_col_log_entries(index): the entries of the log in log order: heap address, transaction, command, kind */
Datum
sift_test_col_log_entries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTombLog *log = sift_col_log(index);
	SiftTomb   *all;
	uint32		n;

	InitMaterializedSRF(fcinfo, 0);
	if (log != NULL)
	{
		n = log->nents + log->nrecent;
		all = palloc(Max(n, 1) * sizeof(SiftTomb));
		memcpy(all, log->ents, log->nents * sizeof(SiftTomb));
		memcpy(all + log->nents, log->recent, log->nrecent * sizeof(SiftTomb));
		/* log order: the position a backend stamps when it reads an entry */
		for (uint32 i = 0; i < n; i++)
			for (uint32 j = i; j > 0 && all[j - 1].seq > all[j].seq; j--)
			{
				SiftTomb	t = all[j];

				all[j] = all[j - 1];
				all[j - 1] = t;
			}
		for (uint32 i = 0; i < n; i++)
		{
			Datum		v[4];
			bool		nulls[4] = {false};
			ItemPointer tid = palloc(sizeof(ItemPointerData));

			sift_key_to_tid(all[i].key, tid);
			v[0] = ItemPointerGetDatum(tid);
			v[1] = TransactionIdGetDatum(all[i].xid);
			v[2] = Int32GetDatum((int32) all[i].cid);
			v[3] = Int32GetDatum(all[i].kind);
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
#endif							/* SIFT_TESTING */
