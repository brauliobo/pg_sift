/*
 * col/f2/store/scan.c - the scan of a sift_table for the aggregates and the other readers of columns (col/f2/store/scan.h), and the granules of a sift_col index or a sift_table one part at a
 * time for a reader that pulls them.
 *
 * The parts come from the manifest as of now (the slabs written out as parts), the partitions and the parts are ruled out by the time window of the predicates, by the key ranges and the marks of the
 * whole key, and by the generations a projection stands for; the rows of a granule are those the versions of the table show to the snapshot (SEC_VERS, the deletion vector, the pending deletes), with
 * the column deltas laid over the decoded columns and the columns a part lacks or wrote in another form read through its map. A sift_col index has no versions of its own: its rows are those its
 * deletion vectors and pending deletes leave, and the heap decides the rest (the reader vets them, as the sink of sift_f2_scan_sink does). The push scan of a sift_col index is the business of its own
 * driver (f2_scan.c).
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "lib/ilist.h"
#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/f2.h"
#include "col/f2/cache.h"
#include "col/f2/gbuf/gbuf.h"
#include "col/f2/marks.h"
#include "col/f2/part/pin_scan.h"
#include "col/f2/pred/pred.h"
#include "col/f2/store/schema.h"
#include "col/f2/view.h"
#include "col/f2/store/scan.h"
#include "col/f2/store/scan_int.h"
#include "col/f2/key/plan.h"
#include "col/f2/l0/stage.h"
#include "col/f2/part/vis.h"
#include "col/f2/del/pend.h"
#include "core/errctx.h"
#include "col/f2/pin_win.h"
#include "col/f2/slab_view.h"
#include "core/memtrack.h"
#include "core/tidbridge.h"
#include "table/f2/blob.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/tbl.h"

/* ---- the predicates on the stored values ---- */

static Verdict
row_verdict(const SiftStorePart *p, int r)
{
	const SiftStoreScan *s = p->s;
	Verdict		all = V_YES;

	for (int k = 0; k < s->o->npreds; k++)
	{
		const SiftColPred *pr = &s->o->preds[k];
		Verdict		v = sift_f2_pred_row(&s->store->schema->cols[pr->col], pr, &s->psets[k], &p->r->views[pr->col], r);

		if (v == V_NO)
			return V_NO;
		all = v == V_MAYBE ? V_MAYBE : all;
	}
	return all;
}


/* ---- the rows of a granule: what each host shows to the snapshot ---- */

static bool
table_row(SiftStorePart *p, uint16 r)
{
	return sift_tbl_f2_vis_row(&p->s->tvis, p->r->gb, r, p->px) && !sift_pdel_idx_has(p->s->staged_px, p->r->tids[r]);
}

static void
index_granule(SiftStorePart *p)
{
	const SiftF2PartPin *pin = p->r->gb->pin;
	const SiftDvGran *dg = pin->dv != NULL ? sift_dv_granule(pin->dv, p->r->gb->granule_id) : NULL;

	p->r->any_dead = dg != NULL && sift_dv_mask64(dg, p->s->o->snap, p->r->dead) > 0;
	p->r->pend_hit = p->px != NULL && sift_pdel_idx_may_hit_granule(p->px, &pin->tidx[p->r->gb->granule_id]);
}

static bool
index_row(SiftStorePart *p, uint16 r)
{
	return !(p->r->any_dead && ((p->r->dead[r >> 6] >> (r & 63)) & 1)) && !(p->r->pend_hit && sift_pdel_idx_has(p->px, p->r->tids[r]));
}

static const RowVis table_vis = {NULL, table_row};
static const RowVis index_vis = {index_granule, index_row};

static SiftPendDelIndex *
table_pending(SiftStoreScan *s, const SiftPartitionEntry *e)
{
	return sift_tbl_f2_pending_index(s->o->rel, e, s->o->snap);
}

static SiftPendDelIndex *
index_pending(SiftStoreScan *s, const SiftPartitionEntry *e)
{
	return sift_f2_pending_index(s->o->rel, e, s->o->snap, s->staged_px);
}

