/*
 * agg/v6/par/winrun.c - the driver of agg/v6/par/winrun.h: the leader makes the items, the threads run agg/v6/par/winpart.h over them, the leader gives the rows of each item to the node in order.
 */
/* check-memory: allow-allocs the arrays of an item follow its rows, and the items in flight are bounded by their share of work_mem */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/float.h"

#include "agg/v6/par/winpart.h"
#include "agg/v6/par/winrun.h"
#include "agg/v6/par/winspill.h"
#include "agg/v6/batch.h"
#include "core/memtrack.h"
#include "core/par/morsel.h"
#include "core/par/pool.h"

#define ITEM_ROWS_MAX	16384
#define ITEM_ROWS_MIN	128

typedef struct WinSlot
{
	uint32		cap;			/* the rows of the input arrays */
	uint32		pcap;
	uint32		ocap;			/* the rows of the output arrays */
	uint32		n;
	uint32		nparts;
	uint64		bytes;			/* what the item counts against the share of work_mem while it is in flight */
	uint64	   *tag;
	int64	   *ord[SIFT_WIN_MAX_KEYS];
	uint8	   *ord_null[SIFT_WIN_MAX_KEYS];
	int64	   *arg[SIFT_WIN_MAX_FUNCS];
	uint8	   *arg_null[SIFT_WIN_MAX_FUNCS];
	uint32	   *pstart;
	uint32	   *perm;
	uint32	   *tmp;
	uint8	   *peer;
	uint64	   *otag;
	SiftWinVal *res;
	uint16	   *res_nulls;
	SiftParWinSpill *spill;		/* the last partition of the item is more than max_part_bytes: its rows are here, and the leader runs them when the item is released */
	SiftParWinItem item;
} WinSlot;

typedef struct ParWin
{
	MemoryContext cx;
	SiftParWinSpec ws;
	SiftV6Source *src;
	SiftWinCarry *carry;
	SiftV6ParWinEmit emit;
	void	   *emit_arg;
	int			n_part;
	int			part_slot[SIFT_WIN_MAX_KEYS];
	int			ord_slot[SIFT_WIN_MAX_KEYS];
	int			arg_slot[SIFT_WIN_MAX_FUNCS];
	WinSlot    *slot;
	uint32		nslots;
	SiftV6Batch cur;
	uint32		cpos;
	bool		cur_live;
	bool		src_done;
	bool		have_prev;
	int64		prev[SIFT_WIN_MAX_KEYS];
	uint8		prev_null[SIFT_WIN_MAX_KEYS];
	uint64		part_bytes;
	uint64		serial_row_bytes;	/* what v6_win_partsort.c counts for a row whose every column holds a value */
	uint64		max_part_bytes;
	uint64		row_bytes;		/* what an item takes for a row */
	uint32		target;			/* the rows an item reaches before it ends at the next partition */
	uint64		budget;
	uint64		inflight_bytes;
	uint32		inflight;
	uint32		nspill;			/* the items that hold a sort: one at a time, each holds up to max_part_bytes */
	uint64		rows;
} ParWin;

static size_t
elem_size(SiftVecType t)
{
	switch (t)
	{
		case SVEC_I32:
			return 4;
		case SVEC_I64:
		case SVEC_F64:
		case SVEC_NUM64:
			return 8;
		default:
			return 16;
	}
}

bool
sift_v6_par_win_wanted(const SiftV6WindowSpec *spec, const SiftV6Input *in)
{
	const SiftWindowSpec *win = &spec->win;

	if (spec->winx != NULL || !spec->partsort || win->mode != SWIN_MODE_WINDOW || win->topk.active || win->n_part == 0 || sift_par_pool_threads() < 2)
		return false;
	for (int k = 0; k < win->n_part; k++)
		if (in->col[spec->part_slot[k]].type != SVEC_I32 && in->col[spec->part_slot[k]].type != SVEC_I64)
			return false;
	for (int k = 0; k < win->n_ord; k++)
	{
		const SiftVecType t = in->col[spec->ord_slot[k]].type;

		if (t != SVEC_I32 && t != SVEC_I64 && t != SVEC_F64)
			return false;
	}
	for (int f = 0; f < win->n_funcs; f++)
		if (!sift_par_win_func_ok(&win->funcs[f]) || (win->funcs[f].range_frame && win->n_ord == 0))
			return false;
	return true;
}

