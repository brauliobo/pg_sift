/*
 * agg/v6/par/winspill.c - the sorted stream of a partition of agg/v6/par/winspill.h.
 */
/* check-memory: allow-allocs the buffers of a batch are of fixed size, and the rows are held by the sort, which spills past work_mem */
#include "postgres.h"

#include "miscadmin.h"

#include "agg/v6/par/winspill.h"
#include "agg/v6/batch.h"
#include "agg/v6/win/spill.h"
#include "core/memtrack.h"

struct SiftParWinSpill
{
	MemoryContext cx;
	MemoryContext bcx;
	SiftWinSpill *sp;
	SiftWindowSpec win;
	int			ncols;
	int			arg_col[SIFT_WIN_MAX_FUNCS];
	SiftVecType type[SIFT_V6_MAX_COLS];
	uint64		rows;
	SiftWinSpillOut out;
	uint8		state[SIFT_V6_MAX_COLS][SIFT_VEC_LEN];
	int64		peer[SIFT_WIN_MAX_KEYS][SIFT_VEC_LEN];
	char	   *buf[SIFT_V6_MAX_COLS];
	uint64		valid[SIFT_V6_MAX_COLS][SIFT_VEC_WORDS];
	uint64		nan[SIFT_V6_MAX_COLS][SIFT_VEC_WORDS];
	int64		zero[SIFT_VEC_LEN];
	uint64		tag[SIFT_VEC_LEN];
	SiftVec		part;
	SiftVec		ord[SIFT_WIN_MAX_KEYS];
	SiftVec		vec[SIFT_V6_MAX_COLS];
};

static SiftVecType
column_type(SiftVecType t)
{
	return t == SVEC_I32 || t == SVEC_F64 ? t : SVEC_I64;
}

