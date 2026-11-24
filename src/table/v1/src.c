/*
 * table/v1/src.c - the column source of SiftAgg over a sift_table (agg/colsrc.h, docs/COL-PLAN.md, docs/COL-TAM.md section 12).
 *
 * The leaves are the columns of the table (the layout of the column groups, table/v1/seg.c). The groups are read by the engine, one
 * source per column layout the segments were written for (a segment keeps its own after ALTER TABLE; its leaves are matched by attribute
 * number), the rows of the write buffers are fed to the source of the table's layout as in-memory batches, one per superblock, and the
 * rows the snapshot sees are the table's: a superblock without tombstones or write buffer rows is entirely visible, the others are filtered by
 * sift_tbl_filter_page like every other reader of the table does.
 *
 * This file: what the engine adds to the unit reader (table/v1/v1_tbl_reader.c): its tests of a begin, the filters and the provider table; the reads are in src_read.c.
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/bitmapset.h"
#include "pgstat.h"
#include "optimizer/optimizer.h"
#include "rewrite/rewriteHandler.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "core/catname.h"
#include "core/meta_peek.h"
#include "agg/pre.h"
#include "col/v1/src_sel.h"
#include "agg/wide.h"
#include "col/col.h"
#include "col/am.h"
#include "col/v1/pagg.h"
#include "col/v1/seg.h"
#include "table/v1/table_v1.h"

#include "src_int.h"

static bool
tbl_owns_index(Oid indexrelam)
{
	return false;
}

static bool
tbl_owns_table(Oid tablerelam)
{
	Oid			am = get_am_oid("sift_table", true);

	return OidIsValid(am) && tablerelam == am;
}

/* the layout keeps the tag of a type the table stores by size (money, time, enum, oid) as OTHER: an integer leaf is as wide as its column */
static bool
tbl_reads_table(Oid tableoid)
{
	Relation	rel = sift_relation_get(tableoid);
	uint16		major = sift_meta_resolve_major(rel);

	RelationClose(rel);
	return major == SIFT_FORMAT_MAJOR;
}

/* The text tests join the zone pruning of every begin (a segment that lacks the column has no bound for it), and filter the rows of a unit on the encoded strings */
static void
tbl_prune_str(void *impl, const SiftSrcStrTest *t, int nt)
{
	TblSource  *tb = impl;

	for (int k = 0; k < tb->rd.nparts; k++)
	{
		TblPart    *p = &tb->rd.part[k];
		TblPartX   *x = &tb->px[k];

		x->spred = MemoryContextAlloc(tb->rd.cxt, Min(nt, COL_MAX_STR_PRUNE) * sizeof(SiftColPred));
		x->nspred = 0;
		for (int i = 0; i < nt && x->nspred < COL_MAX_STR_PRUNE; i++)
			if (p->col[t[i].leaf] >= 0 && !tb->rd.lay->leaf[t[i].leaf].lossy)
				x->spred[x->nspred++] = (SiftColPred) {p->col[t[i].leaf], t[i].op, t[i].nvals, t[i].vals};
	}
}

static void
filter_str_op(const SiftColUnit *cu, SiftColSel *sel, void *pred)
{
	sift_col_filter(cu, pred, 1, sel);
}

static void
filter_elem_op(const SiftColUnit *cu, SiftColSel *sel, void *gp)
{
	sift_col_filter_group(cu, gp, sel);
}

/* a test of an element leaf keeps the rows with an element that passes (col_filter_str) */
static bool
tbl_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	TblSource  *tb = impl;
	TblPart    *p = unit->priv;
	int			parent = tb->rd.lay->leaf[t->leaf].parent;
	SiftColPred pred = {p->col[t->leaf], t->op, t->nvals, t->vals};

	if (p->col[t->leaf] < 0 || tb->rd.lay->leaf[t->leaf].lossy || (parent >= 0 && p->col[parent] < 0))
		return false;
	if (parent >= 0)
	{
		SiftColGroupPred gp = {.group = p->col[parent],.npreds = 1,.preds = &pred};

		sift_srcsel_stored(&tb->rd.sel, p->src, &p->unit, rows, filter_elem_op, &gp);
	}
	else
		sift_srcsel_stored(&tb->rd.sel, p->src, &p->unit, rows, filter_str_op, &pred);
	return true;
}

static void
tbl_begin_part(SiftV1Tbl *rt, int k, uint32 sb_lo, uint32 sb_hi)
{
	TblSource  *t = (TblSource *) rt;
	TblPart    *p = &rt->part[k];
	TblPartX   *x = &t->px[k];
	SiftColPred preds[SIFT_SRCZONE_MAX + COL_MAX_STR_PRUNE + SIFT_COLSRC_MAX_FILTERS];
	int			n = sift_srczone_preds(&p->zones, rt->cxt, p->col, rt->prune, rt->nprune, preds);

	for (int i = 0; i < x->nspred; i++)
		preds[n++] = x->spred[i];
	for (int i = 0; i < x->flt.n; i++)
		preds[n++] = x->flt.pred[i];
	sift_col_begin_zones(p->src, preds, n, p->zones.zone, p->zones.nzone, sb_lo, sb_hi);
}

