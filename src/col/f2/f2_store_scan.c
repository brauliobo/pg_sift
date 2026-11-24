/*
 * col/f2/f2_store_scan.c - the scan of a sift_table for the aggregates and the other readers of columns (col/f2/f2_store_scan.h), and the granules of a sift_col index or a sift_table one part at a
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
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/f2.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_gbuf.h"
#include "col/f2/f2_marks.h"
#include "col/f2/f2_pred.h"
#include "col/f2/f2_store_schema.h"
#include "col/f2/f2_store_scan.h"
#include "col/f2/key_plan.h"
#include "col/f2/l0_stage.h"
#include "col/f2/pend_del.h"
#include "core/memtrack.h"
#include "core/tidbridge.h"
#include "table/f2/f2_blob.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_tbl.h"

typedef enum Verdict
{
	V_NO,
	V_YES,
	V_MAYBE
} Verdict;

/* What decides, per host, which rows of a granule are there for the snapshot: granule (when not NULL) is called when the granule is loaded, row for each of its rows */
typedef struct RowVis
{
	void		(*granule) (SiftStorePart *p);
	bool		(*row) (SiftStorePart *p, uint16 r);
} RowVis;

struct SiftStoreScan
{
	const SiftStoreScanOpts *o;
	SiftF2Store *store;
	const RowVis *vis;
	MemoryContext cx;
	SiftF2Rel	f;
	SiftF2KeySet ks;
	SiftKeyPlan plan;
	SiftF2TimeWin tw;
	SiftTblVis	tvis;			/* a sift_table */
	SiftColDeltaScan *delta;	/* a sift_table */
	const SiftPendDelIndex *staged_px;	/* a sift_col: the deletes of the transaction not yet in a pending delete, as a set */
	bool	   *want;
	SiftPredSet *psets;			/* the constants of each IN predicate, hashed when the list is long */
	int		   *check_cols;		/* the wanted leaves that are not NaN leaves: the ones whose unsure cells a table scan refuses */
	int			ncheck;
	bool		wide;
	bool		possible;
	const SiftPlanSnapshot *view;
	int			next;			/* the next partition of the view */
	List	   *run;			/* the SiftStorePart of the partitions of the run last given */
	List	   *pxs;			/* the SiftPendDelIndex of those partitions */
};

/* What a part holds while it is read: the run holds a SiftStorePart for every part of its partitions, only the one being read holds this */
typedef struct SiftPartRead
{
	MemoryContext gcx;
	SiftF2PartPin *pin;
	uint32		spans[SIFT_F2_MAX_RANGES][2];
	int			nspans;
	int			span;
	uint32		g;				/* the next granule of the span */
	SiftF2GranuleBuf *gb;
	const uint64 *tids;
	int		   *active;			/* the check_cols of the scan that carry tags in the granule loaded */
	int			nactive;
	ColView    *views;
	bool		pend_hit;
	bool		any_dead;
	uint64		dead[SIFT_DV_MASK_WORDS];
	uint64		live[SIFT_DV_MASK_WORDS];
	uint64		maybe[SIFT_DV_MASK_WORDS];
} SiftPartRead;

struct SiftStorePart
{
	SiftStoreScan *s;
	const SiftPartitionEntry *e;
	const SiftPartDesc *d;
	const SiftPendDelIndex *px;
	MemoryContext cx;
	SiftPartRead *r;
	bool		started;
};

/* ---- the predicates on the stored values ---- */

static Verdict
pred_row(const SiftColDef *d, const SiftColPred *p, SiftPredSet *ps, const ColView *cv, int r)
{
	bool		isnull = cv->isnull[r];

	if (isnull && cv->tag != NULL && cv->tag[r] != 0)
		return V_MAYBE;
	if (p->op == SIFT_COLOP_ISNULL)
		return isnull ? V_YES : V_NO;
	if (p->op == SIFT_COLOP_NOTNULL)
		return isnull ? V_NO : V_YES;
	if (isnull)
		return V_NO;
	if (sift_f2_undecided(d, p))
		return V_MAYBE;
	if (cv->kind == SIFT_CELL_STR)
		return sift_f2_str_pred_in(ps, p, cv->s[r], cv->slen[r]) ? V_YES : V_NO;
	if (p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_NOTIN)
		return (cv->kind == SIFT_CELL_INT ? sift_f2_in_i64(ps, p, cv->i[r]) : sift_f2_in_f8(ps, p, cv->f[r])) == (p->op == SIFT_COLOP_IN) ? V_YES : V_NO;
	return sift_f2_cmp_op(p->op, cv->kind == SIFT_CELL_INT ? (cv->i[r] < p->vals[0].v.i ? -1 : cv->i[r] > p->vals[0].v.i) : sift_f2_f8_cmp(cv->f[r], p->vals[0].v.f)) ? V_YES : V_NO;
}

