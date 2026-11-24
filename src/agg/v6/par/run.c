/*
 * agg/v6/par/run.c - SiftAggV6 on the compute threads of the backend (agg/v6/par/run.h): the lifecycle every operator shares. The scan hands its granules to the window; at the end of the scan
 * the leader sizes the arenas of the threads from what the window holds, feeds the granules to the threads through the ring of core/par/morsel.h, and puts the answer of the operator into the groups
 * or the rows of the statement. What a thread does with a granule calls nothing of the server (par_load, then the operator's fill).
 *
 * The operators are the kinds of run_int.h: the grouped aggregation (run_group.c), COUNT(DISTINCT) (run_distinct.c) and the Top-N of rows (run_rows.c).
 */
#include "postgres.h"

#include "miscadmin.h"

#include "agg/v6/par/run_int.h"
#include "agg/v6/v6_datum.h"
#include "agg/v6/v6_pg_expr.h"
#include "core/memtrack.h"
#include "core/par/pool.h"

/* Fewer granules than this are not worth the start of the threads (a granule is a few hundred microseconds of work, the start about a millisecond) */
#define PAR_ITEMS_MIN	16

static const ParKind *const kinds[] = {&par_kind_group, &par_kind_distinct, &par_kind_rows};

int
par_add_leaf(ParShape *sh, int leaf, bool codes)
{
	for (int i = 0; i < sh->nleaves; i++)
		if (sh->leaf[i] == leaf)
		{
			sh->codes[i] &= codes;
			return i;
		}
	sh->leaf[sh->nleaves] = leaf;
	sh->codes[sh->nleaves] = codes;
	return sh->nleaves++;
}

bool
par_plain_col(const SiftV6Col *c)
{
	return c->src == SV6SRC_ATTR && c->derive == 0 && c->jleaf == 0 && c->scale == 0 && c->attno > 0;
}

static void
par_check_leaf(ParShape *sh, int at)
{
	for (int i = 0; i < sh->nchk; i++)
		if (sh->chk[i] == at)
			return;
	sh->chk[sh->nchk++] = at;
}

bool
par_count_sum_of(const SiftV6Agg *a, const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	if (a->fclause != 0 || a->nxcols != 0)
		return false;
	if (a->kind == SV6_COUNT_STAR)
		return true;
	if (a->kind == SV6_COUNT && par_plain_col(&a->arg))
		par_check_leaf(sh, par_add_leaf(sh, par_leaf_of(first, &a->arg), false));
	else if ((a->kind == SV6_SUM || a->kind == SV6_AVG) && (a->arg.typid == INT2OID || a->arg.typid == INT4OID) && sh->nsum < SIFT_PAR_GROUP_MAX_SUM &&
			 par_src_of(sh, sp, first, &a->arg, &sh->sum_src[sh->nsum]))
		sh->nsum++;
	else
		return false;
	return true;
}

bool
par_keys_of(const SiftV6Spec *sp, const int *first, ParShape *sh)
{
	sh->nk = sp->nkeys;
	if (sp->nkeys > SIFT_PAR_GROUP_MAX_KEYS)
		return false;
	for (int i = 0; i < sp->nkeys; i++)
	{
		const SiftV6Class cls = sift_v6_class(sp->key[i].typid);

		sh->ktext[i] = cls == SV6C_TEXT;
		if (!(sh->ktext[i] || cls == SV6C_I32 || cls == SV6C_I64) || sift_v6_key_generic(sp->key[i].typid) || !par_src_of(sh, sp, first, &sp->key[i], &sh->key_src[i]))
			return false;
	}
	return true;
}

static bool
text_result(SiftV6DeriveKind kind)
{
	return kind == SVD_URL_HOST || kind == SVD_CASE_STR;
}

bool
par_src_of(ParShape *sh, const SiftV6Spec *sp, const int *first, const SiftV6Col *c, ParSrc *s)
{
	const bool	text = sift_v6_class(c->typid) == SV6C_TEXT;
	const SiftV6Derive *d = c->derive > 0 ? &sp->derive[c->derive - 1] : NULL;

	*s = (ParSrc) {.d = d};
	if (d == NULL)
	{
		s->at = par_plain_col(c) ? par_add_leaf(sh, par_leaf_of(first, c), text) : -1;
		if (s->at >= 0)
			par_check_leaf(sh, s->at);
		return s->at >= 0;
	}
	if (c->src != SV6SRC_ATTR || c->jleaf != 0 || c->scale != 0 || text != text_result(d->kind) || !par_plain_col(&d->src))
		return false;
	s->at = par_add_leaf(sh, par_leaf_of(first, &d->src), true);
	par_check_leaf(sh, s->at);
	for (int i = 0; i < d->ncond; i++)
	{
		if (!par_plain_col(&d->cond[i]))
			return false;
		s->cond[i] = par_add_leaf(sh, par_leaf_of(first, &d->cond[i]), false);
		par_check_leaf(sh, s->cond[i]);
	}
	return true;
}

