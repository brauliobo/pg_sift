/*
 * agg/v6/par/pinrun.c - the driver of agg/v6/par/pinrun.h.
 *
 * A thread folds the batch of a granule into agg/v6/par/group.h; every group that comes out of it, from a thread or from the drain of an overflow, is a record in the stream of that thread, and the
 * leader reads the streams to the operator's fold when the run is over. A stream that is full says so, and the groups of the threads are then not the whole answer: every granule is read by the scan.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "agg/v6/par/gsink.h"
#include "agg/v6/par/pinrun.h"
#include "agg/v6/par/star_run.h"
#include "agg/v6/par/tseries_run.h"
#include "col/granule/cfilt_str_simd.h"
#include "core/memtrack.h"
#include "core/par/pool.h"

#define WORK_BYTES		(16u << 20)
#define STATE_MIN		(8u << 20)
#define SINK_MIN_BYTES	(1u << 20)

struct SiftV6PinRun
{
	SiftV6PinDesc d;
	const SiftV6Spec *spec;
	SiftV6ScanStats *stats;
	Relation	index;
	SiftPinWin *win;
	SiftParGroup *grp;
	SiftV6GSink *sink;
	uint32		fed;
	uint32		npending;
	uint32	   *pending;		/* the granules a thread left to the leader */
	struct
	{
		uint64		rows;
		char		pad[56];
	}			w[SIFT_PAR_MAX_THREADS];
};

static void
pin_emit(void *arg, int worker, const SiftParGroupRow *row)
{
	sift_v6_gsink_emit(((SiftV6PinRun *) arg)->sink, worker, row);
}

/* ---- a thread ---- */

static bool
pin_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	SiftV6PinRun *r = arg;

	if (sift_par_group_spilled(r->grp))
		return sift_err_fill(err, SIFT_ERR_SERIAL, 0, 0, "the groups are many: the scan folds them"), false;
	if (!r->d.fill(r->d.arg, worker, sift_pin_win_slot(r->win, lo), scratch, out, err))
		return false;
	r->w[worker].rows += out->n;
	return true;
}

static void
pin_overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
}

/* ---- the driver ---- */

static bool
op_feed(void *vr, uint32 item)
{
	SiftV6PinRun *r = vr;
	const bool	fed = sift_pin_win_feed(r->win, item);

	r->fed += fed;
	return fed;
}

static void
op_release(void *vr, uint32 item)
{
	sift_pin_win_release(((SiftV6PinRun *) vr)->win, item);
}

static void
op_fallback(void *vr, uint32 item)
{
	SiftV6PinRun *r = vr;

	r->pending[r->npending++] = item;
}

static bool
op_phase1(void *vr, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	return sift_par_group_ops.phase1(((SiftV6PinRun *) vr)->grp, t, lo, hi, scratch, done, err);
}

static bool
op_seal(void *vr, uint32 *nparts, SiftErr *err)
{
	SiftParGroup *g = ((SiftV6PinRun *) vr)->grp;

	if (sift_par_group_spilled(g))
		return *nparts = 0, true;
	return sift_par_group_ops.seal(g, nparts, err);
}

static bool
op_phase2(void *vr, int t, uint32 part, SiftArena *work, SiftErr *err)
{
	return sift_par_group_ops.phase2(((SiftV6PinRun *) vr)->grp, t, part, work, err);
}

static void
op_overflow(void *vr, const SiftParRange *left, uint32 nleft)
{
	SiftParGroup *g = ((SiftV6PinRun *) vr)->grp;

	if (!sift_par_group_spilled(g))
		sift_par_group_ops.overflow_drain(g, left, nleft);
}

static const SiftParOps pin_ops = {.feed_slot = op_feed,.release_slot = op_release,.fallback_item = op_fallback,.phase1 = op_phase1,.seal = op_seal,.phase2 = op_phase2,
	.overflow_drain = op_overflow};

SiftV6PinRun *
sift_v6_pinrun_new(const SiftV6PinDesc *d, const SiftV6Spec *spec, SiftV6ScanStats *stats, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg)
{
	SiftV6PinRun *r = palloc0(sizeof(SiftV6PinRun));

	r->d = *d;
	r->spec = spec;
	r->stats = stats;
	r->index = index;
	r->win = sift_pin_win_new(index, snap, d->leaf, d->nleaves, 2 * sift_par_pool_threads(), part_ok, part_arg);
	return r;
}

SiftPinWin *
sift_v6_pinrun_window(SiftV6PinRun *r)
{
	return r->win;
}

static void
replay_items(SiftV6PinRun *r, const uint32 *items, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
		sift_pin_win_replay(r->win, items[i]);
}

