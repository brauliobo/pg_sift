/*
 * core/sql_parts.c - sift.parts(rel): the parts of a sift_col index or sift_table, one row each. A part of Format 2.0 is read from the manifest and the header page of the part (no data page); a segment
 * of Format 1 is listed as a part (level 0 a write buffer, level 1 an immutable segment), so that one query reads both formats. A sift (BM25) index has sift.segments.
 *
 * part_id is the partition of a part of Format 2.0 and NULL for Format 1, which has none; the buffer of a partition (its L0 slab) is a row with part_gen 0, as the buffer of Format 1 is.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"

#include "col/f2/col_meta.h"
#include "col/f2/delvec_delta.h"
#include "col/f2/part_format.h"
#include "col/v1/seg.h"
#include "col/v1/wbuf.h"
#include "core/sql_api.h"
#include "core/store.h"
#include "core/v1_col/tomb.h"
#include "table/v1/table_v1.h"

PG_FUNCTION_INFO_V1(sift_parts);

/* columns of sift.parts, in the order of sql/6.0/15-unified-api.sql */
enum PartCol
{
	PC_PART_ID,
	PC_PART_GEN,
	PC_LEVEL,
	PC_KIND,
	PC_ROWS,
	PC_DEAD_ROWS,
	PC_BYTES,
	PC_GRANULES,
	PC_EXTENTS,
	PC_HAS_DV,
	PC_UNFROZEN,
	PC_MIN_XID,
	PC_MAX_XID,
	PC_PREAGG_BYTES,
	PC_NCOLS
};

static const Oid part_types[PC_NCOLS] = {
	INT8OID, INT8OID, INT2OID, TEXTOID, INT8OID, INT8OID, INT8OID, INT4OID, INT4OID, BOOLOID, BOOLOID, XIDOID, XIDOID, INT8OID
};

/* A row under construction: every column is NULL until it is set */
typedef struct PartRow
{
	ReturnSetInfo *rsi;
	Datum		v[PC_NCOLS];
	bool		nulls[PC_NCOLS];
} PartRow;

static void
row_begin(PartRow *row, const char *kind)
{
	memset(row->v, 0, sizeof(row->v));
	memset(row->nulls, true, sizeof(row->nulls));
	row->nulls[PC_KIND] = false;
	row->v[PC_KIND] = CStringGetTextDatum(kind);
}

static void
row_set(PartRow *row, int col, Datum d)
{
	row->v[col] = d;
	row->nulls[col] = false;
}

static void
row_emit(PartRow *row)
{
	tuplestore_putvalues(row->rsi->setResult, row->rsi->setDesc, row->v, row->nulls);
}

/* the bytes of the sections that hold the pre-aggregates of a part of Format 2.0 */
static uint64
preagg_bytes(const SiftPart *part)
{
	uint64		n = 0;

	for (int i = 0; i < part->hdr.nsec; i++)
		switch (part->secdir[i].tag)
		{
			case SEC_PREAGG:
			case SEC_PREAGG_SUPER:
			case SEC_PREAGG_ROLLUP:
			case SEC_PREAGG_EXCEPT:
				n += part->secdir[i].len;
		}
	return n;
}

static void
parts_f2(PartRow *row, Relation rel)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		const SiftPartitionEntry *e = &mr.entries[i];
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, e, &np);

		for (int p = 0; p < np; p++)
		{
			SiftPart   *part = sift_part_open_desc(rel, &pd[p], (uint32) e->part_id);

			row_begin(row, "immutable");
			row_set(row, PC_PART_ID, Int64GetDatum((int64) e->part_id));
			row_set(row, PC_PART_GEN, Int64GetDatum((int64) pd[p].part_gen));
			row_set(row, PC_LEVEL, Int16GetDatum((int16) pd[p].level));
			row_set(row, PC_ROWS, Int64GetDatum((int64) pd[p].nrows));
			row_set(row, PC_DEAD_ROWS, Int64GetDatum((int64) sift_dv_part_ndead(rel, &pd[p])));
			row_set(row, PC_BYTES, Int64GetDatum((int64) pd[p].nbytes));
			row_set(row, PC_GRANULES, Int32GetDatum((int32) part->hdr.ngranules));
			row_set(row, PC_EXTENTS, Int32GetDatum((int32) part->hdr.total_extents));
			row_set(row, PC_HAS_DV, BoolGetDatum(pd[p].dv_blk != InvalidBlockNumber));
			row_set(row, PC_UNFROZEN, BoolGetDatum((pd[p].flags & SIFT_PART_FLAG_UNFROZEN) != 0));
			row_set(row, PC_MIN_XID, TransactionIdGetDatum(XidFromFullTransactionId(pd[p].min_xid)));
			row_set(row, PC_MAX_XID, TransactionIdGetDatum(XidFromFullTransactionId(pd[p].max_xid)));
			row_set(row, PC_PREAGG_BYTES, Int64GetDatum((int64) preagg_bytes(part)));
			row_emit(row);
		}
		if (e->l0_slab_blk != InvalidBlockNumber)
		{
			row_begin(row, "buffer");
			row_set(row, PC_PART_ID, Int64GetDatum((int64) e->part_id));
			row_set(row, PC_PART_GEN, Int64GetDatum(0));
			row_set(row, PC_LEVEL, Int16GetDatum(0));
			row_emit(row);
		}
	}
}

