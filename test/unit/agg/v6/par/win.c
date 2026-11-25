/*
 * agg/par/win.c - agg/par/winpart.c on 1 to 4 threads (the trap shim flags the helpers: no palloc, no ereport): the partitions of an item sorted and run through the window functions, against a plain
 * reference that sorts each partition by insertion and folds every frame from its start. Random windows (0 to 3 order keys of int4, int8 and float8 with ties, NULLs, NaN, -0 and 0, each ascending or
 * descending with the NULLs first or last; eight of the functions of agg/window.h over int4, int8 and float8 arguments with NULLs, ROWS and RANGE, LAG and LEAD of every offset with and without a default)
 * over items of partitions of every size, from one row to a few thousand. The average of integers is checked against the division in 128 bits, and a float8 sum that overflows is the serial path's.
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "agg/par/winpart.h"
#include "core/hash.h"
#include "core/testrng.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define MAX_THREADS	4
#define NCFG		60
#define NITEMS		12

static int	nchecks;

#define PASS(cond) do { nchecks++; SIFT_CHECK(cond); } while (0)

static uint64 rng_state = 11;

static uint64
rng(void)
{
	return sift_splitmix_next(&rng_state);
}

static uint32
rnd(uint32 n)
{
	return (uint32) (rng() % n);
}

typedef struct WItem
{
	SiftParWinItem it;
	uint32	   *pstart;
	uint64	   *tag;
	int64	   *ord[SIFT_WIN_MAX_KEYS];
	uint8	   *ord_null[SIFT_WIN_MAX_KEYS];
	int64	   *arg[SIFT_WIN_MAX_FUNCS];
	uint8	   *arg_null[SIFT_WIN_MAX_FUNCS];
	uint32	   *perm,
			   *tmp;
	uint8	   *peer;
	uint64	   *otag;
	SiftWinVal *res;
	uint16	   *res_nulls;
	SiftWinVal *want;
	uint64	   *want_cnt;		/* AVG of integers: the rows folded */
	uint64	   *want_tag;
	uint16	   *want_nulls;
	bool		ok;
	SiftErr		err;
} WItem;

typedef struct Cfg
{
	SiftParWinSpec sp;
	WItem		item[NITEMS];
} Cfg;

static int64
bits_of(double d)
{
	int64		b;

	memcpy(&b, &d, sizeof(b));
	return b;
}

static double
f8_of(int64 b)
{
	double		d;

	memcpy(&d, &b, sizeof(d));
	return d;
}

static const double F8_POOL[] = {0.0, -0.0, 1.5, -2.25, 3.0, 1e10, -1e10, 7.125, 0.5, 100.0, NAN, INFINITY, -INFINITY};

static int64
make_value(SiftVecType type, bool nan_ok)
{
	switch (type)
	{
		case SVEC_I32:
			return (int64) ((int32) rnd(9) - 3);
		case SVEC_I64:
			return (int64) rnd(7) * (rnd(2) ? 1000000007 : 1) - 2;
		default:
		{
			const uint32 pool = nan_ok ? lengthof(F8_POOL) : lengthof(F8_POOL) - 3;

			return rnd(3) == 0 ? bits_of(F8_POOL[rnd(pool)]) : bits_of((double) ((int) rnd(12) - 4) / 4.0);
		}
	}
}

static SiftVecType
pick_type(bool no_i64)
{
	const uint32 r = rnd(no_i64 ? 2 : 3);

	return r == 0 ? SVEC_I32 : r == 1 ? SVEC_F64 : SVEC_I64;
}

