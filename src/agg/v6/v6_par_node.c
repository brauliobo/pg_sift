/*
 * agg/v6/v6_par_node.c - the parallel SiftAggV6 as the executor runs it: the callbacks of a parallel custom scan (estimate, initialize, reinitialize, worker, shutdown) that lay the shared block of
 * v6_par_dsm.c out in the DSM chunk of the node, and the node's next tuple, which is the next finished group of this participant's run (v6_par.h).
 *
 * Every participant (the leader and each worker) returns the groups it owns and no other does, so the Gather above passes the tuples through, and the leader and the workers run the same code. A node
 * that finds no shared block (a Gather with no workers does not make one) runs the serial path of exec.c.
 */
#include "postgres.h"

#include "storage/dsm.h"
#include "utils/guc.h"

#include <math.h>

#include "agg/v6/v6_cost_model.h"
#include "agg/v6/group_int.h"
#include "agg/v6/v6_par_dist.h"
#include "agg/v6/v6_par_pg.h"

#define SIFT_PAR_MIN_POOL_BYTES	(4 << 20)
#define SIFT_PAR_POOL_BIG_GROUPS	400000	/* from this many expected groups a run fills its pool, and the estimate is the low one (it leaves out part of the key bytes) */
#define SIFT_PAR_POOL_BIG		1.5		/* of the bytes of the estimate: a short pool sends partitions to temporary files, which costs more than the pages of the segment */
#define SIFT_PAR_POOL_SMALL		0.5		/* a smaller run is estimated high (the groups of the planner are) and the pages of the segment are all made when it is created */

const char *
sift_v6_par_strategy_name(int strategy)
{
	switch ((SiftV6ParStrategy) strategy)
	{
		case SV6P_TWO_STAGE:
			return "two-stage";
		case SV6P_RADIX_GROUPED:
			return "radix";
		case SV6P_RADIX_UNGROUPED_DIST:
			return "radix distinct";
		case SV6P_SERIAL:
			break;
	}
	return "serial";
}

SiftV6ParNode *
sift_v6_par_node_begin(const SiftV6Spec *spec, TupleDesc desc, const SiftV6ScanSrc *sources, int nsources, Snapshot snap, MemoryContext mcx, MemoryContext tuple_cx)
{
	SiftV6ParNode *pn = MemoryContextAllocZero(mcx, sizeof(SiftV6ParNode));

	*pn = (SiftV6ParNode) {.spec = spec,.desc = desc,.sources = sources,.nsources = nsources,.snap = snap,.mcx = mcx,.tuple_cx = tuple_cx};
	return pn;
}

bool
sift_v6_par_node_attached(const SiftV6ParNode *pn)
{
	return pn->hdr != NULL;
}

void
sift_v6_par_node_summary(const SiftV6ParNode *pn, SiftV6ParSummary *out)
{
	*out = (SiftV6ParSummary) {.strategy = sift_v6_par_strategy_name(pn->spec->par_strategy),.ran = pn->have_total,.escalated = pn->escalated,
		.morsels_stolen = pn->total.morsels_stolen,.radix_parts = pn->radix_parts,.partial_recs = pn->ring_recs,.partial_bytes = pn->ring_bytes,
		.grans_scanned = pn->total.grans_scanned,.grans_preagg = pn->total.grans_preagg,.grans_preagg_delta = pn->total.grans_preagg_delta,.heap_vet_rows = pn->total.heap_vet_rows};
}

static uint32
count_distinct(const SiftV6Spec *spec)
{
	uint32		n = 0;

	for (int j = 0; j < spec->naggs; j++)
		n += sift_v6_kind_distinct(spec->agg[j].kind);
	return n;
}

/*
 * What the partitions of the run take at most before their records go to files: the records the participants hand over, each group once for every participant that saw it. A COUNT(DISTINCT) sends
 * its values too: those of a query of no keys that its participants dedup first (v6_par_dist.h; a third of the rows is what the benchmark table needs), every row of a GROUP BY. The block is made of
 * that and not of work_mem: the operating system fills every byte of a segment when it is created.
 */
