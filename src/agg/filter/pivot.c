/*
 * agg/filter/pivot.c - the single-pass multi-FILTER pivot (v6_filter_pivot.h).
 */
#include "agg/filter/pivot.h"
#include "agg/distinct/distinct.h"
#include "core/popcount.h"

#include <math.h>

typedef struct PivotAcc
{
	uint64		n;
	__int128	sum;
	__int128	ext;			/* min or max of a numeric of 38 digits */
	int64		i;
	double		f;
} PivotAcc;

struct SiftPivot
{
	MemoryContext cx;
	int			nfilters;
	int			naggs;
	SiftPivotAgg *agg;
	SiftDistinct **dist;		/* of each COUNT(DISTINCT) aggregate */
	PivotAcc   *acc;
	uint32		ngroups;
	bool		float_overflow;
	SiftPivotStats st;
	SiftFilterMasks *fm;
	SiftBatchSel *sub;			/* the rows one COUNT(DISTINCT) takes */
};

struct SiftFilterMasks
{
	MemoryContext cx;
	int			nfilters;
	bool		rows_valid;
	uint64		active[SIFT_VEC_WORDS];
	uint64	   *bits;			/* nfilters x SIFT_VEC_WORDS */
	uint64	   *rows;			/* SIFT_VEC_LEN */
};

SiftFilterMasks *
sift_fmasks_create(MemoryContext cx, int nfilters)
{
	SiftFilterMasks *m = MemoryContextAllocZero(cx, sizeof(SiftFilterMasks));

	SIFT_INVARIANT(nfilters >= 0 && nfilters <= SIFT_PIVOT_MAX_FILTERS, "filter masks: %d clauses", nfilters);
	m->cx = cx;
	m->nfilters = nfilters;
	m->bits = MemoryContextAlloc(cx, sizeof(uint64) * SIFT_VEC_WORDS * Max(nfilters, 1));
	m->rows = MemoryContextAlloc(cx, sizeof(uint64) * SIFT_VEC_LEN);
	return m;
}

void
sift_fmasks_eval(SiftFilterMasks *m, const SiftBatchSel *bs, SiftPivotEval eval, void *ctx)
{
	const uint32 nwords = (uint32) ((bs->n_raw + 63) / 64);
	SiftBatchIter it;
	uint32		r;

	memset(m->active, 0, sizeof(m->active));
	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
		m->active[r >> 6] |= UINT64_C(1) << (r & 63);
	for (int f = 0; f < m->nfilters; f++)
	{
		uint64	   *b = m->bits + (size_t) f * SIFT_VEC_WORDS;

		memset(b, 0, sizeof(uint64) * SIFT_VEC_WORDS);
		eval(ctx, f, b);
		for (uint32 w = 0; w < nwords; w++)
			b[w] &= m->active[w];
	}
	m->rows_valid = false;
}

const uint64 *
sift_fmasks_active(const SiftFilterMasks *m)
{
	return m->active;
}

const uint64 *
sift_fmasks_bits(const SiftFilterMasks *m, int f)
{
	return m->bits + (size_t) f * SIFT_VEC_WORDS;
}

const uint64 *
sift_fmasks_rows(SiftFilterMasks *m)
{
	if (!m->rows_valid)
	{
		for (uint32 w = 0; w < SIFT_VEC_WORDS; w++)
		{
			uint64		blk[64];

			for (int f = 0; f < 64; f++)
				blk[f] = f < m->nfilters ? m->bits[(size_t) f * SIFT_VEC_WORDS + w] : 0;
			sift_pivot_transpose64(blk);
			memcpy(m->rows + w * 64, blk, sizeof(blk));
		}
		m->rows_valid = true;
	}
	return m->rows;
}

void
sift_fmasks_free(SiftFilterMasks *m)
{
	pfree(m->bits);
	pfree(m->rows);
	pfree(m);
}