static Verdict
row_verdict(const SiftStorePart *p, int r)
{
	const SiftStoreScan *s = p->s;
	Verdict		all = V_YES;

	for (int k = 0; k < s->o->npreds; k++)
	{
		const SiftColPred *pr = &s->o->preds[k];
		Verdict		v = pred_row(&s->store->schema->cols[pr->col], pr, &s->psets[k], &p->r->views[pr->col], r);

		if (v == V_NO)
			return V_NO;
		all = v == V_MAYBE ? V_MAYBE : all;
	}
	return all;
}

/* ---- the columns of a granule ---- */

static ColView
constant_view(SiftStorePart *p, int leaf, const SiftF2ColMap *map)
{
	SiftF2GranuleBuf *gb = p->r->gb;
	int			n = gb->nrows;
	int16		attno = p->s->store->leaf_attno[leaf];
	SiftCellKind kind = p->s->store->kinds[leaf];
	ColView		v = {.kind = kind};
	bool	   *isnull = MemoryContextAlloc(p->r->gcx, n);
	int64	   *ints = kind == SIFT_CELL_INT ? MemoryContextAlloc(p->r->gcx, sizeof(int64) * n) : NULL;
	double	   *fs = kind == SIFT_CELL_F8 ? MemoryContextAlloc(p->r->gcx, sizeof(double) * n) : NULL;
	const char **ss = kind == SIFT_CELL_STR ? MemoryContextAlloc(p->r->gcx, sizeof(char *) * n) : NULL;
	uint32	   *lens = kind == SIFT_CELL_STR ? MemoryContextAlloc(p->r->gcx, sizeof(uint32) * n) : NULL;

	for (int r = 0; r < n; r++)
	{
		const SiftColDeltaPatch *patch = gb->patch != NULL ? sift_col_delta_find_row(gb->patch_arg, attno, (uint16) r) : NULL;
		SiftCell	c = map->missing_cell[leaf];

		if (patch != NULL)
		{
			c.isnull = (patch->flags & SIFT_CDCOL_IS_NULL) != 0;
			c.i = patch->val.i;
			c.f = patch->val.f;
			c.s = patch->val.s;
			c.slen = patch->vlen;
		}
		isnull[r] = c.isnull;
		if (ints != NULL)
			ints[r] = c.i;
		if (fs != NULL)
			fs[r] = c.f;
		if (ss != NULL)
		{
			ss[r] = c.s;
			lens[r] = c.slen;
		}
	}
	v.isnull = isnull;
	v.i = ints;
	v.f = fs;
	v.s = ss;
	v.slen = lens;
	return v;
}

static ColView
decoded_view(SiftStorePart *p, int leaf, int phys, SiftCellKind kind)
{
	const SiftStoreScan *s = p->s;
	SiftF2GranuleBuf *gb = p->r->gb;
	ColView		v = {.kind = kind};

	if (!gb->col_decoded[phys])
		sift_f2_gbuf_decode(gb, phys, kind, s->wide || s->store->hints[leaf] == SIFT_CHINT_NUM64);
	v.isnull = gb->col_isnull[phys];
	v.tag = gb->col_tag[phys];
	v.i = gb->col_i[phys];
	v.f = gb->col_f[phys];
	if (kind == SIFT_CELL_STR)
	{
		const char **ss = MemoryContextAlloc(p->r->gcx, sizeof(char *) * gb->nrows);	/* check-memory: allow the rows of one granule (SIFT_GRANULE_ROWS at most) */
		uint32	   *lens = MemoryContextAlloc(p->r->gcx, sizeof(uint32) * gb->nrows);
		bool	   *nulls = memcpy(MemoryContextAlloc(p->r->gcx, gb->nrows), gb->col_isnull[phys], gb->nrows);
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(s->o->rel), s->store->leaf_attno[leaf] - 1);

		for (int r = 0; r < gb->nrows; r++)
		{
			const SiftStrCell *c = &gb->col_s[phys][r];

			if (gb->col_tag[phys] != NULL && gb->col_tag[phys][r] == SIFT_COLTAG_WIDE_BLOB)
			{
				uint32		len;
				const char *ref = sift_granule_exc_payload(&gb->view, phys, r, &len);
				struct varlena *val = (struct varlena *) DatumGetPointer(s->store->host->read_wide_blob(s->o->rel, ref, len, att->atttypid, att->attlen, p->r->gcx));

				ss[r] = VARDATA_ANY(val);
				lens[r] = VARSIZE_ANY_EXHDR(val);
				nulls[r] = false;
				continue;
			}
			ss[r] = c->p;
			lens[r] = c->len;
		}
		v.s = ss;
		v.slen = lens;
		v.isnull = nulls;
	}
	return v;
}

