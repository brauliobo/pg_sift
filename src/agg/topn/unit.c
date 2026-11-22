/*
 * agg/topn/unit.c - the read of one unit for the heap
 * The buffers of a unit, the keys and the outputs read for its rows, the rank of a row
 * against the bar, and the sorted rows a unit gives the heap.
 */
/* check-memory: allow-allocs every array here follows the rows of one unit (at most SIFT_COL_UNIT_MAX_ROWS, 74,496) or of one chunk, not the rows of the table */
#include "postgres.h"

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"
#include "utils/tuplesort.h"

#include "agg/topn/int.h"

#define ST_SORT sort_rows
#define ST_ELEMENT_TYPE uint32
#define ST_COMPARE(a, b) SIFT_CMP3(*(a), *(b))
#define ST_SCOPE
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

/* A unit starts with scratch buffers (keep false: nothing in them is read again, so the larger ones are new and the old are freed, where repalloc would copy a megabyte of a unit that has just ended); a walk that fills them as it goes keeps what it has */
static void *
regrow(SiftTopN *t, void *p, Size sz, bool keep)
{
	if (!keep && p)
	{
		pfree(p);
		p = NULL;
	}
	return grow(t, p, sz);
}

void
grow_unit_buffers(SiftTopN *t, int nrow, bool keep)
{
	if (t->ccap >= nrow)
		return;
	t->ccap = Max(nrow, 2 * t->ccap);
	t->crow = regrow(t, t->crow, t->ccap * sizeof(uint32), keep);
	t->ck = regrow(t, t->ck, t->ccap * sizeof(int64), keep);
	t->cnul = regrow(t, t->cnul, t->ccap, keep);
	t->cand = regrow(t, t->cand, t->ccap * sizeof(uint32), keep);
	t->ce = regrow(t, t->ce, t->ccap * sizeof(TopEnt), keep);
	if (t->all)
	{
		t->tids = regrow(t, t->tids, t->ccap * sizeof(ItemPointerData), keep);
		t->skey = regrow(t, t->skey, (Size) t->ccap * t->nkey * sizeof(int64), keep);
		t->snull = regrow(t, t->snull, (Size) t->ccap * t->nkey, keep);
	}
}

/* The rows cand[0..nc) (ascending) with their first key in ce: the other keys, by position */
static void
read_other_keys(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int nc)
{
	for (int j = 1; j < t->nkey; j++)
	{
		SiftSrcVec	v;

		key_rows(t, src, unit, j, nc, &v);
		for (int x = 0; x < nc; x++)
			if (!row_val(t, j, &v, x, &t->ce[x].k[j]))
				t->ce[x].nul |= 1 << j;
	}
}

/* The output columns of the rows of lh: the value of a sort key is in the entry, the others are read from the unit. The values of row i are uv[i * nov ...] */
static void
read_outputs(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit)
{
	for (int c = 0; c < t->nov; c++)
	{
		SiftSrcVec	v;
		int			j = t->okey[c];

		if (j >= 0)
			for (int i = 0; i < t->ln; i++)
			{
				t->uvn[(Size) i * t->nov + c] = (t->lh[i].nul >> j) & 1;
				t->uv[(Size) i * t->nov + c] = t->lh[i].k[j];
			}
		else
		{
			sift_colsrc_read_rows(src, unit, t->oleaf[c], t->rows, t->ln, &v);
			for (int i = 0; i < t->ln; i++)
			{
				uint32		x = row_at(t->rows, t->ln, t->lh[i].row, i);
				Size		at = (Size) i * t->nov + c;

				t->uvn[at] = v.isnull && v.isnull[x];
				t->uv[at] = t->uvn[at] ? 0 : t->owide[c] ? ((const int64 *) v.data)[x] : ((const int32 *) v.data)[x];
			}
		}
	}
}

/* The rows of lh that the heap would take as it is now: those whose output the unit has to give */
static int
out_rows(const SiftTopN *t)
{
	int			n = 0;

	if (t->n < t->limit)
		return t->ln;
	for (int i = 0; i < t->ln; i++)
		n += ent_cmp(t, &t->lh[i], &t->heap[0]) < 0;
	return n;
}

