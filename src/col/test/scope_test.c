/*
 * col/test/scope_test.c - test hooks for the fill of a scope index (col/scope_fill.c)
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "access/table.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "access/relation.h"
#include "utils/builtins.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/scope.h"
#include "col/test/test_int.h"

PG_FUNCTION_INFO_V1(sift_test_scope_expand);
PG_FUNCTION_INFO_V1(sift_test_col_scope_entries);

/*
 * sift._test_scope_expand(rel, btree, member, key, fks): the scope entries the fill gives for the member keys of fks (NULLs kept), one row (ordinal, key) per entry; the
 * ordinal is the position in fks, from 0. The way it knows the pairs (read once or probed) is the one sift.test_conn_fill names.
 */
Datum
sift_test_scope_expand(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Oid			btree = PG_GETARG_OID(1);
	AttrNumber	member = get_attnum(RelationGetRelid(rel), text_to_cstring(PG_GETARG_TEXT_PP(2))),
				key = get_attnum(RelationGetRelid(rel), text_to_cstring(PG_GETARG_TEXT_PP(3)));
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(4);
	int			n = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	Datum	   *vals;
	bool	   *isnull;
	int64	   *fk = palloc(Max(n, 1) * sizeof(int64));
	uint64	   *nulls = palloc0((n / 64 + 1) * sizeof(uint64));
	uint32	   *row = NULL;
	int64	   *k = NULL;
	uint32		cap = 0,
				entries;
	struct SiftScopeFill *f;

	InitMaterializedSRF(fcinfo, 0);
	deconstruct_array(arr, INT8OID, 8, true, TYPALIGN_DOUBLE, &vals, &isnull, &n);
	for (int i = 0; i < n; i++)
	{
		fk[i] = isnull[i] ? 0 : DatumGetInt64(vals[i]);
		if (isnull[i])
			nulls[i / 64] |= (uint64) 1 << (i % 64);
	}
	f = sift_scope_fill_begin_rel(CurrentMemoryContext, rel, btree, member, key, n);
	entries = sift_scope_fill_expand(f, 0, fk, nulls, n, &row, &k, &cap);
	for (uint32 i = 0; i < entries; i++)
	{
		Datum		v[2] = {Int32GetDatum((int32) row[i]), Int64GetDatum(k[i])};
		bool		nl[2] = {false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nl);
	}
	sift_scope_fill_end(f);
	table_close(rel, NoLock);
	return (Datum) 0;
}

/*
 * sift._test_col_scope_entries(index): every live entry of the scope segments, (generation, unit, address of the fact row, scope leaf, scope key) in the order the units are
 * stored; the scope leaf is the ordinal of its derived key among those of the schema
 */
Datum
sift_test_col_scope_entries(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	SiftColBatch *batch = sift_cbatch_create(CurrentMemoryContext, sift_col_rel(index)->schema);

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftColSegment *seg;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE || !(dir->segs[i].flags & SIFT_SEG_SCOPE))
			continue;
		seg = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			sift_cseg_decode_unit(seg, u, batch);
			for (uint32 r = 0; r < batch->nrows; r++)
				for (int c = 0, ord = 0; c < seg->schema->ncols; c++)
				{
					ItemPointerData tid;
					Datum		v[5];
					bool		nulls[5] = {false};

					if (!(seg->schema->cols[c].flags & SIFT_COLD_SCOPE))
						continue;
					if (!sift_col_bit_test(batch->col[c].nulls, r))
					{
						sift_key_to_tid(batch->keys[r], &tid);
						v[0] = Int64GetDatum(dir->segs[i].gen);
						v[1] = Int32GetDatum((int32) u);
						v[2] = ItemPointerGetDatum(&tid);
						v[3] = Int32GetDatum(ord);
						v[4] = Int64GetDatum(batch->col[c].key[r]);
						tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
					}
					ord++;
				}
		}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
#endif