/* ---- the items ---- */

static void *
grow_array(ParWin *w, void *p, uint32 oldcap, uint32 cap, size_t esz)
{
	return oldcap == 0 ? MemoryContextAlloc(w->cx, esz * cap) : repalloc(p, esz * cap);
}

static void
in_grow(ParWin *w, WinSlot *s, uint32 need)
{
	const uint32 cap = Max(Max(need, s->cap * 2), 1024);

	s->tag = grow_array(w, s->tag, s->cap, cap, sizeof(uint64));
	for (int k = 0; k < w->ws.n_ord; k++)
	{
		s->ord[k] = grow_array(w, s->ord[k], s->cap, cap, sizeof(int64));
		s->ord_null[k] = grow_array(w, s->ord_null[k], s->cap, cap, sizeof(uint8));
	}
	for (int f = 0; f < w->ws.nfuncs; f++)
		if (w->arg_slot[f] >= 0)
		{
			s->arg[f] = grow_array(w, s->arg[f], s->cap, cap, sizeof(int64));
			s->arg_null[f] = grow_array(w, s->arg_null[f], s->cap, cap, sizeof(uint8));
		}
	s->cap = cap;
}

static void
out_free(WinSlot *s)
{
	if (s->ocap == 0)
		return;
	pfree(s->perm);
	pfree(s->tmp);
	pfree(s->peer);
	pfree(s->otag);
	pfree(s->res);
	pfree(s->res_nulls);
	s->ocap = 0;
}

static void
out_reserve(ParWin *w, WinSlot *s, uint32 n)
{
	if (s->ocap >= n)
		return;
	out_free(s);
	s->perm = MemoryContextAlloc(w->cx, sizeof(uint32) * n);
	s->tmp = MemoryContextAlloc(w->cx, sizeof(uint32) * n);
	s->peer = MemoryContextAlloc(w->cx, n);
	s->otag = MemoryContextAlloc(w->cx, sizeof(uint64) * n);
	s->res = MemoryContextAllocAligned(w->cx, sizeof(SiftWinVal) * w->ws.nfuncs * n, _Alignof(SiftWinVal), 0);
	s->res_nulls = MemoryContextAlloc(w->cx, sizeof(uint16) * n);
	s->ocap = n;
}

static void
slot_free(WinSlot *s)
{
	if (s->cap > 0)
	{
		pfree(s->tag);
		for (int k = 0; k < SIFT_WIN_MAX_KEYS; k++)
			if (s->ord[k] != NULL)
			{
				pfree(s->ord[k]);
				pfree(s->ord_null[k]);
				s->ord[k] = NULL;
			}
		for (int f = 0; f < SIFT_WIN_MAX_FUNCS; f++)
			if (s->arg[f] != NULL)
			{
				pfree(s->arg[f]);
				pfree(s->arg_null[f]);
				s->arg[f] = NULL;
			}
		s->cap = 0;
	}
	if (s->pcap > 0)
		pfree(s->pstart);
	s->pcap = 0;
	out_free(s);
}

static inline int64
win_cell(const SiftVec *v, uint32 r, uint8 *null)
{
	*null = !sift_vec_valid_at(v, r);
	if (*null)
		return 0;
	return v->type == SVEC_I32 ? (int64) ((const int32 *) v->data)[r] : ((const int64 *) v->data)[r];
}

static inline bool
batch_row_active(const SiftV6Batch *b, uint32 r)
{
	return b->mask == NULL || ((b->mask[r >> 6] >> (r & 63)) & 1);
}

