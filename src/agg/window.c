/*
 * agg/window.c - the streaming window executor of agg/window.h. A batch is compacted to its taken rows and its columns to int64 (a float8 as its bits) with a
 * validity bitmap; the partition and peer boundaries of the whole batch are two bitmaps from stream_run_ne_i64. A row is then one step: close the
 * partition or the peer group if it starts a new one, run the functions that know their value now (ranks, LAG, the ROWS frames), and put the row in the
 * ring of rows not yet emitted; a row leaves the ring when the rows it looks ahead to (LEAD) have arrived and its peer group (RANGE frames) has ended,
 * or when its partition ends, and takes its LEAD and RANGE results as it leaves.
 */
#include "postgres.h"

#include <stdalign.h>

#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"

#include "agg/kern/int.h"
#include "agg/window.h"

typedef struct FuncState
{
	uint64		count;			/* rows (COUNT(*)) or non-NULL values in the frame */
	SiftI128	sum_i;
	double		sum_f;
	SiftWinVal	mm;
	bool		has_first;
	bool		first_null;
	SiftWinVal	first;
	int64		rank;
	int64		dense;
	SiftWinVal *hist;			/* LAG: the arguments of the last offset rows, row r at r % offset */
	uint8	   *hist_null;
} FuncState;

struct SiftWindowExec
{
	SiftWindowSpec spec;
	MemoryContext cxt;
	uint64		max_bytes;
	uint64		fixed_bytes;	/* the column buffers and the LAG histories: what the ring shares max_bytes with */
	SiftWinNext next;
	SiftWinSkip skip;
	void	   *src;
	SiftWinEmit emit;
	void	   *emit_arg;

	bool		has_cur;
	int64		cur_part[SIFT_WIN_MAX_KEYS];
	int64		cur_ord[SIFT_WIN_MAX_KEYS];
	uint8		cur_pnull;
	uint8		cur_onull;
	uint64		n_recv;			/* rows of the partition taken so far = the index of the next one */
	uint64		pend_first;		/* the index of the first row in the ring */
	uint64		group_first;	/* the index of the first row of the open peer group */
	BufFile    *spill;			/* the rows of the open peer group that left the ring, in order: they are the rows before pend_first */
	uint64		n_spilled;
	bool		skipping;
	uint32		max_lead;
	bool		has_range;
	SiftWinRow *ring;
	uint32		cap;
	FuncState	fs[SIFT_WIN_MAX_FUNCS];
	SiftWinRow	cand;			/* SWIN_MODE_DISTINCT_LAST: the last row of the partition so far */
	bool		has_cand;
	uint64		seen;			/* rows that reached the offset and limit gate */
	bool		done;
	SiftWinStats st;

	/* the batch in the form the loop wants: part keys, order keys, then arguments, SIFT_VEC_LEN int64 each */
	int64	   *col;
	uint64	   *colv;
	uint16	   *sel;
	uint64		pchg[SIFT_VEC_WORDS];
	uint64		ochg[SIFT_VEC_WORDS];
	uint64		tmp[SIFT_VEC_WORDS];
	uint16		cn;
	const SiftWinBatch *batch;
};

#define COL(w, c)	((w)->col + (size_t) (c) * SIFT_VEC_LEN)
#define COLV(w, c)	((w)->colv + (size_t) (c) * SIFT_VEC_WORDS)
#define PCOL(k)		(k)
#define OCOL(w, k)	((w)->spec.n_part + (k))
#define ACOL(w, f)	((w)->spec.n_part + (w)->spec.n_ord + (f))
#define BIT(m, i)	(((m)[(i) >> 6] >> ((i) & 63)) & 1)