/* The columns that every immutable segment of Format 1 has, read from the segment: sift_col index and sift_table alike */
static void
segment_v1(PartRow *row, Relation rel, const SiftSegEntry *e, SiftColSegment *seg)
{
	uint64		dead = e->ndocs - sift_cseg_live_rows(seg);

	row_set(row, PC_PART_GEN, Int64GetDatum((int64) e->gen));
	row_set(row, PC_LEVEL, Int16GetDatum(1));
	row_set(row, PC_ROWS, Int64GetDatum((int64) e->ndocs));
	row_set(row, PC_DEAD_ROWS, Int64GetDatum((int64) dead));
	row_set(row, PC_BYTES, Int64GetDatum((int64) e->bytes));
	row_set(row, PC_GRANULES, Int32GetDatum((int32) seg->nsb));
	row_set(row, PC_EXTENTS, Int32GetDatum((int32) seg->hdr->nextents));
	row_set(row, PC_HAS_DV, BoolGetDatum(dead > 0));
	row_set(row, PC_PREAGG_BYTES, Int64GetDatum((int64) sift_cseg_preagg_bytes(seg)));
}

static void
parts_col_v1(PartRow *row, Relation rel)
{
	SiftDirSnapshot *snap = sift_dir_read(rel);

	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];

		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(rel, e, CurrentMemoryContext);

			row_begin(row, "immutable");
			segment_v1(row, rel, e, seg);
			sift_cseg_close(seg);
		}
		else if (e->kind == SIFT_SEG_TOMB)
		{
			row_begin(row, "tombstones");
			row_set(row, PC_PART_GEN, Int64GetDatum((int64) e->gen));
			row_set(row, PC_ROWS, Int64GetDatum((int64) sift_tomb_entries(rel, snap)));
		}
		else
		{
			uint64		nrows,
						npages;

			sift_cwbuf_summary(rel, e, &nrows, &npages);
			row_begin(row, e->kind == SIFT_SEG_WBUF_OPEN ? "buffer" : "sealed buffer");
			row_set(row, PC_PART_GEN, Int64GetDatum((int64) e->gen));
			row_set(row, PC_LEVEL, Int16GetDatum(0));
			row_set(row, PC_ROWS, Int64GetDatum((int64) nrows));
			row_set(row, PC_BYTES, Int64GetDatum((int64) npages * BLCKSZ));
		}
		row_emit(row);
	}
}

static void
parts_table_v1(PartRow *row, Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir->nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir->segs[i];

		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(rel, e, CurrentMemoryContext);
			bool		unfrozen = sift_tbl_seg_unfrozen(e);

			row_begin(row, "immutable");
			segment_v1(row, rel, e, seg);
			row_set(row, PC_UNFROZEN, BoolGetDatum(unfrozen));
			if (unfrozen)
			{
				uint32		lo,
							hi;

				sift_cseg_ver_range(seg, &lo, &hi);
				row_set(row, PC_MIN_XID, TransactionIdGetDatum(lo));
				row_set(row, PC_MAX_XID, TransactionIdGetDatum(hi));
			}
			sift_cseg_close(seg);
		}
		else
		{
			row_begin(row, e->flags & SIFT_TBL_SEG_TOMB ? "tombstones" : e->kind == SIFT_SEG_WBUF_OPEN ? "buffer" : "sealed buffer");
			row_set(row, PC_PART_GEN, Int64GetDatum((int64) e->gen));
			if (!(e->flags & SIFT_TBL_SEG_TOMB))
				row_set(row, PC_LEVEL, Int16GetDatum(0));
			if (e->flags & SIFT_TBL_SEG_WBUF)
			{
				SiftTblSeg	seg = {.gen = e->gen,.hdr = e->header_blk};

				sift_tbl_seg_load(rel, &seg);
				row_set(row, PC_BYTES, Int64GetDatum((int64) seg.npages * BLCKSZ));
			}
		}
		row_emit(row);
	}
}

Datum
sift_parts(PG_FUNCTION_ARGS)
{
	PartRow		row = {.rsi = (ReturnSetInfo *) fcinfo->resultinfo};
	SiftApiRel	r;
	Snapshot	snap;

	InitMaterializedSRF(fcinfo, 0);
	sift_api_check_desc(row.rsi->setDesc, part_types, PC_NCOLS);
	r = sift_api_open(PG_GETARG_OID(0), AccessShareLock);
	if (r.am == SIFT_API_BM25)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is a sift index, which has segments, not parts", RelationGetRelationName(r.rel)), errhint("Use sift.segments.")));
	snap = RegisterSnapshot(GetTransactionSnapshot());
	if (r.f2)
		parts_f2(&row, r.rel);
	else if (r.peek.status == SIFT_FMT_V1 && r.am == SIFT_API_TABLE)
		parts_table_v1(&row, r.rel);
	else if (r.peek.status == SIFT_FMT_V1)
		parts_col_v1(&row, r.rel);
	UnregisterSnapshot(snap);
	sift_api_close(&r, AccessShareLock);
	return (Datum) 0;
}
