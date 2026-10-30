/*
 * agg/topn/scan.c - the rows of a unit read for the heap
 * The vector read of a segment, the monotone read of a key that follows the table, and the
 * entry that picks the way a unit is read.
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "col/kern.h"
#include "agg/topn/int.h"

typedef struct SegBest
{
	int64		best;
	int			seg;
} SegBest;

#define MONO_WIN	1024			/* the rows a delta chunk decodes in one block */

#define MONO_WINS	4				/* blocks the walk is expected to read at most, else the unit is read whole */

static int
trunc_cmp(const void *a, const void *b)
{
	const RunTrunc *x = a,
			   *y = b;

	return rank_after(x->next, y->next) ? 1 : rank_after(y->next, x->next) ? -1 : 0;
}

/* The superblock of the next unit the first pass left unfinished whose rest the bar does not exclude; false when there is none */
bool
sift_topn_revisit_next(SiftTopN *t, uint32 *sb)
{
	Rank		bar;

	if (!t->revisit)
	{
		t->revisit = true;
		t->rpos = 0;
		if (t->ntrunc > 1)
			qsort(t->trunc, t->ntrunc, sizeof(RunTrunc), trunc_cmp);
	}
	if (t->yielded)
		t->trunc[t->rpos].done = true;
	t->yielded = false;
	while (t->rpos < t->ntrunc && t->trunc[t->rpos].done)
		t->rpos++;
	if (t->rpos >= t->ntrunc)
		return false;
	if (bar_rank(t, &bar) && rank_after(t->trunc[t->rpos].next, bar))
		return false;
	*sb = t->trunc[t->rpos].sb;
	/* done when the next is asked for, whether the unit was read (it marks itself) or its zone could not beat the bar now */
	t->yielded = true;
	return true;
}

/*
 * The rows of the words w0..w0+nw of mask (a unit's row mask) go to the unit's heap in row order: those the bar leaves, with their keys in crow, ck and cnul
 * (the candidates finish_unit reads the other keys of). The bar tightens as the heap fills.
 */
static void
scan_rows(SiftTopN *t, const TopKey *k0, const SiftSrcVec *key0, const uint64 *mask, int w0, int nw, Rank *bar, bool *barred, int *m)
{
	for (int w = 0; w < nw; w++)
		for (uint64 b = mask[w]; b; b &= b - 1)
		{
			int			i = (w0 + w) * 64 + pg_rightmost_one_pos64(b);
			int64		v = 0;
			bool		isnull = !row_key(k0, key0, i, &v);
			TopEnt		e;

			/* one key: a row that only ties with the n-th wins nothing (the NULL rows of a column with 30% NULLs, NULLS FIRST, are all ties) */
			if (*barred && (t->nkey == 1 ? !rank_after(*bar, rank_of(k0, isnull, v)) : rank_after(rank_of(k0, isnull, v), *bar)))
				continue;
			memset(&e, 0, sizeof(e));
			e.k[0] = v;
			e.nul = isnull;
			e.row = e.tie = i;
			t->crow[*m] = i;
			t->ck[*m] = v;
			t->cnul[(*m)++] = isnull;
			heap_push(t, t->lh, &t->ln, &e);	/* the other keys are 0 in all of them: the order is the first key's, then the row */
			if (t->ln == t->limit)
			{
				Rank		mine = ent_rank(t, &t->lh[0]);

				if (!*barred || rank_after(*bar, mine))
				{
					*bar = mine;
					*barred = true;
				}
			}
		}
}

static int
segbest_asc(const void *a, const void *b)
{
	int64		x = ((const SegBest *) a)->best,
				y = ((const SegBest *) b)->best;

	return x < y ? -1 : x > y;
}

static int
segbest_desc(const void *a, const void *b)
{
	return segbest_asc(b, a);
}

/*
 * scan_rows for an integer or float first key, TOPN_SEG rows at a time: the segments are taken by the best value they hold (a descending scan of a column that
 * ascends, or the reverse, would push every row of the unit in turn), a segment whose best cannot pass the bar ends the unit, and for the others a range
 * kernel marks the rows that can pass: only those reach the heap. A value equal to the bar's passes, as in scan_rows. The NULL rows (the vector's value
 * there is undefined) go first, through scan_rows.
 */