SiftVecType
sift_win_result_type(const SiftWinFuncSpec *f)
{
	switch (f->kind)
	{
		case SWIN_ROW_NUMBER:
		case SWIN_RANK:
		case SWIN_DENSE_RANK:
		case SWIN_COUNT_STAR:
		case SWIN_COUNT:
			return SVEC_I64;
		case SWIN_SUM:
			return f->arg_type == SVEC_F64 ? SVEC_F64 : SVEC_NUM128;
		case SWIN_AVG:
			return f->arg_type == SVEC_F64 ? SVEC_F64 : SVEC_NUM128;
		default:
			return f->arg_type;
	}
}

static bool
is_range_func(const SiftWinFuncSpec *f)
{
	return f->kind >= SWIN_COUNT_STAR && f->range_frame;
}

static void *
alloc_i128(MemoryContext cxt, Size size)
{
	return MemoryContextAllocAligned(cxt, size, alignof(SiftI128), MCXT_ALLOC_ZERO);
}

/* the bytes a window holds whatever its input: the columns of a batch, the selection, the history of each LAG and the first ring */
static uint64
fixed_bytes(const SiftWindowSpec *spec)
{
	const int	ncols = spec->n_part + spec->n_ord + spec->n_funcs;
	uint64		b = (uint64) ncols * (SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_WORDS * sizeof(uint64)) + (SIFT_VEC_LEN + SIFT_SEL_SLACK) * sizeof(uint16);

	for (int f = 0; f < spec->n_funcs; f++)
		if (spec->funcs[f].kind == SWIN_LAG)
			b += (uint64) spec->funcs[f].offset * (sizeof(SiftWinVal) + 1);
	return b;
}

SiftWindowExec *
sift_window_create(MemoryContext cxt, const SiftWindowSpec *spec, uint64 max_bytes, SiftWinNext next, SiftWinSkip skip, void *src, SiftWinEmit emit,
				   void *emit_arg)
{
	SiftWindowExec *w = alloc_i128(cxt, sizeof(SiftWindowExec));
	const int	ncols = spec->n_part + spec->n_ord + spec->n_funcs;

	Assert(spec->n_part <= SIFT_WIN_MAX_KEYS && spec->n_ord <= SIFT_WIN_MAX_KEYS && spec->n_funcs <= SIFT_WIN_MAX_FUNCS);
	w->spec = *spec;
	w->fixed_bytes = fixed_bytes(spec);
	w->done = spec->limit == 0;
	w->cxt = cxt;
	w->next = next;
	w->skip = skip;
	w->src = src;
	w->emit = emit;
	w->emit_arg = emit_arg;
	w->col = MemoryContextAllocAligned(cxt, (Size) ncols * SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	w->colv = MemoryContextAllocAligned(cxt, (Size) ncols * SIFT_VEC_WORDS * sizeof(uint64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	w->sel = MemoryContextAllocAligned(cxt, (SIFT_VEC_LEN + SIFT_SEL_SLACK) * sizeof(uint16), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	for (int f = 0; f < spec->n_funcs; f++)
	{
		const SiftWinFuncSpec *fn = &spec->funcs[f];

		if (fn->kind == SWIN_LEAD)
			w->max_lead = Max(w->max_lead, fn->offset);
		w->has_range |= is_range_func(fn);
		if ((fn->kind == SWIN_LAG || fn->kind == SWIN_LEAD) && fn->offset > SIFT_WIN_MAX_OFFSET)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("window offset %u is above the limit %d", fn->offset, SIFT_WIN_MAX_OFFSET),
							errhint("Use an offset of at most %d.", SIFT_WIN_MAX_OFFSET)));
		if (fn->kind == SWIN_LAG && fn->offset > 0)
		{
			w->fs[f].hist = alloc_i128(cxt, (Size) fn->offset * sizeof(SiftWinVal));
			w->fs[f].hist_null = MemoryContextAllocZero(cxt, fn->offset);
		}
	}
	if (spec->topk.active && spec->mode != SWIN_MODE_WINDOW)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a top-K of each partition is of a window, not of DISTINCT ON"),
						errhint("Run the DISTINCT ON without a top-K.")));
	if (spec->topk.active && (w->max_lead > 0 || w->has_range))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a top-K of each partition cannot go with a LEAD or a RANGE frame"),
						errhint("Use ROWS frames and no LEAD, or take the rows after the window.")));
	w->cap = 64;
	while (w->cap < w->max_lead + 2)
		w->cap *= 2;
	w->max_bytes = Max(max_bytes, w->fixed_bytes + (uint64) w->cap * sizeof(SiftWinRow));
	w->ring = alloc_i128(cxt, (Size) w->cap * sizeof(SiftWinRow));
	return w;
}