/* The best n of those rows by all the keys go to the heap with their TIDs. best_last: the rows are in ascending position and the best on the first key is the last (the heap then takes them from the last, and rejects the rest at its root) */
void
finish_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int nc, bool best_last)
{
	uint16		part;
	bool		vals;
	TopEnt	   *lh = t->lh;

	t->ln = 0;
	read_other_keys(t, src, unit, nc);
	if (nc <= t->limit)
	{
		/* the unit has no more rows than the heap holds: all of them go on, in the entries they were made in (a copy of a heap's worth of them is a megabyte of new pages) */
		t->lh = t->ce;
		t->ln = nc;
	}
	else
	{
		/* the rows come in position order: when the last sorts before the first, the best are at the end and the heap rejects the others at its root (a unit of a key that ties on its first sort column and follows the table on the next) */
		bool		from_end = best_last || ent_cmp(t, &t->ce[nc - 1], &t->ce[0]) < 0;

		for (int x = 0; x < nc; x++)
			heap_push(t, t->lh, &t->ln, &t->ce[from_end ? nc - 1 - x : x]);
	}
	if (t->ln == 0)
	{
		t->lh = lh;
		return;
	}
	for (int i = 0; i < t->ln; i++)
		t->rows[i] = t->lh[i].row;
	sort_rows(t->rows, t->ln);
	sift_colsrc_tids(src, unit, t->rows, t->ln, t->tids);
	part = unit_part(t, src);
	for (int i = 0; i < t->ln; i++)
	{
		TopEnt	   *e = &t->lh[i];

		e->tid = t->tids[row_at(t->rows, t->ln, e->row, i)];
		e->part = part;
		e->tie = tie_key(&e->tid, part);
	}
	vals = t->nov > 0 && (t->nread == 0 || out_rows(t) >= Max(TOPN_OUT_ROWS, t->limit / 2));
	if (vals)
		read_outputs(t, src, unit);
	if (vals || t->lh != t->ce || !res_take(t, t->ln))
		for (int i = 0; i < t->ln; i++)
			res_push(t, &t->lh[i], vals ? &t->uv[(Size) i * t->nov] : NULL, vals ? &t->uvn[(Size) i * t->nov] : NULL);
	t->lh = lh;
	share_bar(t);
}

/*
 * The rank of the n-th row so far: the heap's worst when it is full, or the bound the participants of a parallel scan share when that sorts
 * before it. A row or a zone strictly after it cannot be in the result, whoever holds the n rows.
 */
bool
bar_rank(const SiftTopN *t, Rank *bar)
{
	bool		have = t->n == t->limit;

	if (have)
		*bar = ent_rank(t, &t->heap[0]);
	if (t->shared)
	{
		uint64		b = pg_atomic_read_u64(t->shared);
		Rank		r = {1, b};

		if (b != PG_UINT64_MAX && (!have || rank_after(*bar, r)))
		{
			*bar = r;
			have = true;
		}
	}
	return have;
}

/* a full heap whose worst row has a value tightens the bound of the other participants (a NULL worst row is not a rank among the values) */
void
share_bar(SiftTopN *t)
{
	Rank		w;
	uint64		cur;

	if (t->shared == NULL || t->n < t->limit || t->key[0].blob)
		return;
	w = ent_rank(t, &t->heap[0]);
	cur = pg_atomic_read_u64(t->shared);
	while (w.cls == 1 && w.ord < cur && !pg_atomic_compare_exchange_u64(t->shared, &cur, w.ord))
		;
}

/* Bytes of the rows kept in memory before they go to a tuplesort: hash_mem, as the other tables of the scan (one allocation is MaxAllocSize at most) */
static int64
sort_memory(void)
{
	return Min((int64) get_hash_memory_limit(), (int64) 512 << 20);
}

static inline bool
mem_null(const SiftTopN *t, const int64 *r, int j)
{
	return (r[t->nkey + j / 64] >> (j % 64)) & 1;
}

static void
sort_put_tuple(SiftTopN *t, const int64 *k, const bool *isnull, uint64 tid)
{
	ExecClearTuple(t->sin);
	for (int j = 0; j < t->nkey; j++)
	{
		const TopKey *key = &t->key[j];

		if (!key->blob || isnull[j])
			t->sin->tts_values[j] = Int64GetDatum(k[j]);
		else if (key->bkind == BK_TEXT)
		{
			struct varlena *v = MemoryContextAlloc(t->ucxt, VARHDRSZ + KEY_BLOB(k[j])->len);

			SET_VARSIZE(v, VARHDRSZ + KEY_BLOB(k[j])->len);
			memcpy(VARDATA(v), KEY_BLOB(k[j])->data, KEY_BLOB(k[j])->len);
			t->sin->tts_values[j] = PointerGetDatum(v);
		}
		else
			t->sin->tts_values[j] = PointerGetDatum(KEY_BLOB(k[j])->data);	/* the image of a datum: what the tuplesort copies */
		t->sin->tts_isnull[j] = isnull[j];
	}
	t->sin->tts_values[t->nkey] = Int64GetDatum(tid);
	t->sin->tts_isnull[t->nkey] = false;
	ExecStoreVirtualTuple(t->sin);
	tuplesort_puttupleslot(t->sort, t->sin);
}