static void
vec_rows(SiftTopN *t, const TopKey *k0, const SiftSrcVec *key0, const uint64 *rmask, int nrow, Rank *bar, bool *barred, int *m)
{
	const bool	wide = k0->vtype == SCV_I64,
				isf = k0->vtype == SCV_F64;
	const char *data = key0->data;
	const int	esz = wide || isf ? 8 : 4;
	int			nw = (nrow + 63) / 64,
				nseg = (nrow + TOPN_SEG - 1) / TOPN_SEG,
				nb = 0;
	bool		all;
	uint64	   *vm = palloc0((SIFT_COL_NWORDS(nrow) + SIFT_COL_SELWORDS) * sizeof(uint64));
	uint64	   *nullw = NULL;
	bool		anynull = false;
	uint64		hit[SIFT_COL_SELWORDS];
	SegBest    *sb = palloc(nseg * sizeof(SegBest));

	memcpy(vm, rmask, nw * sizeof(uint64));
	if (key0->isnull)
	{
		nullw = palloc((SIFT_COL_NWORDS(nrow) + SIFT_COL_SELWORDS) * sizeof(uint64));
		sift_colk_nonzero(key0->isnull, nrow, nullw);
		for (int w = 0; w < nw; w++)
		{
			uint64		nm = vm[w] & nullw[w];

			vm[w] &= ~nm;
			nullw[w] = nm;
			anynull |= nm != 0;
		}
		/* NULLs first: they are the first rows of the sort. NULLs last: the values come first and a bar of a value spares them all (ORDER BY ni LIMIT 10 of 30% NULLs pushed 6,000 rows of the first unit) */
		if (anynull && k0->nulls_first)
			scan_rows(t, k0, key0, nullw, 0, nw, bar, barred, m);
	}
	/* every row of the unit is selected: the kernels need no mask */
	all = sift_colk->bits_count(vm, nw) == nrow;
	for (int s = 0; s < nseg; s++)
	{
		int			off = s * TOPN_SEG,
					len = Min(TOPN_SEG, nrow - off);
		int64		mn,
					mx;
		bool		some;

		if (isf)
		{
			double		a,
						b;

			some = sift_aggk->minmax_f64(&a, &b, (const double *) data + off, all ? NULL : vm + off / 64, len);
			mn = f8_key(a);
			mx = f8_key(b);
		}
		else if (wide)
			some = sift_aggk->minmax_i64(&mn, &mx, (const int64 *) data + off, all ? NULL : vm + off / 64, len);
		else
		{
			int32		a,
						b;

			some = sift_aggk->minmax_i32(&a, &b, (const int32 *) data + off, all ? NULL : vm + off / 64, len);
			mn = a;
			mx = b;
		}
		if (some)
		{
			sb[nb].best = k0->desc ? mx : mn;
			sb[nb++].seg = s;
		}
	}
	qsort(sb, nb, sizeof(SegBest), k0->desc ? segbest_desc : segbest_asc);
	for (int q = 0; q < nb; q++)
	{
		int			off = sb[q].seg * TOPN_SEG,
					len = Min(TOPN_SEG, nrow - off),
					sw = (len + 63) / 64;
		Rank		g;

		if (bar_rank(t, &g) && (!*barred || rank_after(*bar, g)))
		{
			*bar = g;
			*barred = true;
		}
		if (*barred && bar->cls == 0)
			break;				/* the worst row is a NULL, which sorts before every value */
		if (*barred && bar->cls == 1)
		{
			if (rank_after(rank_of(k0, false, sb[q].best), *bar))
				break;			/* the segments after this one hold worse values */
			if (isf)
				sift_aggk->cmp_f64(hit, (const double *) data + off, len, k0->desc ? SCMP_GE : SCMP_LE,
								   f8_of_key((int64) ((k0->desc ? ~bar->ord : bar->ord) ^ UINT64CONST(0x8000000000000000))));
			else if (wide)
			{
				uint64		bv = (uint64) (~bar->ord ^ UINT64CONST(0x8000000000000000));

				sift_colk->range[SIFT_COLW_64](data + (Size) off * esz, len, k0->desc ? bv : UINT64CONST(0x8000000000000000),
											   k0->desc ? (uint64) PG_INT64_MAX - bv : bar->ord, hit);
			}
			else
			{
				uint32		bv = (uint32) (~bar->ord ^ UINT64CONST(0x8000000000000000));

				sift_colk->range[SIFT_COLW_32](data + (Size) off * esz, len, k0->desc ? bv : 0x80000000u,
											   k0->desc ? (uint32) PG_INT32_MAX - bv : (uint32) bar->ord ^ 0x80000000u, hit);
			}
			for (int w = 0; w < sw; w++)
				hit[w] &= vm[off / 64 + w];
			scan_rows(t, k0, key0, hit, off / 64, sw, bar, barred, m);
		}
		else
			scan_rows(t, k0, key0, vm + off / 64, off / 64, sw, bar, barred, m);
	}
	if (anynull && !k0->nulls_first)
	{
		Rank		g;

		if (bar_rank(t, &g) && (!*barred || rank_after(*bar, g)))
		{
			*bar = g;
			*barred = true;
		}
		if (!*barred || bar->cls != 1)
			scan_rows(t, k0, key0, nullw, 0, nw, bar, barred, m);
	}
	if (nullw)
		pfree(nullw);
	pfree(sb);
	pfree(vm);
}