/* ---- a granule ---- */

static bool
in_bridge(const SiftTidBridge *br, uint64 tid)
{
	float4		score;

	return sift_tidbridge_probe_tid(br, tid, &score) != SIFT_TIDPROBE_NO;
}

static void
select_tagged_cols(SiftStorePart *p)
{
	const SiftStoreScan *s = p->s;

	p->r->nactive = 0;
	for (int k = 0; k < s->ncheck; k++)
		if (p->r->views[s->check_cols[k]].tag != NULL)
			p->r->active[p->r->nactive++] = s->check_cols[k];
}

static void
refuse_unsure_cells(const SiftStorePart *p, int r)
{
	for (int k = 0; k < p->r->nactive; k++)
	{
		const ColView *v = &p->r->views[p->r->active[k]];

		if (v->isnull[r] && v->tag[r] == SIFT_COLTAG_UNSURE)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a numeric NaN cannot be aggregated on the stored values"),
							errhint("Run the statement with sift.enable_agg = off.")));
	}
}

/* the rows of the loaded granule as a batch; false when no row is left */
static bool
granule_batch(SiftStorePart *p, SiftF2Batch *b)
{
	const SiftStoreScan *s = p->s;
	int			n = p->r->gb->nrows;
	int			any = 0;

	p->r->tids = sift_f2_gbuf_tids(p->r->gb);
	memset(p->r->live, 0, sizeof(p->r->live));
	memset(p->r->maybe, 0, sizeof(p->r->maybe));
	sift_store_scan_load_views(p);
	select_tagged_cols(p);
	if (s->vis->granule != NULL)
		s->vis->granule(p);
	for (int r = 0; r < n; r++)
	{
		Verdict		v;

		if (!s->vis->row(p, (uint16) r) || (s->o->bridge != NULL && !in_bridge(s->o->bridge, p->r->tids[r])))
			continue;
		v = row_verdict(p, r);
		if (v == V_MAYBE && !s->o->emit_maybe)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a qual of the aggregate cannot be decided on the stored values"),
							errhint("Run the statement with sift.enable_agg = off: the heap decides such a qual.")));
		if (v == V_YES)
			refuse_unsure_cells(p, r);
		p->r->live[r >> 6] |= (uint64) (v == V_YES) << (r & 63);
		p->r->maybe[r >> 6] |= (uint64) (v == V_MAYBE) << (r & 63);
		any += v != V_NO;
	}
	*b = (SiftF2Batch) {.views = p->r->views,.tid = p->r->tids,.live = p->r->live,.maybe = p->r->maybe,.n = n};
	return any > 0;
}

/* ---- the parts ---- */

static bool
part_wanted(const SiftStoreScan *s, const SiftPartDesc *d, const bool dirty[SIFT_MANIFEST_TIME_COLS])
{
	if (sift_store_gen_skipped(s->o->skip_gens, s->o->nskip_gens, d->part_gen) || !sift_f2_part_overlaps(d, &s->ks))
		return false;
	for (int k = 0; k < SIFT_MANIFEST_TIME_COLS; k++)
	{
		SiftStoreTimeWindow w = {.use = s->tw.use[k],.lo = s->tw.lo[k],.hi = s->tw.hi[k]};

		if (!sift_store_time_may_hold(&w, (d->flags & (k == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1)) != 0, d->time_min[k], d->time_max[k], dirty[k]))
			return false;
	}
	return true;
}

const int16 *
sift_f2_store_part_phys(const SiftF2Store *store, const SiftF2ColMap *map, MemoryContext cx)
{
	int16	   *phys;

	if (map == NULL)
		return NULL;
	phys = MemoryContextAlloc(cx, sizeof(int16) * store->ncols);
	for (int leaf = 0; leaf < store->ncols; leaf++)
		phys[leaf] = map->mode[leaf] == SIFT_CMAP_DIRECT ? map->phys_col[leaf] : -1;
	return phys;
}