static bool
starts_partition(const ParWin *w, uint32 r)
{
	if (!w->have_prev)
		return true;
	for (int k = 0; k < w->n_part; k++)
	{
		uint8		null;
		const int64 x = win_cell(&w->cur.col[w->part_slot[k]], r, &null);

		if (null != w->prev_null[k] || x != w->prev[k])
			return true;
	}
	return false;
}

static void
open_partition(ParWin *w, WinSlot *s, uint32 r)
{
	if (s->nparts + 2 > s->pcap)
	{
		const uint32 pcap = Max(s->pcap * 2, 256);

		s->pstart = s->pcap == 0 ? MemoryContextAlloc(w->cx, sizeof(uint32) * pcap) : repalloc(s->pstart, sizeof(uint32) * pcap);
		s->pcap = pcap;
	}
	s->pstart[s->nparts++] = s->n;
	for (int k = 0; k < w->n_part; k++)
		w->prev[k] = win_cell(&w->cur.col[w->part_slot[k]], r, &w->prev_null[k]);
	w->have_prev = true;
	w->part_bytes = 0;
}

static void
spill_put_item_row(const ParWin *w, WinSlot *s, uint32 i)
{
	int64		ord[SIFT_WIN_MAX_KEYS],
				arg[SIFT_WIN_MAX_FUNCS] = {0};
	uint8		ord_null[SIFT_WIN_MAX_KEYS],
				arg_null[SIFT_WIN_MAX_FUNCS] = {0};

	for (int k = 0; k < w->ws.n_ord; k++)
	{
		ord[k] = s->ord[k][i];
		ord_null[k] = s->ord_null[k][i];
	}
	for (int f = 0; f < w->ws.nfuncs; f++)
		if (w->arg_slot[f] >= 0)
		{
			arg[f] = s->arg[f][i];
			arg_null[f] = s->arg_null[f][i];
		}
	sift_par_win_spill_put(s->spill, s->tag[i], ord, ord_null, arg, arg_null);
}

static void
spill_put_batch_row(const ParWin *w, WinSlot *s, uint32 r)
{
	int64		ord[SIFT_WIN_MAX_KEYS],
				arg[SIFT_WIN_MAX_FUNCS] = {0};
	uint8		ord_null[SIFT_WIN_MAX_KEYS],
				arg_null[SIFT_WIN_MAX_FUNCS] = {0};

	for (int k = 0; k < w->ws.n_ord; k++)
		ord[k] = win_cell(&w->cur.col[w->ord_slot[k]], r, &ord_null[k]);
	for (int f = 0; f < w->ws.nfuncs; f++)
		if (w->arg_slot[f] >= 0)
			arg[f] = win_cell(&w->cur.col[w->arg_slot[f]], r, &arg_null[f]);
	sift_par_win_spill_put(s->spill, w->cur.tag[r], ord, ord_null, arg, arg_null);
}

/* the partition being read is more than max_part_bytes: its rows leave the item for a sort, and the rest of it goes there as it is read */
static void
open_spill(ParWin *w, WinSlot *s)
{
	const uint32 from = s->pstart[s->nparts - 1];

	s->spill = sift_par_win_spill_open(w->cx, &w->ws, w->arg_slot, w->max_part_bytes);
	w->nspill++;
	for (uint32 i = from; i < s->n; i++)
		spill_put_item_row(w, s, i);
	s->n = from;
	s->nparts--;
}

static void
add_row(ParWin *w, WinSlot *s, uint32 r)
{
	const uint32 i = s->n;

	if (s->spill != NULL)
	{
		spill_put_batch_row(w, s, r);
		return;
	}
	if (i == s->cap)
		in_grow(w, s, i + 1);
	s->tag[i] = w->cur.tag[r];
	for (int k = 0; k < w->ws.n_ord; k++)
		s->ord[k][i] = win_cell(&w->cur.col[w->ord_slot[k]], r, &s->ord_null[k][i]);
	for (int f = 0; f < w->ws.nfuncs; f++)
		if (w->arg_slot[f] >= 0)
			s->arg[f][i] = win_cell(&w->cur.col[w->arg_slot[f]], r, &s->arg_null[f][i]);
	s->n++;
	w->part_bytes += w->serial_row_bytes;
	if (w->part_bytes > w->max_part_bytes)
		open_spill(w, s);
}

