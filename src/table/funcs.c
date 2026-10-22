/*
 * table/funcs.c - SQL functions that show the segments and the space of a sift_table
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/skey.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/pg.h"
#include "col/seg.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/merge.h"
#include "core/ops.h"
#include "table/table.h"
#include "table/seg_int.h"

PG_FUNCTION_INFO_V1(sift_table_segments);
PG_FUNCTION_INFO_V1(sift_table_space);

/* One row per segment of the table: buffer (row format write buffers), sealed buffer (waiting for promotion), columns, tombstones */
Datum
sift_table_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	Snapshot	snap = RegisterSnapshot(GetTransactionSnapshot());

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	InitMaterializedSRF(fcinfo, 0);
	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		/* columns 9 to 12 (format, unfrozen, min_xid, max_xid) are there when the SQL function declares them (pg_sift--3.0--3.1.sql); more values than columns are ignored */
		Datum		v[12] = {0};
		bool		nulls[12] = {0};

		v[0] = Int64GetDatum(e->gen);
		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(rel, e, CurrentMemoryContext);

			v[1] = CStringGetTextDatum("columns");
			v[2] = Int64GetDatum(e->ndocs);
			v[3] = Int64GetDatum(e->bytes);
			v[4] = Int32GetDatum(e->sb_lo);
			v[5] = Int32GetDatum(e->sb_hi);
			v[6] = Int32GetDatum(seg->nsb);
			v[7] = Int64GetDatum(e->ndocs - sift_cseg_live_rows(seg));
			v[8] = Int32GetDatum(e->format);
			v[9] = BoolGetDatum(sift_tbl_seg_unfrozen(e));
			if (sift_tbl_seg_unfrozen(e))
			{
				uint32		lo,
							hi;

				sift_cseg_ver_range(seg, &lo, &hi);
				v[10] = TransactionIdGetDatum(lo);
				v[11] = TransactionIdGetDatum(hi);
			}
			nulls[10] = nulls[11] = !sift_tbl_seg_unfrozen(e);
			sift_cseg_close(seg);
		}
		else
		{
			SiftTblSeg seg = {.gen = e->gen,.hdr = e->header_blk};

			v[1] = CStringGetTextDatum(e->flags & SIFT_TBL_SEG_TOMB ? "tombstones" :
									   e->kind == SIFT_SEG_WBUF_OPEN ? "buffer" : "sealed buffer");
			memset(nulls + 2, true, 10);
			if (e->flags & SIFT_TBL_SEG_WBUF)
			{
				sift_tbl_seg_load(rel, &seg);
				v[3] = Int64GetDatum((int64) seg.npages * BLCKSZ);
				v[4] = Int32GetDatum(e->sb_lo);
				v[5] = Int32GetDatum(e->sb_hi);
				nulls[3] = nulls[4] = nulls[5] = false;
			}
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	UnregisterSnapshot(snap);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

#ifdef SIFT_TESTING
PG_FUNCTION_INFO_V1(sift_test_tbl_bulk_refusal);

/* Why the bulk load of the table keeps the write buffers; NULL when it qualifies */
Datum
sift_test_tbl_bulk_refusal(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	const char *why;

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	why = sift_tbl_bulk_refusal(rel);
	table_close(rel, AccessShareLock);
	if (why == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(why));
}

PG_FUNCTION_INFO_V1(sift_test_tbl_hold_maint);

/* Takes the maintenance lock of the table and keeps it to the end of the transaction, as a long VACUUM or merge does */
Datum
sift_test_tbl_hold_maint(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	(void) sift_maint_lock(rel, true);
	table_close(rel, NoLock);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(sift_test_tbl_cube_damage);

/* sift_test_cube_damage (col/test.c) of the cube blob of the first units of the column segments of the table; with_month: blobs that hold a month dim */
Datum
sift_test_tbl_cube_damage(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	int			maxunits = PG_GETARG_INT32(1);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "cube damage", ALLOCSET_DEFAULT_SIZES);
	SiftTestDamage r = {0};
	TupleDesc	tupdesc;
	Datum		v[8];
	bool		nulls[8] = {false};

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		SiftColSegment *seg;

		if (e->kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(rel, e, CurrentMemoryContext);
		for (uint32 u = 0; u < seg->nsb && r.blobs < maxunits; u++)
		{
			uint32		len;
			const uint8 *blob = sift_cseg_cube(seg, u, &len);

			if (blob != NULL)
				sift_test_cube_damage(seg->schema, NULL, blob, len, NULL, 0, cx, &r);
		}
		sift_cseg_close(seg);
	}
	table_close(rel, AccessShareLock);
	v[0] = Int64GetDatum(r.blobs);
	v[1] = Int64GetDatum(r.intact);
	v[2] = Int64GetDatum(r.month);
	v[3] = Int64GetDatum(r.cut);
	v[4] = Int64GetDatum(r.cut_accepted);
	v[5] = Int64GetDatum(r.damaged);
	v[6] = Int64GetDatum(r.accepted);
	v[7] = Int64GetDatum(r.unsound);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

PG_FUNCTION_INFO_V1(sift_test_tbl_preagg_bytes);

/* The bytes of the pre-aggregates (sums, cubes and rollups) of the column segments of the table */
Datum
sift_test_tbl_preagg_bytes(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	int64		bytes = 0;

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		SiftColSegment *seg;

		if (st->dir.segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(rel, &st->dir.segs[i], CurrentMemoryContext);
		bytes += sift_cseg_preagg_bytes(seg);
		sift_cseg_close(seg);
	}
	table_close(rel, AccessShareLock);
	PG_RETURN_INT64(bytes);
}

PG_FUNCTION_INFO_V1(sift_test_tbl_verify);

/* The checks of sift.col_verify on every column segment of the table (chunks, zones, the cube and rollups against the chunks); the live rows, an error on damage */
Datum
sift_test_tbl_verify(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	int64		live = 0;

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		SiftColSegment *seg;

		if (st->dir.segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(rel, &st->dir.segs[i], CurrentMemoryContext);
		live += sift_cseg_verify(seg);
		sift_cseg_close(seg);
	}
	table_close(rel, AccessShareLock);
	PG_RETURN_INT64(live);
}

PG_FUNCTION_INFO_V1(sift_test_tbl_tagged);

/* The column groups of the table with unfrozen rows (rows that are not final) */
Datum
sift_test_tbl_tagged(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	int32		n = 0;

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
		n += sift_tbl_seg_unfrozen(&st->dir.segs[i]);
	table_close(rel, AccessShareLock);
	PG_RETURN_INT32(n);
}

/* Does a write buffer page of one row, whose slot holds `slot` and whose record claims `inline_len` bytes, give out a record (a record outside the page is an error) */
static bool
page_rec_found(uint32 slot, uint32 inline_len)
{
	Page		page = palloc0(BLCKSZ);
	SiftTblPageHdr *ph;
	uint16		len16 = (uint16) inline_len;
	bool		found;

	sift_page_init(page, SIFT_PT_WBUFDATA, 7);
	ph = TBL_PAGE(page);
	ph->nrows = 1;
	ph->slot[0] = (uint16) slot;
	/* only the length field is written: a header that reaches past the page must not overwrite the opaque area behind it */
	if (slot > 0 && slot + offsetof(SiftTblRec, inline_len) + sizeof(len16) <= SIFT_PAGE_PAYLOAD)
		memcpy(SiftPageGetData(page) + slot + offsetof(SiftTblRec, inline_len), &len16, sizeof(len16));
	found = sift_tbl_page_rec(page, 7, 1) != NULL;
	pfree(page);
	return found;
}

PG_FUNCTION_INFO_V1(sift_test_tbl_page_rec);

Datum
sift_test_tbl_page_rec(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(page_rec_found((uint32) PG_GETARG_INT32(0), (uint32) PG_GETARG_INT32(1)));
}

PG_FUNCTION_INFO_V1(sift_test_tbl_page_rec_end);

/* The same for a record of inline_len bytes whose last byte lies `over` bytes past the end of the page's data (0: the largest record a page can hold) */
Datum
sift_test_tbl_page_rec_end(PG_FUNCTION_ARGS)
{
	uint32		inline_len = (uint32) PG_GETARG_INT32(0);

	PG_RETURN_BOOL(page_rec_found(SIFT_PAGE_PAYLOAD - sizeof(SiftTblRec) - inline_len + PG_GETARG_INT32(1), inline_len));
}
#endif

/* The space of a table as one row: the file, the blocks ever used, and the free list (reusable: no snapshot can read them) */
Datum
sift_table_space(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	TupleDesc	tupdesc;
	SiftFreeStats fs;
	Datum		v[5];
	bool		nulls[5] = {0};

	if (!sift_tbl_is_table(rel))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_tbl_refresh_full(rel, st);
	fs = sift_store_free_stats(rel);
	v[0] = Int64GetDatum(RelationGetNumberOfBlocks(rel));
	v[1] = Int64GetDatum(st->dir.meta.high_water);
	v[2] = Int64GetDatum(fs.blocks);
	v[3] = Int64GetDatum(fs.reusable);
	v[4] = Int64GetDatum(fs.records);
	table_close(rel, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

PG_FUNCTION_INFO_V1(sift_table_maintain);

/*
 * The work of VACUUM on the column segments and the file, without the indexes: the dead rows are cleared, the write buffers promoted, the
 * segments merged or rewritten, the free space gathered and returned. The table is taken as VACUUM takes it (ShareUpdateExclusiveLock) but
 * only if that is free now, as is the lock of the file's tail (docs/COL-TAM.md section 7); busy says that it was not.
 */
Datum
sift_table_maintain(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	SiftTblMaintain r = {0};
	Datum		v[6];
	bool		nulls[6] = {0};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	PreventCommandIfReadOnly("sift.table_maintain()");
	PreventCommandDuringRecovery("sift.table_maintain()");
	if (ConditionalLockRelationOid(relid, ShareUpdateExclusiveLock))
	{
		Relation	rel = try_table_open(relid, NoLock);

		if (rel == NULL)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation with OID %u does not exist", relid)));
		if (!sift_tbl_is_table(rel))
			ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_table", RelationGetRelationName(rel))));
		if (pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) != ACLCHECK_OK)
			aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(rel->rd_rel->relkind), RelationGetRelationName(rel));
		sift_tbl_maintain(rel, false, true, &r);
		table_close(rel, NoLock);
	}
	else
		r.busy = true;
	v[0] = BoolGetDatum(r.busy);
	v[1] = Int64GetDatum(r.dead_rows);
	v[2] = Int32GetDatum(r.promoted);
	v[3] = Int32GetDatum(r.merged);
	v[4] = Int64GetDatum(r.blocks_before);
	v[5] = Int64GetDatum(r.blocks_after);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}
