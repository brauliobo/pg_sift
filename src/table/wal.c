/*
 * table/wal.c - logical replication of a sift_table (docs/COL-TAM.md section 5).
 *
 * Page changes go through generic WAL and need no setup. Logical decoding does not decode generic WAL, so a published table, or any
 * table of a database that has a logical slot of a plugin that decodes every table (wal_level = logical), also writes one record of the
 * custom resource manager per row change: {rlocator, new row, replica identity key of the old row}. Redo ignores it; rm_decode turns it
 * into a reorder-buffer change.
 * A custom resource manager can only be registered while shared_preload_libraries is processed, so that is the one thing pg_sift must
 * be preloaded for (primary and every server replaying this WAL). Without it a DML on a published table fails with 0A000 instead of
 * leaving a gap; a table that only a slot decodes writes no record, as no record can be written.
 * Before 5.0 the records carried the experimental id; it stays registered as a replay and decode alias (SIFT_TBL_LEGACY_RMGR_ID) so
 * that a standby or a crash recovery reads the WAL of 4.x, and nothing is written under it.
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "catalog/pg_publication.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"
#include "storage/lwlock.h"
#include "storage/spin.h"
#include "utils/rel.h"

#include "table/dispatch.h"
#include "table/rmgr_id.h"
#include "table/wal.h"

#define XLOG_SIFT_TBL_INSERT	0x00
#define XLOG_SIFT_TBL_UPDATE	0x10
#define XLOG_SIFT_TBL_DELETE	0x20

/* Main data: this header, the new row (newlen bytes of heap tuple), then the old key row (oldlen bytes) */
typedef struct SiftTblChange
{
	RelFileLocator rlocator;
	uint32		newlen;
	uint32		oldlen;
} SiftTblChange;

static bool rmgr_registered;

PG_FUNCTION_INFO_V1(sift_tbl_rmgr_registered);

Datum
sift_tbl_rmgr_registered(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(rmgr_registered);
}

bool
sift_tbl_published(Relation rel)
{
	PublicationDesc pd;

	if (!XLogLogicalInfoActive() || !RelationIsPermanent(rel))
		return false;
	RelationBuildPublicationDesc(rel, &pd);
	return pd.pubactions.pubinsert || pd.pubactions.pubupdate || pd.pubactions.pubdelete;
}

/* A logical slot of this database whose plugin decodes every table, publication or not (test_decoding, wal2json): pgoutput sends what its publications name, which sift_tbl_published answers */
static bool
any_slot_decodes(void)
{
	bool		found = false;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots && !found; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		SpinLockAcquire(&s->mutex);
		found = s->in_use && SlotIsLogical(s) && s->data.database == MyDatabaseId && strcmp(NameStr(s->data.plugin), "pgoutput") != 0;
		SpinLockRelease(&s->mutex);
	}
	LWLockRelease(ReplicationSlotControlLock);
	return found;
}

/*
 * The slots are walked once for a transaction that has its xid, not for every row it changes: a slot made after the walk builds its snapshot from a record of the running transactions that
 * names this one, and decodes no transaction that ran then (snapbuild.c), so the answer stands until the transaction ends.
 */
static FullTransactionId decodes_for;
static bool decodes;

static bool
slot_decodes(Relation rel)
{
	FullTransactionId top = GetTopFullTransactionIdIfAny();

	if (!XLogLogicalInfoActive() || !RelationIsPermanent(rel))
		return false;
	if (!FullTransactionIdIsValid(top) || !FullTransactionIdEquals(top, decodes_for))
	{
		decodes = any_slot_decodes();
		decodes_for = top;
	}
	return decodes;
}

bool
sift_tbl_check_publication(Relation rel)
{
	bool		published = sift_tbl_published(rel);

	if (!published && !(rmgr_registered && slot_decodes(rel)))
		return false;
	if (!rmgr_registered)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_table table \"%s\" is published and wal_level is \"logical\", but pg_sift is not in shared_preload_libraries",
						RelationGetRelationName(rel)),
				 errdetail("Logical replication of a sift_table table needs the pg_sift resource manager, which is registered only when the library is preloaded."),
				 errhint("Add pg_sift to shared_preload_libraries (primary and every standby), for example ALTER SYSTEM SET shared_preload_libraries = 'pg_sift' with the libraries it lists now, and restart, or publish a heap staging table instead of \"%s\".",
						 RelationGetRelationName(rel))));
	return true;
}

/* The replica identity part of a row given as values: all of it, the identity key columns (the others are null, so no value of them is copied), or nothing */
static HeapTuple
identity_of_values(Relation rel, Datum *values, bool *isnull)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Bitmapset  *key;
	bool	   *keyed;
	HeapTuple	res;

	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_NOTHING)
		return NULL;
	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		return heap_form_tuple(desc, values, isnull);
	if (bms_is_empty(key = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_IDENTITY_KEY)))
		return NULL;
	keyed = palloc(desc->natts * sizeof(bool));
	for (int i = 0; i < desc->natts; i++)
		keyed[i] = isnull[i] || !bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber, key);
	res = heap_form_tuple(desc, values, keyed);
	pfree(keyed);
	bms_free(key);
	return res;
}