static void
part_read_begin(SiftStorePart *p)
{
	const SiftStoreScan *s = p->s;

	p->cx = sift_mem_allocset_create(s->cx, "sift f2 store scan part");
	p->r = MemoryContextAllocZero(p->cx, sizeof(SiftPartRead));
	p->r->gcx = sift_mem_allocset_create(p->cx, "sift f2 store scan granule");
	p->r->views = MemoryContextAllocZero(p->cx, sizeof(ColView) * s->store->ncols);
	p->r->active = MemoryContextAlloc(p->cx, sizeof(int) * Max(s->ncheck, 1));
}

static void
part_open_pin(SiftStorePart *p)
{
	SiftStoreScan *s = p->s;

	part_read_begin(p);
	p->pin = sift_f2_pin_open(s->o->rel, s->store, p->d, (uint32) p->e->part_id);
	p->r->nspans = sift_f2_granule_spans(s->o->rel, p->pin->part, &s->f, &s->ks, &s->plan, p->r->spans);
	p->r->g = p->r->nspans > 0 ? p->r->spans[0][0] : 0;
	p->r->phys = s->o->gran_sink != NULL && s->vis == &table_vis ? sift_f2_store_part_phys(s->store, p->pin->col_map, p->cx) : NULL;
	p->started = true;
}

/* How the table knows the rows of the granule: the inserters of all of them are settled for an MVCC snapshot of a transaction that is not serializable, or they are asked one by one */
static SiftF2GranVis
granule_vis(const SiftStorePart *p, uint32 g)
{
	const SiftStoreScan *s = p->s;

	if (s->tvis.serializable || s->o->snap->snapshot_type != SNAPSHOT_MVCC)
		return SIFT_GVIS_PER_ROW;
	return sift_vis_granule_inserts_settled(p->d, &p->pin->gdir[g], sift_vis_snap_xmin(s->o->snap)) ? SIFT_GVIS_SETTLED : SIFT_GVIS_PER_ROW;
}

/* The zone and the exception of granule g of the leaf of each predicate, as the part holds the leaf */
static void
offer_zones(const SiftStorePart *p, uint32 g)
{
	const SiftStoreScan *s = p->s;
	int			nexc;
	const SiftVariantExcSummary *exc = sift_f2_pin_exc(s->o->rel, p->pin, &nexc);

	for (int k = 0; k < s->o->npreds; k++)
	{
		const int	leaf = p->r->phys != NULL ? p->r->phys[s->o->preds[k].col] : s->o->preds[k].col;
		const SiftZone *zones = leaf >= 0 ? sift_f2_pin_zones(s->o->rel, p->pin, leaf) : NULL;

		s->offer_zones[k] = zones != NULL ? &zones[g] : NULL;
		s->offer_exceptions[k] = leaf < 0 || sift_f2_exc_has(exc, nexc, g, leaf);
	}
}

/* The granule of a table offered to the sink of the scan before a byte of it is read: true when the sink answered it */
static bool
granule_answered(const SiftStorePart *p, uint32 g)
{
	const SiftStoreScan *s = p->s;
	const SiftF2PartPin *pin = p->pin;
	const SiftStoreScanOpts *o = s->o;

	if (o->gran_sink == NULL || s->vis != &table_vis || o->bridge != NULL)
		return false;
	offer_zones(p, g);
	return o->gran_sink(o->gran_arg, &(SiftF2GranOffer) {.rel = o->rel,.part = pin->part,.desc = p->d,.schema = s->store->schema,.g = g,.gd = &pin->gdir[g],
						 .dead = pin->dv != NULL ? sift_dv_granule(pin->dv, g) : NULL,.snap = o->snap,.preds = o->preds,.npreds = o->npreds,.decided = s->offer_decided,
						 .zones = s->offer_zones,.exceptions = s->offer_exceptions,.pend_hit = p->px != NULL && sift_pdel_idx_may_hit_granule(p->px, &pin->tidx[g]),
						 .patch = sift_col_delta_scan_granule(s->delta, p->e, pin, g),.patch_attno = s->store->leaf_attno,.px = p->px,.vis = granule_vis(p, g),.phys = p->r->phys});
}

