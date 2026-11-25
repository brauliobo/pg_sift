/*
 * agg/win/datebin.c - date_bin, the exact numeric average and SiftWinx (v6_datebin_win.h).
 */
#include "agg/win/datebin.h"

#include <stdio.h>

#include "agg/tseries_fast.h"

/* ---- date_bin ---- */

void
sift_datebin_init(SiftDateBin *b, const SiftInterval16 *stride, int64 origin)
{
	int64		us;

	b->origin = origin;
	b->stride_us = 0;
	if (!sift_ts_finite(origin))
		b->stride_status = SDB_ORIGIN_RANGE;
	else if (stride->month != 0)
		b->stride_status = SDB_STRIDE_MONTHS;
	else if (__builtin_mul_overflow((int64) stride->day, USECS_PER_DAY, &us) || __builtin_add_overflow(us, stride->time, &us))
		b->stride_status = SDB_INTERVAL_RANGE;
	else if (us <= 0)
		b->stride_status = SDB_STRIDE_NOT_POSITIVE;
	else
	{
		b->stride_status = SDB_OK;
		b->stride_us = us;
	}
}

SiftDateBinStatus
sift_datebin_one(const SiftDateBin *b, int64 ts, int64 *out)
{
	if (!sift_ts_finite(ts))
	{
		*out = ts;
		return SDB_OK;
	}
	if (b->stride_status != SDB_OK)
		return b->stride_status;
	return sift_datebin_row(ts, b->origin, b->stride_us, out);
}

SiftDateBinStatus
sift_datebin_batch(const SiftDateBin *b, const SiftBatchSel *bs, const SiftVec *in, int64 *out, uint32 *err_row)
{
	const int64 *x = in->data;
	SiftBatchIter it;
	uint32		r;

	if (b->stride_status == SDB_OK)
		return sift_datebin_batch_run(b, bs, in, out, err_row);
	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
		if (sift_vec_valid_at(in, r))
		{
			if (sift_ts_finite(x[r]))
			{
				*err_row = r;
				return b->stride_status;
			}
			out[r] = x[r];
		}
	return SDB_OK;
}

const char *
sift_datebin_message(SiftDateBinStatus s)
{
	switch (s)
	{
		case SDB_OK:
			return "";
		case SDB_ORIGIN_RANGE:
			return "origin out of range";
		case SDB_STRIDE_MONTHS:
			return "timestamps cannot be binned into intervals containing months or years";
		case SDB_INTERVAL_RANGE:
			return "interval out of range";
		case SDB_STRIDE_NOT_POSITIVE:
			return "stride must be greater than zero";
		case SDB_TS_RANGE:
			return "timestamp out of range";
	}
	return "";
}

/* ---- the average of a numeric ---- */

typedef unsigned __int128 u128;

#define U128_MAX	(~(u128) 0)

static int
u128_digits(u128 v)
{
	int			d = 0;

	do
	{
		d++;
		v /= 10;
	} while (v != 0);
	return d;
}

static u128
u128_pow10(int e)
{
	u128		p = 1;

	while (e-- > 0)
		p *= 10;
	return p;
}

/* the weight (in digits of base 10000) and the value of the first nonzero digit of m * 10^-scale, as NumericVar has them; a zero is weight 0, digit 0 */
static void
numvar_lead(u128 m, int scale, int *weight, int *first)
{
	int			e10, w, sh;

	if (m == 0)
	{
		*weight = 0;
		*first = 0;
		return;
	}
	e10 = u128_digits(m) - 1 - scale;
	w = e10 >= 0 ? e10 / 4 : -((-e10 + 3) / 4);
	sh = scale + 4 * w;
	*weight = w;
	*first = (int) (sh >= 0 ? m / u128_pow10(sh) : m * u128_pow10(-sh));
}

/* select_div_scale of numeric.c for the sum (dscale sum_scale) and the count (dscale 0) */
static int
div_scale(u128 m, int sum_scale, uint64 count)
{
	int			w1, f1, w2, f2, qweight, rscale;

	numvar_lead(m, sum_scale, &w1, &f1);
	numvar_lead((u128) count, 0, &w2, &f2);
	qweight = w1 - w2;
	if (f1 <= f2)
		qweight--;
	rscale = 16 - qweight * 4;
	rscale = Max(rscale, sum_scale);
	rscale = Max(rscale, 0);
	return Min(rscale, 1000);
}