static void
make_spec(SiftParWinSpec *sp, SiftVecType *argtype)
{
	memset(sp, 0, sizeof(*sp));
	sp->n_ord = rnd(4);
	for (int k = 0; k < sp->n_ord; k++)
		sp->ord[k] = (SiftParWinKey) {.desc = rnd(2),.nulls_first = rnd(2),.f64 = rnd(3) == 0};
	sp->nfuncs = SIFT_WIN_MAX_FUNCS;
	for (int f = 0; f < sp->nfuncs; f++)
	{
		SiftWinFuncSpec *fn = &sp->func[f];
		const int	kind = rnd(12);

		*fn = (SiftWinFuncSpec) {0};
		fn->kind = kind == 0 ? SWIN_ROW_NUMBER : kind == 1 ? SWIN_RANK : kind == 2 ? SWIN_DENSE_RANK : kind == 3 ? SWIN_LAG : kind == 4 ? SWIN_LEAD :
			kind == 5 ? SWIN_FIRST_VALUE : kind == 6 ? SWIN_COUNT_STAR : kind == 7 ? SWIN_COUNT : kind == 8 ? SWIN_SUM : kind == 9 ? SWIN_AVG : kind == 10 ? SWIN_MIN : SWIN_MAX;
		fn->arg_type = pick_type(fn->kind == SWIN_AVG);
		argtype[f] = fn->arg_type;
		if (fn->kind >= SWIN_COUNT_STAR)
			fn->range_frame = sp->n_ord > 0 && rnd(2);
		if (fn->kind == SWIN_LAG || fn->kind == SWIN_LEAD)
		{
			fn->offset = rnd(6);
			fn->has_default = rnd(2);
			if (fn->has_default)
			{
				if (fn->arg_type == SVEC_F64)
					fn->default_val.f = 0.5;
				else
					fn->default_val.i = 77;
			}
		}
		if (fn->kind <= SWIN_DENSE_RANK || fn->kind == SWIN_COUNT_STAR)
			argtype[f] = SVEC_I64;
	}
}

static bool
has_arg(const SiftWinFuncSpec *fn)
{
	return fn->kind > SWIN_DENSE_RANK && fn->kind != SWIN_COUNT_STAR;
}

/* ---- the reference ---- */

typedef struct Ref
{
	const SiftParWinSpec *sp;
	const WItem *item;
} Ref;

static int
ref_f8(double a, double b)
{
	if (isnan(a) || isnan(b))
		return isnan(a) && isnan(b) ? 0 : isnan(a) ? 1 : -1;
	return a < b ? -1 : a > b ? 1 : 0;
}

static int
ref_cmp(const Ref *r, uint32 a, uint32 b)
{
	for (int k = 0; k < r->sp->n_ord; k++)
	{
		const bool	na = r->item->ord_null[k][a],
					nb = r->item->ord_null[k][b];
		int			c;

		if (na || nb)
		{
			if (na && nb)
				continue;
			return na == r->sp->ord[k].nulls_first ? -1 : 1;
		}
		c = r->sp->ord[k].f64 ? ref_f8(f8_of(r->item->ord[k][a]), f8_of(r->item->ord[k][b])) : (r->item->ord[k][a] < r->item->ord[k][b] ? -1 : r->item->ord[k][a] > r->item->ord[k][b]);
		if (c != 0)
			return r->sp->ord[k].desc ? -c : c;
	}
	return 0;
}

typedef struct Acc
{
	uint64		count;
	__int128	sum;
	double		sum_f;
	double		mm_f;
	int64		mm_i;
} Acc;

static void
acc_row(const SiftWinFuncSpec *fn, const WItem *it, int f, uint32 row, Acc *a)
{
	const bool	flt = fn->arg_type == SVEC_F64;

	if (fn->kind == SWIN_COUNT_STAR)
	{
		a->count++;
		return;
	}
	if (it->arg_null[f][row])
		return;
	if (fn->kind == SWIN_SUM || fn->kind == SWIN_AVG)
	{
		if (flt)
			a->sum_f = a->count ? a->sum_f + f8_of(it->arg[f][row]) : f8_of(it->arg[f][row]);
		else
			a->sum += (__int128) it->arg[f][row];
	}
	else if (fn->kind == SWIN_MIN || fn->kind == SWIN_MAX)
	{
		const bool	mn = fn->kind == SWIN_MIN;

		if (flt)
		{
			const double x = f8_of(it->arg[f][row]);

			if (!a->count || (mn ? ref_f8(a->mm_f, x) >= 0 : ref_f8(a->mm_f, x) <= 0))
				a->mm_f = x;
		}
		else if (!a->count || (mn ? it->arg[f][row] < a->mm_i : it->arg[f][row] > a->mm_i))
			a->mm_i = it->arg[f][row];
	}
	a->count++;
}