void
sift_window_free(SiftWindowExec *w)
{
	for (int f = 0; f < w->spec.n_funcs; f++)
	{
		if (w->fs[f].hist)
			pfree(w->fs[f].hist);
		if (w->fs[f].hist_null)
			pfree(w->fs[f].hist_null);
	}
	if (w->spill)
		BufFileClose(w->spill);
	pfree(w->ring);
	pfree(w->col);
	pfree(w->colv);
	pfree(w->sel);
	pfree(w);
}

const SiftWinStats *
sift_window_stats(const SiftWindowExec *w)
{
	return &w->st;
}

/* ---- output ---- */

static void
emit_row(SiftWindowExec *w, const SiftWinRow *row)
{
	if (w->seen++ < w->spec.offset)
		return;
	w->emit(w->emit_arg, row);
	w->st.rows_out++;
	if (w->spec.limit != SIFT_WIN_NO_LIMIT && w->seen - w->spec.offset >= w->spec.limit)
		w->done = true;
}

static SiftWinRow *
slot_of(SiftWindowExec *w, uint64 idx)
{
	return &w->ring[idx & (w->cap - 1)];
}

static void
set_res(SiftWinRow *r, int f, SiftWinVal v, bool null)
{
	r->res[f] = v;
	r->res_nulls = (uint16) ((r->res_nulls & ~(1u << f)) | ((null ? 1u : 0u) << f));
}

static void
default_of(const SiftWinFuncSpec *fn, SiftWinRow *r, int f)
{
	SiftWinVal	z;

	memset(&z, 0, sizeof(z));
	set_res(r, f, fn->has_default ? fn->default_val : z, !fn->has_default);
}

/* the LEAD values of the row at idx, from the rows ahead that have arrived (idx + offset < n_recv), or their default */
static void
resolve_lead(SiftWindowExec *w, SiftWinRow *r, uint64 idx)
{
	for (int f = 0; f < w->spec.n_funcs; f++)
	{
		const SiftWinFuncSpec *fn = &w->spec.funcs[f];
		const uint64 target = idx + fn->offset;

		if (fn->kind != SWIN_LEAD)
			continue;
		if (fn->offset == 0)
			set_res(r, f, r->arg[f], (r->arg_nulls >> f) & 1);
		else if (target < w->n_recv)
			set_res(r, f, slot_of(w, target)->arg[f], (slot_of(w, target)->arg_nulls >> f) & 1);
		else
			default_of(fn, r, f);
	}
}

/* the rows of the open peer group that no LEAD waits for leave the ring for the file, with their LEAD values: the last max_lead rows stay, and the rows before pend_first are the file's */
static void
spill_ring(SiftWindowExec *w)
{
	const uint64 upto = w->n_recv - w->max_lead;

	if (!w->spill)
		w->spill = BufFileCreateTemp(false);
	for (uint64 i = w->pend_first; i < upto; i++)
	{
		SiftWinRow *r = slot_of(w, i);

		resolve_lead(w, r, i);
		BufFileWrite(w->spill, r, sizeof(SiftWinRow));
	}
	w->n_spilled += upto - w->pend_first;
	w->pend_first = upto;
}

