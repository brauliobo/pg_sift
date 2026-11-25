/*
 * agg/par/radix_run.c - SV6P_RADIX_GROUPED and SV6P_RADIX_UNGROUPED_DIST of v6_par.h: many groups, or a count(DISTINCT) of no keys, so the merge is as parallel as the scan.
 *
 * Phase 0: every participant folds its morsels into a table that stays in its cache; when the table holds its share of the hash memory it hands its groups to the partitions, as records routed by the hash of the key (of
 * the value, for a count(DISTINCT) of no keys), and starts again. A partition holds every record of the groups it owns, the distinct values of a group with it. Barrier: all records are written.
 * Phase 1: every participant claims whole partitions from a shared cursor, merges the records of each into a table of its own (no other participant holds a key of it: no lock), finishes it, and emits its groups through
 * the Gather above: HAVING runs on them in the node, and no group is emitted twice. Nothing waits after the barrier, so a participant that stops (a LIMIT above, an interrupt) holds nobody.
 *
 * The distinct strategy: a partition counts its values exactly and adds the number to a shared word of its aggregate; after a second barrier one participant (the first to take final_emitter) merges the
 * states of the aggregates that the participants left in their slots, sets the counts and emits the one row. A participant that comes after the first barrier leaves: its work is done.
 */
#include "core/host.h"

#include "agg/par/par.h"

static void
relief(void *arg)
{
	SiftV6ParRun *r = arg;

	if (r->eng->local_load(r->local) >= r->export_cap)
		sift_v6_par_export(r);
}

/* Phase 1 for a participant that takes part in no other: its partitions, one at a time as the Gather asks */
bool
sift_v6_par_emit_parts(SiftV6ParRun *r, Datum *values, bool *isnull)
{
	for (;;)
	{
		uint32		part;

		if (r->plocal != NULL)
		{
			if (sift_v6_par_local_next(r, r->plocal, values, isnull))
			{
				r->stats.groups_emitted++;
				if ((++r->tick & 1023) == 0)
					CHECK_FOR_INTERRUPTS();
				return true;
			}
			r->eng->local_free(r->plocal);
			r->plocal = NULL;
		}
		if (!sift_v6_par_claim_part(r, &part))
		{
			r->stage = SV6S_DONE;
			return false;
		}
		r->plocal = r->eng->local_new(r->eng->env, false);
		sift_v6_par_merge_part(r, r->plocal, part);
		r->eng->local_finish(r->plocal);
	}
}

static void
grouped_body(SiftV6ParRun *r)
{
	r->local = r->eng->local_new(r->eng->env, false);
	r->rl = sift_radix_local_new_raw(r->coord);
	sift_v6_par_scan(r, relief);
	sift_v6_par_export(r);
	r->eng->local_free(r->local);
	r->local = NULL;
	sift_v6_par_finish_stages(r);
	BarrierArriveAndWait(&r->hdr->phase_barrier, sift_v6_par_barrier_event());
}

static void
run_grouped(SiftV6ParRun *r)
{
	if (sift_v6_par_attach(r) == 0)
		sift_v6_par_guarded(r, grouped_body);
	sift_v6_par_leave(r);
	r->stage = SV6S_PARTS;
}

bool
sift_v6_par_radix_next(SiftV6ParRun *r, Datum *values, bool *isnull)
{
	if (r->stage == SV6S_NEW)
		run_grouped(r);
	return r->stage == SV6S_PARTS && sift_v6_par_emit_parts(r, values, isnull);
}

/* Every partition's distinct values of each aggregate, counted and added to the shared words */
static void
count_parts(SiftV6ParRun *r)
{
	void	   *l = r->eng->local_new(r->eng->env, false);
	uint32		part;

	while (sift_v6_par_claim_part(r, &part))
	{
		sift_v6_par_merge_part(r, l, part);
		for (uint32 s = 0; s < r->hdr->n_dist; s++)
			pg_atomic_fetch_add_u64(&r->hdr->dist_total[s], (int64) r->eng->local_dist_count(l, s));
		r->eng->local_reset(l);
	}
	r->eng->local_free(l);
}

/* The states the participants left in their slots, merged, with the counts of the aggregates: the row of the query */
static void
final_state(SiftV6ParRun *r)
{
	r->local = r->eng->local_new(r->eng->env, true);
	for (uint32 i = 0; i < r->hdr->nworkers_planned + 1U; i++)
	{
		const uint8 *slot = (const uint8 *) r->hdr + r->hdr->plain_off + (Size) i * r->hdr->plain_slot_bytes;

		if (((const SiftV6RecHdr *) slot)->len != 0)
			r->eng->local_merge(r->local, slot);
	}
	for (uint32 s = 0; s < r->hdr->n_dist; s++)
		r->eng->local_set_dist(r->local, s, pg_atomic_read_u64(&r->hdr->dist_total[s]));
	r->eng->local_finish(r->local);
}

static void
dist_body(SiftV6ParRun *r)
{
	r->local = r->eng->local_new(r->eng->env, false);
	r->plocal = r->eng->local_new(r->eng->env, false);
	r->rl = sift_radix_local_new_raw(r->coord);
	sift_v6_par_scan(r, relief);
	sift_v6_par_export(r);
	sift_v6_par_export_plain(r);
	sift_v6_par_finish_stages(r);
	r->eng->local_free(r->local);
	r->eng->local_free(r->plocal);
	r->local = r->plocal = NULL;
	BarrierArriveAndWait(&r->hdr->phase_barrier, sift_v6_par_barrier_event());
	count_parts(r);
	BarrierArriveAndWait(&r->hdr->phase_barrier, sift_v6_par_barrier_event());
}

static void
run_dist(SiftV6ParRun *r)
{
	if (sift_v6_par_attach(r) != 0)
	{
		sift_v6_par_leave(r);
		r->stage = SV6S_DONE;
		return;
	}
	sift_v6_par_guarded(r, dist_body);
	sift_v6_par_leave(r);
	if (pg_atomic_exchange_u32(&r->hdr->final_emitter, 1) != 0)
	{
		r->stage = SV6S_DONE;
		return;
	}
	final_state(r);
	r->stage = SV6S_LOCAL;
}

bool
sift_v6_par_dist_next(SiftV6ParRun *r, Datum *values, bool *isnull)
{
	if (r->stage == SV6S_NEW)
		run_dist(r);
	if (r->stage == SV6S_LOCAL && sift_v6_par_local_next(r, r->local, values, isnull))
	{
		r->stats.groups_emitted++;
		return true;
	}
	r->stage = SV6S_DONE;
	return false;
}