/* A numeric column that ALTER TABLE .. TYPE left unconstrained or wide keeps the parts written as scaled integers: their values are read as the numerics they hold, a NaN from its exception */
static ColView
widened_view(SiftStorePart *p, int leaf, int phys, uint8 scale)
{
	SiftF2GranuleBuf *gb = p->r->gb;
	int			n = gb->nrows;
	const char **ss = MemoryContextAlloc(p->r->gcx, sizeof(char *) * n);	/* check-memory: allow the rows of one granule (SIFT_GRANULE_ROWS at most) */
	uint32	   *lens = MemoryContextAlloc(p->r->gcx, sizeof(uint32) * n);
	bool	   *nulls = MemoryContextAlloc(p->r->gcx, n);
	MemoryContext old;
	ColView		v;

	if (!gb->col_decoded[phys])
		sift_f2_gbuf_decode(gb, phys, SIFT_CELL_INT, true);
	v = decoded_view(p, leaf, phys, SIFT_CELL_INT);
	old = MemoryContextSwitchTo(p->r->gcx);
	for (int r = 0; r < n; r++)
	{
		const bool	exc = v.tag != NULL && v.tag[r] != 0;

		nulls[r] = v.isnull[r] && !exc;
		ss[r] = NULL;
		lens[r] = 0;
		if (exc)
		{
			const char *payload = sift_granule_exc_payload(&gb->view, phys, r, &lens[r]);

			ss[r] = DatumGetPointer(sift_f2_num64_exception_datum(payload, lens[r], p->r->gcx));
		}
		else if (!nulls[r])
		{
			ss[r] = DatumGetPointer(sift_f2_num64_datum(v.i[r], scale));
			lens[r] = VARSIZE_ANY(ss[r]);
		}
	}
	MemoryContextSwitchTo(old);
	return (ColView) {.kind = SIFT_CELL_STR,.isnull = nulls,.s = ss,.slen = lens};
}

static void
load_views(SiftStorePart *p)
{
	const SiftStoreScan *s = p->s;
	const SiftF2ColMap *map = p->r->gb->pin->col_map;

	for (int leaf = 0; leaf < s->store->ncols; leaf++)
	{
		int			phys = map != NULL ? map->phys_col[leaf] : leaf;

		if (!s->want[leaf])
			continue;
		if (phys < 0)
			p->r->views[leaf] = constant_view(p, leaf, map);
		else if (map != NULL && map->phys_kind[leaf] == SIFT_CELL_INT && s->store->kinds[leaf] == SIFT_CELL_STR)
			p->r->views[leaf] = widened_view(p, leaf, phys, map->old_scale[leaf]);
		else
			p->r->views[leaf] = decoded_view(p, leaf, phys, s->store->kinds[leaf]);
	}
}

/* ---- the rows of a granule: what each host shows to the snapshot ---- */