static void
grow_ring(SiftWindowExec *w)
{
	const uint32 ncap = w->cap * 2;
	SiftWinRow *nr;

	if (w->fixed_bytes + (uint64) ncap * sizeof(SiftWinRow) > w->max_bytes)
	{
		spill_ring(w);
		return;
	}
	nr = alloc_i128(w->cxt, (Size) ncap * sizeof(SiftWinRow));
	for (uint64 i = w->pend_first; i < w->n_recv; i++)
		nr[i & (ncap - 1)] = w->ring[i & (w->cap - 1)];
	pfree(w->ring);
	w->ring = nr;
	w->cap = ncap;
}

/* sum as a numeric: sum = (hi * 2^32 + lo >> 32) * 2^32 + (lo & 0xFFFFFFFF), each step PostgreSQL's own numeric arithmetic */
static Numeric
numeric_of_i128(SiftI128 sum)
{
	const Datum two32 = NumericGetDatum(int64_to_numeric(INT64CONST(1) << 32));
	Datum		n = NumericGetDatum(int64_to_numeric(sum.hi));
	const uint64 parts[2] = {sum.lo >> 32, sum.lo & UINT32_MAX};

	for (int i = 0; i < 2; i++)
	{
		const Datum shifted = DirectFunctionCall2(numeric_mul, n, two32),
					part = NumericGetDatum(int64_to_numeric((int64) parts[i]));

		pfree(DatumGetPointer(n));
		n = DirectFunctionCall2(numeric_add, shifted, part);
		pfree(DatumGetPointer(shifted));
		pfree(DatumGetPointer(part));
	}
	pfree(DatumGetPointer(two32));
	return DatumGetNumeric(n);
}

/* avg of an int4 or int8 as PostgreSQL gives it: the exact sum over the count by numeric_div, at the scale it picks */
static SiftWinNum
avg_numeric(SiftI128 sum, uint64 count)
{
	Numeric		total = numeric_of_i128(sum),
				rows = int64_to_numeric((int64) count),
				avg = DatumGetNumeric(DirectFunctionCall2(numeric_div, NumericGetDatum(total), NumericGetDatum(rows)));
	char	   *text = DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(avg)));
	const char *point = strchr(text, '.');
	unsigned __int128 coef = 0;
	SiftWinNum	r;

	for (const char *c = text + (text[0] == '-'); *c; c++)
		if (*c != '.')
			coef = coef * 10 + (unsigned) (*c - '0');
	r.scale = point ? (int32) strlen(point + 1) : 0;
	SIFT_INVARIANT(coef >> 95 == 0, "sift: the average of integers has more than 95 bits");
	r.coef_lo = (uint64) coef;
	r.coef_hi = (int32) (coef >> 64);
	if (text[0] == '-')
	{
		const unsigned __int128 neg = -coef;

		r.coef_lo = (uint64) neg;
		r.coef_hi = (int32) (neg >> 64);
	}
	pfree(total);
	pfree(rows);
	pfree(avg);
	pfree(text);
	return r;
}

/* the value of a function over its frame so far; false when it is NULL */
static bool
snapshot(const SiftWindowExec *w, int f, SiftWinVal *v)
{
	const SiftWinFuncSpec *fn = &w->spec.funcs[f];
	const FuncState *s = &w->fs[f];
	const bool	flt = fn->arg_type == SVEC_F64;

	memset(v, 0, sizeof(*v));
	switch (fn->kind)
	{
		case SWIN_COUNT_STAR:
		case SWIN_COUNT:
			v->i = (int64) s->count;
			return true;
		case SWIN_SUM:
			if (!s->count)
				return false;
			if (flt)
				v->f = s->sum_f;
			else
				v->w = s->sum_i;
			return true;
		case SWIN_AVG:
			if (!s->count)
				return false;
			if (flt)
				v->f = s->sum_f / (double) s->count;
			else
				v->num = avg_numeric(s->sum_i, s->count);
			return true;
		default:
			if (!s->count)
				return false;
			*v = s->mm;
			return true;
	}
}

