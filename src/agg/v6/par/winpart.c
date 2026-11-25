/*
 * agg/v6/par/winpart.c - the partitions of agg/v6/par/winpart.h.
 */
#include "postgres.h"

#include <math.h>

#include "agg/kern6/int.h"
#include "agg/v6/par/winpart.h"
#include "agg/v6/win/datebin.h"

typedef unsigned __int128 u128;

typedef struct Part
{
	const SiftParWinSpec *sp;
	const SiftParWinItem *it;
	uint32		lo;
	uint32		hi;
} Part;

bool
sift_par_win_func_ok(const SiftWinFuncSpec *fn)
{
	const bool	flt = fn->arg_type == SVEC_F64;

	switch (fn->kind)
	{
		case SWIN_ROW_NUMBER:
		case SWIN_RANK:
		case SWIN_DENSE_RANK:
		case SWIN_COUNT_STAR:
			return true;
		case SWIN_LAG:
		case SWIN_LEAD:
			return fn->offset <= SIFT_WIN_MAX_OFFSET && (flt || fn->arg_type == SVEC_I32 || fn->arg_type == SVEC_I64);
		case SWIN_AVG:
			return flt || fn->arg_type == SVEC_I32;
		default:
			return flt || fn->arg_type == SVEC_I32 || fn->arg_type == SVEC_I64;
	}
}

static inline double
bits_f8(int64 v)
{
	double		d;

	memcpy(&d, &v, sizeof(d));
	return d;
}

static inline int
key_cmp(const Part *p, int k, uint32 a, uint32 b)
{
	const SiftParWinKey *key = &p->sp->ord[k];
	const uint8 na = p->it->ord_null[k][a],
				nb = p->it->ord_null[k][b];
	const int64 x = p->it->ord[k][a],
				y = p->it->ord[k][b];
	int			r;

	if (na | nb)
		return na == nb ? 0 : ((na != 0) == key->nulls_first ? -1 : 1);
	r = key->f64 ? ak6_f8_cmp(bits_f8(x), bits_f8(y)) : (x < y ? -1 : x > y);
	return key->desc ? -r : r;
}

static inline int
row_cmp(const Part *p, uint32 a, uint32 b)
{
	for (int k = 0; k < p->sp->n_ord; k++)
	{
		const int	r = key_cmp(p, k, a, b);

		if (r != 0)
			return r;
	}
	return 0;
}

#define RUN_LEN		8

static void
insertion_sort(const Part *p, uint32 *v, uint32 n)
{
	for (uint32 i = 1; i < n; i++)
	{
		const uint32 x = v[i];
		uint32		j = i;

		for (; j > 0 && row_cmp(p, v[j - 1], x) > 0; j--)
			v[j] = v[j - 1];
		v[j] = x;
	}
}

static void
merge(const Part *p, const uint32 *src, uint32 *dst, uint32 lo, uint32 mid, uint32 hi)
{
	uint32		i = lo,
				j = mid,
				o = lo;

	while (i < mid && j < hi)
		dst[o++] = row_cmp(p, src[j], src[i]) < 0 ? src[j++] : src[i++];
	while (i < mid)
		dst[o++] = src[i++];
	while (j < hi)
		dst[o++] = src[j++];
}

/* a stable sort of v[0 .. n): runs of RUN_LEN by insertion, then merges that double the run, between v and tmp */
static void
sort_rows(const Part *p, uint32 *v, uint32 *tmp, uint32 n)
{
	uint32	   *from = v,
			   *to = tmp;

	for (uint32 i = 0; i < n; i += RUN_LEN)
		insertion_sort(p, v + i, Min(RUN_LEN, n - i));
	for (uint32 width = RUN_LEN; width < n; width *= 2)
	{
		uint32	   *t;

		for (uint32 lo = 0; lo < n; lo += 2 * width)
			merge(p, from, to, lo, Min(lo + width, n), Min(lo + 2 * width, n));
		t = from;
		from = to;
		to = t;
	}
	if (from != v)
		memcpy(v, from, sizeof(uint32) * n);
}

static void
order_partition(const Part *p)
{
	const SiftParWinItem *it = p->it;
	bool		sorted = true;

	for (uint32 i = p->lo; i < p->hi; i++)
		it->perm[i] = i;
	for (uint32 i = p->lo + 1; sorted && p->sp->n_ord > 0 && i < p->hi; i++)
		sorted = row_cmp(p, i - 1, i) <= 0;
	if (!sorted)
		sort_rows(p, it->perm + p->lo, it->tmp + p->lo, p->hi - p->lo);
	for (uint32 o = p->lo; o < p->hi; o++)
		it->otag[o] = it->tag[it->perm[o]];
}

