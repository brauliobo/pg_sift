/*
 * col/f2/par.c - the parallel scan of a format-2 index: the leader lists the units of the scan once (a part of a partition that the key ranges and the zone maps leave, or the L0 slab of a partition)
 * into the references of col/f2/sink/sink.h, and every participant takes runs of the granules of the units. The units, the granule runs and the visibility of a row are those of the serial scan
 * (f2_scan.c): this file only decides which participant reads which run, and keeps the part a participant has open between its runs.
 */
#include "postgres.h"

#include "access/relscan.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "col/f2/scan/par_int.h"
#include "col/f2/cache.h"
#include "col/f2/part/pin_scan.h"
#include "col/f2/l0/stage.h"
#include "col/f2/part/vis.h"
#include "core/errctx.h"
#include "core/memtrack.h"

typedef struct PlanBuild
{
	const SiftF2Rel *f;
	SiftF2ParPlan *out;
	uint32		cap;
} PlanBuild;

static void
plan_add(PlanBuild *b, const SiftParPartRef *ref)
{
	if (b->out->nrefs == b->cap)
		b->out->refs = repalloc(b->out->refs, (b->cap *= 2) * sizeof(SiftParPartRef));
	b->out->refs[b->out->nrefs++] = *ref;
}

static void
plan_unit(F2Scan *s, Relation rel, const F2Unit *u, void *arg)
{
	PlanBuild  *b = arg;
	SiftParPartRef ref = {.part_id = u->e->part_id,.l0_slab_blk = InvalidBlockNumber,.pend_del_blk = u->e->pend_del_blk,.entry_flags = u->e->flags,.manifest_gen = u->e->manifest_gen,
	.manifest_blk = u->e->manifest_blk};
	uint32		spans[SIFT_F2_MAX_RANGES][2];
	int			nspans;
	SiftPart   *part;

	if (u->d == NULL)
	{
		ref.l0_slab_blk = u->e->l0_slab_blk;
		ref.ngranules = 1;
		ref.flags = SIFT_PAR_PART_L0_SLAB;
		plan_add(b, &ref);
		return;
	}
	part = sift_part_open_desc(rel, u->d, (uint32) u->e->part_id);
	nspans = sift_f2_granule_spans(rel, part, b->f, &s->keys, &s->keyplan, spans);
	ref.desc = *u->d;
	if ((u->d->flags & SIFT_PART_FLAG_ALL_VISIBLE) && u->d->dv_blk == InvalidBlockNumber && u->e->pend_del_blk == InvalidBlockNumber && (u->e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		ref.flags |= SIFT_PAR_PART_ALL_VISIBLE;
	if (u->e->pend_del_blk != InvalidBlockNumber)
		ref.flags |= SIFT_PAR_PART_HAS_PEND_DEL;
	for (int i = 0; i < nspans; i++)
	{
		ref.g_base = spans[i][0];
		ref.ngranules = spans[i][1] - spans[i][0];
		plan_add(b, &ref);
	}
}

void
sift_f2_par_plan_build(IndexScanDesc scan, SiftF2ParPlan *out)
{
	F2Scan	   *s = scan->opaque;
	Relation	rel = scan->indexRelation;
	Snapshot	snap = scan->xs_snapshot != NULL ? scan->xs_snapshot : GetActiveSnapshot();
	PlanBuild	b = {.f = sift_f2_rel(rel),.out = out,.cap = 16};
	int			nstaged;
	uint64	   *staged;

	*out = (SiftF2ParPlan) {.refs = palloc(b.cap * sizeof(SiftParPartRef))};
	if (!s->prepared)
		sift_f2_scan_prepare(scan, s);
	if (!s->possible)
		return;
	if (!RecoveryInProgress() && sift_stage_has_pending(rel))
		sift_stage_flush(rel);
	staged = sift_stage_dead(rel, snap, &nstaged);
	if (!s->compiled)
		compile_preds(s, sift_col_rel(rel)->schema);
	walk_units(rel, s, sift_plan_cache_get(rel, s->cx), snap, staged, nstaged, false, plan_unit, &b);
	out->staged_dead = staged;
	out->nstaged = (uint32) nstaged;
}

/* What a participant keeps between the runs of granules it takes: the pending deletes of the partition and the part it has open */
typedef struct MorselCx
{
	const SiftPendDelIndex *staged_px;	/* the rows the transaction deleted (the leader's list), as one set for every partition */
	uint64		px_part_id;
	const SiftPendDelIndex *px;
	bool		have_px;
	MemoryContext part_cx;		/* what is built for the open part: reset when another is opened */
	SiftF2PartPin *pin;
	uint64		part_gen;
	uint64		part_id;
	bool		open;			/* false: the predicates ruled out every row of the part */
	SiftPartDesc desc;
	PartCx		cx;
	MemoryContextCallback release;
} MorselCx;

static void
morsel_release(MorselCx *m)
{
	if (m->pin != NULL)
		sift_f2_pin_release(m->pin);
	m->pin = NULL;
}

static void
release_at_reset(void *arg)
{
	morsel_release(arg);
}

void
sift_f2_scan_morsel_begin(IndexScanDesc scan, SiftF2Sink sink, void *arg, const int *leaves, int nleaves, const uint64 *staged_dead, uint32 nstaged)
{
	F2Scan	   *s = scan->opaque;
	MemoryContext old = MemoryContextSwitchTo(s->cx);
	MorselCx   *m = palloc0(sizeof(MorselCx));

	s->sink = sink;
	s->sink_arg = arg;
	s->sink_leaves = leaves;
	s->nsink_leaves = nleaves;
	sift_f2_scan_prepare(scan, s);
	if (s->recheck_all)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a qual of the aggregate cannot be decided on the stored values"),
						errhint("Run the statement with sift.enable_agg = off: the heap decides such a qual.")));
	if (!s->compiled)
		compile_preds(s, sift_col_rel(scan->indexRelation)->schema);
	m->staged_px = sift_f2_staged_index(staged_dead, (int) nstaged);
	m->part_cx = sift_mem_allocset_create(s->cx, "sift f2 morsel part");
	m->release = (MemoryContextCallback) {.func = release_at_reset,.arg = m};
	MemoryContextRegisterResetCallback(s->cx, &m->release);
	s->morsel = m;
	MemoryContextSwitchTo(old);
}

