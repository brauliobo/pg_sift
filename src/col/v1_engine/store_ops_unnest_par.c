/*
 * col/v1_engine/store_ops_unnest_par.c - the aggregate scan of a LATERAL unnest of a Format 1 index (store_ops_unnest.c) in the participants of a parallel run (agg/par/par.h): the leader lists the
 * superblocks of the directory view it exported, every participant runs the statement as the serial run does over the superblocks it claims, into the groups of its own state.
 */
#include "col/v1_engine/store_ops_unnest.h"

typedef struct V1UnPar
{
	UnRun		u;
	Relation	heap;
	Relation	index;
	Snapshot	snap;
	const void *view;
	MemoryContext cx;
	MemoryContext morsel_cx;	/* the source of a run of superblocks */
} V1UnPar;

static struct SiftV6Scan *
par_begin(const SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, const struct SiftStoreQuals *quals, Snapshot snap, SiftV6ScanStats *stats,
		  struct SiftV6StarRun *star, SiftV6BatchFn emit, void *emit_arg)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 unnest parallel scan");
	MemoryContext old = MemoryContextSwitchTo(cx);
	V1UnPar    *p = sift_palloc0_aligned(V1UnPar, 1);

	SIFT_INVARIANT(index != NULL, "a parallel unnest reads a sift_col index");
	*p = (V1UnPar) {.heap = heap,.index = index,.snap = snap,.cx = cx,.morsel_cx = sift_mem_allocset_create(cx, "sift v1 unnest parallel morsel")};
	sift_v1_unnest_open(&p->u, spec, NULL, heap, index, stats, star);
	MemoryContextSwitchTo(old);
	return (struct SiftV6Scan *) p;
}

static void
par_plan(struct SiftV6Scan *scan, SiftF2ParPlan *out)
{
	V1UnPar    *p = (V1UnPar *) scan;

	sift_v1_par_plan(p->heap, p->index, p->snap, p->cx, out);
}

static void
par_morsel_begin(struct SiftV6Scan *scan, const uint64 *view, uint32 nview)
{
	((V1UnPar *) scan)->view = nview > 0 ? view : NULL;
}

static void
par_morsel(struct SiftV6Scan *scan, SiftV6Group *into, const SiftParPartRef *ref, uint32 first, uint32 n)
{
	V1UnPar    *p = (V1UnPar *) scan;
	const SiftV1Range range = {.view = p->view,.sb_lo = ref->g_base + first,.sb_hi = ref->g_base + first + n};
	MemoryContext old = MemoryContextSwitchTo(p->morsel_cx);

	p->u.pipe->group = into;
	sift_v1_unnest_read(&p->u, p->heap, p->index, NULL, 0, p->snap, &range);
	MemoryContextSwitchTo(old);
	MemoryContextReset(p->morsel_cx);
}

static void
par_end(struct SiftV6Scan *scan)
{
	V1UnPar    *p = (V1UnPar *) scan;

	sift_v1_unnest_close(&p->u);
	MemoryContextDelete(p->cx);
}

const SiftStoreParOps sift_v1_unnest_par_ops = {
	.begin = par_begin,
	.plan = par_plan,
	.morsel_begin = par_morsel_begin,
	.morsel = par_morsel,
	.end = par_end
};
