/*
 * col/v1/order/sort.c - the rows of a merge that do not fit maintenance_work_mem, sorted by a tuplesort
 * An input of a merge that is not cut the way the merge reads it is sorted again (col/v1/order/order.c). Its rows are the keys of the order (an int8 each, NULL last) and the
 * TID, which a tuplesort sorts, and the row itself (col/v1/rowpack.c): the sort holds work_kb of them and writes the rest to temporary files, as the sort of the executor
 * does, so the memory of the merge does not depend on the rows. The rows come back in the order the loaded-whole sort gives them (leaves, then TID).
 *
 * A sort of more rows than half its memory holds can instead go by key ranges (sift_colsort_sample_end): the rows are packed into a file for each range of the first leaf, with the
 * packing and the compression of a build by ranges (col/v1/rowfile.c), and a range is sorted by a tuplesort of its own when its turn comes. The files take what the compressed rows
 * take, once, where a tuplesort's runs take every row uncompressed and then again for each merge pass; and the ranges do not overlap in the first leaf, as the sorted rows do not.
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "utils/tuplesort.h"

#include "col/v1/order/sort.h"
#include "col/v1/seg/seg.h"
#include "col/v1/build.h"
#include "col/v1/rowfile.h"
#include "col/v1/rowpack.h"
#include "col/v1/runs.h"
#include "core/cmp.h"
#include "core/compat.h"

#ifdef SIFT_TESTING
int64		sift_test_cut_sort_rows = 0;
int64		sift_test_cut_sort_disk_kb = 0;
int64		sift_test_cut_sort_cut = 0;
#endif

#define RANGE_SAMPLE_KEYS	65536	/* the keys of the sample the ranges are cut at */
#define RANGE_SLACK			2		/* a sort holds no more than the fraction 1 / RANGE_SLACK of its memory in estimated bytes: a tuplesort needs more than the bytes of its rows (the array of entries grows by doubling) and the quantiles of a sample are not exact */
#define RANGE_MAX			512
#define TUPLE_OVERHEAD		64		/* the bytes of a row in a tuplesort besides the packed row: the minimal tuple, its keys and the datum header; it holds the row in a chunk of the next power of 2 and an entry of 24 bytes */
#define SORT_ENTRY			24

typedef struct SortRanges
{
	int			n;
	int64	   *bound;			/* [n - 1]: range i holds the first keys up to bound[i] (the last the rest and the NULLs) */
	SiftRowFile **file;
	uint64	   *rows;
	int			next;			/* the range the sort reads next */
	StringInfoData scratch;
} SortRanges;

struct SiftColSorter
{
	const SiftColSchema *schema;
	int			leaf[SIFT_COL_CUT_MAX];
	int			nleaf;
	int			work_kb;
	AttrNumber	att[SIFT_COL_CUT_MAX + 1];
	TupleDesc	desc;
	Tuplesortstate *ts;
	SortRanges *ranges;			/* NULL: one tuplesort holds the rows */
	int64	   *sample;			/* the first-leaf keys of the sample of sift_colsort_sample_put_scoped, and the bytes of its packed rows */
	uint32		nsample;
	uint64		sampled,
				sample_bytes;
	TupleTableSlot *put;
	TupleTableSlot *get;
	SiftColRow	row;			/* the row in hand: put side from a batch, get side from the bytes */
	SiftColRow	in;
	StringInfoData buf;
	MemoryContext cx;
	MemoryContext rcx;			/* the arrays of elements of the row read */
	uint64		rows;
};

static void
ts_begin(SiftColSorter *s)
{
	MemoryContext old = MemoryContextSwitchTo(s->cx);
	Oid			ops[SIFT_COL_CUT_MAX + 1],
				coll[SIFT_COL_CUT_MAX + 1] = {InvalidOid};
	bool		nulls[SIFT_COL_CUT_MAX + 1] = {false};

	for (int i = 0; i <= s->nleaf; i++)
		ops[i] = Int8LessOperator;
	s->ts = tuplesort_begin_heap(s->desc, s->nleaf + 1, s->att, ops, coll, nulls, s->work_kb, NULL, TUPLESORT_NONE);
	MemoryContextSwitchTo(old);
}