bool
sift_numavg_exact(SiftI128 sum, uint64 count, int sum_scale, SiftNumAvg *out)
{
	__int128	s = (__int128) (((u128) (uint64) sum.hi << 64) | sum.lo);
	bool		neg = s < 0;
	u128		m = neg ? (u128) 0 - (u128) s : (u128) s;
	int			rscale = div_scale(m, sum_scale, count);
	u128		q = m / count, r = m % count;

	out->ok = false;
	out->neg = false;
	out->mag = 0;
	out->scale = rscale;
	for (int i = sum_scale; i < rscale; i++)
	{
		u128		d;

		r *= 10;
		d = r / count;
		r %= count;
		if (q > (U128_MAX - d) / 10)
			return false;
		q = q * 10 + d;
	}
	if (r * 2 >= count)
	{
		if (q == U128_MAX)
			return false;
		q++;
	}
	out->ok = true;
	out->neg = neg && q != 0;
	out->mag = q;
	return true;
}

int
sift_numavg_text(const SiftNumAvg *a, char *buf, size_t cap)
{
	char		dig[1100];
	int			nd = 0, pos = 0;
	u128		v = a->mag;

	do
	{
		dig[nd++] = (char) ('0' + (int) (v % 10));
		v /= 10;
	} while (v != 0);
	if (a->scale + 1 > (int) sizeof(dig) - 48)
		return -1;
	while (nd < a->scale + 1)
		dig[nd++] = '0';
	if (nd + 3 > (int) cap)
		return -1;
	if (a->neg)
		buf[pos++] = '-';
	for (int i = nd - 1; i >= 0; i--)
	{
		buf[pos++] = dig[i];
		if (i == a->scale && a->scale > 0)
			buf[pos++] = '.';
	}
	buf[pos] = '\0';
	return pos;
}

/* ---- SiftWinx ---- */

/* A value of the argument of a row, whatever its type: an integer, a date, a timestamp or a numeric of at most 18 digits as an integer, a float8 as its bits, a numeric of more digits as 128 bits */
typedef struct WinxCell
{
	SiftWinI128 v;
	bool		null;
	bool		nan;			/* a numeric NaN: not NULL, and v holds nothing */
} WinxCell;

typedef struct WinxState
{
	uint64		n;				/* the rows with a value */
	uint64		nans;			/* the NaN rows */
	SiftWinI128 sum;
	SiftWinI128 ext;
	int64		i;
	double		f;
	bool		dirty;
	SiftWinxVal val;
	SiftWinVarF64 vf;
	SiftWinVarInt vi;
	WinxCell	first;
	bool		have_first;
	WinxCell   *ring;			/* LAG: the arguments of the last offset rows of the partition, row j in slot j % offset */
} WinxState;

struct SiftWinx
{
	MemoryContext cx;
	SiftWinxSpec spec;
	SiftWinxNext next;
	void	   *src;
	SiftWinxEmit emit;
	void	   *emit_arg;
	SiftWinxStats st;
	bool		have_prev;
	int64		prev_part[SIFT_WINX_MAX_KEYS];
	uint8		prev_part_null;
	int64		prev_ord[SIFT_WINX_MAX_KEYS];
	uint8		prev_ord_null;
	uint64		part_idx;
	uint64		rank;
	uint64		dense;
	WinxState	fs[SIFT_WINX_MAX_FUNCS];
	int			ncells;			/* the LAG and LEAD functions: each has a cell per row */
	int			cell_of_func[SIFT_WINX_MAX_FUNCS];
	uint32		max_lead;
	bool		has_lead;
	uint64	   *ptag;
	uint64	   *pnum;
	WinxCell   *pcells;
	uint64		npend;
	uint64		pcap;
	BufFile    *pspill;			/* the rows of a peer group past max_pending, (tag, row number, cells) each, in arrival order */
	uint64		nspilled;
	uint32		lcap;			/* the ring of the rows that wait for the rows a LEAD looks ahead to: max_lead + 1 of them */
	uint32		lhead;
	uint32		lcount;
	uint64	   *ltag;
	SiftWinxVal *lres;
	WinxCell   *lcells;
};

static inline bool
winx_has_cell(SiftWinxKind k)
{
	return k == SWX_LAG || k == SWX_LEAD;
}

static inline bool
winx_is_state(SiftWinxKind k)
{
	return k >= SWX_COUNT_STAR && !winx_has_cell(k);
}

