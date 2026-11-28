/*
 * agg/cost/card_est.c - the estimator of agg/cost/card_est.h.
 */
#include "postgres.h"

#include <math.h>

#include "agg/cost/card_est.h"

#define ST_SORT sort_i64
#define ST_ELEMENT_TYPE int64
#define ST_COMPARE(a, b) (*(a) < *(b) ? -1 : (*(a) > *(b) ? 1 : 0))
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

#define ST_SORT sort_u64
#define ST_ELEMENT_TYPE uint64
#define ST_COMPARE(a, b) (*(a) < *(b) ? -1 : (*(a) > *(b) ? 1 : 0))
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

#define DEFAULT_EQ_SEL			0.005
#define DEFAULT_KEY_SEL			0.01
#define DEFAULT_RANGE_SEL		0.3333
#define DEFAULT_PREFIX_SEL		0.05
#define PARAM_IN_FRACTION		0.05
#define PARAM_IN_MAX			10.0
#define MIN_SELECTIVITY			1e-9
#define CONF_DEFAULT			0.3
#define CONF_LINEAR				0.7
#define CONF_NO_SAMPLE			0.8
#define NDV_DAMPING				0.75

#define VAL_STACK		256

/* the values of an IN list, sorted and distinct (signed order, unsigned for hashes): the list itself when it already is, else a copy in the stack or the heap */
typedef struct PredVals
{
	const uint64 *vals;
	uint32		n;
} PredVals;

typedef struct ValArena
{
	uint64		stack[VAL_STACK];
	uint32		used;
	int			nheap;
	uint64	   *heap[SIFT_CARD_MAX_PREDS];
} ValArena;

/* the share of a predicate alone: sel, how far to trust it, and whether it is exactly 0 from what was counted */
typedef struct Single
{
	double		sel;
	double		conf;
	bool		exact_zero;
} Single;

static inline double
clamp01(double x)
{
	return x < 0 ? 0 : x > 1 ? 1 : x;
}

static inline double
frac_of(uint64 part, uint64 whole)
{
	return whole ? clamp01((double) part / (double) whole) : 0;
}

static double
null_frac(const SiftColCardStats *cs)
{
	return frac_of(cs->null_rows, cs->total_rows);
}

static double
mcv_sum(const SiftColCardStats *cs)
{
	double		sum = 0;

	if (cs->flags & SIFT_CSTAT_HAS_MCV)
		for (int i = 0; i < cs->n_mcv; i++)
			sum += cs->mcvs[i].freq;
	return sum;
}

static bool
mcv_find(const SiftColCardStats *cs, uint64 v, bool is_hash, double *freq)
{
	if (cs->flags & SIFT_CSTAT_HAS_MCV)
		for (int i = 0; i < cs->n_mcv; i++)
			if (cs->mcvs[i].val_or_hash == v && ((cs->mcvs[i].flags & SIFT_MCV_IS_HASH) != 0) == is_hash)
			{
				*freq = cs->mcvs[i].freq;
				return true;
			}
	return false;
}

/* a value that is none of the common ones: what the common ones and the NULLs leave, over the distinct values that are not common */
static double
eq_rest(const SiftColCardStats *cs, double *conf)
{
	*conf = 1.0;
	if (cs->flags & SIFT_CSTAT_HAS_NDV)
	{
		const double rest = Max(0.0, 1.0 - mcv_sum(cs) - null_frac(cs));
		double		sel = rest / Max(cs->ndv_est - cs->n_mcv, 1.0);

		if (cs->n_mcv == SIFT_CARD_MCV_MAX)
			sel = Min(sel, cs->mcvs[cs->n_mcv - 1].freq);
		return sel;
	}
	*conf = CONF_DEFAULT;
	return DEFAULT_EQ_SEL;
}

static bool
out_of_range(const SiftColCardStats *cs, uint64 v, bool is_hash)
{
	return !is_hash && (cs->flags & SIFT_CSTAT_HAS_RANGE) && ((int64) v < cs->min_i64 || (int64) v > cs->max_i64);
}

static double
eq_sel(const SiftColCardStats *cs, uint64 v, bool is_hash, double *conf)
{
	double		freq;

	*conf = 1.0;
	if (out_of_range(cs, v, is_hash))
		return 0;
	if (mcv_find(cs, v, is_hash, &freq))
		return freq;
	return eq_rest(cs, conf);
}