SiftColSorter *
sift_colsort_begin(const SiftColSchema *schema, const int *leaf, int nleaf, int work_kb)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift_col merge sort");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColSorter *s = palloc0(sizeof(SiftColSorter));
	TupleDesc	desc = CreateTemplateTupleDesc(nleaf + 2);

	for (int i = 0; i <= nleaf; i++)
	{
		TupleDescInitEntry(desc, i + 1, "k", INT8OID, -1, 0);
		s->att[i] = i + 1;
	}
	TupleDescInitEntry(desc, nleaf + 2, "row", BYTEAOID, -1, 0);
	sift_tupdesc_finalize(desc);
	s->schema = schema;
	s->nleaf = nleaf;
	s->work_kb = work_kb;
	s->desc = desc;
	memcpy(s->leaf, leaf, nleaf * sizeof(int));
	s->cx = cx;
	s->rcx = AllocSetContextCreate(cx, "sift_col merge sort row", ALLOCSET_SMALL_SIZES);
	s->put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	s->get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	ts_begin(s);
	initStringInfo(&s->buf);
	s->in.val = palloc0(schema->ncols * sizeof(SiftColValue));
	s->in.elem = palloc0((schema->ncols + 1) * sizeof(SiftColValue *));
	MemoryContextSwitchTo(old);
	return s;
}

/* The sort keys of a row: its leaves of the cut (NULL last), then the address */
typedef struct SortKeys
{
	int64		key[SIFT_COL_CUT_MAX + 1];
	bool		isnull[SIFT_COL_CUT_MAX + 1];
	uint32		ver[2];
} SortKeys;

/* Row r of b, or its copy for a scope key (scope_leaf >= 0: that leaf holds key in the copy, an integer leaf; kept: the leaves that are NULL in it when not set, as sift_cbatch_add_scoped): s->row, and its keys */
static void
build_row(SiftColSorter *s, const SiftColBatch *b, uint32 r, int scope_leaf, int64 key, const bool *kept, SortKeys *k)
{
	const SiftColSchema *sc = s->schema;

	k->ver[0] = b->hasver ? b->xmin[r] : 0;
	k->ver[1] = b->hasver ? b->cmin[r] : 0;
	for (int i = 0; i < s->nleaf; i++)
	{
		int			leaf = s->leaf[i];
		const SiftCVals *v = &b->col[leaf];

		k->isnull[i] = leaf == scope_leaf ? false : (kept != NULL && !kept[leaf]) || (v->nulls != NULL && sift_col_bit_test(v->nulls, r));
		k->key[i] = k->isnull[i] ? 0 : sift_crun_key(&sc->cols[leaf], leaf == scope_leaf ? key : v->key[r]);
	}
	k->isnull[s->nleaf] = false;
	k->key[s->nleaf] = (int64) b->keys[r];
	sift_cbatch_row(b, r, &s->row);
	for (int id = 0; scope_leaf >= 0 && id < sc->ncols; id++)
	{
		if (sc->cols[id].parent >= 0)
			continue;
		if (id == scope_leaf)
		{
			memset(&s->row.val[id], 0, sizeof(SiftColValue));
			s->row.val[id].tag = SIFT_COLTAG_UNKNOWN;
			s->row.val[id].v.i = key;
		}
		else if (kept != NULL && !kept[id] && sc->cols[id].kind != SIFT_COLK_GROUP && sc->cols[id].store != SIFT_COLS_VAR)
		{
			memset(&s->row.val[id], 0, sizeof(SiftColValue));
			s->row.val[id].tag = SIFT_COLTAG_UNKNOWN;
			s->row.val[id].isnull = true;
		}
	}
}

/* The keys of a row read back from a file of ranges: the cut leaves of a range sort are integers */
static void
read_keys(const SiftColSorter *s, const SiftColRow *row, SortKeys *k)
{
	for (int i = 0; i < s->nleaf; i++)
	{
		const SiftColValue *v = &row->val[s->leaf[i]];

		k->isnull[i] = v->isnull || v->overflow;
		k->key[i] = k->isnull[i] ? 0 : v->v.i;
	}
	k->isnull[s->nleaf] = false;
	k->key[s->nleaf] = (int64) sift_key_from_tid((ItemPointer) &row->tid);
}

static void
pack_row(SiftColSorter *s, const SiftColRow *row, const SortKeys *k)
{
	resetStringInfo(&s->buf);
	appendStringInfoSpaces(&s->buf, VARHDRSZ);
	appendBinaryStringInfo(&s->buf, (const char *) k->ver, sizeof(k->ver));
	sift_colrow_pack(&s->buf, s->schema, row);
	SET_VARSIZE(s->buf.data, s->buf.len);
}