/* The rows in memory go to the tuplesort: work_mem is spent */
static void
mem_spill(SiftTopN *t)
{
	bool	   *isnull = palloc(t->nkey * sizeof(bool));
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(t));

	t->sort = sort_begin(t);
	MemoryContextSwitchTo(old);
	for (int64 i = 0; i < t->nmem; i++)
	{
		const int64 *r = mem_row(t, i);

		for (int j = 0; j < t->nkey; j++)
			isnull[j] = mem_null(t, r, j);
		sort_put_tuple(t, r, isnull, r[t->stride - 1]);
	}
	pfree(isnull);
	pfree(t->mem);
	t->mem = NULL;
}

/* One row that passes (no bound for the heap): kept in memory while the rows fit work_mem, else in the tuplesort */
static void
sort_put(SiftTopN *t, const int64 *k, const bool *isnull, uint64 tid)
{
	int64	   *r;

	if (t->memmax == 0)
		t->memmax = sort_memory();
	if (t->mem && (t->nmem + 1) * t->stride * (int64) sizeof(int64) > t->memmax)
		mem_spill(t);
	if (t->mem == NULL)
	{
		sort_put_tuple(t, k, isnull, tid);
		return;
	}
	if (t->nmem == t->capmem)
	{
		t->capmem *= 2;
		t->mem = repalloc(t->mem, t->capmem * t->stride * sizeof(int64));
	}
	r = mem_row(t, t->nmem);
	memset(r, 0, t->stride * sizeof(int64));
	for (int j = 0; j < t->nkey; j++)
		if (isnull[j])
			r[t->nkey + j / 64] |= (int64) 1 << (j % 64);
		else
			r[j] = k[j];
	r[t->stride - 1] = tid;
	if (t->bar && mem_cmp(r, t->bar, t) > 0)
		return;
	t->nmem++;
	if (t->keep > 0 && t->nmem >= 2 * t->keep)
		mem_cut(t);
}

static inline void
mem_swap(const SiftTopN *t, int64 a, int64 b)
{
	int64	   *x = mem_row(t, a),
			   *y = mem_row(t, b);

	for (int i = 0; i < t->stride; i++)
	{
		int64		v = x[i];

		x[i] = y[i];
		y[i] = v;
	}
}

/* the index of the median of the rows a, b and c */
static int64
mem_median3(const SiftTopN *t, int64 a, int64 b, int64 c)
{
	const bool	ab = mem_cmp(mem_row(t, a), mem_row(t, b), (void *) t) < 0,
				bc = mem_cmp(mem_row(t, b), mem_row(t, c), (void *) t) < 0,
				ac = mem_cmp(mem_row(t, a), mem_row(t, c), (void *) t) < 0;

	return ab == bc ? b : (ab == ac ? c : a);
}

static void
mem_select_range(SiftTopN *t, int64 lo, int64 hi, int64 n, bool exact);

/* the rows [lo, hi] put in order: a group of at most five */
static void
mem_insertion_sort(SiftTopN *t, int64 lo, int64 hi)
{
	for (int64 i = lo + 1; i - 1 < hi; i++)
		for (int64 j = i; j > lo && mem_cmp(mem_row(t, j), mem_row(t, j - 1), t) < 0; j--)
			mem_swap(t, j, j - 1);
}

/*
 * The row to split [lo, hi] at: the median of nine rows spread over it (of three for a short range), which an input that is sorted, reversed or organ-piped splits in the
 * middle; exact: the median of the medians of the groups of five, which leaves each side at least three tenths of the range whatever the input holds (the select below that
 * runs on the medians is itself exact, so a range of n rows takes O(n)).
 */
static int64
mem_pivot(SiftTopN *t, int64 lo, int64 hi, bool exact)
{
	const int64 n = hi - lo + 1,
				mid = lo + n / 2;

	if (!exact)
	{
		const int64 step = n / 8;

		if (n < 64)
			return mem_median3(t, lo, mid, hi);
		return mem_median3(t, mem_median3(t, lo, lo + step, lo + 2 * step), mem_median3(t, mid - step, mid, mid + step),
						   mem_median3(t, hi - 2 * step, hi - step, hi));
	}
	if (n <= 5)
	{
		mem_insertion_sort(t, lo, hi);
		return mid;
	}
	{
		int64		ng = 0;

		for (int64 g = lo; g - 1 < hi; g += 5, ng++)
		{
			const int64 end = Min(g + 4, hi);

			mem_insertion_sort(t, g, end);
			mem_swap(t, lo + ng, g + (end - g) / 2);
		}
		mem_select_range(t, lo, lo + ng - 1, lo + (ng - 1) / 2, true);
		return lo + (ng - 1) / 2;
	}
}

