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
#include "core/sql.h"
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

	sift_tbl_require(rel);
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
				v[8] = Int32GetDatum(e->format);
				nulls[8] = e->format == 0;
			}
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	UnregisterSnapshot(snap);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}

/* The space of a table as one row: the file, the blocks ever used, the free list (reusable: no snapshot can read them) and the virtual blocks of TIDs not handed out yet */
Datum
sift_table_space(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftFreeStats fs;
	Datum		v[6];
	bool		nulls[6] = {0};

	sift_tbl_require(rel);
	sift_tbl_refresh_full(rel, st);
	fs = sift_store_free_stats(rel);
	v[0] = Int64GetDatum(RelationGetNumberOfBlocks(rel));
	v[1] = Int64GetDatum(st->dir.meta.high_water);
	v[2] = Int64GetDatum(fs.blocks);
	v[3] = Int64GetDatum(fs.reusable);
	v[4] = Int64GetDatum(fs.records);
	v[5] = Int64GetDatum((int64) MaxBlockNumber - (int64) SIFT_TBL_NEXT_VBLOCK(&st->dir.meta));
	table_close(rel, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
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
	SiftTblMaintain r = {0};
	Datum		v[6];
	bool		nulls[6] = {0};

	PreventCommandIfReadOnly("sift.table_maintain()");
	PreventCommandDuringRecovery("sift.table_maintain()");
	if (ConditionalLockRelationOid(relid, ShareUpdateExclusiveLock))
	{
		Relation	rel = try_table_open(relid, NoLock);

		if (rel == NULL)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation with OID %u does not exist", relid)));
		sift_tbl_require(rel);
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
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}
