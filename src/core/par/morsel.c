/*
 * core/par/morsel.c - the guided work queue of core/par/morsel.h.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "core/par/morsel.h"

Size
sift_morsel_size(uint32 n_parts)
{
	return add_size(offsetof(SiftMorselQueue, part_start), mul_size(sizeof(uint32), (Size) n_parts + 1));
}

void
sift_morsel_init(SiftMorselQueue *q, uint32 n_parts, const uint32 *granules, uint32 n_participants)
{
	uint64		total = 0;

	Assert(n_participants >= 1);
	for (uint32 p = 0; p < n_parts; p++)
	{
		q->part_start[p] = (uint32) total;
		total += granules[p];
		if (total > UINT32_MAX)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("too many granules in one parallel scan"),
							errdetail("A parallel scan holds at most %u granules.", UINT32_MAX), errhint("Scan fewer parts at once.")));
	}
	q->part_start[n_parts] = (uint32) total;
	q->total = (uint32) total;
	q->n_parts = n_parts;
	q->n_participants = n_participants;
	q->n_lanes = Min(n_participants, SIFT_MORSEL_LANES);
	sift_bound_init(&q->topn_theta, 0);
	for (uint32 l = 0; l < q->n_lanes; l++)
	{
		pg_atomic_init_u32(&q->lanes[l].cursor, (uint32) (total * l / q->n_lanes));
		q->lanes[l].end = (uint32) (total * (l + 1) / q->n_lanes);
	}
}

uint32
sift_morsel_run(uint32 left, uint32 n_participants)
{
	return pg_prevpower2_32(Max(1, Min(left / (4 * n_participants), SIFT_MORSEL_MAX)));
}

/* The part of the flat granule g: the last part that starts at or before it (the parts with no granule start where the next does) */
static uint32
part_of(const SiftMorselQueue *q, uint32 g)
{
	uint32		lo = 0,
				hi = q->n_parts;

	while (hi - lo > 1)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (q->part_start[mid] <= g)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

/* A run from the lane: guided by the granules the lane has left */
static bool
lane_claim(SiftMorselQueue *q, SiftMorselLane *lane, SiftMorsel *m)
{
	uint32		cur = pg_atomic_read_u32(&lane->cursor);

	while (cur < lane->end)
	{
		const uint32 part = part_of(q, cur);
		const uint32 run = Min(sift_morsel_run(lane->end - cur, q->n_participants), q->part_start[part + 1] - cur);

		if (pg_atomic_compare_exchange_u32(&lane->cursor, &cur, cur + run))
		{
			m->part = part;
			m->first = cur - q->part_start[part];
			m->n = run;
			return true;
		}
	}
	return false;
}

/* The lane with the most granules left, NULL when every lane is consumed */
static SiftMorselLane *
fullest_lane(SiftMorselQueue *q)
{
	SiftMorselLane *best = NULL;
	uint32		best_left = 0;

	for (uint32 l = 0; l < q->n_lanes; l++)
	{
		const uint32 cur = pg_atomic_read_u32(&q->lanes[l].cursor);
		const uint32 left = cur < q->lanes[l].end ? q->lanes[l].end - cur : 0;

		if (left > best_left)
		{
			best = &q->lanes[l];
			best_left = left;
		}
	}
	return best;
}

bool
sift_morsel_claim(SiftMorselQueue *q, uint32 participant, SiftMorsel *m)
{
	SiftMorselLane *lane = &q->lanes[participant % q->n_lanes];

	while (lane)
	{
		if (lane_claim(q, lane, m))
			return true;
		lane = fullest_lane(q);
	}
	return false;
}

uint64
sift_morsel_theta_load(SiftMorselQueue *q)
{
	return sift_bound_load(&q->topn_theta);
}

void
sift_morsel_theta_raise(SiftMorselQueue *q, uint64 bits)
{
	sift_bound_raise(&q->topn_theta, bits);
}

void
sift_morsel_stop(SiftMorselQueue *q)
{
	for (uint32 l = 0; l < q->n_lanes; l++)
		pg_atomic_write_u32(&q->lanes[l].cursor, q->lanes[l].end);
}

/* ---- the driver ---- */

SiftParArenas *
sift_par_arenas_create(MemoryContext cx, uint32 nthreads, size_t scratch_bytes, size_t state_bytes, size_t work_bytes)
{
	SiftParArenas *ar = MemoryContextAllocZero(cx, sizeof(SiftParArenas));
	const size_t sz[3] = {TYPEALIGN(SIFT_ARENA_ALIGN, scratch_bytes), TYPEALIGN(SIFT_ARENA_ALIGN, state_bytes), TYPEALIGN(SIFT_ARENA_ALIGN, work_bytes)};

	Assert(nthreads >= 1 && nthreads <= SIFT_PAR_MAX_THREADS);
	ar->n = nthreads;
	for (uint32 t = 0; t < nthreads; t++)
	{
		uint8	   *slab = MemoryContextAllocAligned(cx, sz[0] + sz[1] + sz[2], SIFT_ARENA_ALIGN, MCXT_ALLOC_HUGE);

		sift_arena_init(&ar->w[t].scratch, slab, sz[0]);
		sift_arena_init(&ar->w[t].state, slab + sz[0], sz[1]);
		sift_arena_init(&ar->w[t].work, slab + sz[0] + sz[1], sz[2]);
	}
	return ar;
}

typedef struct SiftParRun
{
	const SiftParOps *ops;
	void	   *ctx;
	SiftParArenas *ar;
	SiftMorselQueue *q;
	int			nthreads;
	SiftErrLatch lt;
	atomic_bool halt;
	atomic_uint next_part;
	uint32		nparts;
	SiftParRange left[SIFT_PAR_MAX_THREADS];
} SiftParRun;

static bool
halted(const SiftParRun *r)
{
	return atomic_load_explicit(&r->halt, memory_order_relaxed) || sift_errlatch_peek(&r->lt);
}

static void
fail(SiftParRun *r, const SiftErr *err)
{
	sift_errlatch_post(&r->lt, err);
	atomic_store_explicit(&r->halt, true, memory_order_relaxed);
}

static void
phase1_loop(SiftParRun *r, int t)
{
	SiftArena  *scratch = &r->ar->w[t].scratch;
	SiftMorsel	m;

	while (!halted(r) && sift_morsel_claim(r->q, t, &m))
	{
		uint32		done = 0;
		SiftErr		err = {0};

		sift_arena_reset(scratch);
		if (!r->ops->phase1(r->ctx, t, m.first, m.first + m.n, scratch, &done, &err))
		{
			r->left[t] = (SiftParRange) {m.first + done, m.first + m.n};
			fail(r, &err);
			return;
		}
		if (t == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

static void
phase2_loop(SiftParRun *r, int t)
{
	SiftArena  *work = &r->ar->w[t].work;

	while (!halted(r))
	{
		const uint32 part = atomic_fetch_add_explicit(&r->next_part, 1, memory_order_relaxed);
		const SiftArenaMark mark = sift_arena_mark(work);
		SiftErr		err = {0};
		bool		ok;

		if (part >= r->nparts)
			return;
		ok = r->ops->phase2(r->ctx, t, part, work, &err);
		sift_arena_rewind(work, mark);
		if (!ok)
			return fail(r, &err);
		if (t == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

static void
job_phase1(int worker, void *r)
{
	phase1_loop(r, worker);
}

static void
job_phase2(int worker, void *r)
{
	phase2_loop(r, worker);
}

static void
job_stop(void *r)
{
	atomic_store_explicit(&((SiftParRun *) r)->halt, true, memory_order_relaxed);
}

static void
run_phase(SiftParRun *r, SiftParJobFn job, void (*loop) (SiftParRun *r, int t))
{
	if (r->nthreads > 1)
		sift_par_pool_start(r->nthreads, job, job_stop, r);
	loop(r, 0);
	if (r->nthreads > 1)
		sift_par_pool_wait();
}

/* What the threads did not fold when they stopped: the rest of the morsels they failed in, and what no thread claimed */
static uint32
leftover(const SiftParRun *r, SiftParRange *out)
{
	uint32		n = 0;

	for (int t = 0; t < r->nthreads; t++)
		if (r->left[t].lo < r->left[t].hi)
			out[n++] = r->left[t];
	for (uint32 l = 0; l < r->q->n_lanes; l++)
	{
		const uint32 cur = pg_atomic_read_u32(&r->q->lanes[l].cursor);

		if (cur < r->q->lanes[l].end)
			out[n++] = (SiftParRange) {cur, r->q->lanes[l].end};
	}
	return n;
}

static void
run_phases(SiftParRun *r)
{
	SiftErr		err = {0};

	run_phase(r, job_phase1, phase1_loop);
	if (halted(r))
		return;
	if (r->ops->seal != NULL && !r->ops->seal(r->ctx, &r->nparts, &err))
		return fail(r, &err);
	if (r->nparts > 0)
		run_phase(r, job_phase2, phase2_loop);
}

bool
sift_par_run(const SiftParCfg *cfg, const SiftParOps *ops, void *ctx, SiftParArenas *arenas, Relation rel)
{
	SiftParRun	r = {.ops = ops,.ctx = ctx,.ar = arenas,.nthreads = cfg->nthreads};
	SiftErr		err;

	Assert(cfg->nthreads >= 1 && cfg->nthreads <= arenas->n);
	r.q = MemoryContextAllocAligned(CurrentMemoryContext, sift_morsel_size(1), SIFT_PAR_SHM_ALIGN, 0);
	sift_morsel_init(r.q, 1, &cfg->items, cfg->nthreads);
	sift_errlatch_init(&r.lt);
	atomic_init(&r.halt, false);
	atomic_init(&r.next_part, 0);
	PG_TRY();
	{
		run_phases(&r);
	}
	PG_FINALLY();
	{
		if (sift_par_pool_busy())
		{
			job_stop(&r);
			sift_par_pool_wait();
		}
	}
	PG_END_TRY();
	if (sift_errlatch_get(&r.lt, &err))
	{
		SiftParRange left[SIFT_PAR_MAX_THREADS + SIFT_MORSEL_LANES];

		if (err.code != SIFT_ERR_WORK_MEM || ops->overflow_drain == NULL)
			sift_err_rethrow(rel, &err);
		ops->overflow_drain(ctx, left, leftover(&r, left));
		pfree(r.q);
		return false;
	}
	if (ops->finish != NULL)
		ops->finish(ctx);
	pfree(r.q);
	return true;
}
