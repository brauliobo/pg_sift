/*
 * test_gtopk - the heap of the best K groups (agg/group/topk.c): the by-reference values it copies stay within work_mem, past which the rows move to a
 * sort, and a row whose values are over 1 GB fails before a copy, never writing past the buffer of its slot.
 */
#include "postgres.h"

#include <limits.h>

#include "access/tupdesc.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"
#include "varatt.h"

#include "agg/group/topk.h"
#include "test/common/check.h"
#include "test/common/pgshim.h"

int			work_mem = 4096;

static const SiftV6TopKItem item = {.pos = 0};

Size
datumGetSize(Datum value, bool typByVal, int typLen)
{
	return typLen > 0 ? (Size) typLen : VARSIZE_ANY(DatumGetPointer(value));
}

static int
int_cmp(Datum a, Datum b, SortSupport ssup)
{
	return DatumGetInt64(a) < DatumGetInt64(b) ? -1 : DatumGetInt64(a) > DatumGetInt64(b);
}

void
PrepareSortSupportFromOrderingOp(Oid orderingOp, SortSupport ssup)
{
	ssup->comparator = int_cmp;
}

Tuplesortstate *
tuplesort_begin_heap(TupleDesc tupDesc, int nkeys, AttrNumber *attNums, Oid *sortOperators, Oid *sortCollations, bool *nullsFirstFlags, int workMem,
					 SortCoordinate coordinate, int sortopt)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("moved to a sort")));
}

/* an int8 sort column, then ncols - 1 text columns */
static TupleDesc
desc_of(int ncols)
{
	TupleDesc	d = calloc(1, 4096);

	d->natts = ncols;
	for (int c = 0; c < ncols; c++)
	{
		TupleDescAttr(d, c)->attlen = c == 0 ? 8 : -1;
		TupleDescAttr(d, c)->attbyval = c == 0;
	}
	return d;
}

static struct varlena *
text_of(Size bytes)
{
	struct varlena *v = malloc(bytes);

	memset(v, 'x', bytes);
	SET_VARSIZE(v, bytes);
	return v;
}

typedef struct Fill
{
	uint32		k;
	uint32		rows;
	Size		bytes;
	int			ncols;
	uint32		put;
	struct varlena *v;
	SiftGTopK  *t;
} Fill;

static void
fill(void *arg)
{
	Fill	   *f = arg;
	Datum		values[3] = {0, PointerGetDatum(f->v), PointerGetDatum(f->v)};
	bool		isnull[3] = {false, false, false};

	f->t = sift_gtopk_new(CurrentMemoryContext, f->k, desc_of(f->ncols), 1, &item);
	for (f->put = 0; f->put < f->rows; f->put++)
	{
		values[0] = Int64GetDatum(-(int64) f->put);
		sift_gtopk_put(f->t, values, isnull);
	}
}

static bool
run(Fill *f)
{
	bool		ok;

	f->v = text_of(f->bytes);
	ok = pgshim_try(fill, f);
	free(f->v);
	return ok;
}

static void
test_values_within_work_mem_are_held(void)
{
	Fill		f = {.k = 16,.rows = 64,.bytes = 1024,.ncols = 2};
	const Datum *values;
	const bool *isnull;

	SIFT_CHECK(run(&f));
	SIFT_CHECK(sift_gtopk_count(f.t) == 16);
	sift_gtopk_finish(f.t);
	for (int64 best = -63; sift_gtopk_next(f.t, &values, &isnull); best++)
		SIFT_CHECK(DatumGetInt64(values[0]) == best);
	pgshim_reset();
}

static void
test_values_past_work_mem_move_to_a_sort(void)
{
	Fill		f = {.k = 64,.rows = 64,.bytes = 128 * 1024,.ncols = 2};

	work_mem = 1024;
	SIFT_CHECK(!run(&f));
	SIFT_CHECK(f.put == 8 && strstr(pgshim_errmsg, "moved to a sort") != NULL);
	pgshim_reset();
	work_mem = 4096;
}

static void
test_row_over_1gb_fails_before_the_copy(void)
{
	Fill		f = {.k = 1,.rows = 1,.bytes = (Size) 600 << 20,.ncols = 3};

	work_mem = INT_MAX;
	SIFT_CHECK(!run(&f));
	SIFT_CHECK(pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED && strstr(pgshim_errmsg, "1 GB") != NULL);
	pgshim_reset();
	work_mem = 4096;
}

int
main(void)
{
	test_values_within_work_mem_are_held();
	test_values_past_work_mem_move_to_a_sort();
	test_row_over_1gb_fails_before_the_copy();
	puts("test_gtopk: ok");
	return 0;
}