static void
accumulate(SiftWindowExec *w, int f, SiftWinVal arg, bool null)
{
	const SiftWinFuncSpec *fn = &w->spec.funcs[f];
	FuncState  *s = &w->fs[f];
	const bool	flt = fn->arg_type == SVEC_F64;

	if (fn->kind == SWIN_COUNT_STAR)
	{
		s->count++;
		return;
	}
	if (null)
		return;
	if (fn->kind == SWIN_SUM || fn->kind == SWIN_AVG)
	{
		if (flt)
			s->sum_f = s->count ? float8_pl(s->sum_f, arg.f) : arg.f;
		else
			s->sum_i = ak6_i128_of(ak6_u128_of(s->sum_i) + (ak6_u128) (__int128) arg.i);
	}
	else if (fn->kind == SWIN_MIN || fn->kind == SWIN_MAX)
	{
		const bool	mn = fn->kind == SWIN_MIN;

		if (!s->count)
			s->mm = arg;
		else if (flt)
		{
			const int	c = ak6_f8_cmp(s->mm.f, arg.f);

			if (mn ? !(c < 0) : !(c > 0))
				s->mm = arg;
		}
		else if (mn ? arg.i < s->mm.i : arg.i > s->mm.i)
			s->mm = arg;
	}
	s->count++;
}

/* the rows of the group that left the ring come back first, in order, with the value of the group */
static void
replay_spill(SiftWindowExec *w, const SiftWinVal *v, const bool *ok)
{
	SiftWinRow	r;

	BufFileSeek(w->spill, 0, 0, SEEK_SET);
	for (; w->n_spilled && !w->done; w->n_spilled--)
	{
		CHECK_FOR_INTERRUPTS();
		BufFileReadExact(w->spill, &r, sizeof(r));
		for (int f = 0; f < w->spec.n_funcs; f++)
			if (is_range_func(&w->spec.funcs[f]))
				set_res(&r, f, v[f], !ok[f]);
		emit_row(w, &r);
	}
	BufFileClose(w->spill);
	w->spill = NULL;
	w->n_spilled = 0;
}

/* the rows [from, to) of the group, in the file and in the ring, are one peer group that has ended: the RANGE functions give them their value */
static void
stamp_range(SiftWindowExec *w, uint64 from, uint64 to)
{
	SiftWinVal	v[SIFT_WIN_MAX_FUNCS];
	bool		ok[SIFT_WIN_MAX_FUNCS];

	for (int f = 0; f < w->spec.n_funcs; f++)
		if (is_range_func(&w->spec.funcs[f]))
			ok[f] = snapshot(w, f, &v[f]);
	if (w->n_spilled)
		replay_spill(w, v, ok);
	for (int f = 0; f < w->spec.n_funcs; f++)
		if (is_range_func(&w->spec.funcs[f]))
			for (uint64 i = Max(from, w->pend_first); i < to; i++)
				set_res(slot_of(w, i), f, v[f], !ok[f]);
}

/* emits the rows that are ready: LEAD gets its value from the row ahead, or its default */
static void
drain(SiftWindowExec *w, bool closing)
{
	uint64		upto = w->n_recv;

	if (!closing)
	{
		upto = w->n_recv > w->max_lead ? w->n_recv - w->max_lead : 0;
		if (w->has_range)
			upto = Min(upto, w->group_first);
	}
	for (; w->pend_first < upto && !w->done; w->pend_first++)
	{
		SiftWinRow *r = slot_of(w, w->pend_first);

		resolve_lead(w, r, w->pend_first);
		emit_row(w, r);
	}
}

static void
close_partition(SiftWindowExec *w)
{
	if (!w->has_cur)
		return;
	if (w->spec.mode == SWIN_MODE_DISTINCT_LAST && w->has_cand)
		emit_row(w, &w->cand);
	w->has_cand = false;
	if (w->spec.mode == SWIN_MODE_WINDOW)
	{
		if (w->has_range && w->n_recv > w->group_first)
			stamp_range(w, w->group_first, w->n_recv);
		drain(w, true);
	}
	w->n_recv = 0;
	w->pend_first = 0;
	w->group_first = 0;
}