uint64
sift_winx_lead_bytes(const SiftWinxSpec *spec)
{
	uint32		max_lead = 0;
	int			ncells = 0;
	bool		any = false;

	for (int f = 0; f < spec->nfuncs; f++)
	{
		const SiftWinxFunc *fn = &spec->func[f];

		ncells += winx_has_cell(fn->kind);
		if (fn->kind == SWX_LEAD)
		{
			any = true;
			max_lead = Max(max_lead, fn->offset);
		}
	}
	return !any ? 0 : (uint64) (max_lead + 1) * (sizeof(uint64) + sizeof(SiftWinxVal) * spec->nfuncs + sizeof(WinxCell) * ncells);
}

SiftWinx *
sift_winx_create(MemoryContext cx, const SiftWinxSpec *spec, SiftWinxNext next, void *src, SiftWinxEmit emit, void *emit_arg)
{
	SiftWinx   *w = MemoryContextAllocAligned(cx, sizeof(SiftWinx), _Alignof(SiftWinx), MCXT_ALLOC_ZERO);

	SIFT_INVARIANT(spec->n_part <= SIFT_WINX_MAX_KEYS && spec->n_ord <= SIFT_WINX_MAX_KEYS && spec->nfuncs <= SIFT_WINX_MAX_FUNCS, "window: too many keys or functions");
	w->cx = cx;
	w->spec = *spec;
	w->next = next;
	w->src = src;
	w->emit = emit;
	w->emit_arg = emit_arg;
	for (int f = 0; f < spec->nfuncs; f++)
	{
		const SiftWinxFunc *fn = &spec->func[f];

		SIFT_INVARIANT(!winx_has_cell(fn->kind) || fn->offset <= SIFT_WINX_MAX_OFFSET, "window: LAG or LEAD offset out of range");
		SIFT_INVARIANT(!sift_winx_is_var(fn->kind) || fn->arg_type != SVEC_NUM128, "window: the variance of a numeric of more than 18 digits stays with PostgreSQL");
		w->cell_of_func[f] = winx_has_cell(fn->kind) ? w->ncells++ : -1;
		if (fn->kind == SWX_LAG && fn->offset > 0)
			w->fs[f].ring = MemoryContextAllocZero(cx, sizeof(WinxCell) * fn->offset);
		if (fn->kind == SWX_LEAD)
		{
			w->has_lead = true;
			w->max_lead = Max(w->max_lead, fn->offset);
		}
	}
	if (w->has_lead)
	{
		if (sift_winx_lead_bytes(spec) > spec->max_pending * sizeof(uint64))
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("window: LEAD looks %u rows ahead, more than work_mem holds", w->max_lead),
							errhint("Raise work_mem or use a smaller LEAD offset.")));
		w->lcap = w->max_lead + 1;
		w->ltag = MemoryContextAlloc(cx, sizeof(uint64) * w->lcap);
		w->lres = MemoryContextAllocAligned(cx, sizeof(SiftWinxVal) * spec->nfuncs * w->lcap, _Alignof(SiftWinxVal), 0);
		w->lcells = MemoryContextAlloc(cx, sizeof(WinxCell) * Max(w->ncells, 1) * w->lcap);
	}
	return w;
}

static inline bool
vec_i64(const SiftVec *v, uint32 r, int64 *out)
{
	if (!sift_vec_valid_at(v, r))
		return false;
	*out = v->type == SVEC_I32 ? (int64) ((const int32 *) v->data)[r] : ((const int64 *) v->data)[r];
	return true;
}

static WinxCell
cell_at(SiftVecType type, const SiftVec *v, uint32 r)
{
	WinxCell	c = {0};

	if (sift_vec_nan_at(v, r))
		return (WinxCell) {.nan = true};
	if (!sift_vec_valid_at(v, r))
		return (WinxCell) {.null = true};
	switch (type)
	{
		case SVEC_F64:
		{
			uint64		bits;

			memcpy(&bits, &((const double *) v->data)[r], sizeof(bits));
			c.v = (__int128) bits;
			break;
		}
		case SVEC_NUM128:
		{
			const SiftI128 *p = &((const SiftI128 *) v->data)[r];

			c.v = (__int128) (((u128) (uint64) p->hi << 64) | p->lo);
			break;
		}
		default:
			c.v = type == SVEC_I32 ? (int64) ((const int32 *) v->data)[r] : ((const int64 *) v->data)[r];
			break;
	}
	return c;
}