static void
unit_reset(SiftTopN *t)
{
	if (t->ucxt)
	{
		MemoryContextReset(t->ucxt);
		for (int j = 0; j < t->nkey; j++)
			t->key[j].xid = NULL;
	}
}

/*
 * A unit whose first key never decreases (or increases) in row order, as a delta chunk says, has its best rows at one end: they are the first (last) rows that pass, so a
 * block of 1024 rows at a time is read from that end and the unit is not decoded whole. When the NULL rows sort first the best rows are the first NULL rows that pass, in
 * any chunk that reads a run of rows cheaply. One key and no condition on it (a bound of it is tested later, not in rmask). The walk is chosen before it reads, from
 * the rows that pass and the rows that can win (it expects them within MONO_WINS blocks), because it cannot give the unit back: its reads reuse the buffer of the leaf
 * that the executor's vector of the leaf stands on, which a filter on the same column may have filled. Once started it goes on to the limit, the bar or the end of
 * the unit. Rows tied with the limit-th win nothing on one key.
 */
static bool
mono_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const uint64 *rmask, int nrow)
{
	const TopKey *k0 = &t->key[0];
	SiftSrcZone z;
	Rank		bar;
	bool		barred,
				front,
				nulls,
				stop = false,
				barstop = false;
	uint32		rows[MONO_WIN],
				nblk = ((uint32) nrow + MONO_WIN - 1) / MONO_WIN,
				npass = 0;
	int			dir,
				m = 0;
	double		cands;

	if (!(sift_topn_runs && t->nkey == 1 && t->nexpr == 0 && !t->all && !t->sort && !t->revisit && k0->xk == TXK_NONE && k0->leaf >= 0 && !k0->blob && !k0->nenum &&
		  (k0->vtype == SCV_I32 || k0->vtype == SCV_I64) && t->limit > 0 && nblk > MONO_WINS) || t->kbox.has_lo || t->kbox.has_hi)
		return false;
	dir = sift_colsrc_key_order(src, unit, k0->leaf);
	if (dir == 0)
		return false;
	sift_colsrc_zone(src, unit, k0->leaf, &z);
	/* the rows the leaf cannot store read as NULL: they are not the NULL rows of the query */
	if (z.inexact)
		return false;
	nulls = k0->nulls_first && z.nnull > 0;
	if (dir == 2 && !nulls)
		return false;
	for (int i = 0; i < (nrow + 63) / 64; i++)
		npass += pg_popcount64(rmask[i]);
	/* the rows that can win, among the rows that pass, if the two do not go together */
	cands = (nulls ? z.nnull : (double) (nrow - z.nnull)) / nrow * ((double) npass / nrow) * nrow;
	if (cands < 2 * t->limit || (double) t->limit * nrow / cands > (double) MONO_WINS * MONO_WIN / 2)
		return false;
	front = nulls || (dir > 0) != k0->desc;
	barred = bar_rank(t, &bar);
	/* the n-th row so far is a NULL row: the NULL rows of the unit only tie with it, and a tie wins nothing on one key */
	if (nulls && barred && !rank_after(bar, rank_of(k0, true, 0)))
	{
		t->nmono++;
		return true;
	}
	grow_unit_buffers(t, Min(nrow, t->limit));
	for (uint32 w = 0; w < nblk && !stop; w++)
	{
		uint32		blk = front ? w : nblk - 1 - w,
					from = blk * MONO_WIN,
					cnt = Min((uint32) nrow - from, MONO_WIN);
		SiftSrcVec	v;

		for (uint32 i = 0; i < cnt; i++)
			rows[i] = from + i;
		sift_colsrc_read_rows(src, unit, k0->leaf, rows, cnt, &v);
		for (uint32 i = 0; i < cnt && !stop; i++)
		{
			uint32		k = front ? i : cnt - 1 - i,
						row = from + k;
			int64		key = 0;
			bool		isnull;

			if (!(rmask[row >> 6] >> (row & 63) & 1))
				continue;
			isnull = !row_key(k0, &v, k, &key);
			if (isnull != nulls)
				continue;
			if (barred && rank_after(rank_of(k0, isnull, key), bar))
			{
				stop = barstop = true;
				break;
			}
			t->crow[m] = row;
			t->ck[m++] = key;
			stop = m == t->limit;
		}
	}
	if (!barstop && m < t->limit)
	{
		/* the rows of the other class (the values, or the NULL rows) fill the rest: the unit is read whole after all, which first puts the leaf's buffer back as the executor's vector has it */
		uint32	   *all = palloc(Max(nrow, 1) * sizeof(uint32));
		SiftSrcVec	v;

		for (int i = 0; i < nrow; i++)
			all[i] = i;
		sift_colsrc_read_rows(src, unit, k0->leaf, all, nrow, &v);
		pfree(all);
		return false;
	}
	for (int q = 0; q < m; q++)
	{
		int			x = front ? q : m - 1 - q;
		TopEnt	   *e = &t->ce[q];

		memset(e, 0, sizeof(TopEnt));
		e->k[0] = t->ck[x];
		e->nul = nulls;
		e->row = e->tie = t->cand[q] = t->crow[x];
	}
	t->nmono++;
	if (m > 0)
		finish_unit(t, src, unit, m);
	return true;
}

