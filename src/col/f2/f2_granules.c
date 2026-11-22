/*
 * col/f2/f2_granules.c - sift.col_granules(rel): the 8,192-row granules of a sift_col index or sift_table of Format 2.0, one row each, from the granule directory of each part and its deletion vector.
 * No row of Format 1, which has units, not granules (sift.parts lists its segments). The file is not f2_inspect.c, which holds sift_index_size_breakdown.
 *
 * has_tstride says whether the part keeps the time strides (SEC_GRANULE_TSTRIDE) its granules are read through: the section is one for the part, written for every granule of it.
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "storage/lmgr.h"

#include "col/f2/col_meta.h"
#include "col/f2/delvec.h"
#include "col/f2/part_format.h"
#include "col/granule/granule.h"
#include "core/sql_api.h"

PG_FUNCTION_INFO_V1(sift_col_granules);

/* columns of sift.col_granules, in the order of sql/6.0/15-unified-api.sql */
enum GranuleCol
{
	GC_PART_ID,
	GC_PART_GEN,
	GC_GRANULE,
	GC_NROWS,
	GC_DEAD_ROWS,
	GC_BYTES,
	GC_ALL_FROZEN,
	GC_HAS_PREAGG,
	GC_HAS_TSTRIDE,
	GC_MIN_XID,
	GC_MAX_XID,
	GC_NCOLS
};

static const Oid granule_types[GC_NCOLS] = {
	INT8OID, INT8OID, INT4OID, INT4OID, INT4OID, INT4OID, BOOLOID, BOOLOID, BOOLOID, XIDOID, XIDOID
};

/* the dead rows of each granule of a part: its deletion vector, none without one */
static int
dead_in(const SiftDv *dv, uint32 granule)
{
	const SiftDvGran *g = dv == NULL ? NULL : sift_dv_granule(dv, granule);

	return g == NULL ? 0 : g->ndead;
}

static void
part_granules(ReturnSetInfo *rsi, Relation rel, uint64 part_id, const SiftPartDesc *d)
{
	SiftPart   *part = sift_part_open_desc(rel, d, (uint32) part_id);
	SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
	bool		tstride = sift_part_section(part, SEC_GRANULE_TSTRIDE) != NULL;
	SiftDv	   *dv = NULL;

	if (d->dv_blk != InvalidBlockNumber)
	{
		uint16	   *rows = palloc(Max(part->hdr.ngranules, 1) * sizeof(uint16));

		for (uint32 g = 0; g < part->hdr.ngranules; g++)
			rows[g] = dir[g].nrows;
		dv = sift_dv_read(rel, d, (uint32) part_id, rows);
	}
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		Datum		v[GC_NCOLS] = {0};
		bool		nulls[GC_NCOLS] = {false};

		v[GC_PART_ID] = Int64GetDatum((int64) part_id);
		v[GC_PART_GEN] = Int64GetDatum((int64) d->part_gen);
		v[GC_GRANULE] = Int32GetDatum((int32) g);
		v[GC_NROWS] = Int32GetDatum((int32) dir[g].nrows);
		v[GC_DEAD_ROWS] = Int32GetDatum(dead_in(dv, g));
		v[GC_BYTES] = Int32GetDatum((int32) dir[g].data_len);
		v[GC_ALL_FROZEN] = BoolGetDatum((dir[g].flags & SIFT_GDESC_ALL_FROZEN) != 0);
		v[GC_HAS_PREAGG] = BoolGetDatum((dir[g].flags & SIFT_GDESC_HAS_PREAGG) != 0);
		v[GC_HAS_TSTRIDE] = BoolGetDatum(tstride);
		v[GC_MIN_XID] = TransactionIdGetDatum(XidFromFullTransactionId(dir[g].min_xid));
		v[GC_MAX_XID] = TransactionIdGetDatum(XidFromFullTransactionId(dir[g].max_xid));
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
}

Datum
sift_col_granules(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftApiRel	r;

	InitMaterializedSRF(fcinfo, 0);
	sift_api_check_desc(rsi->setDesc, granule_types, GC_NCOLS);
	r = sift_api_open(PG_GETARG_OID(0), AccessShareLock);
	if (r.am == SIFT_API_BM25)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is a sift index, which has no granules", RelationGetRelationName(r.rel)), errhint("Use sift.segments.")));
	if (r.f2)
	{
		SiftManifestRead mr = sift_manifest_read_range(r.rel, NULL, NULL);

		for (int i = 0; i < mr.n; i++)
		{
			int			np;
			SiftPartDesc *pd = sift_manifest_read_parts(r.rel, &mr.entries[i], &np);

			for (int p = 0; p < np; p++)
				part_granules(rsi, r.rel, mr.entries[i].part_id, &pd[p]);
		}
	}
	sift_api_close(&r, AccessShareLock);
	return (Datum) 0;
}
