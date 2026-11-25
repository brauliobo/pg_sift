/*
 * agg/par/twostage.c - SV6P_TWO_STAGE of v6_par.h: few groups, so the partial states are small and one participant merges them.
 *
 * Phase 0: every participant folds the morsels it claims into a table of its own. The first to finish keeps its table (the leader's own groups never cross the shared memory when the leader is first, and a
 * run of one process hands nothing to anyone); the others hand their groups to the partitions as records and take no further part. A table that passes its share of the hash memory is handed over at once, and
 * a chunk pool that ran out (the partitions went to files) is noted: both mean the groups are many, and the flag escalated says so. Barrier: the records are all written. Phase 1: with the flag clear the
 * keeper merges the partitions into its table and emits it, and the others return no tuple; with it set every participant (the keeper hands its table over first, a second barrier) finishes whole
 * partitions and emits them itself, as the radix strategy does.
 *
 * A participant that comes after the barrier has nothing to scan, nothing to hand over and no table: it leaves and returns no tuple.
 */
#include "core/host.h"

#include "agg/par/par.h"

static void
relief(void *arg)
{
	SiftV6ParRun *r = arg;

	if (r->eng->local_load(r->local) < r->export_cap)
		return;
	pg_atomic_write_u32(&r->hdr->escalated, 1);
	sift_v6_par_export(r);
}

/* Phase 0 and the hand-over: returns after the first barrier */
static bool
scan_and_hand_over(SiftV6ParRun *r)
{
	bool		keeper;

	r->local = r->eng->local_new(r->eng->env, true);
	r->rl = sift_radix_local_new_raw(r->coord);
	sift_v6_par_scan(r, relief);
	keeper = pg_atomic_exchange_u32(&r->hdr->keeper, 1) == 0;
	if (!keeper)
	{
		sift_v6_par_export(r);
		r->eng->local_free(r->local);
		r->local = NULL;
	}
	sift_v6_par_finish_stages(r);
	BarrierArriveAndWait(&r->hdr->phase_barrier, sift_v6_par_barrier_event());
	return keeper;
}

static void
merge_all_parts(SiftV6ParRun *r)
{
	for (uint32 p = 0; p < r->coord->n_parts; p++)
		sift_v6_par_merge_part(r, r->local, p);
}

static void
hand_over_keeper(SiftV6ParRun *r)
{
	r->rl = sift_radix_local_new_raw(r->coord);
	sift_v6_par_export(r);
	r->eng->local_free(r->local);
	r->local = NULL;
	sift_v6_par_finish_stages(r);
}

static void
run_body(SiftV6ParRun *r)
{
	const bool	keeper = scan_and_hand_over(r);

	if (pg_atomic_read_u32(&r->hdr->escalated) == 0)
	{
		sift_v6_par_leave(r);
		if (keeper)
		{
			merge_all_parts(r);
			r->eng->local_finish(r->local);
		}
		r->stage = keeper ? SV6S_LOCAL : SV6S_DONE;
	}
	else
	{
		if (keeper)
			hand_over_keeper(r);
		BarrierArriveAndWait(&r->hdr->phase_barrier, sift_v6_par_barrier_event());
		sift_v6_par_leave(r);
		r->stage = SV6S_PARTS;
	}
}

static void
run(SiftV6ParRun *r)
{
	if (sift_v6_par_attach(r) != 0)
	{
		sift_v6_par_leave(r);
		r->stage = SV6S_DONE;
		return;
	}
	sift_v6_par_guarded(r, run_body);
}

bool
sift_v6_par_two_stage_next(SiftV6ParRun *r, Datum *values, bool *isnull)
{
	if (r->stage == SV6S_NEW)
		run(r);
	switch (r->stage)
	{
		case SV6S_LOCAL:
			if (sift_v6_par_local_next(r, r->local, values, isnull))
			{
				r->stats.groups_emitted++;
				return true;
			}
			r->stage = SV6S_DONE;
			return false;
		case SV6S_PARTS:
			return sift_v6_par_emit_parts(r, values, isnull);
		default:
			return false;
	}
}