static WinxCell
default_cell(const SiftWinxFunc *fn)
{
	WinxCell	c = {0};
	uint64		bits;

	if (fn->arg_type != SVEC_F64)
	{
		c.v = fn->def_i;
		return c;
	}
	memcpy(&bits, &fn->def_f, sizeof(bits));
	c.v = (SiftWinI128) bits;
	return c;
}

static void
val_of_cell(SiftVecType type, const WinxCell *c, SiftWinxVal *v)
{
	memset(v, 0, sizeof(*v));
	v->null = c->null;
	v->nan = c->nan;
	if (c->null || c->nan)
		return;
	if (type == SVEC_F64)
	{
		uint64		bits = (uint64) c->v;

		memcpy(&v->f, &bits, sizeof(bits));
	}
	else if (type == SVEC_NUM128)
	{
		v->w.lo = (uint64) c->v;
		v->w.hi = (int64) (c->v >> 64);
	}
	else
		v->i = (int64) c->v;
}

static void
var_fill(const SiftWinxFunc *fn, const WinxState *s, SiftWinxVal *v)
{
	const SiftWinVarKind vk = sift_winx_var_kind(fn->kind);

	if (s->nans > 0)
	{
		v->null = s->n + s->nans < (sift_winvar_is_sample(vk) ? 2 : 1);
		v->nan = !v->null;
	}
	else if (fn->arg_type == SVEC_F64)
		v->null = !sift_winvar_f64_final(&s->vf, vk, &v->f);
	else
	{
		const bool	fits = sift_winvar_int_final(&s->vi, vk, &v->var);

		if (!fits)
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("window: the variance of more rows than 256 bits hold")));
		v->null = v->var.null;
	}
}

static void
winx_fill(const SiftWinx *w, int f, WinxState *s, SiftWinxVal *v)
{
	const SiftWinxFunc *fn = &w->spec.func[f];
	bool		is_float = fn->arg_type == SVEC_F64;
	bool		is_wide = fn->arg_type == SVEC_NUM128;
	bool		is_num = fn->arg_type == SVEC_NUM64 || is_wide;

	memset(v, 0, sizeof(*v));
	v->n = s->n;
	if (fn->kind == SWX_COUNT_STAR || fn->kind == SWX_COUNT)
	{
		v->n = s->n + s->nans;
		v->i = (int64) v->n;
		return;
	}
	if (fn->kind == SWX_FIRST_VALUE)
	{
		val_of_cell(fn->arg_type, &s->first, v);
		v->n = s->have_first;
		return;
	}
	if (sift_winx_is_var(fn->kind))
	{
		var_fill(fn, s, v);
		return;
	}
	if (s->nans > 0 && (fn->kind != SWX_MIN || s->n == 0))
	{
		v->nan = true;
		return;
	}
	if (s->n == 0)
	{
		v->null = true;
		return;
	}
	switch (fn->kind)
	{
		case SWX_SUM:
			if (is_float)
				v->f = s->f;
			else
			{
				v->w.lo = (uint64) s->sum;
				v->w.hi = (int64) (s->sum >> 64);
			}
			break;
		case SWX_AVG:
			if (is_float)
				v->f = s->f / (double) s->n;
			else
			{
				SiftI128	sum = {(uint64) s->sum, (int64) (s->sum >> 64)};

				sift_numavg_exact(sum, s->n, is_num ? fn->scale : 0, &v->avg);
			}
			break;
		default:
			if (is_float)
				v->f = s->f;
			else if (is_wide)
			{
				v->w.lo = (uint64) s->ext;
				v->w.hi = (int64) (s->ext >> 64);
			}
			else
				v->i = s->i;
			break;
	}
}

static inline int
f8_cmp(double a, double b)
{
	bool		an = a != a, bn = b != b;

	if (an || bn)
		return an == bn ? 0 : (an ? 1 : -1);
	return a < b ? -1 : a > b;
}