static void
ref_put(const WItem *it, uint32 o, int f, const SiftWinVal *v, bool null, int nf)
{
	((WItem *) it)->want[(size_t) o * nf + f] = *v;
	if (null)
		((WItem *) it)->want_nulls[o] |= (uint16) (1u << f);
}

static void
ref_partition(const Ref *r, uint32 lo, uint32 hi, uint32 *idx)
{
	const SiftParWinSpec *sp = r->sp;
	const WItem *it = r->item;
	const int	nf = sp->nfuncs;

	for (uint32 i = lo; i < hi; i++)
	{
		uint32		j = i;

		idx[i] = i;
		while (j > lo && ref_cmp(r, idx[j - 1], idx[j]) > 0)
		{
			const uint32 t = idx[j];

			idx[j] = idx[j - 1];
			idx[j - 1] = t;
			j--;
		}
	}
	for (uint32 o = lo; o < hi; o++)
		((WItem *) it)->want_tag[o] = it->tag[idx[o]];
	for (int f = 0; f < nf; f++)
	{
		const SiftWinFuncSpec *fn = &sp->func[f];

		for (uint32 o = lo; o < hi; o++)
		{
			SiftWinVal	v;
			bool		null = false;
			uint32		first = o,
						last = o;

			memset(&v, 0, sizeof(v));
			while (first > lo && ref_cmp(r, idx[first - 1], idx[o]) == 0)
				first--;
			while (last + 1 < hi && ref_cmp(r, idx[last + 1], idx[o]) == 0)
				last++;
			if (sp->n_ord == 0)
			{
				first = lo;
				last = hi - 1;
			}
			if (fn->kind == SWIN_ROW_NUMBER)
				v.i = o - lo + 1;
			else if (fn->kind == SWIN_RANK)
				v.i = first - lo + 1;
			else if (fn->kind == SWIN_DENSE_RANK)
			{
				int64		d = 1;

				for (uint32 q = lo + 1; q <= o; q++)
					d += sp->n_ord > 0 && ref_cmp(r, idx[q - 1], idx[q]) != 0;
				v.i = d;
			}
			else if (fn->kind >= SWIN_LAG && fn->kind <= SWIN_FIRST_VALUE)
			{
				int64		at = fn->kind == SWIN_FIRST_VALUE ? (int64) lo : fn->kind == SWIN_LAG ? (int64) o - fn->offset : (int64) o + fn->offset;

				if (at >= (int64) lo && at < (int64) hi)
				{
					const uint32 row = idx[at];

					null = it->arg_null[f][row];
					if (!null)
					{
						if (fn->arg_type == SVEC_F64)
							v.f = f8_of(it->arg[f][row]);
						else
							v.i = it->arg[f][row];
					}
				}
				else
				{
					null = !fn->has_default;
					if (fn->has_default)
						v = fn->default_val;
				}
			}
			else
			{
				Acc			a = {0};
				const uint32 end = fn->range_frame ? last : o;

				for (uint32 q = lo; q <= end; q++)
					acc_row(fn, it, f, idx[q], &a);
				if (fn->kind == SWIN_COUNT_STAR || fn->kind == SWIN_COUNT)
					v.i = (int64) a.count;
				else if (!a.count)
					null = true;
				else if (fn->kind == SWIN_SUM)
				{
					if (fn->arg_type == SVEC_F64)
						v.f = a.sum_f;
					else
					{
						v.w.lo = (uint64) a.sum;
						v.w.hi = (int64) (a.sum >> 64);
					}
				}
				else if (fn->kind == SWIN_AVG)
				{
					if (fn->arg_type == SVEC_F64)
						v.f = a.sum_f / (double) a.count;
					else
					{
						v.w.lo = (uint64) a.sum;
						v.w.hi = (int64) (a.sum >> 64);
						((WItem *) it)->want_cnt[(size_t) o * nf + f] = a.count;
					}
				}
				else if (fn->arg_type == SVEC_F64)
					v.f = a.mm_f;
				else
					v.i = a.mm_i;
			}
			ref_put(it, o, f, &v, null, nf);
		}
	}
}