static bool
shape_of(const SiftV6Spec *spec, const int *first, ParShape *sh)
{
	if (spec->nparts != 0 || spec->ext != NULL || spec->jsonqual != NIL || OidIsValid(spec->text_indexoid) || sift_v6_spec_pg_reads(spec))
		return false;
	for (int k = 0; k < lengthof(kinds); k++)
	{
		*sh = (ParShape) {.kind = kinds[k]};
		if (kinds[k]->shape(spec, first, sh))
			return true;
	}
	return false;
}

/* The state each thread gets: what the operator wants within what the budget leaves; false when the least it needs does not fit, which is known before the scan hands a granule over */
static bool
arenas_size(const ParShape *sh, uint32 groups_est, uint64 rows, int nthreads, ParSizes *sz, size_t *state)
{
	const uint64 each = (uint64) work_mem * 1024 * 3 / 4 / nthreads;

	sh->kind->sizes(sh, groups_est, rows, nthreads, sz);
	if (each < (uint64) sz->scratch + sz->work + sz->state_min)
		return false;
	*state = Min(sz->state_want, each - sz->scratch - sz->work);
	return true;
}

bool
sift_v6_par_wanted(const SiftV6Spec *spec)
{
	ParShape	sh;
	ParSizes	sz;
	size_t		state;
	const int	nthreads = sift_par_pool_threads();

	return nthreads > 1 && shape_of(spec, NULL, &sh) && arenas_size(&sh, spec->groups_est, 0, nthreads, &sz, &state);
}

bool
par_arena_full(SiftArena *a, SiftErr *err)
{
	sift_err_fill(err, SIFT_ERR_ARENA_FULL, 0, a->cap - a->used, "the arena of the thread has no room for a granule");
	return false;
}

bool
par_serial(SiftErr *err, const char *why)
{
	sift_err_fill(err, SIFT_ERR_SERIAL, 0, 0, why);
	return false;
}

bool
par_load(const ParShape *sh, const SiftPinSlot *sl, SiftArena *scratch, SiftErr *err, ParLoad *ld)
{
	struct SiftStrMemo *memo = sift_pin_memo_new(scratch);
	uint16	   *rows = NULL;
	int			nrows = 0;

	ld->n = sift_pin_slot_nrows(sl);
	ld->active = sift_arena_alloc(scratch, SIFT_PIN_ACTIVE_WORDS * sizeof(uint64), SIFT_ARENA_ALIGN);
	if (memo == NULL || ld->active == NULL)
		return par_arena_full(scratch, err);
	if (!sift_pin_slot_filter_a(sl, scratch, err, memo, ld->active, &ld->nact))
		return false;
	if (ld->nact == 0)
		return true;
	if (ld->nact * 4 < ld->n && ld->nact <= SIFT_PIN_POINT_ROWS_MAX)
	{
		if ((rows = sift_arena_alloc(scratch, ld->nact * sizeof(uint16), 8)) == NULL)
			return par_arena_full(scratch, err);
		PAR_EACH_ROW(ld, r, i)
			rows[i] = r;
		nrows = ld->nact;
	}
	for (int i = 0; i < sh->nleaves; i++)
		if (!sift_pin_slot_view_a(sl, scratch, err, memo, sh->leaf[i], sh->codes[i], rows, nrows, &ld->cv[i]))
			return false;
	return true;
}

static bool
op_feed(void *vp, uint32 item)
{
	SiftV6ParScan *p = vp;
	const uint32 jobs = sift_pin_win_items(p->win);
	uint32		job = p->next_job;

	while (job < jobs && p->sh.kind->skip != NULL && p->sh.kind->skip(p, job))
		job++;
	p->next_job = Min(job + 1, jobs);
	if (job >= jobs)
		return false;
	p->job_of[item] = job;
	return sift_pin_win_feed(p->win, item, job);
}

static void
op_release(void *vp, uint32 item)
{
	sift_pin_win_release(((SiftV6ParScan *) vp)->win, item);
}

static void
op_fallback(void *vp, uint32 item)
{
	SiftV6ParScan *p = vp;

	p->pending[p->npending++] = p->job_of[item];
}

static bool
op_phase1(void *vp, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	SiftV6ParScan *p = vp;

	return p->sh.kind->ops->phase1(p->op, t, lo, hi, scratch, done, err);
}

static bool
op_seal(void *vp, uint32 *nparts, SiftErr *err)
{
	SiftV6ParScan *p = vp;

	*nparts = 0;
	return p->sh.kind->ops->seal == NULL || p->sh.kind->ops->seal(p->op, nparts, err);
}

static bool
op_phase2(void *vp, int t, uint32 part, SiftArena *work, SiftErr *err)
{
	SiftV6ParScan *p = vp;

	return p->sh.kind->ops->phase2(p->op, t, part, work, err);
}

