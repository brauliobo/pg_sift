/*
 * table/funcs_test.c - the test hooks of the table: the column segments of a table, the page records
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/table.h"
#include "bm25/maint.h"
#include "core/sql.h"
#include "table/seg_int.h"

/* The next column segment of the table from directory entry *i on, open (the caller closes it); NULL after the last */
static SiftColSegment *
next_column_segment(Relation rel, SiftTblRel *st, int *i)
{
	while (*i < st->dir.nsegs)
	{
		const SiftSegEntry *e = &st->dir.segs[(*i)++];

		if (e->kind == SIFT_SEG_IMMUTABLE)
			return sift_cseg_open(rel, e, CurrentMemoryContext);
	}
	return NULL;
}

PG_FUNCTION_INFO_V1(sift_test_tbl_bulk_refusal);

/* Why the bulk load of the table keeps the write buffers; NULL when it qualifies */
Datum
sift_test_tbl_bulk_refusal(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	const char *why;

	sift_tbl_require(rel);
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

	sift_tbl_require(rel);
	(void) sift_maint_lock(rel, true);
	table_close(rel, NoLock);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(sift_test_tbl_cube_damage);

/* sift_test_cube_damage (col/test/test.c) of the cube blob of the first units of the column segments of the table; with_calendar: blobs that hold a calendar dim */
Datum
sift_test_tbl_cube_damage(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftColSegment *seg;
	int			maxunits = PG_GETARG_INT32(1);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "cube damage", ALLOCSET_DEFAULT_SIZES);
	SiftTestDamage r = {0};
	Datum		v[8];
	bool		nulls[8] = {false};

	sift_tbl_require(rel);
	sift_tbl_refresh(rel, st);
	for (int i = 0; (seg = next_column_segment(rel, st, &i)) != NULL; sift_cseg_close(seg))
		for (uint32 u = 0; u < seg->nsb && r.blobs < maxunits; u++)
		{
			uint32		len;
			const uint8 *blob = sift_cseg_cube(seg, u, &len);

			if (blob != NULL)
				sift_test_cube_damage(seg->schema, NULL, blob, len, NULL, 0, cx, &r);
		}
	table_close(rel, AccessShareLock);
	v[0] = Int64GetDatum(r.blobs);
	v[1] = Int64GetDatum(r.intact);
	v[2] = Int64GetDatum(r.calendar);
	v[3] = Int64GetDatum(r.cut);
	v[4] = Int64GetDatum(r.cut_accepted);
	v[5] = Int64GetDatum(r.damaged);
	v[6] = Int64GetDatum(r.accepted);
	v[7] = Int64GetDatum(r.unsound);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

PG_FUNCTION_INFO_V1(sift_test_tbl_cal_grains);

/* The calendar dims of the cubes of the column segments of the table: how many are of cells of a day, a month and a year, and the cubes that hold none */
Datum
sift_test_tbl_cal_grains(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftColSegment *seg;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "cal grains", ALLOCSET_DEFAULT_SIZES);
	int64		n[4] = {0};
	Datum		v[4];
	bool		nulls[4] = {false};

	sift_tbl_require(rel);
	sift_tbl_refresh(rel, st);
	for (int i = 0; (seg = next_column_segment(rel, st, &i)) != NULL; sift_cseg_close(seg))
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			uint32		len;
			const uint8 *blob = sift_cseg_cube(seg, u, &len);
			SiftCHist	h;
			bool		any = false;

			MemoryContextReset(cx);
			if (blob == NULL || !sift_cpagg_decode(seg->schema, blob, len, cx, &h))
				continue;
			for (int d = 0; d < h.ndims; d++)
				if (h.dim[d].leaf >= SIFT_PAGG_CAL)
				{
					n[h.dim[d].leaf / SIFT_PAGG_CAL - 1]++;
					any = true;
				}
			n[3] += !any;
		}
	table_close(rel, AccessShareLock);
	for (int k = 0; k < 4; k++)
		v[k] = Int64GetDatum(n[k]);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

PG_FUNCTION_INFO_V1(sift_test_tbl_preagg_bytes);

/* The bytes of the pre-aggregates (sums, cubes and rollups) of the column segments of the table */
Datum
sift_test_tbl_preagg_bytes(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftColSegment *seg;
	int64		bytes = 0;

	sift_tbl_require(rel);
	sift_tbl_refresh(rel, st);
	for (int i = 0; (seg = next_column_segment(rel, st, &i)) != NULL; sift_cseg_close(seg))
		bytes += sift_cseg_preagg_bytes(seg);
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
	SiftColSegment *seg;
	int64		live = 0;

	sift_tbl_require(rel);
	sift_tbl_refresh(rel, st);
	for (int i = 0; (seg = next_column_segment(rel, st, &i)) != NULL; sift_cseg_close(seg))
		live += sift_cseg_verify(seg);
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

	sift_tbl_require(rel);
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

PG_FUNCTION_INFO_V1(sift_test_tbl_hot_probes);

/* The calls of sift_tbl_hot_offsets of this backend since the last call */
Datum
sift_test_tbl_hot_probes(PG_FUNCTION_ARGS)
{
	int64		n = (int64) sift_tbl_hot_probes;

	sift_tbl_hot_probes = 0;
	PG_RETURN_INT64(n);
}

PG_FUNCTION_INFO_V1(sift_test_tbl_page_rec);

Datum
sift_test_tbl_page_rec(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(page_rec_found((uint32) PG_GETARG_INT32(0), (uint32) PG_GETARG_INT32(1)));
}

PG_FUNCTION_INFO_V1(sift_test_tbl_ovf_piece);

/* The bytes a continuation page of an overflow row holds when it was given `piece` of them and these flags (a damaged page is an error) */
Datum
sift_test_tbl_ovf_piece(PG_FUNCTION_ARGS)
{
	Page		page = palloc0(BLCKSZ);
	uint32		piece;

	sift_page_init(page, SIFT_PT_WBUFDATA, 7);
	TBL_PAGE(page)->flags = (uint16) PG_GETARG_INT32(1);
	SiftPageSetUsed(page, (uint32) (TBL_PAGE_INIT + PG_GETARG_INT32(0)));
	piece = sift_tbl_ovf_piece(page, 7);
	pfree(page);
	PG_RETURN_INT32((int32) piece);
}

PG_FUNCTION_INFO_V1(sift_test_tbl_page_rec_end);

/* The same for a record of inline_len bytes whose last byte lies `over` bytes past the end of the page's data (0: the largest record a page can hold) */
Datum
sift_test_tbl_page_rec_end(PG_FUNCTION_ARGS)
{
	uint32		inline_len = (uint32) PG_GETARG_INT32(0);

	PG_RETURN_BOOL(page_rec_found(SIFT_PAGE_PAYLOAD - sizeof(SiftTblRec) - inline_len + PG_GETARG_INT32(1), inline_len));
}

#endif							/* SIFT_TESTING */
