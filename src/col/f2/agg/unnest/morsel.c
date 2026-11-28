/*
 * col/f2/agg/unnest/morsel.c - the aggregate scan of a LATERAL unnest (agg/v6_unnest_run.h) in the participants of a parallel run (agg/par/par.h): the leader lists the units of the index, every
 * participant runs the statement as the serial run does over the units it claims, each read by a pull of that unit (col/f2/sink/ext.h), into the groups of its own state. The cubes of the
 * elements (col/f2/agg/map_preagg.h) answer the granules of the units they can, into the same groups.
 */
#include "col/f2/agg/unnest/int.h"

#include "col/f2/sink/sink.h"
#include "core/compat.h"
#include "core/store/ops.h"

typedef struct UnPar
{
	UnRun		u;
	Relation	heap;
	Relation	index;
	SiftV6MapPreagg *map;
	MemoryContext cx;
} UnPar;

static struct SiftV6Scan *
par_begin(const SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, const struct SiftStoreQuals *quals, Snapshot snap, SiftV6ScanStats *stats,
		  struct SiftV6StarRun *star, SiftV6BatchFn emit, void *emit_arg)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 unnest parallel scan");
	MemoryContext old = MemoryContextSwitchTo(cx);
	UnPar	   *p = sift_palloc0_aligned(UnPar, 1);

	SIFT_INVARIANT(index != NULL, "a parallel unnest reads a sift_col index");
	p->cx = cx;
	p->heap = heap;
	p->index = index;
	sift_f2_unnest_open(&p->u, spec, NULL, heap, index, NULL, 0, snap, stats, star);
	p->map = sift_v6_map_preagg_begin(spec, NULL, &p->u.vet, stats);
	p->u.ps.gran_sink = p->map != NULL ? sift_v6_map_preagg_try_granule : NULL;
	p->u.ps.gran_arg = p->map;
	MemoryContextSwitchTo(old);
	return (struct SiftV6Scan *) p;
}

/* every unit of the index: the predicates of the statement are the pull's, which rules out the granules of a unit it reads */
static void
par_plan(struct SiftV6Scan *scan, SiftF2ParPlan *out)
{
	UnPar	   *p = (UnPar *) scan;
	IndexScanDesc is = sift_index_beginscan(p->heap, p->index, p->u.snap, 0);

	index_rescan(is, NULL, 0, NULL, 0);
	sift_f2_par_plan_build(is, out);
	index_endscan(is);
}

static void
par_morsel_begin(struct SiftV6Scan *scan, const uint64 *staged_dead, uint32 nstaged)
{
	UnPar	   *p = (UnPar *) scan;
	MemoryContext old = MemoryContextSwitchTo(p->cx);

	p->u.pull = sift_f2_pull_begin_units(p->index, p->u.snap, &p->u.ps, staged_dead, nstaged);
	MemoryContextSwitchTo(old);
}

static void
par_morsel(struct SiftV6Scan *scan, SiftV6Group *into, const SiftParPartRef *ref, uint32 first, uint32 n)
{
	UnPar	   *p = (UnPar *) scan;
	const int64 read = sift_f2_pull_stats(p->u.pull)->granules_read;
	const SiftF2ExtBatch *b;

	p->u.pipe->group = into;
	if (p->map != NULL)
		sift_v6_map_preagg_set_group(p->map, into);
	sift_f2_pull_unit(p->u.pull, ref, first, n);
	while ((b = sift_f2_pull_next(p->u.pull)) != NULL)
		sift_f2_unnest_batch(&p->u, b);
	p->u.stats->grans += (uint64) (sift_f2_pull_stats(p->u.pull)->granules_read - read);
}

static void
par_end(struct SiftV6Scan *scan)
{
	UnPar	   *p = (UnPar *) scan;

	if (p->u.pull != NULL)
		sift_f2_pull_end(p->u.pull);
	if (p->map != NULL)
		sift_v6_map_preagg_end(p->map);
	sift_f2_unnest_close(&p->u);
	MemoryContextDelete(p->cx);
}

const SiftStoreParOps sift_f2_unnest_par_ops = {
	.begin = par_begin,
	.plan = par_plan,
	.morsel_begin = par_morsel_begin,
	.morsel = par_morsel,
	.end = par_end
};