HeapTuple
sift_tbl_old_key(Relation rel, HeapTuple old)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	res;

	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_NOTHING)
		return NULL;
	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		return old;
	values = palloc(desc->natts * sizeof(Datum));
	isnull = palloc(desc->natts * sizeof(bool));
	heap_deform_tuple(old, desc, values, isnull);
	res = identity_of_values(rel, values, isnull);
	pfree(values);
	pfree(isnull);
	return res;
}

HeapTuple
sift_tbl_slot_old_key(Relation rel, TupleTableSlot *old)
{
	slot_getallattrs(old);
	return identity_of_values(rel, old->tts_values, old->tts_isnull);
}

Datum *
sift_tbl_slot_values(TupleDesc desc, TupleTableSlot *slot)
{
	Datum	   *values;

	slot_getallattrs(slot);
	values = slot->tts_values;
	for (int i = 0; i < desc->natts; i++)
		if (!slot->tts_isnull[i] && TupleDescAttr(desc, i)->attlen == -1 && VARATT_IS_EXTERNAL(DatumGetPointer(values[i])))
		{
			if (values == slot->tts_values)
			{
				values = palloc(desc->natts * sizeof(Datum));
				memcpy(values, slot->tts_values, desc->natts * sizeof(Datum));
			}
			values[i] = PointerGetDatum(detoast_external_attr((struct varlena *) DatumGetPointer(values[i])));
		}
	return values;
}

void
sift_tbl_slot_values_free(TupleDesc desc, TupleTableSlot *slot, Datum *values)
{
	if (values == slot->tts_values)
		return;
	for (int i = 0; i < desc->natts; i++)
		if (values[i] != slot->tts_values[i])
			pfree(DatumGetPointer(values[i]));
	pfree(values);
}

MinimalTuple
sift_tbl_detoast_slot_tuple(Relation rel, TupleTableSlot *slot, bool *should_free)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values = sift_tbl_slot_values(desc, slot);
	TupleTableSlot *expanded;
	MinimalTuple mtup;

	if (values == slot->tts_values)
		return ExecFetchSlotMinimalTuple(slot, should_free);
	expanded = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	memcpy(expanded->tts_values, values, desc->natts * sizeof(Datum));
	memcpy(expanded->tts_isnull, slot->tts_isnull, desc->natts * sizeof(bool));
	ExecStoreVirtualTuple(expanded);
	mtup = ExecFetchSlotMinimalTuple(expanded, should_free);
	ExecDropSingleTupleTableSlot(expanded);
	sift_tbl_slot_values_free(desc, slot, values);
	return mtup;
}

static void
log_guard(Relation rel, uint64 newlen, uint64 oldlen)
{
	uint64		total = newlen + oldlen + 64;

	if (total > SIFT_TBL_LOG_MAX_BYTES)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("logical replication WAL record for sift_table \"%s\" is %llu bytes, exceeding the %llu-byte limit", RelationGetRelationName(rel),
							   (unsigned long long) total, (unsigned long long) SIFT_TBL_LOG_MAX_BYTES),
						errdetail("sift_table stores wide rows without TOAST; a logical replication change record must fit within %llu bytes (new=%llu, old=%llu).",
								  (unsigned long long) SIFT_TBL_LOG_MAX_BYTES, (unsigned long long) newlen, (unsigned long long) oldlen),
						errhint("Use a primary key REPLICA IDENTITY instead of FULL, keep values above %llu MB out of published sift_table columns, or exclude the table from logical publications.",
								(unsigned long long) (SIFT_TBL_LOG_MAX_BYTES / (1024 * 1024)))));
}

void
sift_tbl_log_key(Relation rel, SiftTblOp op, MinimalTuple newrow, HeapTuple key)
{
	static const uint8 info[] = {[SIFT_TBL_OP_INSERT] = XLOG_SIFT_TBL_INSERT,[SIFT_TBL_OP_UPDATE] = XLOG_SIFT_TBL_UPDATE,
	[SIFT_TBL_OP_DELETE] = XLOG_SIFT_TBL_DELETE};
	SiftTblChange hdr = {.rlocator = rel->rd_locator};
	HeapTuple	newtup;

	log_guard(rel, newrow ? newrow->t_len : 0, key ? key->t_len : 0);
	newtup = newrow ? heap_tuple_from_minimal_tuple(newrow) : NULL;
	hdr.newlen = newtup ? newtup->t_len : 0;
	hdr.oldlen = key ? key->t_len : 0;
	XLogBeginInsert();
	XLogRegisterData((char *) &hdr, sizeof(hdr));
	if (newtup)
		XLogRegisterData((char *) newtup->t_data, newtup->t_len);
	if (key)
		XLogRegisterData((char *) key->t_data, key->t_len);
	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);
	XLogInsert(SIFT_TBL_RMGR_ID, info[op]);
	if (newtup)
		heap_freetuple(newtup);
}