/*
 * The rows [lo, hi] rearranged so that row n is the one that sorts n-th among them, the rows before it sort first and those after it last (in no order within): introselect.
 * Each round splits the range in three at a pivot copy (the rows that sort before it, those equal to it, those after it), so that rows that tie are done in one pass, and goes on
 * in the side that holds n; after 2 log2(range) rounds that did not finish (an input made against the pivot rule) the pivots are exact, which bounds the rest by O(range).
 */
static void
mem_select_range(SiftTopN *t, int64 lo, int64 hi, int64 n, bool exact)
{
	int			rounds;
	int64		stack_pivot[64],
			   *pv;

	if (lo >= hi)
		return;
	rounds = 2 * (pg_leftmost_one_pos64((uint64) (hi - lo + 1)) + 1);
	pv = t->stride <= lengthof(stack_pivot) ? stack_pivot : palloc(t->stride * sizeof(int64));
	while (lo < hi)
	{
		int64		lt = lo,
					i = lo,
					gt = hi;

		memcpy(pv, mem_row(t, mem_pivot(t, lo, hi, exact || rounds-- <= 0)), t->stride * sizeof(int64));
		while (i <= gt)
		{
			const int	c = mem_cmp(mem_row(t, i), pv, t);

			if (c < 0)
				mem_swap(t, lt++, i++);
			else if (c > 0)
				mem_swap(t, i, gt--);
			else
				i++;
		}
		if (n < lt)
			hi = lt - 1;
		else if (n > gt)
			lo = gt + 1;
		else
			break;
	}
	if (pv != stack_pivot)
		pfree(pv);
}

/* The rows of mem rearranged so that the first n are the n that sort first (in no order), and row n the one after them: the rows differ by their TIDs, a tie of the keys is not a tie of the rows */
static void
mem_select(SiftTopN *t, int64 n)
{
	mem_select_range(t, 0, t->nmem - 1, n, false);
}

/* mem keeps the best `keep` rows; the worst of them is the bar of the rows that come after */
void
mem_cut(SiftTopN *t)
{
	if (t->keep <= 0 || t->nmem <= t->keep)
		return;
	mem_select(t, t->keep - 1);
	t->nmem = t->keep;
	if (t->bar == NULL)
		t->bar = MemoryContextAlloc(GetMemoryChunkContext(t), t->stride * sizeof(int64));
	memcpy(t->bar, mem_row(t, t->keep - 1), t->stride * sizeof(int64));
}

int
mem_cmp(const void *a, const void *b, void *arg)
{
	const SiftTopN *t = arg;
	const int64 *x = a,
			   *y = b;

	for (int j = 0; j < t->nkey; j++)
	{
		bool		xn = mem_null(t, x, j),
					yn = mem_null(t, y, j);

		if (xn != yn)
			return xn == t->key[j].nulls_first ? -1 : 1;
		if (xn || x[j] == y[j])
			continue;
		return (x[j] < y[j]) != t->key[j].desc ? -1 : 1;
	}
	return SIFT_CMP3(x[t->stride - 1], y[t->stride - 1]);
}

/* With no bound for the heap: every row that passes goes to the sort, with its keys and its TID */
void
sort_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow)
{
	int			nk = t->nkey,
				nc = 0;
	uint16		part;

	grow_unit_buffers(t, nrow, false);
	for (int w = 0; w < (nrow + 63) / 64; w++)
		for (uint64 b = rmask[w]; b; b &= b - 1)
		{
			t->cand[nc] = w * 64 + pg_rightmost_one_pos64(b);
			t->snull[(Size) nc * nk] = !row_val(t, 0, key0, t->cand[nc], &t->skey[(Size) nc * nk]);
			nc++;
		}
	if (nc == 0)
		return;
	for (int j = 1; j < nk; j++)
	{
		SiftSrcVec	v;

		key_rows(t, src, unit, j, nc, &v);
		for (int x = 0; x < nc; x++)
			t->snull[(Size) x * nk + j] = !row_val(t, j, &v, x, &t->skey[(Size) x * nk + j]);
	}
	sift_colsrc_tids(src, unit, t->cand, nc, t->tids);
	part = unit_part(t, src);
	for (int x = 0; x < nc; x++)
		sort_put(t, &t->skey[(Size) x * nk], &t->snull[(Size) x * nk], tie_key(&t->tids[x], part));
	t->nsort += nc;
}

