/*
 * col/vstore/vstore_test.c - the test functions of the shared value store (sift._test_vstore_*): the append, the view, the runs
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/table.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "col/vstore/vstore.h"
#include "core/store.h"

PG_FUNCTION_INFO_V1(sift_test_vstore_intern);
PG_FUNCTION_INFO_V1(sift_test_vstore_info);
PG_FUNCTION_INFO_V1(sift_test_vstore_value);
PG_FUNCTION_INFO_V1(sift_test_vstore_forget);

/* sift._test_vstore_intern(store, vals text[]): the code of each value (a new one is appended; all the new ones of the call are one run) */
Datum
sift_test_vstore_intern(PG_FUNCTION_ARGS)
{
	Oid			store = PG_GETARG_OID(0);
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(1);
	Datum	   *vals;
	bool	   *nulls;
	int			n;
	SiftVStore *vs = sift_vstore_open(store, RowExclusiveLock);
	SiftCDict  *batch = sift_cdict_create(CurrentMemoryContext);
	uint32	   *codes,
			   *map;
	Datum	   *out;
	SiftCcPolicy pol;

	deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT, &vals, &nulls, &n);
	codes = palloc(Max(n, 1) * sizeof(uint32));
	for (int i = 0; i < n; i++)
	{
		text	   *t = DatumGetTextPP(vals[i]);

		codes[i] = sift_cdict_intern(batch, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	}
	map = palloc(Max(batch->n, 1) * sizeof(uint32));
	sift_cc_policy_default(&pol);
	(void) sift_vstore_intern(vs, batch, &pol, map);
	out = palloc(Max(n, 1) * sizeof(Datum));
	for (int i = 0; i < n; i++)
		out[i] = Int64GetDatum(map[codes[i]]);
	sift_vstore_close(vs);
	PG_RETURN_ARRAYTYPE_P(construct_array(out, n, INT8OID, 8, FLOAT8PASSBYVAL, TYPALIGN_DOUBLE));
}

/* sift._test_vstore_info(store): the runs of the newest epoch and the values the view of this backend holds */
Datum
sift_test_vstore_info(PG_FUNCTION_ARGS)
{
	Oid			store = PG_GETARG_OID(0);
	SiftVStore *vs = sift_vstore_open(store, AccessShareLock);
	Relation	rel = table_open(store, NoLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TupleDesc	td;
	Datum		v[4];
	bool		nl[4] = {false, false, false, false};
	int			runs = 0;

	if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	v[2] = Int64GetDatum(sift_vstore_epoch(vs));
	v[3] = Int64GetDatum(sift_vstore_view(vs)->n);
	sift_dir_read(rel, snap);
	for (int i = 0; i < snap->nsegs; i++)
		runs += snap->segs[i].kind == SIFT_SEG_VALRUN && snap->segs[i].reserved == DatumGetInt64(v[2]);
	v[0] = Int32GetDatum(runs);
	v[1] = Int64GetDatum(RelationGetNumberOfBlocks(rel));
	table_close(rel, NoLock);
	sift_vstore_close(vs);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(td), v, nl)));
}

/* sift._test_vstore_value(store, code) */
Datum
sift_test_vstore_value(PG_FUNCTION_ARGS)
{
	SiftVStore *vs = sift_vstore_open(PG_GETARG_OID(0), AccessShareLock);
	SiftCDict  *view = sift_vstore_view(vs);
	int64		code = PG_GETARG_INT64(1);
	uint32		len;
	const char *s;
	text	   *res;

	if (code < 0 || code >= view->n)
	{
		sift_vstore_close(vs);
		PG_RETURN_NULL();
	}
	s = sift_cdict_str(view, (uint32) code, &len);
	res = cstring_to_text_with_len(s, len);
	sift_vstore_close(vs);
	PG_RETURN_TEXT_P(res);
}

/* sift._test_vstore_forget(): the backend forgets the values it read, so that the next call reads the runs again */
Datum
sift_test_vstore_forget(PG_FUNCTION_ARGS)
{
	sift_vstore_forget_views();
	PG_RETURN_VOID();
}

#endif