/* the average of integers: the value is coef * 10^-scale, the half away from zero of sum / count at that scale */
static __int128
i128_of(uint64 lo, int64 hi)
{
	return (__int128) (((unsigned __int128) (uint64) hi << 64) | lo);
}

static bool
avg_is(const SiftWinNum *n, __int128 sum, uint64 count)
{
	__int128	coef = i128_of(n->coef_lo, n->coef_hi),
				scaled = sum;
	const bool	neg = scaled < 0;
	unsigned __int128 num = neg ? (unsigned __int128) 0 - (unsigned __int128) scaled : (unsigned __int128) scaled,
				q,
				r;

	for (int i = 0; i < n->scale; i++)
		num *= 10;
	q = num / count;
	r = num % count;
	if (r * 2 >= count)
		q++;
	coef = coef < 0 ? -coef : coef;
	return (unsigned __int128) coef == q;
}

static void
check_item(const SiftParWinSpec *sp, const WItem *it)
{
	const int	nf = sp->nfuncs;

	for (uint32 o = 0; o < it->it.n; o++)
	{
		PASS(it->otag[o] == it->want_tag[o]);
		PASS(it->res_nulls[o] == it->want_nulls[o]);
		for (int f = 0; f < nf; f++)
		{
			const SiftWinFuncSpec *fn = &sp->func[f];
			const SiftWinVal *got = &it->res[(size_t) o * nf + f],
					   *want = &it->want[(size_t) o * nf + f];

			if ((it->want_nulls[o] >> f) & 1)
				continue;
			if (fn->kind == SWIN_AVG && fn->arg_type != SVEC_F64)
			{
				const __int128 sum = i128_of(want->w.lo, want->w.hi);
				const __int128 coef = i128_of(got->num.coef_lo, got->num.coef_hi);

				PASS(got->num.scale >= 0 && got->num.scale < 30);
				PASS((coef < 0) == (sum < 0) || coef == 0);
				PASS(avg_is(&got->num, sum, it->want_cnt[(size_t) o * nf + f]));
				continue;
			}
			if (has_arg(fn) && fn->kind != SWIN_COUNT && fn->arg_type == SVEC_F64)
				PASS((isnan(got->f) && isnan(want->f)) || memcmp(&got->f, &want->f, sizeof(double)) == 0);
			else if (fn->kind == SWIN_SUM)
				PASS(got->w.lo == want->w.lo && got->w.hi == want->w.hi);
			else
				PASS(got->i == want->i);
		}
	}
}

/* ---- the items ---- */

static void *
alloc0(size_t n)
{
	return calloc(1, n > 0 ? n : 1);
}