/* The unit from its sorted run with no filter run: false when the walk finds the conditions fail for too many rows (the unit goes on to its filters) */
bool
sift_topn_unit_lazy(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const uint64 *rmask, int nrow)
{
	unit_reset(t);
	return run_unit(t, src, unit, NULL, rmask, nrow, true);
}

/*
 * The rows of the unit are cut in two steps, so that the leaves of the other sort keys are read for a handful of rows and not for all of
 * them. First the first key of every row that passes: a row strictly worse on it than the n-th of the heap, or than the n-th of the unit
 * so far, cannot be in the result whatever its other keys are. Then the others, for the rows that remain (the n best and those tied with
 * the n-th).
 */
void
sift_topn_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow, SiftTopLeaf leaf, void *ctx)
{
	const TopKey *k0 = &t->key[0];
	Rank		bar = {0, 0};
	bool		barred = false;
	int			nw = (nrow + 63) / 64,
				m = 0,
				nc = 0;

	unit_reset(t);
	/* the first key of the rows is read whole only by the paths that test every row: a unit from its sorted run reads the keys of the rows it takes, by position */
	if (key0 == NULL && k0->leaf >= 0 && (t->nexpr > 0 || t->all || k0->blob || !sift_topn_runs))
		key0 = leaf(ctx, k0->leaf);
	if (t->nexpr > 0)
	{
		for (int j = 0; j < t->nkey; j++)
			if (t->key[j].x)
				expr_eval_unit(t, &t->key[j], leaf, ctx, rmask, nrow);
		if (t->key[0].x)
			key0 = &t->key[0].xv;
	}
	if (t->all)
	{
		sort_unit(t, src, unit, key0, rmask, nrow);
		return;
	}
	if (k0->blob)
	{
		unit_blob(t, src, unit, key0, rmask, nrow);
		return;
	}
	if (t->revisit)
	{
		(void) run_unit(t, src, unit, key0, rmask, nrow, false);
		return;
	}
	if (key0 == NULL && head_unit(t, src, unit, rmask, nrow))
		return;
	if (run_unit(t, src, unit, key0, rmask, nrow, false))
		return;
	if (key0 == NULL && mono_unit(t, src, unit, rmask, nrow))
		return;
	if (key0 == NULL && k0->leaf >= 0)
		key0 = leaf(ctx, k0->leaf);
	grow_unit_buffers(t, nrow);
	t->ln = 0;
	barred = bar_rank(t, &bar);
	if (nrow >= TOPN_VEC_ROWS && k0->xk == TXK_NONE && k0->nenum == 0 && (k0->vtype == SCV_I32 || k0->vtype == SCV_I64 || k0->vtype == SCV_F64) &&
		sift_colk->bits_count(rmask, nw) * TOPN_VEC_DENSITY >= nrow)
		vec_rows(t, k0, key0, rmask, nrow, &bar, &barred, &m);
	else
		scan_rows(t, k0, key0, rmask, 0, nw, &bar, &barred, &m);
	for (int q = 0; q < m; q++)
	{
		TopEnt	   *e = &t->ce[nc];

		if (barred && rank_after(rank_of(k0, t->cnul[q], t->ck[q]), bar))
			continue;
		memset(e, 0, sizeof(TopEnt));
		e->k[0] = t->ck[q];
		e->nul = t->cnul[q];
		e->row = t->cand[nc] = t->crow[q];
		e->tie = e->row;
		nc++;
	}
	if (nc > 0)
		finish_unit(t, src, unit, nc);
}

/* The zone of the key's values from the zone of its leaf: coalesce(x, c) is c where x is NULL */
SiftSrcZone
key_zone(const TopKey *k, const SiftSrcZone *z)
{
	SiftSrcZone o = *z;

	/* the zone is of OIDs, which are not in the order of the values */
	o.inexact |= k->nenum > 0;
	if (k->xk != TXK_COALESCE || o.inexact || z->nnull == 0)
		return o;
	if (z->nnull == z->nvals)
		o.min = o.max = k->xc;
	else
	{
		o.min = Min(z->min, k->xc);
		o.max = Max(z->max, k->xc);
	}
	o.nnull = 0;
	return o;
}