static void
winx_fold(WinxState *s, SiftWinxKind kind, SiftVecType type, const SiftVec *v, uint32 r)
{
	bool		sum = kind == SWX_SUM || kind == SWX_AVG;
	bool		min = kind == SWX_MIN;

	switch (type)
	{
		case SVEC_F64:
		{
			double		x = ((const double *) v->data)[r];

			if (sift_winx_is_var(kind))
			{
				if (!sift_winvar_f64_add(&s->vf, x))
					ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("value out of range: overflow")));
			}
			else if (sum)
				s->f = s->n == 0 ? x : s->f + x;
			else if (s->n == 0 || (min ? f8_cmp(x, s->f) < 0 : f8_cmp(x, s->f) > 0))
				s->f = x;
			break;
		}
		case SVEC_NUM128:
		{
			const SiftI128 *p = &((const SiftI128 *) v->data)[r];
			__int128	x = (__int128) (((u128) (uint64) p->hi << 64) | p->lo);

			if (sum)
				s->sum += x;
			else if (s->n == 0 || (min ? x < s->ext : x > s->ext))
				s->ext = x;
			break;
		}
		default:
		{
			int64		x = type == SVEC_I32 ? (int64) ((const int32 *) v->data)[r] : ((const int64 *) v->data)[r];

			if (sift_winx_is_var(kind))
			{
				if (!sift_winvar_int_add(&s->vi, x))
					ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("window: the sum of squares needs more than 256 bits")));
			}
			else if (sum)
				s->sum += x;
			else if (s->n == 0 || (min ? x < s->i : x > s->i))
				s->i = x;
			break;
		}
	}
	s->n++;
}

static inline bool
mask_bit(const uint64 *m, uint32 r)
{
	return (m[r >> 6] >> (r & 63)) & 1;
}

static void
winx_row_fold(SiftWinx *w, const SiftWinxBatch *b, uint32 r)
{
	for (int f = 0; f < w->spec.nfuncs; f++)
	{
		const SiftWinxFunc *fn = &w->spec.func[f];
		WinxState  *s = &w->fs[f];

		if (!winx_is_state(fn->kind))
			continue;
		if (fn->kind == SWX_FIRST_VALUE)
		{
			if (!s->have_first)
			{
				s->first = cell_at(fn->arg_type, b->arg[f], r);
				s->have_first = true;
				s->dirty = true;
			}
			continue;
		}
		if (fn->filter >= 0 && !mask_bit(b->filter[fn->filter], r))
			continue;
		if (fn->kind == SWX_COUNT_STAR)
			s->n++;
		else if (sift_vec_nan_at(b->arg[f], r))
			s->nans++;
		else if (!sift_vec_valid_at(b->arg[f], r))
			continue;
		else if (fn->kind == SWX_COUNT)
			s->n++;
		else
			winx_fold(s, fn->kind, fn->arg_type, b->arg[f], r);
		s->dirty = true;
	}
}

/* The cells of the LAG and LEAD functions for the row: LAG reads the ring of its argument and writes the row's, LEAD keeps the row's argument for the rows before it */
static void
winx_row_cells(SiftWinx *w, const SiftWinxBatch *b, uint32 r, WinxCell *cells)
{
	const uint64 j = w->part_idx - 1;

	for (int f = 0; f < w->spec.nfuncs; f++)
	{
		const SiftWinxFunc *fn = &w->spec.func[f];
		WinxCell   *out = &cells[w->cell_of_func[f] < 0 ? 0 : w->cell_of_func[f]];
		WinxCell	own;

		if (!winx_has_cell(fn->kind))
			continue;
		own = cell_at(fn->arg_type, b->arg[f], r);
		if (fn->kind == SWX_LEAD)
			*out = own;
		else if (fn->offset == 0)
			*out = own;
		else
		{
			WinxCell   *slot = &w->fs[f].ring[j % fn->offset];

			*out = j >= fn->offset ? *slot : fn->has_default ? default_cell(fn) : (WinxCell) {.null = true};
			*slot = own;
		}
	}
}

static void
winx_row_result(SiftWinx *w, uint64 row_number, const WinxCell *cells, SiftWinxVal *res)
{
	for (int f = 0; f < w->spec.nfuncs; f++)
	{
		const SiftWinxFunc *fn = &w->spec.func[f];
		WinxState  *s = &w->fs[f];

		if (winx_is_state(fn->kind))
		{
			if (s->dirty)
			{
				winx_fill(w, f, s, &s->val);
				s->dirty = false;
			}
			res[f] = s->val;
		}
		else if (fn->kind == SWX_LAG)
			val_of_cell(fn->arg_type, &cells[w->cell_of_func[f]], &res[f]);
		else
		{
			memset(&res[f], 0, sizeof(res[f]));
			res[f].i = fn->kind == SWX_ROW_NUMBER ? (int64) row_number : fn->kind == SWX_RANK ? (int64) w->rank : (int64) w->dense;
		}
	}
}