void
sift_tbl_log_change(Relation rel, SiftTblOp op, MinimalTuple newrow, HeapTuple old)
{
	HeapTuple	key = old ? sift_tbl_old_key(rel, old) : NULL;

	sift_tbl_log_key(rel, op, newrow, key);
	if (key && key != old)
		heap_freetuple(key);
}

static void
tbl_redo(XLogReaderState *record)
{
	/* the pages are restored by generic WAL; this record only exists for logical decoding */
}

static const char *
tbl_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_SIFT_TBL_INSERT:
			return "INSERT";
		case XLOG_SIFT_TBL_UPDATE:
			return "UPDATE";
		case XLOG_SIFT_TBL_DELETE:
			return "DELETE";
	}
	return NULL;
}

static void
tbl_desc(StringInfo buf, XLogReaderState *record)
{
	const SiftTblChange *hdr = (const SiftTblChange *) XLogRecGetData(record);

	appendStringInfo(buf, "rel %u/%u/%u new %u old %u bytes", hdr->rlocator.spcOid, hdr->rlocator.dbOid, hdr->rlocator.relNumber,
					 hdr->newlen, hdr->oldlen);
}

/* PostgreSQL 18 renamed the reorder buffer allocators */
#if PG_VERSION_NUM < 180000
#define ReorderBufferAllocChange(rb)		ReorderBufferGetChange(rb)
#define ReorderBufferAllocTupleBuf(rb, len)	ReorderBufferGetTupleBuf(rb, len)
#endif

/* A reorder-buffer tuple holding a copy of len bytes of heap tuple */
static HeapTuple
decoded_tuple(ReorderBuffer *rb, const char *data, uint32 len)
{
	HeapTuple	tup = ReorderBufferAllocTupleBuf(rb, len);

	tup->t_len = len;
	tup->t_tableOid = InvalidOid;
	ItemPointerSetInvalid(&tup->t_self);
	memcpy(tup->t_data, data, len);
	return tup;
}

static void
tbl_decode(LogicalDecodingContext *ctx, XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	TransactionId xid = XLogRecGetXid(r);
	const SiftTblChange *hdr = (const SiftTblChange *) XLogRecGetData(r);
	const char *rows = (const char *) (hdr + 1);
	ReorderBufferChange *change;

	ReorderBufferProcessXid(ctx->reorder, xid, buf->origptr);
	if (SnapBuildCurrentState(ctx->snapshot_builder) < SNAPBUILD_FULL_SNAPSHOT ||
		!SnapBuildProcessChange(ctx->snapshot_builder, xid, buf->origptr) || ctx->fast_forward)
		return;
	if (hdr->rlocator.dbOid != ctx->slot->data.database || filter_by_origin_cb_wrapper(ctx, XLogRecGetOrigin(r)))
		return;
	change = ReorderBufferAllocChange(ctx->reorder);
	switch (XLogRecGetInfo(r) & ~XLR_INFO_MASK)
	{
		case XLOG_SIFT_TBL_INSERT:
			change->action = REORDER_BUFFER_CHANGE_INSERT;
			break;
		case XLOG_SIFT_TBL_UPDATE:
			change->action = REORDER_BUFFER_CHANGE_UPDATE;
			break;
		default:
			change->action = REORDER_BUFFER_CHANGE_DELETE;
			break;
	}
	change->origin_id = XLogRecGetOrigin(r);
	change->data.tp.rlocator = hdr->rlocator;
	change->data.tp.clear_toast_afterwards = true;
	if (hdr->newlen > 0)
		change->data.tp.newtuple = decoded_tuple(ctx->reorder, rows, hdr->newlen);
	if (hdr->oldlen > 0)
		change->data.tp.oldtuple = decoded_tuple(ctx->reorder, rows + hdr->newlen, hdr->oldlen);
	ReorderBufferQueueChange(ctx->reorder, xid, buf->origptr, change, false);
}

static RmgrData
tbl_rmgr(const char *name)
{
	return (RmgrData) {
		.rm_name = name,
		.rm_redo = tbl_redo,
		.rm_desc = tbl_desc,
		.rm_identify = tbl_identify,
		.rm_decode = tbl_decode,
	};
}

/* Called from _PG_init: only a preloading server can register the resource managers */
void
sift_tbl_init(void)
{
	RmgrData	current = tbl_rmgr(SIFT_TBL_RMGR_NAME);
	RmgrData	legacy = tbl_rmgr(SIFT_TBL_LEGACY_RMGR_NAME);

	sift_tbl_v1_init();
	sift_tbl_v2_init();
	sift_tbl_utility_init();
	if (!process_shared_preload_libraries_in_progress)
		return;
	RegisterCustomRmgr(SIFT_TBL_RMGR_ID, &current);
	RegisterCustomRmgr(SIFT_TBL_LEGACY_RMGR_ID, &legacy);
	rmgr_registered = true;
}