static void
make_item(const SiftParWinSpec *sp, const SiftVecType *argtype, WItem *it, bool big)
{
	const int	nf = sp->nfuncs;
	uint32		np = 1 + rnd(24);
	uint32		n = 0;
	uint32	   *size = alloc0(sizeof(uint32) * np);
	uint32	   *idx;

	for (uint32 p = 0; p < np; p++)
	{
		size[p] = rnd(12) == 0 ? 1 + rnd(300) : 1 + rnd(30);
		if (big && p == 0)
			size[p] = 3000;
		n += size[p];
	}
	it->pstart = alloc0(sizeof(uint32) * (np + 1));
	for (uint32 p = 0; p < np; p++)
		it->pstart[p + 1] = it->pstart[p] + size[p];
	it->tag = alloc0(sizeof(uint64) * n);
	for (uint32 i = 0; i < n; i++)
		it->tag[i] = rng();
	for (int k = 0; k < sp->n_ord; k++)
	{
		it->ord[k] = alloc0(sizeof(int64) * n);
		it->ord_null[k] = alloc0(n);
		for (uint32 i = 0; i < n; i++)
		{
			it->ord_null[k][i] = rnd(7) == 0;
			it->ord[k][i] = it->ord_null[k][i] ? 0 : make_value(sp->ord[k].f64 ? SVEC_F64 : (k % 2 ? SVEC_I64 : SVEC_I32), true);
		}
	}
	for (int f = 0; f < nf; f++)
	{
		if (!has_arg(&sp->func[f]))
			continue;
		it->arg[f] = alloc0(sizeof(int64) * n);
		it->arg_null[f] = alloc0(n);
		for (uint32 i = 0; i < n; i++)
		{
			it->arg_null[f][i] = rnd(5) == 0;
			it->arg[f][i] = it->arg_null[f][i] ? 0 : make_value(argtype[f], true);
		}
	}
	it->perm = alloc0(sizeof(uint32) * n);
	it->tmp = alloc0(sizeof(uint32) * n);
	it->peer = alloc0(n);
	it->otag = alloc0(sizeof(uint64) * n);
	it->res = aligned_alloc(16, ((sizeof(SiftWinVal) * nf * n + 15) / 16) * 16 + 16);
	memset(it->res, 0, sizeof(SiftWinVal) * nf * n);
	it->res_nulls = alloc0(sizeof(uint16) * n);
	it->want = aligned_alloc(16, ((sizeof(SiftWinVal) * nf * n + 15) / 16) * 16 + 16);
	memset(it->want, 0, sizeof(SiftWinVal) * nf * n);
	it->want_cnt = alloc0(sizeof(uint64) * nf * n);
	it->want_tag = alloc0(sizeof(uint64) * n);
	it->want_nulls = alloc0(sizeof(uint16) * n);
	it->it = (SiftParWinItem) {.n = n,.nparts = np,.pstart = it->pstart,.tag = it->tag,.perm = it->perm,.tmp = it->tmp,.peer = it->peer,.otag = it->otag,.res = it->res,
		.res_nulls = it->res_nulls};
	for (int k = 0; k < sp->n_ord; k++)
	{
		it->it.ord[k] = it->ord[k];
		it->it.ord_null[k] = it->ord_null[k];
	}
	for (int f = 0; f < nf; f++)
	{
		it->it.arg[f] = it->arg[f];
		it->it.arg_null[f] = it->arg_null[f];
	}
	idx = alloc0(sizeof(uint32) * n);
	for (uint32 p = 0; p < np; p++)
		ref_partition(&(Ref) {sp, it}, it->pstart[p], it->pstart[p + 1], idx);
	free(idx);
	free(size);
}

static void
free_item(WItem *it)
{
	free(it->pstart);
	free(it->tag);
	for (int k = 0; k < SIFT_WIN_MAX_KEYS; k++)
	{
		free(it->ord[k]);
		free(it->ord_null[k]);
	}
	for (int f = 0; f < SIFT_WIN_MAX_FUNCS; f++)
	{
		free(it->arg[f]);
		free(it->arg_null[f]);
	}
	free(it->perm);
	free(it->tmp);
	free(it->peer);
	free(it->otag);
	free(it->res);
	free(it->res_nulls);
	free(it->want);
	free(it->want_cnt);
	free(it->want_tag);
	free(it->want_nulls);
	memset(it, 0, sizeof(*it));
}

typedef struct Work
{
	Cfg		   *cfg;
	int			nthreads;
} Work;

static void
work_fn(void *arg, int worker)
{
	Work	   *w = arg;

	for (int i = worker; i < NITEMS; i += w->nthreads)
		w->cfg->item[i].ok = sift_par_win_run(&w->cfg->sp, &w->cfg->item[i].it, &w->cfg->item[i].err);
}

static void
run_cfg(int c)
{
	static Cfg	cfg;
	SiftVecType argtype[SIFT_WIN_MAX_FUNCS];
	const int	nthreads = 1 + c % MAX_THREADS;
	Work		w = {&cfg, nthreads};

	make_spec(&cfg.sp, argtype);
	for (int i = 0; i < NITEMS; i++)
		make_item(&cfg.sp, argtype, &cfg.item[i], i == 0 && c % 10 == 0);
	unit_run_workers(nthreads, work_fn, &w);
	for (int i = 0; i < NITEMS; i++)
	{
		PASS(cfg.item[i].ok);
		check_item(&cfg.sp, &cfg.item[i]);
		free_item(&cfg.item[i]);
	}
}