static bool
table_row(SiftStorePart *p, uint16 r)
{
	return sift_tbl_f2_vis_row(&p->s->tvis, p->r->gb, r, p->px);
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
	load_views(p);
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

static void
part_open_pin(SiftStorePart *p)
{
	SiftStoreScan *s = p->s;

	p->cx = sift_mem_allocset_create(s->cx, "sift f2 store scan part");
	p->r = MemoryContextAllocZero(p->cx, sizeof(SiftPartRead));
	p->r->pin = sift_f2_pin_open(s->o->rel, s->store, p->d, (uint32) p->e->part_id);
	p->r->nspans = sift_f2_granule_spans(s->o->rel, p->r->pin->part, &s->f, &s->ks, &s->plan, p->r->spans);
	p->r->g = p->r->nspans > 0 ? p->r->spans[0][0] : 0;
	p->r->gcx = sift_mem_allocset_create(p->cx, "sift f2 store scan granule");
	p->r->views = MemoryContextAllocZero(p->cx, sizeof(ColView) * s->store->ncols);
	p->r->active = MemoryContextAlloc(p->cx, sizeof(int) * Max(s->ncheck, 1));
	p->started = true;
}

static void
part_release_pin(SiftStorePart *p)
{
	if (p->r == NULL)
		return;
	sift_f2_pin_release(p->r->pin);
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
	const SiftTidIndexEntry *t = &p->r->pin->tidx[g];
	bool		wanted = o->bridge == NULL || sift_tidbridge_granule_may_match(o->bridge, t->tid_min, t->tid_max, t->blk_bloom);

	if (o->bridge_seen != NULL)
		(*o->bridge_seen)++;
	if (!wanted && o->bridge_skipped != NULL)
		(*o->bridge_skipped)++;
	return wanted;
}

static bool
part_load_granule(SiftStorePart *p)
{
	const SiftStoreScan *s = p->s;
	uint32		g;

	if (!p->started)
		part_open_pin(p);
	while (p->r != NULL && part_next_granule_id(p, &g))
		if (bridge_wants(p, g))
		{
			p->r->gb = sift_f2_gbuf_load(s->o->rel, s->store, p->r->pin, g);
			if (s->delta != NULL)
				sift_col_delta_scan_attach(s->delta, p->e, p->r->gb);
			return true;
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
	part_release_granule(p);
	part_release_pin(p);
	pfree(p);
}

static SiftStorePart *
part_new(SiftStoreScan *s, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px)
{
	SiftStorePart *p = MemoryContextAllocZero(s->cx, sizeof(SiftStorePart));

	p->s = s;
	p->e = e;
	p->d = d;
	p->px = px;
	return p;
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
	while (s->run == NIL && s->possible && s->next < s->view->npartitions)
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

SiftStoreScan *
sift_f2_store_scan_open(const SiftStoreScanOpts *o)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 store scan");
	SiftStoreScan *s = MemoryContextAllocZero(cx, sizeof(SiftStoreScan));
	bool		table = o->store->host->host_kind == SIFT_F2_HOST_TABLE;

	s->o = o;
	s->store = o->store;
	s->cx = cx;
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
		if (s->want[leaf] && !(o->nan_leaves != NULL && o->nan_leaves[leaf]))
			s->check_cols[s->ncheck++] = leaf;
	s->possible = sift_f2_key_set(&s->f, o->preds, o->npreds, &s->ks) && sift_f2_time_window(&s->f, o->preds, o->npreds, &s->tw);
	sift_f2_key_plan(&s->f, o->store->schema->cols, o->preds, o->npreds, &s->plan);
	s->possible = s->possible && s->plan.state != SIFT_KEY_PLAN_EMPTY;
	if (!s->possible)
		return s;
	s->view = o->plan != NULL ? o->plan : sift_f2_view(o->rel, o->store, cx);
	if (table)
	{
		sift_tbl_f2_vis_begin(&s->tvis, o->rel, o->snap);
		s->delta = sift_col_delta_scan_begin(o->rel, &s->tvis, cx);
	}
	else
	{
		int			nstaged;
		uint64	   *staged = sift_stage_dead(o->rel, o->snap, &nstaged);

		s->staged_px = sift_f2_staged_index(staged, nstaged);
	}
	return s;
}

void
sift_f2_store_scan_close(SiftStoreScan *s)
{
	run_reset(s);
	MemoryContextDelete(s->cx);
}

static void
scan_table(const SiftStoreScanOpts *o, SiftF2Sink sink, void *arg)
{
	SiftStoreScan *s = sift_f2_store_scan_open(o);
	SiftF2Batch b;

	for (List *run; (run = sift_f2_store_scan_next_run(s, false)) != NIL;)
		foreach_ptr(SiftStorePart, p, run)
			while (sift_f2_store_part_next(p, &b))
				sink(arg, &b);
	sift_f2_store_scan_close(s);
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