static void
part_give_back_pin(SiftStorePart *p)
{
	if (p->pin != NULL)
		sift_f2_pin_release(p->pin);
	p->pin = NULL;
}

static void
part_release_pin(SiftStorePart *p)
{
	if (p->r == NULL)
		return;
	part_give_back_pin(p);
	MemoryContextDelete(p->cx);
	p->cx = NULL;
	p->r = NULL;
}

static void
part_release_granule(SiftStorePart *p)
{
	if (p->r == NULL)
		return;
	if (p->r->gb != NULL)
		sift_f2_gbuf_unref(p->r->gb);
	p->r->gb = NULL;
	MemoryContextReset(p->r->gcx);
}

static bool
part_next_granule_id(SiftStorePart *p, uint32 *g)
{
	while (p->r->span < p->r->nspans)
	{
		if (p->r->g < p->r->spans[p->r->span][1])
		{
			*g = p->r->g++;
			return true;
		}
		if (++p->r->span < p->r->nspans)
			p->r->g = p->r->spans[p->r->span][0];
	}
	return false;
}

/* the hybrid scan counts the granules it misses, and reads none of them */
static bool
bridge_wants(const SiftStorePart *p, uint32 g)
{
	const SiftStoreScanOpts *o = p->s->o;
	const SiftTidIndexEntry *t = &p->pin->tidx[g];
	bool		wanted = o->bridge == NULL || sift_tidbridge_granule_may_match(o->bridge, t->tid_min, t->tid_max, t->blk_bloom);

	if (o->bridge_seen != NULL)
		(*o->bridge_seen)++;
	if (!wanted && o->bridge_skipped != NULL)
		(*o->bridge_skipped)++;
	return wanted;
}

static void part_replay(void *arg, const void *host, SiftF2PartPin *pin, uint32 g);