SiftParWinSpill *
sift_par_win_spill_open(MemoryContext cx, const SiftParWinSpec *ws, const int *arg_slot, uint64 max_bytes)
{
	SiftParWinSpill *s = MemoryContextAllocAligned(cx, sizeof(SiftParWinSpill), _Alignof(SiftParWinSpill), MCXT_ALLOC_ZERO);
	SiftWinPsSpec ps = {.n_ord = ws->n_ord,.max_bytes = max_bytes};

	s->cx = sift_mem_allocset_create(cx, "sift agg v6 window spill");
	s->bcx = sift_mem_allocset_create(s->cx, "sift agg v6 window spill batch");
	s->win = (SiftWindowSpec) {.mode = SWIN_MODE_WINDOW,.n_part = 1,.n_ord = (uint8) ws->n_ord,.n_funcs = (uint16) ws->nfuncs,.limit = SIFT_WIN_NO_LIMIT};
	memcpy(s->win.funcs, ws->func, sizeof(SiftWinFuncSpec) * ws->nfuncs);
	for (int k = 0; k < ws->n_ord; k++)
	{
		s->type[s->ncols] = ws->ord[k].f64 ? SVEC_F64 : SVEC_I64;
		ps.ord[k] = (SiftWinPsKey) {.slot = s->ncols,.desc = ws->ord[k].desc,.nulls_first = ws->ord[k].nulls_first};
		s->ncols++;
	}
	for (int f = 0; f < ws->nfuncs; f++)
	{
		s->arg_col[f] = arg_slot[f] >= 0 ? s->ncols : -1;
		if (arg_slot[f] >= 0)
			s->type[s->ncols++] = column_type(ws->func[f].arg_type);
	}
	SIFT_INVARIANT(s->ncols <= SIFT_V6_MAX_COLS, "window: too many columns to spill");
	ps.ncols = s->ncols;
	memcpy(ps.type, s->type, sizeof(ps.type));
	for (int c = 0; c < s->ncols; c++)
		s->buf[c] = MemoryContextAllocAligned(s->cx, sift_win_ps_elem_size(s->type[c]) * SIFT_VEC_LEN + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	s->out = (SiftWinSpillOut) {.cx = s->bcx,.tag = s->tag};
	for (int c = 0; c < s->ncols; c++)
	{
		s->out.col[c] = s->buf[c];
		s->out.state[c] = s->state[c];
	}
	for (int k = 0; k < ws->n_ord; k++)
		s->out.peer[k] = s->peer[k];
	s->sp = sift_win_spill_begin(s->cx, &ps, max_bytes);
	return s;
}

/* the cell of a column from the integer the item holds: an int32 sign-extended, an int64, the bits of a float8 (a NaN is a value of it) */
static uint8
cell_of(SiftVecType type, int64 v, bool null, int64 *cell)
{
	const int32 narrow = (int32) v;

	*cell = 0;
	if (null)
		return SIFT_CELL_STATE_NULL;
	if (type == SVEC_I32)
		memcpy(cell, &narrow, sizeof(narrow));
	else
		*cell = v;
	return SIFT_CELL_STATE_VALUE;
}

void
sift_par_win_spill_put(SiftParWinSpill *s, uint64 tag, const int64 *ord, const uint8 *ord_null, const int64 *arg, const uint8 *arg_null)
{
	uint8		state[SIFT_V6_MAX_COLS];
	int64		cell[SIFT_V6_MAX_COLS];
	const char *cp[SIFT_V6_MAX_COLS];
	int			c = 0;

	for (int k = 0; k < s->win.n_ord; k++, c++)
		state[c] = cell_of(s->type[c], ord[k], ord_null[k], &cell[c]);
	for (int f = 0; f < s->win.n_funcs; f++)
		if (s->arg_col[f] >= 0)
			state[s->arg_col[f]] = cell_of(s->type[s->arg_col[f]], arg[f], arg_null[f], &cell[s->arg_col[f]]);
	for (c = 0; c < s->ncols; c++)
		cp[c] = (const char *) &cell[c];
	sift_win_spill_put(s->sp, tag, state, cp);
	s->rows++;
}

uint64
sift_par_win_spill_rows(const SiftParWinSpill *s)
{
	return s->rows;
}

static void
seal_column(SiftParWinSpill *s, int c, uint16 m)
{
	bool		has_nan = false;

	memset(s->valid[c], 0, sizeof(s->valid[c]));
	memset(s->nan[c], 0, sizeof(s->nan[c]));
	for (uint16 i = 0; i < m; i++)
	{
		sift_vec_state_put(s->valid[c], s->nan[c], i, s->state[c][i]);
		has_nan |= s->state[c][i] == SIFT_CELL_STATE_NAN;
	}
	s->vec[c] = (SiftVec) {.data = s->buf[c],.valid = s->valid[c],.nan = has_nan ? s->nan[c] : NULL,.len = m,.all_valid = false,.dense = true,.type = s->type[c]};
}

static bool
spill_next(void *p, SiftWinBatch *out)
{
	SiftParWinSpill *s = p;
	uint16		m;

	CHECK_FOR_INTERRUPTS();
	MemoryContextReset(s->bcx);
	m = sift_win_spill_read(s->sp, SIFT_VEC_LEN, &s->out);
	if (m == 0)
		return false;
	*out = (SiftWinBatch) {.n = m,.tag = s->tag};
	s->part = (SiftVec) {.data = s->zero,.len = m,.all_valid = true,.dense = true,.type = SVEC_I64};
	out->part[0] = &s->part;
	for (int k = 0; k < s->win.n_ord; k++)
	{
		s->ord[k] = (SiftVec) {.data = s->peer[k],.len = m,.all_valid = true,.dense = true,.type = SVEC_I64};
		out->ord[k] = &s->ord[k];
	}
	for (int f = 0; f < s->win.n_funcs; f++)
		if (s->arg_col[f] >= 0)
		{
			seal_column(s, s->arg_col[f], m);
			out->arg[f] = &s->vec[s->arg_col[f]];
		}
	return true;
}

void
sift_par_win_spill_run(SiftParWinSpill *s, uint64 max_bytes, SiftWinEmit emit, void *emit_arg)
{
	SiftWindowExec *e;

	sift_win_spill_sort(s->sp);
	e = sift_window_create(s->cx, &s->win, max_bytes, spill_next, NULL, s, emit, emit_arg);
	sift_window_run(e);
	sift_window_free(e);
	sift_win_spill_end(s->sp);
	MemoryContextDelete(s->cx);
	pfree(s);
}