static void
put_tuple(SiftColSorter *s, const SiftColRow *row, const SortKeys *k)
{
	ExecClearTuple(s->put);
	for (int i = 0; i <= s->nleaf; i++)
	{
		s->put->tts_isnull[i] = k->isnull[i];
		s->put->tts_values[i] = Int64GetDatum(k->key[i]);
	}
	pack_row(s, row, k);
	s->put->tts_isnull[s->nleaf + 1] = false;
	s->put->tts_values[s->nleaf + 1] = PointerGetDatum(s->buf.data);
	ExecStoreVirtualTuple(s->put);
	tuplesort_puttupleslot(s->ts, s->put);
}

static int
range_of(const SortRanges *rg, const SortKeys *k)
{
	int			lo = 0,
				hi = rg->n - 1;

	if (k->isnull[0])
		return hi;
	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (k->key[0] <= rg->bound[mid])
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

static void
put_row(SiftColSorter *s, const SiftColBatch *b, uint32 r, int scope_leaf, int64 key, const bool *kept)
{
	SortKeys	k;

	build_row(s, b, r, scope_leaf, key, kept, &k);
	if (s->ranges == NULL)
		put_tuple(s, &s->row, &k);
	else
	{
		int			i = range_of(s->ranges, &k);

		sift_rowfile_put(s->ranges->file[i], &s->row, k.ver[0], k.ver[1]);
		s->ranges->rows[i]++;
	}
	sift_cbatch_row_free(&s->row);	/* its arrays are in the context of the batch, which may be reset before the next row */
	s->rows++;
}

void
sift_colsort_put(SiftColSorter *s, const SiftColBatch *b, uint32 r)
{
	put_row(s, b, r, -1, 0, NULL);
}

void
sift_colsort_put_scoped(SiftColSorter *s, const SiftColBatch *b, uint32 r, int scope_leaf, int64 key, const bool *kept)
{
	put_row(s, b, r, scope_leaf, key, kept);
}

void
sift_colsort_sample_put_scoped(SiftColSorter *s, const SiftColBatch *b, uint32 r, int scope_leaf, int64 key, const bool *kept)
{
	SortKeys	k;

	build_row(s, b, r, scope_leaf, key, kept, &k);
	pack_row(s, &s->row, &k);
	sift_cbatch_row_free(&s->row);
	s->sample_bytes += pg_nextpower2_64(s->buf.len + TUPLE_OVERHEAD) + SORT_ENTRY;
	if (s->sample == NULL)
		s->sample = MemoryContextAlloc(s->cx, RANGE_SAMPLE_KEYS * sizeof(int64));
	if (s->nsample < RANGE_SAMPLE_KEYS && !k.isnull[0])
		s->sample[s->nsample++] = k.key[0];
	s->sampled++;
}

/* The ranges of the sorted keys of a sample: a range closes before the value that would take it past a share of 1 / nr of the sample, so that a range holds that share or the one value past it, and there are at most 2 * nr */
static int
cut_ranges(const int64 *sorted, uint32 n, int nr, int64 *bound)
{
	uint32		share = Max(n / nr, 1),
				held = 0;
	int			cuts = 0;

	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i; j < n && sorted[j] == sorted[i]; j++)
			;
		if (held > 0 && held + (j - i) > share)
		{
			bound[cuts++] = sorted[i - 1];
			held = 0;
		}
		held += j - i;
	}
	return cuts + 1;
}

bool
sift_colsort_may_range(uint64 rows, int work_kb)
{
	return (double) rows * (pg_nextpower2_64(TUPLE_OVERHEAD + 1) + SORT_ENTRY) * RANGE_SLACK > (double) work_kb * 1024;
}

