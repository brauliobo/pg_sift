/*
 * test_gtopk - the heap of the best K groups (agg/group/topk.c): the by-reference values it copies take work_mem or the statement fails naming it,
 * and a row whose values are over 1 GB fails before a copy, never writing past the buffer of its slot.
 */
#include "postgres.h"

#include <limits.h>

#include "utils/datum.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "agg/group/topk.h"
#include "test/common/check.h"
#include "test/common/pgshim.h"

int			work_mem = 4096;

static const int item_pos[1] = {0};
static const int16 typlen[2] = {8, -1};
static const bool typbyval[2] = {true, false};

Size
datumGetSize(Datum value, bool typByVal, int typLen)
{
	return typLen > 0 ? (Size) typLen : VARSIZE_ANY(DatumGetPointer(value));
}

static int
int_cmp(int item, Datum a, bool a_null, Datum b, bool b_null, void *arg)
{
	return DatumGetInt64(a) < DatumGetInt64(b) ? -1 : DatumGetInt64(a) > DatumGetInt64(b);
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
	struct varlena *v;
	SiftGTopK  *t;
} Fill;

static void
fill(void *arg)
{
	Fill	   *f = arg;
	Datum		values[3] = {0, PointerGetDatum(f->v), PointerGetDatum(f->v)};
	bool		isnull[3] = {false, false, false};
	int16		len[3] = {8, -1, -1};
	bool		byval[3] = {true, false, false};

	f->t = sift_gtopk_new(CurrentMemoryContext, f->k, 1, item_pos, f->ncols, f->ncols == 2 ? typlen : len, f->ncols == 2 ? typbyval : byval, int_cmp, NULL);
	for (uint32 i = 0; i < f->rows; i++)
	{
		values[0] = Int64GetDatum(-(int64) i);
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

	SIFT_CHECK(run(&f));
	SIFT_CHECK(sift_gtopk_count(f.t) == 16);
	sift_gtopk_free(f.t);
	pgshim_reset();
}

static void
test_values_over_work_mem_fail_naming_it(void)
{
	Fill		f = {.k = 64,.rows = 64,.bytes = 128 * 1024,.ncols = 2};

	work_mem = 1024;
	SIFT_CHECK(!run(&f));
	SIFT_CHECK(pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED && strstr(pgshim_errmsg, "work_mem") != NULL);
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
	test_values_over_work_mem_fail_naming_it();
	test_row_over_1gb_fails_before_the_copy();
	puts("test_gtopk: ok");
	return 0;
}