static bool
next_batch(ParWin *w)
{
	if (w->src_done)
		return false;
	CHECK_FOR_INTERRUPTS();
	if (!w->src->next(w->src, &w->cur))
	{
		w->src_done = true;
		return false;
	}
	SIFT_INVARIANT(w->cur.tag != NULL, "the rows of a window have no tag");
	if (w->carry != NULL)
		sift_win_carry_put(w->carry, &w->cur);
	w->cur_live = true;
	w->cpos = 0;
	return true;
}

/* true: the item has reached its rows (or ends with the partition it sorts) and the next row starts a partition */
static bool
take_rows(ParWin *w, WinSlot *s)
{
	for (; w->cpos < w->cur.n; w->cpos++)
	{
		const uint32 r = w->cpos;
		bool		start;

		if (!batch_row_active(&w->cur, r))
			continue;
		start = starts_partition(w, r);
		if (start && (s->n >= w->target || s->spill != NULL))
			return true;
		if (start)
			open_partition(w, s, r);
		add_row(w, s, r);
	}
	w->cur_live = false;
	return false;
}

static void
seal(ParWin *w, WinSlot *s)
{
	s->pstart[s->nparts] = s->n;
	out_reserve(w, s, s->n);
	s->item = (SiftParWinItem) {.n = s->n,.nparts = s->nparts,.pstart = s->pstart,.tag = s->tag,.perm = s->perm,.tmp = s->tmp,.peer = s->peer,.otag = s->otag,.res = s->res,
		.res_nulls = s->res_nulls};
	for (int k = 0; k < w->ws.n_ord; k++)
	{
		s->item.ord[k] = s->ord[k];
		s->item.ord_null[k] = s->ord_null[k];
	}
	for (int f = 0; f < w->ws.nfuncs; f++)
	{
		s->item.arg[f] = s->arg[f];
		s->item.arg_null[f] = s->arg_null[f];
	}
	s->bytes = (uint64) s->n * w->row_bytes;
	w->inflight_bytes += s->bytes;
	w->inflight++;
	w->rows += s->n;
}

/* ---- the operator ---- */

static bool
win_feed(void *ctx, uint32 item)
{
	ParWin	   *w = ctx;
	WinSlot    *s = &w->slot[item % w->nslots];

	s->n = 0;
	s->nparts = 0;
	for (;;)
	{
		if (!w->cur_live && !next_batch(w))
			break;
		if (take_rows(w, s))
		{
			seal(w, s);
			return true;
		}
	}
	if (s->n == 0 && s->spill == NULL)
		return false;
	seal(w, s);
	return true;
}

static bool
win_may_feed(void *ctx)
{
	const ParWin *w = ctx;

	return w->nspill == 0 && (w->inflight == 0 || w->inflight_bytes < w->budget);
}

static bool
win_phase1(void *ctx, int worker, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	ParWin	   *w = ctx;
	const WinSlot *s = &w->slot[lo % w->nslots];

	if (s->item.n > 0 && !sift_par_win_run(&w->ws, &s->item, err))
		return false;
	*done = 1;
	return true;
}

static void
spill_emit(void *arg, const SiftWinRow *row)
{
	const ParWin *w = arg;

	w->emit(w->emit_arg, row->tag, row->res, row->res_nulls);
}

static void
win_release(void *ctx, uint32 item)
{
	ParWin	   *w = ctx;
	WinSlot    *s = &w->slot[item % w->nslots];

	for (uint32 o = 0; o < s->n; o++)
		w->emit(w->emit_arg, s->otag[o], s->res + (size_t) o * w->ws.nfuncs, s->res_nulls[o]);
	if (s->spill != NULL)
	{
		w->rows += sift_par_win_spill_rows(s->spill);
		sift_par_win_spill_run(s->spill, w->max_part_bytes, spill_emit, w);
		s->spill = NULL;
		w->nspill--;
	}
	w->inflight_bytes -= s->bytes;
	w->inflight--;
	if (s->cap > 2 * w->target)
		slot_free(s);
}

