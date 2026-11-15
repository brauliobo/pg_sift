/*
 * col/order_sort.c - the rows of a merge that do not fit maintenance_work_mem, sorted by a tuplesort
 * An input of a merge that is not cut the way the merge reads it is sorted again (col/order.c). Its rows are the keys of the order (an int8 each, NULL last) and the
 * TID, which a tuplesort sorts, and the row itself (col/rowpack.c): the sort holds work_kb of them and writes the rest to temporary files, as the sort of the executor
 * does, so the memory of the merge does not depend on the rows. The rows come back in the order the loaded-whole sort gives them (leaves, then TID).
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "utils/tuplesort.h"

#include "col/order_sort.h"
#include "col/seg.h"
#include "col/rowpack.h"
#include "col/runs.h"
#include "core/compat.h"

#ifdef SIFT_TESTING
int64		sift_test_cut_sort_rows = 0;
int64		sift_test_cut_sort_disk_kb = 0;
int64		sift_test_cut_sort_cut = 0;
#endif

struct SiftColSorter
{
	const SiftColSchema *schema;
	int			leaf[SIFT_COL_CUT_MAX];
	int			nleaf;
	Tuplesortstate *ts;
	TupleTableSlot *put;
	TupleTableSlot *get;
	SiftColRow	row;			/* the row in hand: put side from a batch, get side from the bytes */
	SiftColRow	in;
	StringInfoData buf;
	MemoryContext cx;
	MemoryContext rcx;			/* the arrays of elements of the row read */
	uint64		rows;
};

SiftColSorter *
sift_colsort_begin(const SiftColSchema *schema, const int *leaf, int nleaf, int work_kb)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col merge sort", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColSorter *s = palloc0(sizeof(SiftColSorter));
	TupleDesc	desc = CreateTemplateTupleDesc(nleaf + 2);
	AttrNumber	att[SIFT_COL_CUT_MAX + 1];
	Oid			ops[SIFT_COL_CUT_MAX + 1],
				coll[SIFT_COL_CUT_MAX + 1] = {InvalidOid};
	bool		nulls[SIFT_COL_CUT_MAX + 1] = {false};

	for (int i = 0; i <= nleaf; i++)
	{
		TupleDescInitEntry(desc, i + 1, "k", INT8OID, -1, 0);
		att[i] = i + 1;
		ops[i] = Int8LessOperator;
	}
	TupleDescInitEntry(desc, nleaf + 2, "row", BYTEAOID, -1, 0);
	sift_tupdesc_finalize(desc);
	s->schema = schema;
	s->nleaf = nleaf;
	memcpy(s->leaf, leaf, nleaf * sizeof(int));
	s->cx = cx;
	s->rcx = AllocSetContextCreate(cx, "sift_col merge sort row", ALLOCSET_SMALL_SIZES);
	s->put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	s->get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	s->ts = tuplesort_begin_heap(desc, nleaf + 1, att, ops, coll, nulls, work_kb, NULL, TUPLESORT_NONE);
	initStringInfo(&s->buf);
	s->in.val = palloc0(schema->ncols * sizeof(SiftColValue));
	s->in.elem = palloc0((schema->ncols + 1) * sizeof(SiftColValue *));
	MemoryContextSwitchTo(old);
	return s;
}

void
sift_colsort_put(SiftColSorter *s, const SiftColBatch *b, uint32 r)
{
	const SiftColSchema *sc = s->schema;
	uint32		ver[2] = {b->hasver ? b->xmin[r] : 0, b->hasver ? b->cmin[r] : 0};

	ExecClearTuple(s->put);
	for (int i = 0; i < s->nleaf; i++)
	{
		const SiftCVals *v = &b->col[s->leaf[i]];

		s->put->tts_isnull[i] = v->nulls != NULL && sift_col_bit_test(v->nulls, r);
		s->put->tts_values[i] = Int64GetDatum(s->put->tts_isnull[i] ? 0 : sift_crun_key(&sc->cols[s->leaf[i]], v->key[r]));
	}
	s->put->tts_isnull[s->nleaf] = false;
	s->put->tts_values[s->nleaf] = Int64GetDatum((int64) b->keys[r]);
	sift_cbatch_row(b, r, &s->row);
	resetStringInfo(&s->buf);
	appendStringInfoSpaces(&s->buf, VARHDRSZ);
	appendBinaryStringInfo(&s->buf, (char *) ver, sizeof(ver));
	sift_colrow_pack(&s->buf, sc, &s->row);
	SET_VARSIZE(s->buf.data, s->buf.len);
	s->put->tts_isnull[s->nleaf + 1] = false;
	s->put->tts_values[s->nleaf + 1] = PointerGetDatum(s->buf.data);
	ExecStoreVirtualTuple(s->put);
	tuplesort_puttupleslot(s->ts, s->put);
	s->rows++;
}

void
sift_colsort_done(SiftColSorter *s)
{
#ifdef SIFT_TESTING
	TuplesortInstrumentation stats;
#endif

	sift_cbatch_row_free(&s->row);
	tuplesort_performsort(s->ts);
#ifdef SIFT_TESTING
	tuplesort_get_stats(s->ts, &stats);
	sift_test_cut_sort_rows += s->rows;
	if (stats.spaceType == SORT_SPACE_TYPE_DISK)
		sift_test_cut_sort_disk_kb += stats.spaceUsed;
#endif
	elog(DEBUG1, "sift_col merge: %llu rows sorted", (unsigned long long) s->rows);
}

bool
sift_colsort_next(SiftColSorter *s, SiftColBatch *b)
{
	bool		isnull;
	bytea	   *p;
	const char *q;
	uint32		ver[2];
	MemoryContext old;

	if (!tuplesort_gettupleslot(s->ts, true, false, s->get, NULL))
		return false;
	p = DatumGetByteaPP(slot_getattr(s->get, s->nleaf + 2, &isnull));
	q = VARDATA_ANY(p);
	memcpy(ver, q, sizeof(ver));
	old = MemoryContextSwitchTo(s->rcx);
	(void) sift_colrow_unpack(q + sizeof(ver), s->schema, &s->in);
	MemoryContextSwitchTo(old);
	sift_cbatch_add_ver(b, &s->in, ver[0], ver[1]);
	MemoryContextReset(s->rcx);
	return true;
}

void
sift_colsort_end(SiftColSorter *s)
{
	tuplesort_end(s->ts);
	ExecDropSingleTupleTableSlot(s->put);
	ExecDropSingleTupleTableSlot(s->get);
	MemoryContextDelete(s->cx);
}