/* The granule shows every row to the snapshot as the stored values do (no version, delete, patch or form of its own to look at), so a thread folds it as the sink would: the window takes it */
static bool
part_handed_over(SiftStorePart *p, uint32 g)
{
	const SiftStoreScan *s = p->s;
	const SiftTblVis *v = &s->tvis;
	bool		eligible;

	if (s->o->pinwin == NULL || s->replaying)
		return false;
	eligible = (p->px == NULL || !sift_pdel_idx_may_hit_granule(p->px, &p->pin->tidx[g])) && s->o->bridge == NULL && !v->serializable && v->snap->snapshot_type == SNAPSHOT_MVCC &&
		(p->e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0 &&
		((p->pin->gdir[g].flags & SIFT_GDESC_ALL_FROZEN) != 0 || (FullTransactionIdIsValid(v->snap_xmin) && sift_vis_part_inserts_settled(&p->pin->desc, v->snap_xmin)));
	return sift_pin_win_take_table(s->o->pinwin, s->o->rel, s->store->schema, p->e, p->pin, g, eligible, part_replay, (void *) s);
}

static bool
part_load_granule(SiftStorePart *p)
{
	const SiftStoreScan *s = p->s;
	uint32		g;

	if (!p->started)
		part_open_pin(p);
	while (p->r != NULL && part_next_granule_id(p, &g))
	{
		if (!bridge_wants(p, g) || granule_answered(p, g))
			continue;
		if (s->o->grans != NULL && !s->replaying)
			s->o->grans->scanned++;
		if (!part_handed_over(p, g))
		{
			p->r->gb = sift_f2_gbuf_load(s->o->rel, s->store, p->pin, g);
			if (s->delta != NULL)
				sift_col_delta_scan_attach(s->delta, p->e, p->r->gb);
			return true;
		}
	}
	part_release_pin(p);
	return false;
}

bool
sift_f2_store_part_next(SiftStorePart *p, SiftF2Batch *b)
{
	part_release_granule(p);
	while (part_load_granule(p))
	{
		if (granule_batch(p, b))
			return true;
		part_release_granule(p);
	}
	return false;
}

static void
part_close(SiftStorePart *p)
{
	dlist_delete(&p->node);
	part_release_granule(p);
	part_release_pin(p);
	pfree(p);
}

static void
scan_pins_gone(void *arg)
{
	SiftStoreScan *s = arg;
	dlist_iter	it;

	dlist_foreach(it, &s->live)
		part_give_back_pin(dlist_container(SiftStorePart, node, it.cur));
}

static SiftStorePart *
part_new(SiftStoreScan *s, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px)
{
	SiftStorePart *p = MemoryContextAllocZero(s->cx, sizeof(SiftStorePart));

	p->s = s;
	p->e = e;
	p->d = d;
	p->px = px;
	dlist_push_head(&s->live, &p->node);
	return p;
}

/* A granule the window took, read and given to the sink as the scan reads it */
static void
part_replay(void *arg, const void *host, SiftF2PartPin *pin, uint32 g)
{
	SiftStoreScan *s = arg;
	SiftStorePart *p = part_new(s, host, &pin->desc, NULL);
	SiftF2Batch b;

	part_read_begin(p);
	p->pin = sift_f2_pin_open(s->o->rel, s->store, &pin->desc, pin->part_id);
	p->r->nspans = 1;
	p->r->spans[0][0] = p->r->g = g;
	p->r->spans[0][1] = g + 1;
	p->started = true;
	s->replaying = true;
	while (sift_f2_store_part_next(p, &b))
		s->sink(s->sink_arg, &b);
	s->replaying = false;
	part_close(p);
}

/* ---- the partitions ---- */

static void
partition_dirty(const SiftStoreScan *s, const SiftPartitionEntry *e, bool dirty[SIFT_MANIFEST_TIME_COLS])
{
	SiftColDeltaAnchor a;

	if ((e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return;
	a = sift_manifest_read_col_delta(s->o->rel, e);
	dirty[0] = (a.flags & SIFT_CDLA_DIRTY_TIME0) != 0;
	dirty[1] = (a.flags & SIFT_CDLA_DIRTY_TIME1) != 0;
}

static bool
partition_in_window(const SiftStoreScan *s, const SiftPartitionEntry *e, const bool dirty[SIFT_MANIFEST_TIME_COLS])
{
	for (int k = 0; k < SIFT_MANIFEST_TIME_COLS; k++)
	{
		SiftStoreTimeWindow w = {.use = s->tw.use[k],.lo = s->tw.lo[k],.hi = s->tw.hi[k]};

		if (!sift_store_time_may_hold(&w, true, e->time_min[k], e->time_max[k], dirty[k]) && e->l0_slab_blk == InvalidBlockNumber)
			return false;
	}
	return true;
}

static void
run_add_partition(SiftStoreScan *s, int i)
{
	const SiftPartitionEntry *e = &s->view->partitions[i];
	const SiftPlanPartList *list = &s->view->partlists[i];
	bool		dirty[SIFT_MANIFEST_TIME_COLS] = {false};
	SiftPendDelIndex *px;

	partition_dirty(s, e, dirty);
	if (!partition_in_window(s, e, dirty))
		return;
	px = s->vis == &table_vis ? table_pending(s, e) : index_pending(s, e);
	s->pxs = lappend(s->pxs, px);
	for (int j = 0; j < list->nparts; j++)
		if (part_wanted(s, &list->parts[j], dirty))
		{
			SiftStorePart *p = part_new(s, e, &list->parts[j], px);

			s->run = lappend(s->run, p);
		}
}

static const SiftPlanSnapshot *
scan_view(SiftStoreScan *s)
{
	if (s->view == NULL)
		s->view = s->o->plan != NULL ? s->o->plan : sift_f2_view(s->o->rel, s->store, s->cx);
	return s->view;
}

static void
run_reset(SiftStoreScan *s)
{
	foreach_ptr(SiftStorePart, p, s->run)
		part_close(p);
	foreach_ptr(SiftPendDelIndex, px, s->pxs)
		sift_pdel_idx_release(px, s->staged_px);
	list_free(s->run);
	list_free(s->pxs);
	s->run = s->pxs = NIL;
}

static bool
same_lower_bound(const SiftStoreScan *s, int a, int b)
{
	return memcmp(s->view->partitions[a].key_lo, s->view->partitions[b].key_lo, SIFT_BOUND_BYTES) == 0;
}

List *
sift_f2_store_scan_next_run(SiftStoreScan *s, bool ordered)
{
	MemoryContext old = MemoryContextSwitchTo(s->cx);

	run_reset(s);
	while (s->run == NIL && s->possible && s->next < scan_view(s)->npartitions)
	{
		int			first = s->next++;

		run_add_partition(s, first);
		while (ordered && s->next < s->view->npartitions && same_lower_bound(s, first, s->next))
			run_add_partition(s, s->next++);
		CHECK_FOR_INTERRUPTS();
	}
	MemoryContextSwitchTo(old);
	return s->run;
}

/* ---- the scan ---- */

static uint64
wanted_attnum_bloom(const SiftStoreScan *s)
{
	uint64		bloom = 0;

	for (int leaf = 0; leaf < s->store->ncols; leaf++)
		if (s->want[leaf])
			bloom |= sift_cd_att_bit(s->store->leaf_attno[leaf]);
	return bloom;
}

SiftStoreScan *
sift_f2_store_scan_open(const SiftStoreScanOpts *o)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 store scan");
	SiftStoreScan *s = MemoryContextAllocZero(cx, sizeof(SiftStoreScan));
	bool		table = o->store->host->host_kind == SIFT_F2_HOST_TABLE;

	s->o = o;
	s->store = o->store;
	s->cx = cx;
	dlist_init(&s->live);
	s->cb = (MemoryContextCallback) {scan_pins_gone, s, NULL};
	MemoryContextRegisterResetCallback(cx, &s->cb);
	s->vis = table ? &table_vis : &index_vis;
	s->f = sift_f2_store_rel(o->store);
	s->want = MemoryContextAllocZero(cx, sizeof(bool) * o->store->ncols);
	s->psets = sift_f2_pred_sets_new(cx, o->npreds);
	s->wide = table && sift_tbl_extra(o->store)->has_wide;
	for (int i = 0; i < o->nleaves; i++)
		s->want[o->leaves[i]] = true;
	for (int k = 0; k < o->npreds; k++)
		s->want[o->preds[k].col] = true;
	s->check_cols = MemoryContextAlloc(cx, sizeof(int) * Max(o->store->ncols, 1));
	for (int leaf = 0; leaf < o->store->ncols && table; leaf++)
		if (s->want[leaf] && !(o->nan_leaves != NULL && o->nan_leaves[leaf]) && o->store->schema->cols[leaf].parent < 0 && o->store->schema->cols[leaf].kind != SIFT_COLK_GROUP)
			s->check_cols[s->ncheck++] = leaf;
	s->offer_decided = MemoryContextAllocZero(cx, Max(o->npreds, 1));
	s->offer_zones = MemoryContextAllocZero(cx, sizeof(SiftZone *) * Max(o->npreds, 1));
	s->offer_exceptions = MemoryContextAllocZero(cx, Max(o->npreds, 1));
	s->possible = sift_f2_key_set(o->rel, &s->f, o->preds, o->npreds, &s->ks) && sift_f2_time_window(&s->f, o->preds, o->npreds, &s->tw);
	sift_f2_key_plan(&s->f, o->store->schema->cols, o->preds, o->npreds, &s->plan);
	s->possible = s->possible && s->plan.state != SIFT_KEY_PLAN_EMPTY;
	if (!s->possible)
		return s;
	if (table)
	{
		sift_tbl_f2_predicate_scan(o->rel, o->snap);
		sift_tbl_f2_vis_begin(&s->tvis, o->rel, o->snap);
		s->delta = sift_col_delta_scan_begin(o->rel, &s->tvis, cx);
		sift_col_delta_scan_set_attnums(s->delta, wanted_attnum_bloom(s));
		if (o->pinwin != NULL)
			sift_pin_win_table_preds(o->pinwin, o->store->schema, o->preds, o->npreds);
	}
	else
	{
		int			nstaged;
		uint64	   *staged = sift_stage_dead(o->rel, o->snap, &nstaged);

		pgstat_count_index_scan(o->rel);
		s->staged_px = sift_f2_staged_index(staged, nstaged);
	}
	return s;
}

static void morsel_end(SiftStoreScan *s);

void
sift_f2_store_scan_close(SiftStoreScan *s)
{
	run_reset(s);
	if (s->morsel != NULL)
		morsel_end(s);
	MemoryContextDelete(s->cx);
}

static void
scan_table(const SiftStoreScanOpts *o, SiftF2Sink sink, void *arg)
{
	SiftStoreScan *s = sift_f2_store_scan_open(o);
	SiftF2Batch b;
	SiftErrCtx	ctx;

	sift_errctx_push(&ctx, RelationGetRelid(o->rel));
	s->sink = sink;
	s->sink_arg = arg;
	for (List *run; (run = sift_f2_store_scan_next_run(s, false)) != NIL;)
		foreach_ptr(SiftStorePart, p, run)
			while (sift_f2_store_part_next(p, &b))
				sink(arg, &b);
	if (o->pinwin != NULL)
		o->pinwin_done(arg);
	sift_f2_store_scan_close(s);
	sift_errctx_pop(&ctx);
}

void
sift_f2_store_scan_sink(const SiftStoreScanOpts *o, SiftF2Sink sink, void *sink_arg)
{
	if (o->store->host->host_kind == SIFT_F2_HOST_TABLE)
	{
		scan_table(o, sink, sink_arg);
		return;
	}
	if (o->bridge != NULL)
		sift_f2_scan_set_bridge(o->scan, o->bridge, o->bridge_seen, o->bridge_skipped);
	if (o->nskip_gens > 0)
		sift_f2_scan_skip_parts(o->scan, o->skip_gens, o->nskip_gens);
	if (o->emit_maybe)
		sift_f2_scan_sink_maybe(o->scan, sink, sink_arg, o->leaves, o->nleaves);
	else
		sift_f2_scan_sink(o->scan, sink, sink_arg, o->leaves, o->nleaves);
}

/* ---- a parallel scan of a sift_table: the units the leader lists, the runs of granules a participant reads ---- */

/* What a participant keeps between its runs: the part it has open, and the pending deletes of its partition */
typedef struct MorselPart
{
	SiftStorePart *part;
	SiftPartitionEntry e;
	SiftPartDesc d;
	SiftPendDelIndex *px;
	bool		have_px;
} MorselPart;

typedef struct UnitList
{
	SiftF2ParPlan *out;
	uint32		cap;
} UnitList;

static void
add_part_units(SiftStoreScan *s, UnitList *u, const SiftPartitionEntry *e, const SiftPartDesc *d)
{
	SiftParPartRef ref = {.desc = *d,.part_id = e->part_id,.l0_slab_blk = InvalidBlockNumber,.pend_del_blk = e->pend_del_blk,.entry_flags = e->flags,.manifest_gen = e->manifest_gen,
	.manifest_blk = e->manifest_blk};
	uint32		spans[SIFT_F2_MAX_RANGES][2];
	SiftF2PartPin *pin = sift_f2_pin_open_header(s->o->rel, s->store, d, (uint32) e->part_id);
	int			nspans = sift_f2_granule_spans(s->o->rel, pin->part, &s->f, &s->ks, &s->plan, spans);

	sift_f2_pin_release(pin);
	if (u->out->nrefs + nspans > u->cap)
		u->out->refs = repalloc(u->out->refs, (u->cap = Max(2 * u->cap, u->out->nrefs + nspans)) * sizeof(SiftParPartRef));
	for (int i = 0; i < nspans; i++)
	{
		ref.g_base = spans[i][0];
		ref.ngranules = spans[i][1] - spans[i][0];
		u->out->refs[u->out->nrefs++] = ref;
	}
}

void
sift_f2_store_par_plan(SiftStoreScan *s, SiftF2ParPlan *out)
{
	UnitList	u = {.out = out,.cap = 16};
	int			nstaged;

	*out = (SiftF2ParPlan) {.refs = palloc(u.cap * sizeof(SiftParPartRef))};
	if (s->view == NULL)
		s->view = s->o->plan != NULL ? s->o->plan : sift_f2_parallel_view(s->o->rel, s->store, s->cx);
	for (int i = 0; s->possible && i < s->view->npartitions; i++)
	{
		const SiftPartitionEntry *e = &s->view->partitions[i];
		bool		dirty[SIFT_MANIFEST_TIME_COLS] = {false};

		partition_dirty(s, e, dirty);
		if (!partition_in_window(s, e, dirty))
			continue;
		for (int j = 0; j < s->view->partlists[i].nparts; j++)
			if (part_wanted(s, &s->view->partlists[i].parts[j], dirty))
				add_part_units(s, &u, e, &s->view->partlists[i].parts[j]);
	}
	out->staged_dead = sift_stage_dead(s->o->rel, s->o->snap, &nstaged);
	out->nstaged = (uint32) nstaged;
}

void
sift_f2_store_scan_morsel_begin(SiftStoreScan *s, const uint64 *staged_dead, uint32 nstaged)
{
	s->staged_px = sift_f2_staged_index(staged_dead, (int) nstaged);
	s->morsel = MemoryContextAllocZero(s->cx, sizeof(MorselPart));
}

static SiftPartitionEntry
entry_of(const SiftParPartRef *ref)
{
	return (SiftPartitionEntry) {.part_id = ref->part_id,.l0_slab_blk = ref->l0_slab_blk,.pend_del_blk = ref->pend_del_blk,.flags = ref->entry_flags,.manifest_gen = ref->manifest_gen,
	.manifest_blk = ref->manifest_blk};
}

/* The part of the unit, opened once for the runs of it that follow each other; the pending deletes of a partition are read once for the parts of it */
static SiftStorePart *
morsel_part(SiftStoreScan *s, const SiftParPartRef *ref)
{
	MorselPart *m = s->morsel;

	if (m->part != NULL && m->part->r != NULL && m->d.part_gen == ref->desc.part_gen && m->e.part_id == ref->part_id)
		return m->part;
	if (m->part != NULL)
		part_release_pin(m->part);
	if (!m->have_px || m->e.part_id != ref->part_id)
	{
		sift_pdel_idx_release(m->px, s->staged_px);
		m->e = entry_of(ref);
		m->px = table_pending(s, &m->e);
		m->have_px = true;
	}
	m->d = ref->desc;
	if (m->part == NULL)
		m->part = part_new(s, &m->e, &m->d, m->px);
	m->part->px = m->px;
	part_read_begin(m->part);
	m->part->pin = sift_f2_pin_open(s->o->rel, s->store, &m->d, (uint32) m->e.part_id);
	return m->part;
}

void
sift_f2_store_scan_morsel(SiftStoreScan *s, const SiftParPartRef *ref, uint32 first, uint32 n, SiftF2Sink sink, void *arg)
{
	MemoryContext old = MemoryContextSwitchTo(s->cx);
	SiftStorePart *p = morsel_part(s, ref);
	SiftF2Batch b;

	for (uint32 g = ref->g_base + first; g < ref->g_base + first + n; g++)
	{
		p->r->gb = sift_f2_gbuf_load(s->o->rel, s->store, p->pin, g);
		if (s->delta != NULL)
			sift_col_delta_scan_attach(s->delta, p->e, p->r->gb);
		if (granule_batch(p, &b))
			sink(arg, &b);
		part_release_granule(p);
	}
	MemoryContextSwitchTo(old);
}

static void
morsel_end(SiftStoreScan *s)
{
	MorselPart *m = s->morsel;

	if (m->part != NULL)
		part_close(m->part);
	sift_pdel_idx_release(m->px, s->staged_px);
	*m = (MorselPart) {0};
}