static Single
single_eq(const SiftCardPred *p, const SiftColCardStats *cs)
{
	Single		r = {0, 1.0, false};

	if (p->dict_absent)
	{
		r.exact_zero = true;
		return r;
	}
	r.sel = eq_sel(cs, p->vals[0], p->is_hash, &r.conf);
	r.exact_zero = r.sel == 0 && r.conf == 1.0;
	return r;
}

static bool
vals_has(const PredVals *pv, uint32 lo, uint32 hi, uint64 v, bool is_hash)
{
	const uint32 end = hi;

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;
		const bool	below = is_hash ? pv->vals[mid] < v : (int64) pv->vals[mid] < (int64) v;

		if (below)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < end && pv->vals[lo] == v;
}

/* the first index whose value is not below the signed x */
static uint32
vals_lower_bound(const PredVals *pv, int64 x)
{
	uint32		lo = 0,
				hi = pv->n;

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if ((int64) pv->vals[mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* the values of the sorted distinct list inside the range of the leaf: [first, end) */
static void
in_span(const SiftColCardStats *cs, const SiftCardPred *p, const PredVals *pv, uint32 *first, uint32 *end)
{
	*first = 0;
	*end = pv->n;
	if (!p->is_hash && (cs->flags & SIFT_CSTAT_HAS_RANGE))
	{
		*first = vals_lower_bound(pv, cs->min_i64);
		*end = cs->max_i64 == INT64_MAX ? pv->n : vals_lower_bound(pv, cs->max_i64 + 1);
	}
}

/* the sum of the shares of the distinct values: the common ones by their own, the others by what is left, each counted once */
static Single
single_in(const SiftCardPred *p, const SiftColCardStats *cs, const PredVals *pv)
{
	Single		r = {0, 1.0, false};
	uint32		first,
				end,
				hits = 0;

	if (p->unknown_len)
	{
		const double ndv = (cs->flags & SIFT_CSTAT_HAS_NDV) ? Max(cs->ndv_est, 1.0) : 0;

		r.sel = ndv > 0 ? Min(PARAM_IN_MAX, ndv * PARAM_IN_FRACTION) / ndv : DEFAULT_KEY_SEL;
		r.conf = ndv > 0 ? 0.7 : CONF_DEFAULT;
		return r;
	}
	in_span(cs, p, pv, &first, &end);
	if (cs->flags & SIFT_CSTAT_HAS_MCV)
		for (int i = 0; i < cs->n_mcv; i++)
			if (((cs->mcvs[i].flags & SIFT_MCV_IS_HASH) != 0) == p->is_hash && vals_has(pv, first, end, cs->mcvs[i].val_or_hash, p->is_hash))
			{
				r.sel += cs->mcvs[i].freq;
				hits++;
			}
	if (end - first > hits)
	{
		double		conf;

		r.sel += (end - first - hits) * eq_rest(cs, &conf);
		r.conf = Min(r.conf, conf);
	}
	r.sel = Min(r.sel, 1.0 - null_frac(cs));
	r.exact_zero = r.sel == 0 && r.conf == 1.0;
	return r;
}

/* the fraction of the rows that are not NULL and at most x: the fences are equal-depth quantiles, a bucket whose fences are both at most x counts whole */
static double
hist_cdf_le(const SiftColCardStats *cs, int64 x)
{
	const int	nb = cs->n_hist;
	double		whole = 0;

	if (x < cs->hist_bounds[0])
		return 0;
	if (x >= cs->hist_bounds[nb])
		return 1;
	for (int b = 0; b < nb; b++)
	{
		const int64 a = cs->hist_bounds[b],
					c = cs->hist_bounds[b + 1];

		if (x >= c)
			whole += 1;
		else if (x >= a)
			return (whole + (c > a ? (double) (x - a) / (double) (c - a) : 1.0)) / nb;
	}
	return whole / nb;
}

/* every distinct value is a common one: a range is then the sum of the values in it */
static bool
mcv_complete(const SiftColCardStats *cs)
{
	return (cs->flags & SIFT_CSTAT_HAS_MCV) && (cs->flags & SIFT_CSTAT_HAS_NDV) && cs->n_mcv >= cs->ndv_est && cs->n_mcv > 0;
}

static Single
single_range(const SiftCardPred *p, const SiftColCardStats *cs)
{
	Single		r = {DEFAULT_RANGE_SEL, CONF_DEFAULT, false};
	int64		lo = p->has_lo ? p->lo : INT64_MIN,
				hi = p->has_hi ? p->hi : INT64_MAX;
	const double nonnull = 1.0 - null_frac(cs);

	if (!(cs->flags & SIFT_CSTAT_HAS_RANGE))
		return r;
	r.conf = 1.0;
	if (hi < cs->min_i64 || lo > cs->max_i64 || lo > hi)
	{
		r.sel = 0;
		r.exact_zero = true;
		return r;
	}
	lo = Max(lo, cs->min_i64);
	hi = Min(hi, cs->max_i64);
	if (mcv_complete(cs))
	{
		r.sel = 0;
		for (int i = 0; i < cs->n_mcv; i++)
			if ((int64) cs->mcvs[i].val_or_hash >= lo && (int64) cs->mcvs[i].val_or_hash <= hi)
				r.sel += cs->mcvs[i].freq;
	}
	else if (cs->flags & SIFT_CSTAT_HAS_HIST)
		r.sel = nonnull * Max(0.0, hist_cdf_le(cs, hi) - (lo > INT64_MIN ? hist_cdf_le(cs, lo - 1) : 0.0));
	else
	{
		r.sel = nonnull * (cs->max_i64 > cs->min_i64 ? ((double) hi - (double) lo + 1.0) / ((double) cs->max_i64 - (double) cs->min_i64 + 1.0) : 1.0);
		r.conf = CONF_LINEAR;
	}
	r.sel = clamp01(r.sel);
	return r;
}

static Single
single_of(const SiftCardStatsView *s, const SiftCardPred *p, const PredVals *pv)
{
	const SiftColCardStats *cs = p->col < s->ncols ? &s->cols[p->col] : NULL;
	Single		r = {1.0, 1.0, false};

	if (p->kind == SCP_SCOPE_IN)
	{
		bool		exact;
		const double rows = sift_card_scope_rows(s, p->vals, p->nvals, &exact);

		r.sel = frac_of((uint64) rows, s->total_live_rows);
		r.conf = exact ? 1.0 : 0.7;
		r.exact_zero = exact && rows == 0;
		return r;
	}
	if (!cs)
	{
		r.sel = DEFAULT_EQ_SEL;
		r.conf = CONF_DEFAULT;
		return r;
	}
	switch (p->kind)
	{
		case SCP_IS_NULL:
			r.sel = null_frac(cs);
			r.exact_zero = cs->null_rows == 0;
			return r;
		case SCP_IS_NOT_NULL:
			r.sel = 1.0 - null_frac(cs);
			r.exact_zero = cs->null_rows == cs->total_rows;
			return r;
		case SCP_KEY_EXISTS:
		case SCP_KEY_ABSENT:
			if (!(cs->flags & SIFT_CSTAT_HAS_KEY_PRESENT))
			{
				r.sel = p->kind == SCP_KEY_EXISTS ? DEFAULT_KEY_SEL : 1.0 - DEFAULT_KEY_SEL;
				r.conf = CONF_DEFAULT;
				return r;
			}
			r.sel = p->kind == SCP_KEY_EXISTS ? frac_of(cs->key_present_rows, cs->total_rows) :
				frac_of(cs->total_rows - cs->null_rows - Min(cs->key_present_rows, cs->total_rows - cs->null_rows), cs->total_rows);
			r.exact_zero = r.sel == 0;
			return r;
		case SCP_EQ:
			return single_eq(p, cs);
		case SCP_IN:
			return single_in(p, cs, pv);
		case SCP_RANGE:
			return single_range(p, cs);
		default:
			if (p->dict_n == 0)
			{
				r.sel = DEFAULT_PREFIX_SEL;
				r.conf = CONF_DEFAULT;
			}
			else
			{
				r.sel = clamp01((double) (p->dict_hi - p->dict_lo + 1) / p->dict_n) * (1.0 - null_frac(cs));
				r.exact_zero = p->dict_hi < p->dict_lo;
			}
			return r;
	}
}

/* ---- the share of a predicate on one granule: what its zone proves, else the share of the predicate alone ---- */

static bool
outside(const SiftCardZone *z, int64 lo, int64 hi)
{
	return hi < z->min || lo > z->max;
}

/*
 * key = ANY ($1): the chance that the granule holds one of the values of a list the planner does not know, taken as single_in's count of distinct values of the leaf drawn at
 * random: a zone that spans d of them holds one with 1 - (1 - d / ndv)^k
 */
static double
param_list_hit(const SiftColCardStats *cs, const SiftCardZone *z, uint32 nrows)
{
	const double ndv = (cs->flags & SIFT_CSTAT_HAS_NDV) ? Max(cs->ndv_est, 1.0) : 0;
	double		d;

	if (z->nnull >= nrows)
		return 0;
	if (ndv == 0 || !(cs->flags & SIFT_CSTAT_HAS_RANGE))
		return 1;
	d = ((double) z->max - (double) z->min + 1.0) * ndv / Max((double) cs->max_i64 - (double) cs->min_i64 + 1.0, 1.0);
	d = Min(Max(d, 1.0), Min(ndv, (double) nrows));
	return 1.0 - pow(1.0 - d / ndv, Min(PARAM_IN_MAX, ndv * PARAM_IN_FRACTION));
}

/* the zone tells the predicate of every row of the granule: one value and no NULL for a comparison, every document or none for a key test */
static bool
zone_decides(const SiftCardPred *p, const SiftCardZone *z, uint32 nrows)
{
	switch (p->kind)
	{
		case SCP_EQ:
		case SCP_IN:
		case SCP_RANGE:
			return z->nnull == 0 && z->min == z->max;
		case SCP_IS_NULL:
		case SCP_IS_NOT_NULL:
			return z->nnull == 0 || z->nnull == nrows;
		case SCP_KEY_EXISTS:
		case SCP_KEY_ABSENT:
			return (z->flags & (SIFT_CZONE_KEY_ALL | SIFT_CZONE_KEY_NONE)) != 0;
		default:
			return false;
	}
}

static double
zone_sel(const SiftCardPred *p, const PredVals *pv, const SiftCardZone *z, uint32 nrows, double single)
{
	const uint32 nonnull = nrows > z->nnull ? nrows - z->nnull : 0;
	const double nn_frac = frac_of(nonnull, nrows);

	switch (p->kind)
	{
		case SCP_IS_NULL:
			return 1.0 - nn_frac;
		case SCP_IS_NOT_NULL:
			return nn_frac;
		case SCP_KEY_EXISTS:
			return (z->flags & SIFT_CZONE_KEY_NONE) ? 0 : (z->flags & SIFT_CZONE_KEY_ALL) ? 1 : single;
		case SCP_KEY_ABSENT:
			return (z->flags & SIFT_CZONE_KEY_ALL) ? 0 : (z->flags & SIFT_CZONE_KEY_NONE) ? nn_frac : single;
		case SCP_EQ:
		case SCP_IN:
			if (nonnull == 0)
				return 0;
			if (!p->is_hash && !p->unknown_len)
			{
				const uint32 at = vals_lower_bound(pv, z->min);

				if (at == pv->n || (int64) pv->vals[at] > z->max)
					return 0;
				if (p->nvals == 1 && z->min == z->max && z->nnull == 0)
					return 1;
			}
			return single;
		case SCP_RANGE:
			{
				const int64 lo = p->has_lo ? p->lo : INT64_MIN,
							hi = p->has_hi ? p->hi : INT64_MAX;

				if (nonnull == 0 || outside(z, lo, hi))
					return 0;
				if (lo <= z->min && hi >= z->max)
					return nn_frac;
				return nn_frac * Max(((double) Min(hi, z->max) - (double) Max(lo, z->min) + 1.0) / ((double) z->max - (double) z->min + 1.0), 1.0 / nrows);
			}
		default:
			return single;
	}
}

/* the shares most selective first, each under the square root of the one before */
static double
backoff(double *p, int n)
{
	double		r = 1.0,
				e = 1.0;

	for (int i = 1; i < n; i++)
	{
		const double x = p[i];
		int			j = i - 1;

		for (; j >= 0 && p[j] > x; j--)
			p[j + 1] = p[j];
		p[j + 1] = x;
	}
	for (int i = 0; i < n; i++, e /= 2)
		r *= e == 1.0 ? p[i] : pow(p[i], e);
	return r;
}

/* the fraction of a partition the window leaves (the bounds are inclusive integers) */
static double
part_fraction(const SiftCardQuery *q, const SiftCardPartition *pe)
{
	int64		lo,
				hi;

	if (!q->has_window)
		return 1.0;
	if (q->win_hi < pe->lo || q->win_lo > pe->hi)
		return 0;
	lo = Max(q->win_lo, pe->lo);
	hi = Min(q->win_hi, pe->hi);
	return pe->hi > pe->lo ? clamp01(((double) hi - (double) lo + 1.0) / ((double) pe->hi - (double) pe->lo + 1.0)) : 1.0;
}

static double
skip_factor(const SiftCardStatsView *s, const SiftCardPred *preds, const Single *sg, uint32 n, double rows_per_granule)
{
	double		f = 1.0;

	for (uint32 i = 0; i < n; i++)
		if ((preds[i].kind == SCP_EQ || preds[i].kind == SCP_IN) && preds[i].col < s->ncols && (s->cols[preds[i].col].flags & SIFT_CSTAT_HAS_SKIP))
			f *= clamp01(1.0 - exp(-sg[i].sel * rows_per_granule));
	return f;
}

static bool
vals_ordered(const SiftCardPred *p)
{
	for (uint32 i = 1; i < p->nvals; i++)
		if (p->is_hash ? p->vals[i - 1] >= p->vals[i] : (int64) p->vals[i - 1] >= (int64) p->vals[i])
			return false;
	return true;
}

static PredVals
pred_vals(ValArena *a, const SiftCardPred *p)
{
	PredVals	r = {p->vals, p->nvals};
	uint64	   *w;
	uint32		n = 1;

	if ((p->kind != SCP_EQ && p->kind != SCP_IN) || p->unknown_len || p->nvals < 2 || vals_ordered(p))
		return r;
	if (a->used + p->nvals <= VAL_STACK)
		w = a->stack + a->used;
	else
		w = a->heap[a->nheap++] = palloc(p->nvals * sizeof(uint64));	/* check-memory: allow the values of one IN list of the query */
	memcpy(w, p->vals, p->nvals * sizeof(uint64));
	if (p->is_hash)
		sort_u64(w, p->nvals);
	else
		sort_i64((int64 *) w, p->nvals);
	for (uint32 i = 1; i < p->nvals; i++)
		if (w[i] != w[n - 1])
			w[n++] = w[i];
	if (w >= a->stack && w < a->stack + VAL_STACK)
		a->used += p->nvals;
	r.vals = w;
	r.n = n;
	return r;
}

/* An equality on a column with no common values shares the rest out evenly: a guess that must not cap the rows the zones of the sample count where they hold that one value */
static bool
eq_share_guessed(const SiftCardStatsView *s, const SiftCardPred *p)
{
	return (p->kind == SCP_EQ || p->kind == SCP_IN) && p->col < s->ncols && !(s->cols[p->col].flags & SIFT_CSTAT_HAS_MCV);
}

static void
estimate(const SiftCardStatsView *s, const SiftCardQuery *q, SiftCardEst *out, ValArena *arena)
{
	const double n_rows = Max((double) s->total_live_rows, 1.0);
	const uint32 np = Min(q->npreds, SIFT_CARD_MAX_PREDS);
	Single		sg[SIFT_CARD_MAX_PREDS];
	PredVals	pv[SIFT_CARD_MAX_PREDS];
	double		rows_part = 0,
				prod = 1.0,
				min_sel = 1.0,
				conf = 1.0,
				s_part,
				s_joint,
				frac_gran = 1.0,
				sel;
	bool		zero = false;
	const bool	sample_full = s->n_sample > 0 && s->n_sample >= s->total_granules;

	memset(out, 0, sizeof(*out));
	out->total_live_rows = (double) s->total_live_rows;
	out->g_total = (double) s->total_granules;
	out->has_uncovered_quals = q->has_uncovered || q->npreds > np;
	for (uint32 p = 0; p < s->nparts; p++)
	{
		rows_part += part_fraction(q, &s->parts[p]) * (double) s->parts[p].rows;
	}
	if (s->nparts == 0)
		rows_part = n_rows;
	s_part = clamp01(rows_part / n_rows);
	zero = rows_part == 0 && !s->has_l0 && s->nparts > 0;
	for (uint32 i = 0; i < np; i++)
	{
		pv[i] = pred_vals(arena, &q->preds[i]);
		sg[i] = single_of(s, &q->preds[i], &pv[i]);
		zero |= sg[i].exact_zero;
		prod *= sg[i].sel;
		if (!eq_share_guessed(s, &q->preds[i]))
			min_sel = Min(min_sel, sg[i].sel);
		conf = Min(conf, sg[i].conf);
		out->used_scope_l2 |= q->preds[i].kind == SCP_SCOPE_IN && sg[i].conf == 1.0;
	}
	out->confidence = conf;
	if (zero)
	{
		out->exact_zero = true;
		out->est_rows = 1.0;
		out->selectivity = Max(1.0 / n_rows, MIN_SELECTIVITY);
		out->confidence = 1.0;
		return;
	}
	if (np == 0)
	{
		s_joint = s_part;
		frac_gran = s_part;
	}
	else if (s->n_sample > 0)
	{
		double		num = 0,
					den = 0,
					wsurv = 0,
					wall = 0,
					kept = 0,
					dec[SIFT_CARD_MAX_PREDS] = {0};

		for (uint32 g = 0; g < s->n_sample; g++)
		{
			const SiftSampleGranule *sgr = &s->sample[g];
			const double w = (double) sgr->weight,
						rows = (double) sgr->nrows * w;
			double		pj[SIFT_CARD_MAX_PREDS];
			double		pg = sgr->part_idx < s->nparts ? part_fraction(q, &s->parts[sgr->part_idx]) : 1.0;
			double		hit = 1.0;

			for (uint32 i = 0; i < np && pg > 0; i++)
			{
				const SiftCardPred *pr = &q->preds[i];

				pj[i] = pr->col < s->ncols && pr->kind != SCP_SCOPE_IN ?
					zone_sel(pr, &pv[i], &s->sample_zones[(size_t) pr->col * s->n_sample + g], sgr->nrows, sg[i].sel) : sg[i].sel;
				if (pj[i] <= 0)
					pg = 0;
			}
			if (pg > 0)
				pg *= backoff(pj, (int) np);
			for (uint32 i = 0; i < np && pg > 0; i++)
				if (q->preds[i].kind == SCP_IN && q->preds[i].unknown_len && q->preds[i].col < s->ncols)
					hit *= param_list_hit(&s->cols[q->preds[i].col], &s->sample_zones[(size_t) q->preds[i].col * s->n_sample + g], sgr->nrows);
			for (uint32 i = 0; i < np && pg > 0; i++)
				if (q->preds[i].col < s->ncols && zone_decides(&q->preds[i], &s->sample_zones[(size_t) q->preds[i].col * s->n_sample + g], sgr->nrows))
					dec[i] += rows * hit;
			kept += pg > 0 ? rows * hit : 0;
			num += rows * pg;
			den += rows;
			wsurv += pg > 0 ? w * hit : 0;
			wall += w;
		}
		s_joint = den > 0 ? num / den : 1.0;
		frac_gran = wall > 0 ? wsurv / wall : 1.0;
		for (uint32 i = 0; i < np; i++)
			out->decided[i] = kept > 0 ? dec[i] / kept : 0;
		if (s_joint == 0 && sample_full)
		{
			out->exact_zero = true;
			out->est_rows = 1.0;
			out->selectivity = Max(1.0 / n_rows, MIN_SELECTIVITY);
			out->confidence = 1.0;
			return;
		}
		s_joint = Min(s_joint, Min(min_sel, s_part));
		s_joint = Max(s_joint, s_part * prod);
	}
	else
	{
		double		p[SIFT_CARD_MAX_PREDS];

		for (uint32 i = 0; i < np; i++)
			p[i] = sg[i].sel;
		s_joint = s_part * backoff(p, (int) np);
		frac_gran = s_part;
		if (np > 1)
			out->confidence = Min(out->confidence, CONF_NO_SAMPLE);
	}
	sel = Max(s_joint, 1.0 / n_rows);
	out->est_rows = Max(1.0, sel * n_rows);
	out->selectivity = clamp01(Max(out->est_rows / n_rows, MIN_SELECTIVITY));
	out->g_eff = out->g_total * Min(s_part, frac_gran) * skip_factor(s, q->preds, sg, np, n_rows / Max(out->g_total, 1.0));
	for (uint32 i = 0; i < np; i++)
		if (q->preds[i].col < s->ncols && q->preds[i].kind != SCP_SCOPE_IN)
			out->b_pred_comp += s->cols[q->preds[i].col].avg_comp_bytes_row * n_rows * Min(s_part, frac_gran);
	for (uint32 i = 0; i < q->npayload; i++)
		if (q->payload_cols[i] < s->ncols)
			out->b_pay_dec += s->cols[q->payload_cols[i]].avg_dec_bytes_row * out->est_rows;
	if (q->lead_col >= 0 && (uint32) q->lead_col < s->ncols)
		out->key_correlation = clamp01(fabs(s->cols[q->lead_col].zone_monotonicity) * (1.0 - clamp01(s->cols[q->lead_col].zone_overlap_ratio)));
}

void
sift_card_estimate(const SiftCardStatsView *s, const SiftCardQuery *q, SiftCardEst *out)
{
	ValArena	arena;

	arena.used = 0;
	arena.nheap = 0;
	estimate(s, q, out, &arena);
	while (arena.nheap > 0)
		pfree(arena.heap[--arena.nheap]);
}

double
sift_card_mean(const SiftColCardStats *cs)
{
	double		sum = 0;

	if (!(cs->flags & SIFT_CSTAT_HAS_RANGE))
		return -1;
	if (!(cs->flags & SIFT_CSTAT_HAS_HIST) || cs->n_hist == 0)
		return ((double) cs->min_i64 + (double) cs->max_i64) / 2.0;
	for (int b = 0; b < cs->n_hist; b++)
		sum += ((double) cs->hist_bounds[b] + (double) cs->hist_bounds[b + 1]) / 2.0;
	return sum / cs->n_hist;
}

double
sift_card_ndv(double d, double n, double r_out)
{
	const double r = Min(r_out, n);

	if (d <= 1.0 || r <= 0)
		return Min(d, Max(r, 0.0));
	if (r >= n)
		return d;
	return Min(Min(d, r), d * (1.0 - exp((n / d) * log1p(-r / n))));
}

double
sift_card_ndv_multi(const double *d, int nkeys, double r_out, double cells)
{
	double		best = 0;
	int			imax = 0;
	double		prod = 1.0;

	for (int i = 0; i < nkeys; i++)
		if (d[i] > d[imax])
			imax = i;
	best = d[imax];
	for (int i = 0; i < nkeys; i++)
		if (i != imax)
			prod *= pow(Max(d[i], 1.0), NDV_DAMPING);
	best = Min(r_out, best * prod);
	return cells > 0 ? Min(best, cells) : best;
}

double
sift_card_scope_rows(const SiftCardStatsView *s, const uint64 *keys, uint32 nkeys, bool *exact)
{
	double		rows = 0;

	*exact = nkeys <= SIFT_CARD_SCOPE_PROBES;
	if (!*exact)
	{
		uint64		sum = s->scope_total_rows;

		if (sum == 0)
			for (uint32 i = 0; i < s->n_scope; i++)
				sum += s->scope[i].nrows;
		return s->n_scope ? (double) sum / s->n_scope * nkeys : 0;
	}
	for (uint32 k = 0; k < nkeys; k++)
	{
		uint32		lo = 0,
					hi = s->n_scope;

		while (lo < hi)
		{
			const uint32 mid = lo + (hi - lo) / 2;

			if (s->scope[mid].key < (int64) keys[k])
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo < s->n_scope && s->scope[lo].key == (int64) keys[k])
			rows += (double) s->scope[lo].nrows;
	}
	return rows;
}