/* The rows that waited: the head of the ring gets the LEAD of every function from the row offset places after it, or the default when the partition ends first; then it leaves */
static void
winx_lead_pop(SiftWinx *w)
{
	const int	nf = w->spec.nfuncs;
	SiftWinxVal *res = &w->lres[(size_t) w->lhead * nf];

	for (int f = 0; f < nf; f++)
	{
		const SiftWinxFunc *fn = &w->spec.func[f];

		if (fn->kind != SWX_LEAD)
			continue;
		if (fn->offset < w->lcount)
		{
			const uint32 at = (w->lhead + fn->offset) % w->lcap;

			val_of_cell(fn->arg_type, &w->lcells[(size_t) at * w->ncells + w->cell_of_func[f]], &res[f]);
		}
		else if (fn->has_default)
			res[f] = (SiftWinxVal) {.i = fn->def_i,.f = fn->def_f};
		else
		{
			memset(&res[f], 0, sizeof(res[f]));
			res[f].null = true;
		}
	}
	w->emit(w->emit_arg, w->ltag[w->lhead], res);
	w->st.rows++;
	w->lhead = (w->lhead + 1) % w->lcap;
	w->lcount--;
}

static void
winx_lead_drain(SiftWinx *w)
{
	while (w->lcount > 0)
		winx_lead_pop(w);
}

static void
winx_emit_row(SiftWinx *w, uint64 tag, uint64 row_number, const WinxCell *cells)
{
	SiftWinxVal res[SIFT_WINX_MAX_FUNCS];

	if (!w->has_lead)
	{
		winx_row_result(w, row_number, cells, res);
		w->emit(w->emit_arg, tag, res);
		w->st.rows++;
		return;
	}
	if (w->lcount == w->lcap)
		winx_lead_pop(w);
	{
		const uint32 at = (w->lhead + w->lcount) % w->lcap;

		winx_row_result(w, row_number, cells, &w->lres[(size_t) at * w->spec.nfuncs]);
		w->ltag[at] = tag;
		memcpy(&w->lcells[(size_t) at * w->ncells], cells, sizeof(WinxCell) * w->ncells);
		w->lcount++;
	}
}