static void
replay_from(SiftV6PinRun *r, uint32 first)
{
	for (uint32 i = first; i < sift_pin_win_items(r->win); i++)
		sift_pin_win_replay(r->win, i);
}

/*
 * The bytes of the state arena and of the stream of each of nthreads threads: what the rows of the window can fill at most, within the share of work_mem that is left after the decode and phase 2
 * arenas (the planner's number of groups is no bound of either: a window of rows holds at most that many groups, and a state arena that fills hands what it folded to the scan)
 */
static bool
arenas_fit(const SiftV6PinRun *r, int nthreads, size_t *state, size_t *sink)
{
	const SiftParGroupSpec *gs = &r->d.gs;
	const uint64 rows = sift_pin_win_rows(r->win);
	const uint32 words = gs->kw + gs->nsum;
	const uint64 rec = gs->str ? 64 : 8 * (words <= 1 ? 1 : pg_nextpower2_32(words));
	const uint64 out_rec = sift_v6_gsink_record_bytes(gs);
	const uint64 per_thread = (uint64) work_mem * 1024 * 3 / 4 / nthreads;
	const uint64 fixed = r->d.scratch_bytes + WORK_BYTES;
	uint64		share;

	if (per_thread < fixed + STATE_MIN + SINK_MIN_BYTES)
		return false;
	share = (per_thread - fixed) / 2;
	*state = Max((uint64) STATE_MIN, Min(4 * rec * rows / nthreads, share));
	*sink = Max((uint64) SINK_MIN_BYTES, Min(2 * out_rec * rows / nthreads, share));
	return true;
}

static void
run_threads(SiftV6PinRun *r, int nthreads, size_t state, size_t sink)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 pin run");
	MemoryContext old = MemoryContextSwitchTo(cx);
	const uint32 items = sift_pin_win_items(r->win);
	SiftParArenas *ar = sift_par_arenas_create(cx, nthreads, r->d.scratch_bytes, state, WORK_BYTES);
	const SiftParGroupHooks hooks = {.fill = pin_fill,.emit = pin_emit,.overflow = pin_overflow,.arg = r};
	SiftErr		err = {0};
	bool		done;
	bool		kept;

	if (r->d.prepare != NULL)
		r->d.prepare(r->d.arg, cx, nthreads);
	r->pending = palloc(sizeof(uint32) * items);
	r->sink = sift_v6_gsink_new(cx, &r->d.gs, nthreads, sink);
	r->grp = sift_par_group_create(ar, &r->d.gs, false, sift_pin_win_rows(r->win), &hooks, &err);
	if (r->grp == NULL)
	{
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
		replay_from(r, 0);
		return;
	}
	done = sift_par_run(&(SiftParCfg) {.mode = SIFT_PAR_RING,.nthreads = nthreads,.ring_slots = 2 * nthreads}, &pin_ops, r, ar, r->index);
	MemoryContextSwitchTo(old);
	kept = !sift_v6_gsink_full(r->sink) && !sift_par_group_spilled(r->grp);
	if (kept)
	{
		sift_v6_gsink_drain(r->sink, r->d.fold, r->d.arg);
		for (int t = 0; t < nthreads; t++)
			r->stats->rows += r->w[t].rows;
		replay_items(r, r->pending, r->npending);
		if (!done)
			replay_from(r, r->fed);
	}
	else
		replay_from(r, 0);
	elog(DEBUG1, "sift agg v6 %s: %u granules on %d threads, %s (%u left to the scan)", r->d.name, items, nthreads, kept ? "folded by the threads" : "many groups: replayed by the scan",
		 kept ? r->npending + (done ? 0 : items - r->fed) : items);
	MemoryContextDelete(cx);
}

void
sift_v6_pinrun_end(SiftV6PinRun *r)
{
	const uint32 items = sift_pin_win_items(r->win);
	const int	nthreads = Min(sift_par_pool_threads(), (int) Max(items, 1));
	size_t		state = 0,
				sink = 0;

	PG_TRY();
	{
		if (items == 0)
			elog(DEBUG1, "sift agg v6 %s: no granule handed over (%u refused)", r->d.name, sift_pin_win_refused(r->win));
		else if (nthreads < 2 || !arenas_fit(r, nthreads, &state, &sink))
		{
			elog(DEBUG1, "sift agg v6 %s: %u granules replayed by the scan (the arenas do not fit work_mem: %d threads, %zu state, %zu sink, scratch %zu)", r->d.name, items, nthreads, state, sink, r->d.scratch_bytes);
			replay_from(r, 0);
		}
		else
			run_threads(r, nthreads, state, sink);
	}
	PG_FINALLY();
	{
		sift_pin_win_end(r->win);
	}
	PG_END_TRY();
	pfree(r);
}