static void *
tbl_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view, const SiftKeyRange *keys)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	TblSource  *t = palloc0(sizeof(TblSource));

	sift_v1_tbl_init(&t->rd, heap, snapshot, mcxt, view);
	t->rd.begin_part = tbl_begin_part;
	t->px = palloc0(t->rd.nparts * sizeof(TblPartX));
	sift_aggpre_init(&t->pre, mcxt);
	MemoryContextSwitchTo(old);
	return t;
}

static void
tbl_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	sift_v1_tbl_begin(impl, sb_lo, sb_hi, prune, nprune);
}
/* A filter is the source's when every segment layout holds its column as an integer leaf: a segment without the column would need the catalog's default tested */
static uint64
tbl_set_filters(void *impl, const SiftSrcFilter *f, int nf)
{
	TblSource  *t = impl;
	uint64		ok = 0;

	for (int k = 0; k < t->rd.nparts; k++)
		t->px[k].flt = (SiftAggFilters) {0};
	for (int i = 0; i < nf; i++)
	{
		bool		all = true;

		for (int k = 0; k < t->rd.nparts; k++)
			all &= t->rd.part[k].col[f[i].leaf] >= 0 && sift_aggfilt_int(&t->rd.part[k].schema->cols[t->rd.part[k].col[f[i].leaf]]);
		if (!all)
			continue;
		for (int k = 0; k < t->rd.nparts; k++)
			sift_aggfilt_add(&t->px[k].flt, t->rd.cxt, t->rd.part[k].col[f[i].leaf], &f[i]);
		ok |= UINT64CONST(1) << i;
	}
	return ok;
}

static bool
tbl_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	SiftAggFiltRun run = {&t->px[p - t->rd.part].flt, t->rd.lay, t->rd.tol};

	sift_srcsel_stored(&t->rd.sel, p->src, &p->unit, bits, sift_aggfilt_op, &run);
	return sift_srcsel_rows_left(bits, unit->nrow);
}

static const SiftSrcProvider tbl_provider = {
	.owns_index = tbl_owns_index,
	.owns_table = tbl_owns_table,
	.reads_table = tbl_reads_table,
	.layout = sift_v1_tbl_layout,
	.stats = sift_v1_tbl_stats,
	.view_size = sift_v1_tbl_view_size,
	.view_export = sift_v1_tbl_view_export,
	.open = tbl_open,
	.open_part = tbl_open,
	.close = sift_v1_tbl_close,
	.source_layout = sift_v1_tbl_source_layout,
	.nsb = sift_v1_tbl_nsb,
	.begin = tbl_begin,
	.next = sift_v1_tbl_next,
	.read = sift_v1_tbl_read,
	.live = sift_v1_tbl_live,
	.vet = sift_v1_tbl_vet,
	.vet_needed = sift_v1_tbl_vet_needed,
	.counters = tbl_counters,
	.select = sift_v1_tbl_select,
	.tids = sift_v1_tbl_tids,
	.zone = tbl_zone,
	.zone_range = tbl_zone_range,
	.read_rows = sift_v1_tbl_read_rows,
	.preagg = tbl_preagg,
	.unit_exact = tbl_unit_exact,
	.unit_dead = tbl_unit_dead,
	.whole = tbl_whole,
	.super_align = tbl_super_align,
	.super_span = tbl_super_span,
	.split = tbl_split,
	.unit_agg = tbl_unit_agg,
	.unit_hist = tbl_unit_hist,
	.unit_rollup_tuples = tbl_unit_rollup_tuples,
	.unit_rollup = tbl_unit_rollup,
	.unit_rollup_miss = tbl_unit_rollup_miss,
	.unit_presence = tbl_unit_presence,
	.run_info = tbl_run_info,
	.run_heads = tbl_run_heads,
	.run_read = tbl_run_read,
	.read_sel = sift_v1_tbl_read_sel,
	.narrow = tbl_narrow,
	.prune_str = tbl_prune_str,
	.filter_str = tbl_filter_str,
	.set_filters = tbl_set_filters,
	.filter = tbl_filter,
	.set_wide = tbl_set_wide,
	.filter_wide = tbl_filter_wide,
	.overflow = tbl_overflow,
	.tolerate_ovf = tbl_tolerate_ovf,
	.set_raw = tbl_set_raw,
	.filter_types = tbl_filter_types,
	.read_types = tbl_read_types,
	.read_jnull = tbl_read_jnull,
};

void
sift_tbl_src_register(void)
{
	sift_colsrc_register(&tbl_provider);
}