/* ---- the batch ---- */

static void
norm_col(SiftWindowExec *w, int c, const SiftVec *v)
{
	int64	   *d = COL(w, c);
	uint64	   *dv = COLV(w, c);

	memset(dv, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint16 j = 0; j < w->cn; j++)
	{
		const uint32 r = w->batch->mask ? w->sel[j] : j;
		const bool	valid = v->all_valid || BIT(v->valid, r);
		int64		x = 0;

		if (valid)
		{
			if (v->type == SVEC_I32)
				x = ((const int32 *) v->data)[r];
			else
				memcpy(&x, (const int64 *) v->data + r, 8);
		}
		d[j] = x;
		dv[j >> 6] |= (uint64) valid << (j & 63);
	}
}

static SiftWinVal
val_at(const SiftWindowExec *w, int f, uint16 i, bool *null)
{
	const SiftWinFuncSpec *fn = &w->spec.funcs[f];
	SiftWinVal	v;

	memset(&v, 0, sizeof(v));
	*null = !BIT(COLV(w, ACOL(w, f)), i);
	if (!*null)
	{
		if (fn->arg_type == SVEC_F64)
			memcpy(&v.f, COL(w, ACOL(w, f)) + i, 8);
		else
			v.i = COL(w, ACOL(w, f))[i];
	}
	return v;
}

static bool
key_differs(const SiftWindowExec *w, int c0, int n, uint16 i, const int64 *cur, uint8 cur_nulls)
{
	for (int k = 0; k < n; k++)
	{
		const bool	isnull = !BIT(COLV(w, c0 + k), i);

		if (isnull != (bool) ((cur_nulls >> k) & 1) || (!isnull && COL(w, c0 + k)[i] != cur[k]))
			return true;
	}
	return false;
}

static uint16
next_set(const uint64 *m, uint16 from, uint16 n)
{
	for (uint16 w = from >> 6; w < AK6_NW(n); w++)
	{
		const uint64 x = m[w] & (w == (from >> 6) ? ~UINT64CONST(0) << (from & 63) : ~UINT64CONST(0));

		if (x)
			return Min(n, (uint16) (w * 64 + __builtin_ctzll(x)));
	}
	return n;
}

static void
skip_partition(SiftWindowExec *w)
{
	w->skipping = true;
	w->st.skips++;
	if (w->skip)
		w->skip(w->src, w->cur_part, w->cur_pnull);
}

static void
fill_row(SiftWindowExec *w, uint16 i, SiftWinRow *r)
{
	memset(r, 0, sizeof(*r));
	r->tag = w->batch->tag ? w->batch->tag[w->batch->mask ? w->sel[i] : i] : 0;
	r->part_idx = w->n_recv;
	for (int k = 0; k < w->spec.n_part; k++)
		r->part[k] = COL(w, PCOL(k))[i];
	for (int k = 0; k < w->spec.n_part; k++)
		r->part_nulls |= (uint8) (!BIT(COLV(w, PCOL(k)), i) << k);
	for (int k = 0; k < w->spec.n_ord; k++)
	{
		r->ord[k] = COL(w, OCOL(w, k))[i];
		r->ord_nulls |= (uint8) (!BIT(COLV(w, OCOL(w, k)), i) << k);
	}
	for (int f = 0; f < w->spec.n_funcs; f++)
	{
		bool		null = true;

		if (w->batch->arg[f])
			r->arg[f] = val_at(w, f, i, &null);
		r->arg_nulls |= (uint16) (null << f);
	}
}

/* the rank-like value the top-K of a partition tests for the row that is about to be taken */
static int64
topk_value(const SiftWindowExec *w, bool peer_start)
{
	const FuncState *s = &w->fs[w->spec.topk.func];

	switch (w->spec.funcs[w->spec.topk.func].kind)
	{
		case SWIN_ROW_NUMBER:
			return (int64) w->n_recv + 1;
		case SWIN_RANK:
			return peer_start ? (int64) w->n_recv + 1 : s->rank;
		default:
			return peer_start ? s->dense + 1 : s->dense;
	}
}