SiftPivot *
sift_pivot_create(MemoryContext cx, int nfilters, int naggs, const SiftPivotAgg *aggs, uint64 distinct_budget)
{
	SiftPivot  *p = MemoryContextAllocZero(cx, sizeof(SiftPivot));
	int			ndist = 0;

	SIFT_INVARIANT(nfilters >= 0 && nfilters <= SIFT_PIVOT_MAX_FILTERS, "filter pivot: %d clauses", nfilters);
	p->cx = cx;
	p->nfilters = nfilters;
	p->naggs = naggs;
	p->agg = MemoryContextAlloc(cx, sizeof(SiftPivotAgg) * Max(naggs, 1));
	memcpy(p->agg, aggs, sizeof(SiftPivotAgg) * naggs);
	p->dist = MemoryContextAllocZero(cx, sizeof(SiftDistinct *) * Max(naggs, 1));
	for (int j = 0; j < naggs; j++)
	{
		SIFT_INVARIANT(aggs[j].filter < nfilters, "filter pivot: aggregate %d names clause %d of %d", j, aggs[j].filter, nfilters);
		ndist += aggs[j].kind == SFP_COUNT_DISTINCT;
	}
	for (int j = 0; j < naggs; j++)
		if (aggs[j].kind == SFP_COUNT_DISTINCT)
			p->dist[j] = sift_distinct_create(cx, distinct_budget / (uint64) ndist, 0);
	p->fm = sift_fmasks_create(cx, nfilters);
	p->sub = MemoryContextAllocAligned(cx, sizeof(SiftBatchSel), _Alignof(SiftBatchSel), 0);
	return p;
}

void
sift_pivot_transpose64(uint64 *a)
{
	uint64		m = UINT64_C(0x00000000FFFFFFFF);

	for (int j = 32; j != 0; j >>= 1, m ^= m << j)
		for (int k = 0; k < 64; k = (k + j + 1) & ~j)
		{
			uint64		t = ((a[k] >> j) ^ a[k + j]) & m;

			a[k] ^= t << j;
			a[k + j] ^= t;
		}
}

static void
pivot_groups(SiftPivot *p, uint32 ngroups)
{
	if (ngroups <= p->ngroups)
		return;
	{
		uint32		n = Max(Max(p->ngroups * 2, ngroups), 16);
		PivotAcc   *a = MemoryContextAllocAligned(p->cx, sizeof(PivotAcc) * n * (uint32) Max(p->naggs, 1), _Alignof(PivotAcc), MCXT_ALLOC_ZERO);

		if (p->acc != NULL)
		{
			memcpy(a, p->acc, sizeof(PivotAcc) * p->ngroups * (uint32) Max(p->naggs, 1));
			pfree(p->acc);
		}
		p->acc = a;
		p->ngroups = n;
	}
}

static int
f8_cmp(double a, double b)
{
	bool		an = isnan(a), bn = isnan(b);

	if (an || bn)
		return an == bn ? 0 : (an ? 1 : -1);
	return a < b ? -1 : a > b;
}

static inline void
fold_f64(SiftPivot *p, PivotAcc *a, SiftPivotKind kind, double v)
{
	if (kind == SFP_SUM || kind == SFP_AVG)
	{
		double		s = a->n == 0 ? v : a->f + v;

		if (isinf(s) && !isinf(v) && !isinf(a->f))
			p->float_overflow = true;
		a->f = s;
	}
	else if (a->n == 0 || (kind == SFP_MIN ? f8_cmp(v, a->f) < 0 : f8_cmp(v, a->f) > 0))
		a->f = v;
	a->n++;
}

static inline void
fold_int(PivotAcc *a, SiftPivotKind kind, int64 v)
{
	if (kind == SFP_SUM || kind == SFP_AVG)
		a->sum += v;
	else if (a->n == 0 || (kind == SFP_MIN ? v < a->i : v > a->i))
		a->i = v;
	a->n++;
}

static inline void
fold_i128(PivotAcc *a, SiftPivotKind kind, SiftI128 v)
{
	__int128	x = (__int128) (((unsigned __int128) (uint64) v.hi << 64) | v.lo);

	if (kind == SFP_SUM || kind == SFP_AVG)
		a->sum += x;
	else if (a->n == 0 || (kind == SFP_MIN ? x < a->ext : x > a->ext))
		a->ext = x;
	a->n++;
}

