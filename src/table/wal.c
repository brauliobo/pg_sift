/*
 * table/wal.c - logical replication of a sift_table (docs/COL-TAM.md section 5).
 *
 * Page changes go through generic WAL and need no setup. Logical decoding does not decode generic WAL, so a published table
 * (wal_level = logical) also writes one record of the custom resource manager per row change: {rlocator, new row, replica
 * identity key of the old row}. Redo ignores it; rm_decode turns it into a reorder-buffer change. A custom resource manager can
 * only be registered while shared_preload_libraries is processed, so that is the one thing pg_sift must be preloaded for (primary
 * and every server replaying this WAL). Without it a DML on a published table fails with 0A000 instead of leaving a gap.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "catalog/pg_publication.h"
#include "miscadmin.h"
#include "replication/decode.h"
#include "replication/logical.h"
#include "replication/origin.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "utils/rel.h"

#include "table/table.h"

/* A development resource manager id; a permanent one is requested from the PostgreSQL custom rmgr list before a release */
#define SIFT_TBL_RMGR_ID	RM_EXPERIMENTAL_ID

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

/* ---------- writing ---------- */

bool
sift_tbl_check_publication(Relation rel)
{
	PublicationDesc pd;

	if (!XLogLogicalInfoActive() || !RelationIsPermanent(rel))
		return false;
	RelationBuildPublicationDesc(rel, &pd);
	if (!pd.pubactions.pubinsert && !pd.pubactions.pubupdate && !pd.pubactions.pubdelete)
		return false;
	if (!rmgr_registered)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_table table \"%s\" is published and wal_level is \"logical\", but pg_sift is not in shared_preload_libraries",
						RelationGetRelationName(rel)),
				 errdetail("Logical replication of a sift_table table needs the pg_sift resource manager, which is registered only when the library is preloaded."),
				 errhint("Add pg_sift to shared_preload_libraries (primary and every standby) and restart, or publish a heap staging table instead of \"%s\".",
						 RelationGetRelationName(rel))));
	return true;
}

/* The replica identity part of an old row: all of it, the identity key columns, or nothing */
static HeapTuple
old_key(Relation rel, HeapTuple old)
{
	TupleDesc	desc = RelationGetDescr(rel);
	Bitmapset  *key;
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	res;

	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_NOTHING)
		return NULL;
	if (rel->rd_rel->relreplident == REPLICA_IDENTITY_FULL)
		return old;
	if (bms_is_empty(key = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_IDENTITY_KEY)))
		return NULL;
	values = palloc(desc->natts * sizeof(Datum));
	isnull = palloc(desc->natts * sizeof(bool));
	heap_deform_tuple(old, desc, values, isnull);
	for (int i = 0; i < desc->natts; i++)
		isnull[i] |= !bms_is_member(i + 1 - FirstLowInvalidHeapAttributeNumber, key);
	res = heap_form_tuple(desc, values, isnull);
	pfree(values);
	pfree(isnull);
	return res;
}

void
sift_tbl_log_change(Relation rel, SiftTblOp op, MinimalTuple newrow, HeapTuple old)
{
	static const uint8 info[] = {[SIFT_TBL_OP_INSERT] = XLOG_SIFT_TBL_INSERT,[SIFT_TBL_OP_UPDATE] = XLOG_SIFT_TBL_UPDATE,
	[SIFT_TBL_OP_DELETE] = XLOG_SIFT_TBL_DELETE};
	SiftTblChange hdr = {.rlocator = rel->rd_locator};
	HeapTuple	newtup = newrow ? heap_tuple_from_minimal_tuple(newrow) : NULL;
	HeapTuple	key = old ? old_key(rel, old) : NULL;

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
	if (key && key != old)
		heap_freetuple(key);
}

/* ---------- the resource manager ---------- */

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

static const RmgrData tbl_rmgr = {
	.rm_name = "sift_table",
	.rm_redo = tbl_redo,
	.rm_desc = tbl_desc,
	.rm_identify = tbl_identify,
	.rm_decode = tbl_decode,
};

/* Called from _PG_init: only a preloading server can register the resource manager */
void
sift_tbl_init(void)
{
	sift_tbl_define_gucs();
	sift_tbl_slot_init();
	sift_tbl_src_register();
	if (!process_shared_preload_libraries_in_progress)
		return;
	RegisterCustomRmgr(SIFT_TBL_RMGR_ID, &tbl_rmgr);
	rmgr_registered = true;
}