static void
winx_spill_row(SiftWinx *w, uint64 tag, uint64 row_number, const WinxCell *cells)
{
	const uint64 head[2] = {tag, row_number};

	if (w->pspill == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(w->cx);

		w->pspill = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	BufFileWrite(w->pspill, head, sizeof(head));
	BufFileWrite(w->pspill, cells, sizeof(WinxCell) * w->ncells);
	if (++w->nspilled % 1024 == 0)
		CHECK_FOR_INTERRUPTS();
	w->st.max_pending = Max(w->st.max_pending, w->npend + w->nspilled);
}

static void
winx_unspill(SiftWinx *w)
{
	WinxCell	cells[SIFT_WINX_MAX_FUNCS];
	uint64		head[2];

	SIFT_INVARIANT(BufFileSeek(w->pspill, 0, 0, SEEK_SET) == 0, "window: could not rewind the spill of a peer group");
	for (uint64 i = 0; i < w->nspilled; i++)
	{
		BufFileReadExact(w->pspill, head, sizeof(head));
		BufFileReadExact(w->pspill, cells, sizeof(WinxCell) * w->ncells);
		winx_emit_row(w, head[0], head[1], cells);
	}
	BufFileClose(w->pspill);
	w->pspill = NULL;
	w->nspilled = 0;
}

static void
winx_flush(SiftWinx *w)
{
	for (uint64 i = 0; i < w->npend; i++)
		winx_emit_row(w, w->ptag[i], w->pnum[i], &w->pcells[i * w->ncells]);
	w->npend = 0;
	if (w->nspilled > 0)
		winx_unspill(w);
}

static void
winx_pend(SiftWinx *w, uint64 tag, uint64 row_number, const WinxCell *cells)
{
	if (w->npend >= w->spec.max_pending)
	{
		winx_spill_row(w, tag, row_number, cells);
		return;
	}
	if (w->npend == w->pcap)
	{
		w->pcap = Max(w->pcap * 2, 1024);
		w->ptag = w->ptag ? repalloc(w->ptag, sizeof(uint64) * w->pcap) : MemoryContextAlloc(w->cx, sizeof(uint64) * w->pcap);
		w->pnum = w->pnum ? repalloc(w->pnum, sizeof(uint64) * w->pcap) : MemoryContextAlloc(w->cx, sizeof(uint64) * w->pcap);
		w->pcells = w->pcells ? repalloc(w->pcells, sizeof(WinxCell) * w->ncells * w->pcap) : MemoryContextAlloc(w->cx, sizeof(WinxCell) * Max(w->ncells, 1) * w->pcap);
	}
	w->ptag[w->npend] = tag;
	w->pnum[w->npend] = row_number;
	memcpy(&w->pcells[w->npend * w->ncells], cells, sizeof(WinxCell) * w->ncells);
	w->npend++;
	w->st.max_pending = Max(w->st.max_pending, w->npend);
}

static void
winx_new_partition(SiftWinx *w)
{
	for (int f = 0; f < w->spec.nfuncs; f++)
	{
		WinxCell   *ring = w->fs[f].ring;

		memset(&w->fs[f], 0, sizeof(WinxState));
		w->fs[f].ring = ring;
		w->fs[f].dirty = true;
	}
	w->part_idx = 0;
	w->rank = 1;
	w->dense = 1;
	w->st.partitions++;
}

static void
winx_batch(SiftWinx *w, const SiftWinxBatch *b)
{
	for (uint32 r = 0; r < b->n; r++)
	{
		int64		part[SIFT_WINX_MAX_KEYS], ord[SIFT_WINX_MAX_KEYS];
		uint8		part_null = 0, ord_null = 0;
		bool		new_part, new_peer = false;
		uint64		tag = b->tag != NULL ? b->tag[r] : w->st.rows + w->npend + w->nspilled + w->lcount;
		WinxCell	cells[SIFT_WINX_MAX_FUNCS];

		if (b->mask != NULL && !mask_bit(b->mask, r))
			continue;
		for (int k = 0; k < w->spec.n_part; k++)
			if (!vec_i64(b->part[k], r, &part[k]))
				part_null |= (uint8) (1 << k), part[k] = 0;
		for (int k = 0; k < w->spec.n_ord; k++)
			if (!vec_i64(b->ord[k], r, &ord[k]))
				ord_null |= (uint8) (1 << k), ord[k] = 0;
		new_part = !w->have_prev || part_null != w->prev_part_null || memcmp(part, w->prev_part, sizeof(int64) * (size_t) w->spec.n_part) != 0;
		if (!new_part)
			new_peer = ord_null != w->prev_ord_null || memcmp(ord, w->prev_ord, sizeof(int64) * (size_t) w->spec.n_ord) != 0;
		if (new_part || new_peer)
			winx_flush(w);
		if (new_part)
		{
			winx_lead_drain(w);
			winx_new_partition(w);
		}
		else if (new_peer)
		{
			w->rank = w->part_idx + 1;
			w->dense++;
		}
		w->have_prev = true;
		memcpy(w->prev_part, part, sizeof(int64) * (size_t) w->spec.n_part);
		memcpy(w->prev_ord, ord, sizeof(int64) * (size_t) w->spec.n_ord);
		w->prev_part_null = part_null;
		w->prev_ord_null = ord_null;
		w->part_idx++;
		winx_row_cells(w, b, r, cells);
		winx_row_fold(w, b, r);
		if (w->spec.range_frame)
			winx_pend(w, tag, w->part_idx, cells);
		else
			winx_emit_row(w, tag, w->part_idx, cells);
	}
}

void
sift_winx_run(SiftWinx *w)
{
	SiftWinxBatch b;

	for (;;)
	{
		memset(&b, 0, sizeof(b));
		if (!w->next(w->src, &b))
			break;
		winx_batch(w, &b);
	}
	winx_flush(w);
	winx_lead_drain(w);
}

const SiftWinxStats *
sift_winx_stats(const SiftWinx *w)
{
	return &w->st;
}

void
sift_winx_free(SiftWinx *w)
{
	for (int f = 0; f < w->spec.nfuncs; f++)
		if (w->fs[f].ring != NULL)
			pfree(w->fs[f].ring);
	if (w->pspill != NULL)
		BufFileClose(w->pspill);
	if (w->ptag != NULL)
		pfree(w->ptag);
	if (w->pnum != NULL)
		pfree(w->pnum);
	if (w->pcells != NULL)
		pfree(w->pcells);
	if (w->has_lead)
	{
		pfree(w->ltag);
		pfree(w->lres);
		pfree(w->lcells);
	}
	pfree(w);
}