static inline void
fold_row(SiftPivot *p, PivotAcc *a, SiftPivotKind kind, const SiftVec *v, uint32 r)
{
	switch (v->type)
	{
		case SVEC_I32:
			fold_int(a, kind, ((const int32 *) v->data)[r]);
			break;
		case SVEC_I64:
		case SVEC_NUM64:
			fold_int(a, kind, ((const int64 *) v->data)[r]);
			break;
		case SVEC_NUM128:
			fold_i128(a, kind, ((const SiftI128 *) v->data)[r]);
			break;
		case SVEC_F64:
			fold_f64(p, a, kind, ((const double *) v->data)[r]);
			break;
		default:
			SIFT_INVARIANT(false, "filter pivot: aggregate over a vector of type %d", (int) v->type);
	}
}

static void
pivot_distinct(SiftPivot *p, int j, const SiftBatchSel *fbs, const uint32 *gid, const SiftVec *v, const char *slab)
{
	SiftDistinct *d = p->dist[j];

	switch (v->type)
	{
		case SVEC_I32:
			sift_distinct_add_i32(d, fbs, gid, 0, v);
			break;
		case SVEC_I64:
		case SVEC_NUM64:
			sift_distinct_add_i64(d, fbs, gid, 0, v);
			break;
		case SVEC_UUID:
			sift_distinct_add_uuid(d, fbs, gid, 0, v);
			break;
		case SVEC_STRVIEW:
			sift_distinct_add_str(d, fbs, gid, 0, v, slab);
			break;
		default:
			SIFT_INVARIANT(false, "filter pivot: COUNT(DISTINCT) over a vector of type %d", (int) v->type);
	}
}

/* the rows of the batch that the clause (or no clause) and the NULLs of the argument leave, as a batch of their own */
static void
pivot_sub_batch(SiftBatchSel *out, const SiftBatchSel *bs, const uint64 *active, const uint64 *clause, const SiftVec *arg)
{
	uint32		n = 0;

	memset(out, 0, sizeof(*out));
	out->n_raw = bs->n_raw;
	for (uint32 w = 0; w < (uint32) ((bs->n_raw + 63) / 64); w++)
	{
		uint64		bits = active[w] & (clause != NULL ? clause[w] : ~UINT64_C(0));

		if (arg != NULL && !arg->all_valid)
			bits &= arg->valid[w];
		out->mask[w] = bits;
		n += (uint32) sift_pc64(bits);
	}
	out->n_active = (uint16) n;
}