/* runs the functions on the row i of the batch and puts it in the ring */
static void
take_row(SiftWindowExec *w, uint16 i, bool peer_start)
{
	SiftWinRow *r;

	if (w->n_recv - w->pend_first >= w->cap)
		grow_ring(w);
	r = slot_of(w, w->n_recv);
	fill_row(w, i, r);
	for (int f = 0; f < w->spec.n_funcs; f++)
	{
		const SiftWinFuncSpec *fn = &w->spec.funcs[f];
		FuncState  *s = &w->fs[f];
		const bool	null = (r->arg_nulls >> f) & 1;
		SiftWinVal	v;

		memset(&v, 0, sizeof(v));
		switch (fn->kind)
		{
			case SWIN_ROW_NUMBER:
				v.i = (int64) w->n_recv + 1;
				set_res(r, f, v, false);
				break;
			case SWIN_RANK:
				if (peer_start)
					s->rank = (int64) w->n_recv + 1;
				v.i = s->rank;
				set_res(r, f, v, false);
				break;
			case SWIN_DENSE_RANK:
				s->dense += peer_start;
				v.i = s->dense;
				set_res(r, f, v, false);
				break;
			case SWIN_LAG:
				if (fn->offset == 0)
					set_res(r, f, r->arg[f], null);
				else
				{
					const uint32 at = (uint32) (w->n_recv % fn->offset);

					if (w->n_recv >= fn->offset)
						set_res(r, f, s->hist[at], s->hist_null[at]);
					else
						default_of(fn, r, f);
					s->hist[at] = r->arg[f];
					s->hist_null[at] = null;
				}
				break;
			case SWIN_LEAD:
				break;
			case SWIN_FIRST_VALUE:
				if (!w->n_recv)
				{
					s->first = r->arg[f];
					s->first_null = null;
				}
				set_res(r, f, s->first, s->first_null);
				break;
			default:
				accumulate(w, f, r->arg[f], null);
				if (!fn->range_frame)
				{
					const bool	ok = snapshot(w, f, &v);

					set_res(r, f, v, !ok);
				}
		}
	}
	w->n_recv++;
	w->st.max_pending = Max(w->st.max_pending, w->n_recv - w->pend_first);
}

static void
reset_partition(SiftWindowExec *w, uint16 i)
{
	for (int k = 0; k < w->spec.n_part; k++)
		w->cur_part[k] = COL(w, PCOL(k))[i];
	w->cur_pnull = 0;
	for (int k = 0; k < w->spec.n_part; k++)
		w->cur_pnull |= (uint8) (!BIT(COLV(w, PCOL(k)), i) << k);
	w->has_cur = true;
	w->skipping = false;
	for (int f = 0; f < w->spec.n_funcs; f++)
	{
		FuncState  *s = &w->fs[f];

		s->count = 0;
		memset(&s->sum_i, 0, sizeof(s->sum_i));
		s->sum_f = 0;
		memset(&s->mm, 0, sizeof(s->mm));
		s->rank = 0;
		s->dense = 0;
	}
}