static void
mark_peers(const Part *p)
{
	const SiftParWinItem *it = p->it;

	it->peer[p->lo] = 1;
	for (uint32 o = p->lo + 1; o < p->hi; o++)
	{
		uint8		differs = 0;

		for (int k = 0; k < p->sp->n_ord && !differs; k++)
			differs = key_cmp(p, k, it->perm[o - 1], it->perm[o]) != 0;
		it->peer[o] = differs;
	}
}

/* ---- the functions ---- */

static inline SiftWinVal
val_of(const SiftWinFuncSpec *fn, int64 x)
{
	SiftWinVal	v;

	memset(&v, 0, sizeof(v));
	if (fn->arg_type == SVEC_F64)
		v.f = bits_f8(x);
	else
		v.i = x;
	return v;
}

static inline void
put(const Part *p, uint32 o, int f, SiftWinVal v, bool null)
{
	const SiftParWinItem *it = p->it;

	it->res[(size_t) o * p->sp->nfuncs + f] = v;
	it->res_nulls[o] = (uint16) (it->res_nulls[o] | ((uint16) null << f));
}

static inline void
put_arg(const Part *p, uint32 o, int f, uint32 row)
{
	const SiftWinFuncSpec *fn = &p->sp->func[f];
	const bool	null = p->it->arg_null[f][row];

	put(p, o, f, null ? (SiftWinVal) {0} : val_of(fn, p->it->arg[f][row]), null);
}

static inline void
put_default(const Part *p, uint32 o, int f)
{
	const SiftWinFuncSpec *fn = &p->sp->func[f];

	put(p, o, f, fn->has_default ? fn->default_val : (SiftWinVal) {0}, !fn->has_default);
}

static void
ranks(const Part *p, int f)
{
	const SiftWinFuncKind kind = p->sp->func[f].kind;
	int64		rank = 0,
				dense = 0;

	for (uint32 o = p->lo; o < p->hi; o++)
	{
		SiftWinVal	v = {0};

		if (p->it->peer[o])
		{
			rank = o - p->lo + 1;
			dense++;
		}
		v.i = kind == SWIN_ROW_NUMBER ? (int64) (o - p->lo) + 1 : kind == SWIN_RANK ? rank : dense;
		put(p, o, f, v, false);
	}
}

static void
shifted(const Part *p, int f)
{
	const SiftWinFuncSpec *fn = &p->sp->func[f];

	for (uint32 o = p->lo; o < p->hi; o++)
	{
		const uint32 at = o - p->lo;

		if (fn->kind == SWIN_FIRST_VALUE)
			put_arg(p, o, f, p->it->perm[p->lo]);
		else if (fn->offset == 0)
			put_arg(p, o, f, p->it->perm[o]);
		else if (fn->kind == SWIN_LAG)
		{
			if (at >= fn->offset)
				put_arg(p, o, f, p->it->perm[o - fn->offset]);
			else
				put_default(p, o, f);
		}
		else if ((uint64) o + fn->offset < p->hi)
			put_arg(p, o, f, p->it->perm[o + fn->offset]);
		else
			put_default(p, o, f);
	}
}

typedef struct WinFold
{
	uint64		count;
	u128		sum;
	double		sum_f;
	SiftWinVal	mm;
} WinFold;

static inline bool
overflows(double a, double b)
{
	const double r = a + b;

	return isinf(r) && !isinf(a) && !isinf(b);
}

/* false: a float8 sum that overflows */
static inline bool
fold_row(const Part *p, int f, WinFold *s, uint32 row)
{
	const SiftWinFuncSpec *fn = &p->sp->func[f];
	const bool	flt = fn->arg_type == SVEC_F64;

	if (fn->kind == SWIN_COUNT_STAR)
	{
		s->count++;
		return true;
	}
	if (p->it->arg_null[f][row])
		return true;
	if (fn->kind == SWIN_SUM || fn->kind == SWIN_AVG)
	{
		if (flt)
		{
			const double x = bits_f8(p->it->arg[f][row]);

			if (s->count && overflows(s->sum_f, x))
				return false;
			s->sum_f = s->count ? s->sum_f + x : x;
		}
		else
			s->sum += (u128) (__int128) p->it->arg[f][row];
	}
	else if (fn->kind == SWIN_MIN || fn->kind == SWIN_MAX)
	{
		const SiftWinVal x = val_of(fn, p->it->arg[f][row]);
		const bool	mn = fn->kind == SWIN_MIN;

		if (!s->count)
			s->mm = x;
		else if (flt)
		{
			const int	c = ak6_f8_cmp(s->mm.f, x.f);

			if (mn ? !(c < 0) : !(c > 0))
				s->mm = x;
		}
		else if (mn ? x.i < s->mm.i : x.i > s->mm.i)
			s->mm = x;
	}
	s->count++;
	return true;
}

