/*
 * agg/plan/card_est.c - the estimator of agg/plan/card_est.h.
 */
#include "postgres.h"

#include <math.h>

#include "agg/plan/card_est.h"

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
eq_sel(const SiftColCardStats *cs, uint64 v, bool is_hash, double *conf)
{
	double		freq;

	*conf = 1.0;
	if (!is_hash && (cs->flags & SIFT_CSTAT_HAS_RANGE) && ((int64) v < cs->min_i64 || (int64) v > cs->max_i64))
		return 0;
	if (mcv_find(cs, v, is_hash, &freq))
		return freq;
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

static Single
single_in(const SiftCardPred *p, const SiftColCardStats *cs)
{
	Single		r = {0, 1.0, false};

	if (p->unknown_len)
	{
		const double ndv = (cs->flags & SIFT_CSTAT_HAS_NDV) ? Max(cs->ndv_est, 1.0) : 0;

		r.sel = ndv > 0 ? Min(PARAM_IN_MAX, ndv * PARAM_IN_FRACTION) / ndv : DEFAULT_KEY_SEL;
		r.conf = ndv > 0 ? 0.7 : CONF_DEFAULT;
		return r;
	}
	for (uint32 i = 0; i < p->nvals; i++)
	{
		bool		dup = false;
		double		conf;

		for (uint32 j = 0; j < i && !dup; j++)
			dup = p->vals[j] == p->vals[i];
		if (!dup)
		{
			r.sel += eq_sel(cs, p->vals[i], p->is_hash, &conf);
			r.conf = Min(r.conf, conf);
		}
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
single_of(const SiftCardStatsView *s, const SiftCardPred *p)
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
			return single_in(p, cs);
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

static double
zone_sel(const SiftCardPred *p, const SiftCardZone *z, uint32 nrows, double single)
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
				bool		any = false;

				for (uint32 i = 0; i < p->nvals && !any; i++)
					any = !outside(z, (int64) p->vals[i], (int64) p->vals[i]);
				if (!any)
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

void
sift_card_estimate(const SiftCardStatsView *s, const SiftCardQuery *q, SiftCardEst *out)
{
	const double n_rows = Max((double) s->total_live_rows, 1.0);
	const uint32 np = Min(q->npreds, SIFT_CARD_MAX_PREDS);
	Single		sg[SIFT_CARD_MAX_PREDS];
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
		sg[i] = single_of(s, &q->preds[i]);
		zero |= sg[i].exact_zero;
		prod *= sg[i].sel;
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
					wall = 0;

		for (uint32 g = 0; g < s->n_sample; g++)
		{
			const SiftSampleGranule *sgr = &s->sample[g];
			const double w = (double) sgr->weight,
						rows = (double) sgr->nrows * w;
			double		pj[SIFT_CARD_MAX_PREDS];
			double		pg = sgr->part_idx < s->nparts ? part_fraction(q, &s->parts[sgr->part_idx]) : 1.0;

			for (uint32 i = 0; i < np && pg > 0; i++)
			{
				const SiftCardPred *pr = &q->preds[i];

				pj[i] = pr->col < s->ncols && pr->kind != SCP_SCOPE_IN ?
					zone_sel(pr, &s->sample_zones[(size_t) pr->col * s->n_sample + g], sgr->nrows, sg[i].sel) : sg[i].sel;
				if (pj[i] <= 0)
					pg = 0;
			}
			if (pg > 0)
				pg *= backoff(pj, (int) np);
			num += rows * pg;
			den += rows;
			wsurv += pg > 0 ? w : 0;
			wall += w;
		}
		s_joint = den > 0 ? num / den : 1.0;
		frac_gran = wall > 0 ? wsurv / wall : 1.0;
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
		uint64		sum = 0;

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