static double
chunk_budget(const SiftV6ParNode *pn, int nparticipants)
{
	const SiftV6Spec *spec = pn->spec;
	double		rows = 0;
	const double groups = Max((double) spec->groups_est, 1.0);
	double		group_recs;
	double		bytes;

	for (uint32 i = 0; i < pn->plan.nrefs; i++)
		rows += (double) pn->plan.refs[i].ngranules * SIFT_GRANULE_ROWS;
	group_recs = nparticipants * groups * (1.0 - exp(-rows / nparticipants / groups));
	if (count_distinct(spec) == 0)
		bytes = group_recs * (sizeof(SiftV6RecHdr) + (Size) spec->naggs * sizeof(V6Acc) + 32);
	else if (!sift_v6_par_dist_lean(spec))
		bytes = rows * (sizeof(SiftV6RecHdr) + 32);
	else if (spec->nkeys == 0)
		bytes = rows * SIFT_PAR_DIST_FLAT_SHARE * (sizeof(SiftV6DistRec) + 8);
	else
		bytes = rows * (sizeof(SiftV6DistRec) + 8 * (spec->nkeys + 2)) + group_recs * (sizeof(SiftV6RecHdr) + (Size) spec->naggs * sizeof(V6Acc) + 32);
	return Min((double) get_hash_memory_limit(), Max(bytes * (groups >= SIFT_PAR_POOL_BIG_GROUPS ? SIFT_PAR_POOL_BIG : SIFT_PAR_POOL_SMALL), (double) SIFT_PAR_MIN_POOL_BYTES));
}

/* The units of every source, one after the other, each tagged with its source, and the deletes of the transaction of every source with where each starts */
static void
list_units(SiftV6ParNode *pn)
{
	SiftV6ScanStats stats = {0};
	SiftF2ParPlan *each = palloc(pn->nsources * sizeof(SiftF2ParPlan));
	SiftV6Scan **scan = palloc(pn->nsources * sizeof(SiftV6Scan *));
	uint32		nrefs = 0,
				nstaged = 0;

	for (int k = 0; k < pn->nsources; k++)
	{
		const SiftV6ScanSrc *src = &pn->sources[k];

		scan[k] = src->ops->par->begin(pn->spec, src->heap, src->index, src->keys, src->nkeys, src->quals, pn->snap, &stats, NULL, NULL);
		src->ops->par->plan(scan[k], &each[k]);
		nrefs += each[k].nrefs;
		nstaged += each[k].nstaged;
	}
	pn->plan = (SiftF2ParPlan) {.refs = palloc(Max(nrefs, 1) * sizeof(SiftParPartRef)),.staged_dead = palloc(Max(nstaged, 1) * sizeof(uint64))};
	pn->staged_start = palloc((pn->nsources + 1) * sizeof(uint64));
	for (int k = 0; k < pn->nsources; k++)
	{
		pn->staged_start[k] = pn->plan.nstaged;
		for (uint32 i = 0; i < each[k].nrefs; i++)
		{
			pn->plan.refs[pn->plan.nrefs] = each[k].refs[i];
			pn->plan.refs[pn->plan.nrefs++].child_idx = (uint16) k;
		}
		if (each[k].nstaged > 0)
			memcpy(pn->plan.staged_dead + pn->plan.nstaged, each[k].staged_dead, each[k].nstaged * sizeof(uint64));
		pn->plan.nstaged += each[k].nstaged;
		pfree(each[k].refs);
		pn->sources[k].ops->par->end(scan[k]);
	}
	pn->staged_start[pn->nsources] = pn->plan.nstaged;
	pfree(scan);
	pfree(each);
}

/* The leader lists the units of the scan once; the block holds them for every participant */
Size
sift_v6_par_node_estimate(SiftV6ParNode *pn, ParallelContext *pcxt)
{
	MemoryContext old = MemoryContextSwitchTo(pn->mcx);
	const SiftV6Spec *spec = pn->spec;

	list_units(pn);
	pn->shape = (SiftV6ParShape) {.strategy = (SiftV6ParStrategy) spec->par_strategy,.nworkers = pcxt->nworkers,.n_parts = pn->plan.nrefs,.part_ref_bytes = sizeof(SiftParPartRef),
		.n_children = pn->nsources,.n_staged_dead = pn->plan.nstaged,.n_aggs = (uint32) spec->naggs,.n_dist = count_distinct(spec),
		.max_chunks = sift_v6_par_chunks((SiftV6ParStrategy) spec->par_strategy, chunk_budget(pn, pcxt->nworkers + 1)),.radix_nparts = sift_v6_par_radix_parts((SiftV6ParStrategy) spec->par_strategy, pcxt->nworkers),
		.plain_slot_bytes = (uint32) (sizeof(SiftV6RecHdr) + (Size) Max(spec->naggs, 1) * sizeof(V6Acc))};
	MemoryContextSwitchTo(old);
	return sift_v6_par_dsm_size(&pn->shape);
}