static void
win_fallback(void *ctx, uint32 item)
{
	float_overflow_error();
}

static const SiftParOps win_ops = {.feed_slot = win_feed,.release_slot = win_release,.fallback_item = win_fallback,.phase1 = win_phase1,.may_feed = win_may_feed,
	.release_emits = true};

static void
win_open(ParWin *w, const SiftV6WindowSpec *spec, const SiftV6Input *in)
{
	const SiftWindowSpec *win = &spec->win;
	int			nargs = 0;

	w->ws.n_ord = win->n_ord;
	w->ws.nfuncs = win->n_funcs;
	memcpy(w->ws.func, win->funcs, sizeof(SiftWinFuncSpec) * win->n_funcs);
	w->n_part = win->n_part;
	memcpy(w->part_slot, spec->part_slot, sizeof(int) * win->n_part);
	memcpy(w->ord_slot, spec->ord_slot, sizeof(int) * win->n_ord);
	for (int k = 0; k < win->n_ord; k++)
		w->ws.ord[k] = (SiftParWinKey) {.desc = spec->ps.ord[k].desc,.nulls_first = spec->ps.ord[k].nulls_first,.f64 = in->col[spec->ord_slot[k]].type == SVEC_F64};
	for (int f = 0; f < win->n_funcs; f++)
	{
		w->arg_slot[f] = spec->arg_slot[f];
		nargs += spec->arg_slot[f] >= 0;
	}
	for (int c = 0; c < in->ncols; c++)
		w->serial_row_bytes += elem_size(in->col[c].type) + 1;
	w->serial_row_bytes += sizeof(uint64) + sizeof(uint32) + sizeof(int64) * win->n_ord;
	w->max_part_bytes = (uint64) work_mem * 1024 / 4;
	w->row_bytes = sizeof(uint64) * 2 + 9 * (uint64) (win->n_ord + nargs) + 2 * sizeof(uint32) + 1 + sizeof(uint16) + sizeof(SiftWinVal) * win->n_funcs;
	w->budget = (uint64) work_mem * 1024 / 2;
	w->target = (uint32) Min(ITEM_ROWS_MAX, Max(ITEM_ROWS_MIN, w->budget / (w->nslots * w->row_bytes)));
}

int
sift_v6_par_win_run(MemoryContext cx, const SiftV6WindowSpec *spec, const SiftV6Input *in, SiftV6Source *src, SiftWinCarry *carry, SiftV6ParWinEmit emit, void *emit_arg,
					SiftWinStats *stats)
{
	const int	nthreads = sift_par_pool_threads();
	MemoryContext pcx = sift_mem_allocset_create(cx, "sift agg v6 window threads");
	ParWin	   *w = MemoryContextAllocAligned(pcx, sizeof(ParWin), _Alignof(ParWin), MCXT_ALLOC_ZERO);
	SiftParArenas *ar = sift_par_arenas_create(pcx, nthreads, SIFT_ARENA_ALIGN, SIFT_ARENA_ALIGN, SIFT_ARENA_ALIGN);

	w->cx = pcx;
	w->src = src;
	w->carry = carry;
	w->emit = emit;
	w->emit_arg = emit_arg;
	w->nslots = 2 * nthreads;
	w->slot = MemoryContextAllocZero(pcx, sizeof(WinSlot) * w->nslots);
	win_open(w, spec, in);
	sift_par_run(&(SiftParCfg) {.mode = SIFT_PAR_RING,.nthreads = nthreads,.ring_slots = w->nslots}, &win_ops, w, ar, NULL);
	*stats = (SiftWinStats) {.rows_in = w->rows,.rows_looked = w->rows,.rows_out = w->rows};
	MemoryContextDelete(pcx);
	return nthreads;
}