/* a float8 sum that overflows is for the backend, one of infinities is not */
static void
overflow_cases(void)
{
	const uint32 pstart[] = {0, 3};
	const uint64 tag[] = {1, 2, 3};
	const uint8 none[3] = {0};
	const double big[3] = {DBL_MAX, DBL_MAX, 1.0},
				inf[3] = {INFINITY, DBL_MAX, 1.0};
	const double *cases[2] = {big, inf};
	SiftParWinSpec sp = {.n_ord = 0,.nfuncs = 1};

	sp.func[0] = (SiftWinFuncSpec) {.kind = SWIN_SUM,.arg_type = SVEC_F64};
	for (int c = 0; c < 2; c++)
	{
		int64		arg[3];
		uint32		perm[3],
					tmp[3];
		uint8		peer[3];
		uint64		otag[3];
		SiftWinVal	res[3];
		uint16		nulls[3];
		SiftErr		err = {0};
		SiftParWinItem it = {.n = 3,.nparts = 1,.pstart = pstart,.tag = tag,.perm = perm,.tmp = tmp,.peer = peer,.otag = otag,.res = res,.res_nulls = nulls};

		for (int i = 0; i < 3; i++)
			arg[i] = bits_of(cases[c][i]);
		it.arg[0] = arg;
		it.arg_null[0] = none;
		PASS(sift_par_win_run(&sp, &it, &err) == (c == 1));
		PASS(c == 1 || err.code == SIFT_ERR_SERIAL);
	}
}

/* one partition of many rows: the rank, the running count and the sum are the arithmetic the rows say */
static void
big_partition(void)
{
	const uint32 n = 200000;
	uint32		pstart[2] = {0, n};
	uint64	   *tag = alloc0(sizeof(uint64) * n),
			   *otag = alloc0(sizeof(uint64) * n);
	int64	   *ord = alloc0(sizeof(int64) * n),
			   *arg = alloc0(sizeof(int64) * n);
	uint8	   *none = alloc0(n),
			   *peer = alloc0(n);
	uint32	   *perm = alloc0(sizeof(uint32) * n),
			   *tmp = alloc0(sizeof(uint32) * n);
	SiftWinVal *res = aligned_alloc(16, sizeof(SiftWinVal) * 3 * n);
	uint16	   *nulls = alloc0(sizeof(uint16) * n);
	SiftParWinSpec sp = {.n_ord = 1,.nfuncs = 3};
	SiftParWinItem it = {.n = n,.nparts = 1,.pstart = pstart,.tag = tag,.perm = perm,.tmp = tmp,.peer = peer,.otag = otag,.res = res,.res_nulls = nulls};
	SiftErr		err = {0};

	sp.ord[0] = (SiftParWinKey) {.desc = true};
	sp.func[0] = (SiftWinFuncSpec) {.kind = SWIN_ROW_NUMBER};
	sp.func[1] = (SiftWinFuncSpec) {.kind = SWIN_SUM,.arg_type = SVEC_I64};
	sp.func[2] = (SiftWinFuncSpec) {.kind = SWIN_MAX,.arg_type = SVEC_I64,.range_frame = true};
	for (uint32 i = 0; i < n; i++)
	{
		tag[i] = i;
		ord[i] = (int64) ((i * 7919u) % n);
		arg[i] = 1;
	}
	it.ord[0] = ord;
	it.ord_null[0] = none;
	it.arg[1] = arg;
	it.arg_null[1] = none;
	it.arg[2] = ord;
	it.arg_null[2] = none;
	PASS(sift_par_win_run(&sp, &it, &err));
	for (uint32 o = 0; o < n; o++)
	{
		PASS(res[(size_t) o * 3].i == (int64) o + 1);
		PASS(res[(size_t) o * 3 + 1].w.lo == (uint64) o + 1);
		PASS(res[(size_t) o * 3 + 2].i == (int64) n - 1);
		PASS(ord[otag[o]] == (int64) n - 1 - (int64) o);
	}
	free(tag);
	free(otag);
	free(ord);
	free(arg);
	free(none);
	free(peer);
	free(perm);
	free(tmp);
	free(res);
	free(nulls);
}

int
main(void)
{
	for (int c = 0; c < NCFG; c++)
		run_cfg(c);
	overflow_cases();
	big_partition();
	printf("par_win: %d checks ok\n", nchecks);
	return 0;
}