static void
cut_sort_ranges(SiftColSorter *s, uint64 entries)
{
	double		work = (double) s->work_kb * 1024,
				need;
	SortRanges *rg;
	int			nr;

	if (s->sampled == 0)
		return;
	need = (double) entries * ((double) s->sample_bytes / s->sampled);
	if (need * RANGE_SLACK <= work)
		return;
	nr = (int) Min(ceil(RANGE_SLACK * need / work), Max(Min(RANGE_MAX, work / (BLCKSZ + 2 * SIFT_ROWFILE_BLOCK_MIN)) / 2, 2));
	if (s->nsample < (uint32) nr * 8)
		return;
	qsort(s->sample, s->nsample, sizeof(int64), sift_cmp_i64);
	rg = MemoryContextAllocZero(s->cx, sizeof(SortRanges));
	rg->bound = MemoryContextAlloc(s->cx, (2 * nr + 1) * sizeof(int64));
	rg->n = cut_ranges(s->sample, s->nsample, nr, rg->bound);
	if (rg->n < 2)
		return;
	rg->file = MemoryContextAlloc(s->cx, rg->n * sizeof(SiftRowFile *));
	rg->rows = MemoryContextAllocZero(s->cx, rg->n * sizeof(uint64));
	{
		Size		block = Min(Max((Size) work / ((Size) rg->n * 2), SIFT_ROWFILE_BLOCK_MIN), SIFT_ROWFILE_BLOCK_MAX);
		MemoryContext old = MemoryContextSwitchTo(s->cx);

		initStringInfo(&rg->scratch);
		for (int i = 0; i < rg->n; i++)
			rg->file[i] = sift_rowfile_create(s->schema, &rg->scratch, SIFT_ROWS_FILE, block, true);
		MemoryContextSwitchTo(old);
	}
	tuplesort_end(s->ts);
	s->ts = NULL;
	s->ranges = rg;
}

void
sift_colsort_sample_end(SiftColSorter *s, uint64 entries)
{
	cut_sort_ranges(s, entries);
	if (s->sample != NULL)
		pfree(s->sample);
	s->sample = NULL;
	s->nsample = 0;
}

static void
load_range(SiftColSorter *s)
{
	SortRanges *rg = s->ranges;
	SiftRowFile *f = rg->file[rg->next];
	SortKeys	k;

	ts_begin(s);
	for (uint64 n = 0; n < rg->rows[rg->next]; n++)
	{
		MemoryContext old = MemoryContextSwitchTo(s->rcx);
		bool		more = sift_rowfile_get(f, &s->in, &k.ver[0], &k.ver[1]);

		MemoryContextSwitchTo(old);
		SIFT_INVARIANT(more, "sift_col sort: the file of a key range ends before its rows");
		read_keys(s, &s->in, &k);
		put_tuple(s, &s->in, &k);
		MemoryContextReset(s->rcx);
	}
	sift_rowfile_close(f);
	rg->file[rg->next++] = NULL;
	tuplesort_performsort(s->ts);
}

/* The tuplesort of a range ends: the kB of the temporary files it used are counted for the tests */
static void
ts_end(SiftColSorter *s)
{
#ifdef SIFT_TESTING
	TuplesortInstrumentation stats;

	tuplesort_get_stats(s->ts, &stats);
	if (stats.spaceType == SORT_SPACE_TYPE_DISK)
		sift_test_cut_sort_disk_kb += stats.spaceUsed;
#endif
	tuplesort_end(s->ts);
	s->ts = NULL;
}

void
sift_colsort_done(SiftColSorter *s)
{
#ifdef SIFT_TESTING
	TuplesortInstrumentation stats;
#endif

	sift_cbatch_row_free(&s->row);
	if (s->ranges != NULL)
	{
		for (int i = 0; i < s->ranges->n; i++)
		{
			sift_rowfile_seal(s->ranges->file[i]);
#ifdef SIFT_TESTING
			sift_test_col_spill_bytes += (int) sift_rowfile_bytes(s->ranges->file[i]);
			sift_test_col_spill_rows += (int) s->ranges->rows[i];
#endif
		}
		elog(DEBUG1, "sift_col merge: %llu rows in %d key ranges", (unsigned long long) s->rows, s->ranges->n);
		return;
	}
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

	while (s->ts == NULL || !tuplesort_gettupleslot(s->ts, true, false, s->get, NULL))
	{
		if (s->ranges == NULL || s->ranges->next >= s->ranges->n)
			return false;
		if (s->ts != NULL)
			ts_end(s);
		load_range(s);
	}
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
	if (s->ts != NULL)
		tuplesort_end(s->ts);
	if (s->ranges != NULL)
		for (int i = 0; i < s->ranges->n; i++)
			if (s->ranges->file[i] != NULL)
				sift_rowfile_close(s->ranges->file[i]);
	ExecDropSingleTupleTableSlot(s->put);
	ExecDropSingleTupleTableSlot(s->get);
	MemoryContextDelete(s->cx);
}