/* MEM[MEM-AGG-17]: parallel granules_of array (nrefs * 4B, freed after the morsel table copied it; was leaked on every init and rescan) */
static uint32 *
granules_of(const SiftV6ParNode *pn)
{
	uint32	   *g = palloc(Max(pn->plan.nrefs, 1) * sizeof(uint32));

	for (uint32 i = 0; i < pn->plan.nrefs; i++)
		g[i] = pn->plan.refs[i].ngranules;
	return g;
}

void
sift_v6_par_node_initialize(SiftV6ParNode *pn, ParallelContext *pcxt, void *chunk)
{
	uint32	   *granules = granules_of(pn);
	SiftV6ParDsmHdr *h = sift_v6_par_dsm_init(chunk, pcxt->seg, &pn->shape, granules);

	pfree(granules);
	memcpy((char *) h + h->parts_off, pn->plan.refs, (Size) pn->plan.nrefs * sizeof(SiftParPartRef));
	memcpy(sift_v6_par_staged_starts(h), pn->staged_start, (Size) (pn->nsources + 1) * sizeof(uint64));
	if (pn->plan.nstaged > 0)
		memcpy(sift_v6_par_staged_starts(h) + pn->nsources + 1, pn->plan.staged_dead, (Size) pn->plan.nstaged * sizeof(uint64));
	pn->hdr = h;
}

/* The scan starts again from the same units; the run of this process is over and so is what the participants left */
void
sift_v6_par_node_reinitialize(SiftV6ParNode *pn, void *chunk)
{
	sift_v6_par_node_end(pn);
	pn->have_total = false;
	pn->total = (SiftV6ParWorkerStats) {0};
	uint32	   *granules = granules_of(pn);

	pn->hdr = sift_v6_par_dsm_block(chunk);
	sift_v6_par_dsm_reinit(pn->hdr, granules);
	pfree(granules);
}

void
sift_v6_par_node_worker(SiftV6ParNode *pn, void *chunk)
{
	SiftV6ParDsmHdr *h = sift_v6_par_dsm_block(chunk);

	SIFT_INVARIANT(h->magic == SIFT_V6_PAR_DSM_MAGIC, "the leader initialized the shared block of the parallel aggregate");
	sift_par_worker_attach_custom(sift_v6_par_coord(h), dsm_find_mapping(h->seg_handle));
	pn->hdr = h;
}

bool
sift_v6_par_node_next(SiftV6ParNode *pn, Datum *values, bool *isnull)
{
	MemoryContext old = MemoryContextSwitchTo(pn->mcx);
	bool		got;

	if (pn->run == NULL)
	{
		sift_v6_par_engine_make(&pn->eng, pn->spec, pn->mcx, pn->tuple_cx, pn->desc, pn->hdr, pn->sources, pn->snap);
		if (pn->spec->topk > 0)
			pn->topk = sift_v6_par_topk_new(pn->mcx, pn->spec, pn->desc);
		pn->run = sift_v6_par_run_new(pn->hdr, &pn->eng, pn->topk != NULL ? sift_v6_par_topk_heap(pn->topk) : NULL);
	}
	got = pn->topk != NULL ? sift_v6_par_topk_next(pn->topk, pn->run, values, isnull) : sift_v6_par_next(pn->run, values, isnull);
	MemoryContextSwitchTo(old);
	return got;
}

/* This process is done with the run: it leaves the barrier and publishes its counters before the segment goes away */
void
sift_v6_par_node_end(SiftV6ParNode *pn)
{
	if (pn->run == NULL)
		return;
	sift_v6_par_run_end(pn->run);
	pn->run = NULL;
	pn->topk = NULL;
	sift_v6_par_engine_end(&pn->eng);
}

static void
add_stats(SiftV6ParWorkerStats *t, const SiftV6ParWorkerStats *s)
{
	uint64	   *d = (uint64 *) t;
	const uint64 *p = (const uint64 *) s;

	for (Size i = 0; i < sizeof(*t) / sizeof(uint64); i++)
		d[i] += p[i];
}

void
sift_v6_par_node_shutdown(SiftV6ParNode *pn)
{
	SiftV6ParDsmHdr *h = pn->hdr;

	sift_v6_par_node_end(pn);
	if (h == NULL || pn->have_total)
		return;
	for (uint32 pid = 0; pid < pg_atomic_read_u32(&h->next_pid); pid++)
		add_stats(&pn->total, sift_v6_par_stats(h, (int) pid));
	pn->ring_recs = pg_atomic_read_u64(&h->ring.total_recs);
	pn->ring_bytes = pg_atomic_read_u64(&h->ring.total_bytes);
	pn->radix_parts = h->radix_nparts;
	pn->escalated = pg_atomic_read_u32(&h->escalated) != 0;
	pn->have_total = true;
	pn->hdr = NULL;
}