static void
op_finish(void *vp)
{
	SiftV6ParScan *p = vp;

	p->finishing = true;
	if (p->sh.kind->ops->finish != NULL)
		p->sh.kind->ops->finish(p->op);
}

static void
op_overflow(void *vp, const SiftParRange *left, uint32 nleft)
{
	SiftV6ParScan *p = vp;

	if (p->sh.kind->drains)
		p->sh.kind->ops->overflow_drain(p->op, left, nleft);
}

static const SiftParOps run_ops = {.feed_slot = op_feed,.release_slot = op_release,.fallback_item = op_fallback,.phase1 = op_phase1,.seal = op_seal,.phase2 = op_phase2,.finish = op_finish,
	.overflow_drain = op_overflow};

SiftV6ParScan *
sift_v6_par_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6Rows *rows, SiftV6ScanStats *stats, const int *first, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg)
{
	SiftV6ParScan *p;
	ParShape	sh;
	ParSizes	sz;
	size_t		state;
	const int	nthreads = sift_par_pool_threads();

	if (nthreads < 2 || !shape_of(spec, first, &sh) || (sh.kind == &par_kind_rows) != (rows != NULL) || (g == NULL) == (rows == NULL) ||
		!arenas_size(&sh, spec->groups_est, 0, nthreads, &sz, &state))
		return NULL;
	p = palloc0(sizeof(SiftV6ParScan));
	p->spec = spec;
	p->g = g;
	p->rows = rows;
	p->stats = stats;
	p->index = index;
	p->sh = sh;
	p->win = sift_pin_win_new(index, snap, sh.leaf, sh.nleaves, 2 * nthreads, part_ok, part_arg);
	if (sh.kind->setup != NULL)
		sh.kind->setup(p);
	return p;
}

SiftPinWin *
sift_v6_par_window(SiftV6ParScan *p)
{
	return p->win;
}

static void
replay_jobs(SiftV6ParScan *p, const uint32 *jobs, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
		sift_pin_win_replay(p->win, jobs[i]);
}

static void
replay_from(SiftV6ParScan *p, uint32 first)
{
	for (uint32 j = first; j < sift_pin_win_items(p->win); j++)
		sift_pin_win_replay(p->win, j);
}

static void
run_threads(SiftV6ParScan *p, int nthreads, const ParSizes *sz, size_t state)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 par");
	MemoryContext old = MemoryContextSwitchTo(cx);
	const uint32 items = sift_pin_win_items(p->win);
	const ParKind *kind = p->sh.kind;
	SiftParArenas *ar = sift_par_arenas_create(cx, nthreads, sz->scratch, state, sz->work);
	SiftErr		err = {0};
	bool		done;
	p->pending = palloc(sizeof(uint32) * items);
	p->job_of = palloc(sizeof(uint32) * items);
	p->op = kind->create(p, ar, sift_pin_win_rows(p->win), &err);
	if (p->op == NULL)
	{
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
		replay_from(p, 0);
		return;
	}
	p->next_job = 0;
	done = sift_par_run(&(SiftParCfg) {.mode = SIFT_PAR_RING,.nthreads = nthreads,.ring_slots = 2 * nthreads}, &run_ops, p, ar, p->index);
	MemoryContextSwitchTo(old);
	if (!done && kind->drains)
	{
		replay_jobs(p, p->pending, p->npending);
		replay_from(p, p->next_job);
	}
	else if (!done || p->npending > 0)
		replay_from(p, 0);
	else if (kind->done != NULL)
		kind->done(p);
	for (int t = 0; t < nthreads; t++)
		p->stats->rows += p->w[t].rows * (done && p->npending == 0);
	elog(DEBUG1, "sift agg v6: %u granules on %d threads, %s", items, nthreads, done && p->npending == 0 ? "folded by the threads" : "replayed by the scan");
	MemoryContextDelete(cx);
}

void
sift_v6_par_end(SiftV6ParScan *p)
{
	const uint32 items = sift_pin_win_items(p->win);
	const int	nthreads = Min(sift_par_pool_threads(), (int) Max(items, 1));
	ParSizes	sz;
	size_t		state;

	PG_TRY();
	{
		if (items == 0)
			;
		else if ((!p->sh.kind->merges && (p->stats->rows > 0 || sift_pin_win_refused(p->win) > 0)) || nthreads < 2 || items < PAR_ITEMS_MIN ||
				 !arenas_size(&p->sh, p->spec->groups_est, sift_pin_win_rows(p->win), nthreads, &sz, &state))
		{
			elog(DEBUG1, "sift agg v6: %u granules replayed by the scan (%u refused, %lu rows read)", items, sift_pin_win_refused(p->win), (unsigned long) p->stats->rows);
			replay_from(p, 0);
		}
		else
			run_threads(p, nthreads, &sz, state);
	}
	PG_FINALLY();
	{
		sift_pin_win_end(p->win);
	}
	PG_END_TRY();
	pfree(p);
}