static void
row_step(SiftWindowExec *w, uint16 i, bool pstart, bool ostart)
{
	bool		peer_start = ostart;

	if (pstart)
	{
		close_partition(w);
		if (w->done)
			return;
		reset_partition(w, i);
		peer_start = true;
	}
	else if (ostart && w->spec.mode == SWIN_MODE_WINDOW && w->has_range)
		stamp_range(w, w->group_first, w->n_recv);
	if (peer_start)
	{
		for (int k = 0; k < w->spec.n_ord; k++)
			w->cur_ord[k] = COL(w, OCOL(w, k))[i];
		w->cur_onull = 0;
		for (int k = 0; k < w->spec.n_ord; k++)
			w->cur_onull |= (uint8) (!BIT(COLV(w, OCOL(w, k)), i) << k);
		w->group_first = w->n_recv;
		if (w->spec.mode == SWIN_MODE_WINDOW)
			drain(w, false);
	}
	w->st.rows_looked++;
	if (w->spec.mode == SWIN_MODE_DISTINCT_FIRST)
	{
		SiftWinRow	r;

		fill_row(w, i, &r);
		emit_row(w, &r);
		skip_partition(w);
		return;
	}
	if (w->spec.mode == SWIN_MODE_DISTINCT_LAST)
	{
		fill_row(w, i, &w->cand);
		w->has_cand = true;
		w->n_recv++;
		return;
	}
	if (w->spec.topk.active && topk_value(w, peer_start) > w->spec.topk.k)
	{
		skip_partition(w);
		return;
	}
	take_row(w, i, peer_start);
	drain(w, false);
	if (w->spec.topk.active && w->spec.funcs[w->spec.topk.func].kind == SWIN_ROW_NUMBER && (int64) w->n_recv >= w->spec.topk.k)
		skip_partition(w);
}

static void
consume_batch(SiftWindowExec *w, const SiftWinBatch *b)
{
	const int	nk = w->spec.n_part,
				no = w->spec.n_ord;

	w->batch = b;
	w->cn = b->mask ? sift_aggk6->mask_to_sel16(w->sel, b->mask, b->n) : b->n;
	w->st.rows_in += w->cn;
	if (!w->cn)
		return;
	for (int k = 0; k < nk; k++)
		norm_col(w, PCOL(k), b->part[k]);
	for (int k = 0; k < no; k++)
		norm_col(w, OCOL(w, k), b->ord[k]);
	for (int f = 0; f < w->spec.n_funcs; f++)
		if (b->arg[f])
			norm_col(w, ACOL(w, f), b->arg[f]);
	memset(w->pchg, 0, sizeof(w->pchg));
	memset(w->ochg, 0, sizeof(w->ochg));
	for (int k = 0; k < nk + no; k++)
	{
		uint64	   *dst = k < nk ? w->pchg : w->ochg;

		sift_aggk6->stream_run_ne_i64(w->tmp, COL(w, k), COLV(w, k), w->cn);
		for (int x = 0; x < AK6_NW(w->cn); x++)
			dst[x] |= w->tmp[x];
	}
	for (uint16 i = 0; i < w->cn && !w->done; i++)
	{
		const bool	pstart = i == 0 ? (!w->has_cur || key_differs(w, 0, nk, 0, w->cur_part, w->cur_pnull)) : BIT(w->pchg, i);

		if (w->skipping && !pstart)
		{
			i = next_set(w->pchg, i + 1, w->cn) - 1;
			continue;
		}
		row_step(w, i, pstart, i == 0 ? key_differs(w, nk, no, 0, w->cur_ord, w->cur_onull) : (bool) BIT(w->ochg, i));
	}
}

bool
sift_window_step(SiftWindowExec *w)
{
	SiftWinBatch b;

	memset(&b, 0, sizeof(b));
	if (!w->done && w->next(w->src, &b))
	{
		consume_batch(w, &b);
		return true;
	}
	if (!w->done)
		close_partition(w);
	w->done = true;
	w->has_cur = false;
	return false;
}

void
sift_window_run(SiftWindowExec *w)
{
	while (sift_window_step(w))
		;
}

uint32
sift_win_tail_tie_first(const SiftWinZone *zones, uint32 n)
{
	uint32		lo = 0,
				hi = n - 1;

	Assert(n >= 1);
	if (zones[n - 1].has_null)
	{
		while (lo < hi)
		{
			const uint32 mid = lo + (hi - lo) / 2;

			if (zones[mid].has_null)
				hi = mid;
			else
				lo = mid + 1;
		}
		return lo;
	}
	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (zones[mid].max >= zones[n - 1].max)
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}