/* ---- the statements ---- */

bool
sift_v6_pin_slot_cols(const SiftPinSlot *sl, SiftArena *a, SiftErr *err, const int *leaf, int nleaves, const bool *codes, ColView *cv, uint64 **active, uint32 *nact)
{
	SiftStrMemo *memo = sift_arena_alloc0(a, sizeof(SiftStrMemo), SIFT_ARENA_ALIGN);
	uint16	   *rows = NULL;
	const int	n = sl->nrows;

	*active = sift_arena_alloc(a, (SIFT_CFILT_WORDS + 2) * sizeof(uint64), SIFT_ARENA_ALIGN);
	if (memo == NULL || *active == NULL)
		return sift_err_fill(err, SIFT_ERR_ARENA_FULL, 0, a->cap - a->used, "the arena of the thread has no room for a granule"), false;
	if (!sift_pin_slot_filter_a(sl, a, err, memo, *active, nact))
		return false;
	if (*nact == 0)
		return true;
	if (*nact * 4 < (uint32) n && *nact <= SIFT_POINT_ROWS_MAX)
	{
		uint32		i = 0;

		rows = sift_arena_alloc(a, *nact * sizeof(uint16), 8);
		if (rows == NULL)
			return sift_err_fill(err, SIFT_ERR_ARENA_FULL, 0, a->cap - a->used, "the arena of the thread has no room for a granule"), false;
		for (int w = 0; w < (n + 63) / 64; w++)
			for (uint64 bits = (*active)[w]; bits != 0; bits &= bits - 1)
				rows[i++] = w * 64 + __builtin_ctzll(bits);
	}
	for (int i = 0; i < nleaves; i++)
		if (!sift_pin_slot_view_a(sl, a, err, memo, leaf[i], codes != NULL && codes[i], rows, rows != NULL ? (int) *nact : 0, &cv[i]))
			return false;
	return true;
}

void
sift_v6_pin_fold_row(SiftV6Group *g, const SiftV6Spec *spec, const SiftParGroupRow *row, const int64 *key, const SiftV6StrIn *str, uint64 key_nulls)
{
	SiftV6AccIn acc[SIFT_V6_MAX_KEYS + 16];
	int			m = 0;

	Assert(spec->naggs <= lengthof(acc));
	for (int j = 0; j < spec->naggs; j++)
	{
		acc[j] = (SiftV6AccIn) {.n = (int64) row->count};
		if (spec->agg[j].kind == SV6_SUM || spec->agg[j].kind == SV6_AVG)
		{
			acc[j].v0 = row->sum[m++];
			acc[j].v1 = (int64) acc[j].v0 < 0 ? -1 : 0;
		}
	}
	sift_v6_group_fold_rec(g, key, str, key_nulls, acc);
}

void
sift_v6_pin_fold_words(SiftV6Group *g, const SiftV6Spec *spec, const SiftParWordShape *sh, const SiftParGroupRow *row)
{
	int64		key[SIFT_V6_MAX_KEYS] = {0};
	SiftV6StrIn str[SIFT_V6_MAX_KEYS] = {{0}};
	uint64		nulls = 0;

	if (sh->gs.str)
	{
		str[0] = (SiftV6StrIn) {(const char *) row->str, row->len};
		nulls = row->len == SIFT_STR_NULL;
	}
	else
		for (int i = 0; i < sh->nk; i++)
		{
			const bool	null = row->key[i] == UINT64_MAX;

			nulls |= (uint64) null << i;
			key[i] = null ? 0 : (int64) (row->key[i] ^ (UINT64CONST(1) << 63));
		}
	sift_v6_pin_fold_row(g, spec, row, key, str, nulls);
}

bool
sift_v6_pin_wanted(const SiftV6Spec *spec)
{
	return sift_par_pool_threads() > 1 && sift_v6_tseries_wanted(spec);
}

SiftV6PinRun *
sift_v6_pin_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6ScanStats *stats, const int *first, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg,
				  const SiftV6Pipe *pipe, const SiftV6PinCol *cols, int ncols)
{
	if (sift_par_pool_threads() < 2)
		return NULL;
	if (spec->ext != NULL)
		return sift_v6_star_pin_begin(spec, g, stats, index, snap, part_ok, part_arg, pipe, cols, ncols);
	return sift_v6_tseries_begin(spec, g, stats, first, index, snap, part_ok, part_arg);
}
