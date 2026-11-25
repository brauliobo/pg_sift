/*
 * col/f2/uniq/unique_test.c - the test function of the check of a UNIQUE sift_col index that is deferred (col/f2/uniq/unique.h), compiled only with SIFT_TESTING.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/index.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"

#include "col/f2/l0/stage.h"

PG_FUNCTION_INFO_V1(sift_test_unique_partial_insert);

/* sift._test_unique_partial_insert(index regclass, key int4): the first check of a deferred constraint for the key of a one column index, at a row address that is none; 'returned staged' */
Datum
sift_test_unique_partial_insert(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), RowExclusiveLock);
	Relation	heap = table_open(index->rd_index->indrelid, RowExclusiveLock);
	IndexInfo  *info = BuildIndexInfo(index);
	Datum		values[1] = {Int32GetDatum(PG_GETARG_INT32(1))};
	bool		isnull[1] = {false};
	ItemPointerData tid;
	bool		returned;
	text	   *result;

	ItemPointerSet(&tid, 1000, 1);
	returned = index_insert(index, values, isnull, &tid, heap, UNIQUE_CHECK_PARTIAL, false, info);
	result = cstring_to_text(psprintf("%s %lu", returned ? "true" : "false", (unsigned long) sift_stage_rows(index)));

	table_close(heap, NoLock);
	index_close(index, NoLock);
	PG_RETURN_TEXT_P(result);
}

#endif							/* SIFT_TESTING */