/* the average of integers as numeric_div gives it; false when it needs more than the 95 bits of a SiftWinNum */
static bool
avg_of_sum(u128 sum, uint64 count, SiftWinNum *out)
{
	const SiftI128 s = {.lo = (uint64) sum,.hi = (int64) (sum >> 64)};
	SiftNumAvg	a;
	u128		coef;

	if (!sift_numavg_exact(s, count, 0, &a) || (a.mag >> 95) != 0)
		return false;
	coef = a.neg ? (u128) 0 - a.mag : a.mag;
	*out = (SiftWinNum) {.coef_lo = (uint64) coef,.coef_hi = (int32) (coef >> 64),.scale = a.scale};
	return true;
}

/* the value of the function over the rows folded so far; false when it is NULL, and *ok false when the average does not fit */
static bool
snapshot(const SiftWinFuncSpec *fn, const WinFold *s, SiftWinVal *v, bool *ok)
{
	const bool	flt = fn->arg_type == SVEC_F64;

	memset(v, 0, sizeof(*v));
	*ok = true;
	if (fn->kind == SWIN_COUNT_STAR || fn->kind == SWIN_COUNT)
	{
		v->i = (int64) s->count;
		return true;
	}
	if (!s->count)
		return false;
	if (fn->kind == SWIN_SUM)
	{
		if (flt)
			v->f = s->sum_f;
		else
		{
			v->w.lo = (uint64) s->sum;
			v->w.hi = (int64) (s->sum >> 64);
		}
	}
	else if (fn->kind == SWIN_AVG)
	{
		if (flt)
			v->f = s->sum_f / (double) s->count;
		else
			*ok = avg_of_sum(s->sum, s->count, &v->num);
	}
	else
		*v = s->mm;
	return true;
}

static bool
framed(const Part *p, int f, SiftErr *err)
{
	const SiftWinFuncSpec *fn = &p->sp->func[f];
	WinFold		s = {0};
	uint32		o = p->lo;

	while (o < p->hi)
	{
		uint32		to = o + 1;
		SiftWinVal	v;
		bool		ok;
		bool		have;

		if (fn->range_frame)
			for (; to < p->hi && !p->it->peer[to]; to++)
				;
		for (uint32 r = o; r < (fn->range_frame ? to : o + 1); r++)
			if (!fold_row(p, f, &s, p->it->perm[r]))
				return sift_err_fill(err, SIFT_ERR_SERIAL, 0, 0, "a float8 sum overflows: the backend raises it"), false;
		have = snapshot(fn, &s, &v, &ok);
		if (!ok)
			return sift_err_fill(err, SIFT_ERR_CORRUPT, 0, 0, "the average of integers has more than 95 bits"), false;
		for (uint32 r = o; r < to; r++)
			put(p, r, f, v, !have);
		o = to;
	}
	return true;
}

static bool
run_funcs(const Part *p, SiftErr *err)
{
	for (int f = 0; f < p->sp->nfuncs; f++)
	{
		const SiftWinFuncKind kind = p->sp->func[f].kind;

		if (kind <= SWIN_DENSE_RANK)
			ranks(p, f);
		else if (kind <= SWIN_FIRST_VALUE)
			shifted(p, f);
		else if (!framed(p, f, err))
			return false;
	}
	return true;
}

bool
sift_par_win_run(const SiftParWinSpec *sp, const SiftParWinItem *it, SiftErr *err)
{
	memset(it->res_nulls, 0, sizeof(uint16) * it->n);
	for (uint32 i = 0; i < it->nparts; i++)
	{
		const Part	p = {.sp = sp,.it = it,.lo = it->pstart[i],.hi = it->pstart[i + 1]};

		order_partition(&p);
		mark_peers(&p);
		if (!run_funcs(&p, err))
			return false;
	}
	return true;
}