static SiftPartitionEntry
entry_of(const SiftParPartRef *ref)
{
	return (SiftPartitionEntry) {.part_id = ref->part_id,.l0_slab_blk = ref->l0_slab_blk,.pend_del_blk = ref->pend_del_blk,.flags = ref->entry_flags,.manifest_gen = ref->manifest_gen,
	.manifest_blk = ref->manifest_blk};
}

/* The pending deletes of the partition and the staged ones as an index: built once for the runs of the same partition that follow each other */
static const SiftPendDelIndex *
morsel_px(Relation rel, MorselCx *m, const SiftPartitionEntry *e, Snapshot snap)
{
	if (m->have_px && m->px_part_id == e->part_id)
		return m->px;
	m->px = sift_f2_pending_index(rel, e, snap, m->staged_px);
	m->px_part_id = e->part_id;
	m->have_px = true;
	return m->px;
}

static void
morsel_open(Relation rel, F2Scan *s, MorselCx *m, const SiftColSchema *schema, const SiftParPartRef *ref, const SiftPartitionEntry *e, Snapshot snap)
{
	morsel_release(m);
	MemoryContextReset(m->part_cx);
	m->desc = ref->desc;
	m->part_gen = ref->desc.part_gen;
	m->part_id = ref->part_id;
	m->pin = sift_f2_scan_pin(rel, &m->desc, (uint32) ref->part_id);
	s->part = &m->desc;
	s->tids_unneeded = false;
	s->cands = NULL;
	F2_STAT(parts_read);
	MemoryContextSwitchTo(m->part_cx);
	m->open = part_open(rel, s, schema, e, m->pin, snap, &m->cx);
}

void
sift_f2_scan_morsel(IndexScanDesc scan, const SiftParPartRef *ref, uint32 first, uint32 n)
{
	F2Scan	   *s = scan->opaque;
	Relation	rel = scan->indexRelation;
	const SiftColSchema *schema = sift_col_rel(rel)->schema;
	MorselCx   *m = s->morsel;
	Snapshot	snap = scan->xs_snapshot != NULL ? scan->xs_snapshot : GetActiveSnapshot();
	const SiftPartitionEntry e = entry_of(ref);
	const SiftPendDelIndex *px;
	MemoryContext old = MemoryContextSwitchTo(s->cx);
	SiftErrCtx	ctx;

	sift_errctx_push(&ctx, RelationGetRelid(rel));
	px = morsel_px(rel, m, &e, snap);
	if (ref->flags & SIFT_PAR_PART_L0_SLAB)
		scan_slab(rel, s, sift_f2_rel(rel), schema, &e, px, snap);
	else
	{
		if (m->pin == NULL || m->part_gen != ref->desc.part_gen || m->part_id != ref->part_id)
		{
			morsel_open(rel, s, m, schema, ref, &e, snap);
			MemoryContextSwitchTo(s->cx);
		}
		if (m->open)
			scan_span(rel, s, schema, m->pin, &m->cx.pp, ref->g_base + first, ref->g_base + first + n, px, snap);
	}
	sift_errctx_pop(&ctx);
	MemoryContextSwitchTo(old);
}

void
sift_f2_scan_morsel_end(IndexScanDesc scan)
{
	F2Scan	   *s = scan->opaque;

	if (s->morsel != NULL)
		morsel_release(s->morsel);
}
