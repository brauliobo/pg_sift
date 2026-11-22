/*
 * col/v1/build/ranges.c - the key ranges of a build with key_cut: where they are cut and which range a row falls in
 * Range i holds the keys above bound[i - 1] up to bound[i]; the last range holds the rest and the NULLs. A range of one key (single) holds the rows of one value of the key, so
 * whoever fills the range holds nothing but that value.
 */
#include "postgres.h"
#include "col/v1/build/build_int.h"

int
colb_range_of_key(const ColRanges *g, int64 key)
{
	int			lo = 0,
				hi = g->n - 1;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (key <= g->bound[mid])
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

int
colb_range_of_value(const ColRanges *g, const SiftColValue *v)
{
	if (v->isnull || v->overflow)
		return g->n - 1;
	return colb_range_of_key(g, colb_key_of(g->def, v));
}

int
colb_range_of_row(const ColRanges *g, const SiftColBatch *batch, uint32 r)
{
	const SiftCVals *kv = &batch->col[g->leaf];

	return sift_col_bit_test(kv->nulls, r) || sift_col_bit_test(kv->ovf, r) ? g->n - 1
		: colb_range_of_key(g, g->def->store == SIFT_COLS_INT ? kv->key[r] : sift_crun_key(g->def, kv->key[r]));
}

static void
ranges_init(ColBuild *b, ColRanges *g, int leaf, int cap)
{
	g->n = 0;
	g->leaf = leaf;
	g->def = &b->r->schema->cols[leaf];
	g->bound = MemoryContextAlloc(b->cxt, cap * sizeof(int64));
	g->single = MemoryContextAllocZero(b->cxt, cap * sizeof(bool));
}

/* nr ranges of about as many rows, cut at the quantiles of the sorted keys of the sample; false when they come to fewer than three */
bool
colb_ranges_cut(ColBuild *b, ColRanges *g, int leaf, int nr, const int64 *sample, uint32 nsample)
{
	ranges_init(b, g, leaf, nr);
	for (int i = 0; i < nr - 1; i++)
	{
		int64		q = sample[(uint64) (i + 1) * nsample / nr - 1];

		if (g->n == 0 || q > g->bound[g->n - 1])
			g->bound[g->n++] = q;
	}
	g->n++;
	return g->n >= 3;
}

typedef struct KeyRun
{
	int64		key;
	uint32		count;
} KeyRun;

static int
run_cmp_count(const void *a, const void *b)
{
	const KeyRun *x = a,
			   *y = b;

	return x->count != y->count ? (x->count < y->count ? 1 : -1) : (x->key < y->key ? -1 : x->key > y->key);
}

static int
run_cmp_key(const void *a, const void *b)
{
	const KeyRun *x = a,
			   *y = b;

	return x->key < y->key ? -1 : x->key > y->key;
}

static void
bound_add(ColRanges *g, int64 bound, bool single)
{
	if (g->n > 0 && bound <= g->bound[g->n - 1])
		return;
	g->bound[g->n] = bound;
	g->single[g->n++] = single;
}

/*
 * nr ranges of which the values of the key that the sample says hold at least heavy_rows rows (at most max_single of them, the most frequent) are ranges of their own, and the other keys
 * are cut at the quantiles of the rows that remain: a value whose rows fill a unit then has a unit-sized buffer of its own to fill, and its rows lie in as few units as they fit.
 * Returns the ranges of one key.
 */
int
colb_ranges_isolate(ColBuild *b, ColRanges *g, int leaf, int nr, uint64 heavy_rows, int max_single)
{
	double		per_key = (double) b->entries / Max(b->nksample, 1);
	uint32		min_count = (uint32) Max(ceil((double) heavy_rows / Max(per_key, 1e-9)), 3);
	KeyRun	   *run = palloc(b->nksample * sizeof(KeyRun));
	int			nrun = 0,
				nsingle = 0,
				h = 0;
	uint64		tail = 0,
				taken = 0;
	double		target;
	int			emitted = 0;

	for (uint32 i = 0; i < b->nksample;)
	{
		uint32		j = i;

		while (j < b->nksample && b->ksample[j] == b->ksample[i])
			j++;
		run[nrun++] = (KeyRun) {b->ksample[i], j - i};
		i = j;
	}
	qsort(run, nrun, sizeof(KeyRun), run_cmp_count);
	while (nsingle < nrun && nsingle < max_single && run[nsingle].count >= min_count && run[nsingle].key > PG_INT64_MIN)
		nsingle++;
	for (int i = nsingle; i < nrun; i++)
		tail += run[i].count;
	qsort(run, nsingle, sizeof(KeyRun), run_cmp_key);
	/* the heavy keys, in key order, are the first nsingle runs; the rest follow in the order of their counts: sort those apart by key and merge in the one pass below */
	{
		KeyRun	   *rest = run + nsingle;

		qsort(rest, nrun - nsingle, sizeof(KeyRun), run_cmp_key);
	}
	ranges_init(b, g, leaf, nr + 2 * nsingle + 2);
	target = (double) tail / Max(nr - 2 * nsingle, 1);
	for (int i = nsingle; i <= nrun; i++)
	{
		/* the heavy keys that sort before the next tail run (or all that remain at the end) get their ranges first */
		while (h < nsingle && (i == nrun || run[h].key < run[i].key))
		{
			bound_add(g, run[h].key - 1, false);
			bound_add(g, run[h].key, true);
			h++;
		}
		if (i == nrun)
			break;
		taken += run[i].count;
		if ((double) taken >= target * (emitted + 1))
		{
			bound_add(g, run[i].key, false);
			emitted++;
		}
	}
	g->n++;
	pfree(run);
	return nsingle;
}