void
sift_pivot_update(SiftPivot *p, const SiftBatchSel *bs, const uint32 *gid, SiftVec *const *args, const char *slab, SiftPivotEval eval, void *eval_ctx)
{
	uint32		nwords = (uint32) ((bs->n_raw + 63) / 64);
	uint32		maxgid = 0;
	const uint64 *active;
	SiftBatchIter it;
	uint32		r;

	p->st.batches++;
	if (gid != NULL)
	{
		sift_batch_iter_init(&it, bs);
		while (sift_batch_iter_next(&it, &r))
			maxgid = Max(maxgid, gid[r]);
	}
	pivot_groups(p, maxgid + 1);
	sift_fmasks_eval(p->fm, bs, eval, eval_ctx);
	p->st.filter_evals += (uint64) p->nfilters;
	active = sift_fmasks_active(p->fm);

	if (gid == NULL)
	{
		for (int j = 0; j < p->naggs; j++)
		{
			const SiftPivotAgg *a = &p->agg[j];
			const SiftVec *v = a->arg >= 0 ? args[a->arg] : NULL;
			PivotAcc   *acc = &p->acc[j];
			const uint64 *clause = a->filter >= 0 ? sift_fmasks_bits(p->fm, a->filter) : NULL;

			if (a->kind == SFP_COUNT_DISTINCT)
			{
				pivot_sub_batch(p->sub, bs, active, clause, v);
				pivot_distinct(p, j, p->sub, NULL, v, slab);
				continue;
			}
			for (uint32 w = 0; w < nwords; w++)
			{
				uint64		bits = active[w] & (clause != NULL ? clause[w] : ~UINT64_C(0));

				if (v != NULL && !v->all_valid)
					bits &= v->valid[w];
				if (a->kind == SFP_COUNT_STAR || a->kind == SFP_COUNT)
				{
					acc->n += (uint64) sift_pc64(bits);
					continue;
				}
				while (bits != 0)
				{
					fold_row(p, acc, a->kind, v, w * 64 + (uint32) __builtin_ctzll(bits));
					bits &= bits - 1;
				}
			}
		}
		return;
	}

	p->st.row_masks++;
	{
		const uint64 *rowmask = sift_fmasks_rows(p->fm);

		sift_batch_iter_init(&it, bs);
		while (sift_batch_iter_next(&it, &r))
		{
			uint64		m = rowmask[r];
			PivotAcc   *row = &p->acc[(size_t) gid[r] * (uint32) p->naggs];

			for (int j = 0; j < p->naggs; j++)
			{
				const SiftPivotAgg *a = &p->agg[j];
				const SiftVec *v = a->arg >= 0 ? args[a->arg] : NULL;

				if (a->filter >= 0 && !((m >> a->filter) & 1))
					continue;
				if (v != NULL && !sift_vec_valid_at(v, r))
					continue;
				if (a->kind == SFP_COUNT_STAR || a->kind == SFP_COUNT)
					row[j].n++;
				else if (a->kind != SFP_COUNT_DISTINCT)
					fold_row(p, &row[j], a->kind, v, r);
			}
		}
	}
	for (int j = 0; j < p->naggs; j++)
	{
		const SiftPivotAgg *a = &p->agg[j];

		if (a->kind != SFP_COUNT_DISTINCT)
			continue;
		pivot_sub_batch(p->sub, bs, active, a->filter >= 0 ? sift_fmasks_bits(p->fm, a->filter) : NULL, args[a->arg]);
		pivot_distinct(p, j, p->sub, gid, args[a->arg], slab);
	}
}

void
sift_pivot_finish(SiftPivot *p)
{
	for (int j = 0; j < p->naggs; j++)
		if (p->dist[j] != NULL)
			sift_distinct_finish(p->dist[j]);
}

uint64
sift_pivot_count(const SiftPivot *p, uint32 group, int agg)
{
	if (p->agg[agg].kind == SFP_COUNT_DISTINCT)
		return sift_distinct_count(p->dist[agg], group);
	return group < p->ngroups ? p->acc[(size_t) group * (uint32) p->naggs + (uint32) agg].n : 0;
}

bool
sift_pivot_get(const SiftPivot *p, uint32 group, int agg, SiftPivotResult *out)
{
	const PivotAcc *a;

	memset(out, 0, sizeof(*out));
	if (group >= p->ngroups)
		return false;
	a = &p->acc[(size_t) group * (uint32) p->naggs + (uint32) agg];
	out->n = a->n;
	out->w.lo = (uint64) a->sum;
	out->w.hi = (int64) (a->sum >> 64);
	out->w2.lo = (uint64) a->ext;
	out->w2.hi = (int64) (a->ext >> 64);
	out->i = a->i;
	out->f = a->f;
	switch (p->agg[agg].kind)
	{
		case SFP_COUNT_STAR:
		case SFP_COUNT:
			return true;
		case SFP_COUNT_DISTINCT:
			out->n = sift_distinct_count(p->dist[agg], group);
			return true;
		default:
			return a->n > 0;
	}
}

bool
sift_pivot_float_overflow(const SiftPivot *p)
{
	return p->float_overflow;
}

const SiftPivotStats *
sift_pivot_stats(const SiftPivot *p)
{
	return &p->st;
}

void
sift_pivot_free(SiftPivot *p)
{
	for (int j = 0; j < p->naggs; j++)
		if (p->dist[j] != NULL)
			sift_distinct_free(p->dist[j]);
	if (p->acc != NULL)
		pfree(p->acc);
	pfree(p->dist);
	pfree(p->agg);
	sift_fmasks_free(p->fm);
	pfree(p->sub);
	pfree(p);
}
