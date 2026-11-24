/*
 * agg/col.c - the column source of the aggregate scan over a sift_col index: the leaf list becomes the layout, a unit's selection-based reads become whole-unit vectors of the
 * types the kernels take (the engine decodes a chunk straight into the lane of the leaf's vector type, NULL bitmaps expanded to flags),
 * liveness and MVCC vetting are the engine's. Compiled only when the engine is part of the build.
 */
#include "postgres.h"

#include "agg/agg.h"

#if __has_include("col/col.h")

#include "access/table.h"
#include "pgstat.h"
#include "utils/rel.h"

#include "agg/pre.h"
#include "col/v1/src_sel.h"
#include "agg/wide.h"
#include "core/store.h"
#include "core/visibility.h"
#include "col/am.h"
#include "col/col.h"
#include "col/v1/pagg.h"
#include "col/pg.h"
#include "col/v1/runs.h"

#include "agg/col_int.h"

static bool
col_owns_index(Oid indexrelam)
{
	return OidIsValid(indexrelam) && indexrelam == sift_col_am_oid();
}

static bool
col_reads_index(Oid indexoid)
{
	return sift_col_index_oid_engine(indexoid) == SIFT_FORMAT_MAJOR;
}

static bool
col_owns_table(Oid tablerelam)
{
	return false;
}

static void
col_stamp(Relation index, SiftSrcStamp *st)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	st->file = index->rd_locator.relNumber;
	st->options = sift_col_pagg_hash(index);
	st->version = sift_dir_version(index);
	st->pages = RelationGetNumberOfBlocks(heap);
	sift_vis_count(heap, st->pages, &st->visible, NULL);
	table_close(heap, AccessShareLock);
}

void *
col_open_flags(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags, const void *view, const SiftKeyRange *keys)
{
	ColSource  *src = MemoryContextAllocZero(mcxt, sizeof(ColSource));
	MemoryContext old = MemoryContextSwitchTo(mcxt);

	sift_v1_idx_init(&src->rd, heap, index, snapshot, mcxt, flags, view, keys);
	src->rd.wide_dict = col_wide_dict;
	src->numsum = palloc0(sizeof(bool) * src->rd.lay->nleaves);
	sift_aggpre_init(&src->pre, mcxt);
	MemoryContextSwitchTo(old);
	return src;
}

static void *
col_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view, const SiftKeyRange *keys)
{
	return col_open_flags(heap, index, snapshot, mcxt, SIFT_COLO_NOACL | SIFT_COLO_POINTS | SIFT_COLO_POINT_ROWS, view, keys);
}

/* a partition read through its partitioned table: the privileges and row security of the table were checked, not those of the partition */
static void *
col_open_part(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view, const SiftKeyRange *keys)
{
	ColSource  *src = col_open_flags(heap, index, snapshot, mcxt, SIFT_COLO_NOCHECKS | SIFT_COLO_SNAP_HELD | SIFT_COLO_POINTS | SIFT_COLO_POINT_ROWS, view, NULL);

	/* the open left the header page of each segment pinned, and a scan of thousands of partitions opens them all before it reads one */
	sift_col_unpin(src->rd.cs);
	return src;
}

static void
col_close(void *impl)
{
	ColSource  *src = impl;

	if (src->rck)
		recheck_close(src->rck);
	sift_v1_idx_close(&src->rd);
}

static void
col_unpin(void *impl)
{
	ColSource  *src = impl;

	if (src->rck)
		recheck_unpin(src->rck);
	sift_col_unpin(src->rd.cs);
}

static bool
col_sb_heap(void *impl)
{
	return sift_col_sb_heap(((ColSource *) impl)->rd.cs);
}

#define COL_PREDS_MAX	(2 * SIFT_COLSRC_MAX_LEAVES + COL_MAX_STR_PRUNE + SIFT_COLSRC_MAX_FILTERS + COL_MAX_WIDE)

/* The predicates a begin over the prune entries tests (the zone trees and the tests behind them are made once for the entries) */
static int
col_preds(ColSource *src, const SiftSrcPrune *prune, int nprune, SiftColPred *preds)
{
	int			n = sift_aggzone_preds(&src->zones, src->rd.cxt, NULL, prune, nprune, preds);

	src->spoff = n;
	for (int i = 0; i < src->nspred; i++)
		preds[n++] = src->spred[i];
	for (int i = 0; i < src->flt.n; i++)
		preds[n++] = src->flt.pred[i];
	for (int i = 0; i < src->nwpred; i++)
		preds[n++] = src->wpred[i];
	return n;
}

static void
col_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	ColSource  *src = impl;
	SiftColPred	preds[COL_PREDS_MAX];
	int			n;

	if (src->rck)
		src->rck->taken = src->rck->active = false, src->rck->npend = 0;
	n = col_preds(src, prune, nprune, preds);
	sift_col_begin_zones(src->rd.cs, preds, n, src->zones.zone, src->zones.nzone, sb_lo, sb_hi);
}

static bool
col_kept(void *impl, const SiftSrcPrune *prune, int nprune, uint32 nspace, uint64 *kept, uint64 *rows, bool count)
{
	ColSource  *src = impl;
	SiftColPred	preds[COL_PREDS_MAX];
	int			n = col_preds(src, prune, nprune, preds);

	return sift_col_kept_map(src->rd.cs, preds, n, src->zones.zone, src->zones.nzone, nspace, kept, rows, count);
}

/* the tests of prune_str whose rows the point directory of the unit answers exactly */
static uint64
col_point_exact(void *impl, const SiftSrcUnit *unit)
{
	ColSource  *src = impl;
	uint64		bits = sift_col_point_exact(&src->rd.unit),
				mask = 0;

	for (int i = 0; bits != 0 && i < src->nspred; i++)
		if (src->spoff + i < 64 && (bits >> (src->spoff + i) & 1))
			mask |= UINT64CONST(1) << src->spmap[i];
	return mask;
}

static bool
col_next(void *impl, SiftSrcUnit *unit)
{
	ColSource  *src = impl;

	src->rd.sel.compact = false;
	if (src->rck)
	{
		bool		again = src->rck->taken && recheck_pending(src);

		src->rck->active = false;
		src->rck->taken = false;
		if (again && recheck_next(src, unit))
			return true;
	}
	return sift_v1_idx_next(&src->rd, unit);
}

/* The rows of bits are few: the unit goes on as them alone. Not the unit of rows read again from the heap, which has no stored numbering to go back to. */
static bool
col_narrow(void *impl, SiftSrcUnit *unit, const uint64 *bits)
{
	ColSource  *src = impl;
	uint32		nrow;

	if (src->rd.sel.compact || (src->rck && src->rck->active))
		return false;
	sift_srcsel_narrow(&src->rd.sel, src->rd.cs, &src->rd.unit, bits, &nrow);
	unit->nrow = nrow;
	return true;
}

static bool
col_whole(void *impl, int leaf, const int *meas, int nmeas)
{
	ColSource  *src = impl;

	/* a selection of heap addresses (a semi-join's) is the rows of units, not of a segment */
	if (src->rd.sel.keep == NULL)
		sift_col_super_whole(src->rd.cs, leaf, meas, nmeas);
	return false;
}

static void
col_count_range(void *impl, int leaf, SiftColOp op, const char *s, uint32 len)
{
	ColSource  *src = impl;

	/* a selection of heap addresses (a semi-join's) is the rows of units, not of a segment */
	if (src->rd.sel.keep == NULL && (op == SIFT_COLOP_EQ || (op >= SIFT_COLOP_LT && op <= SIFT_COLOP_GE) || op == SIFT_COLOP_PREFIX))
		sift_col_count_range(src->rd.cs, leaf, op, s, len);
}

static void
col_count_cmp(void *impl, int leaf)
{
	ColSource  *src = impl;

	if (src->rd.sel.keep == NULL)
		sift_col_count_cmp(src->rd.cs, leaf);
}

static void
col_count_strs(void *impl, int leaf, uint32 collid, const char *lo, uint32 lolen, bool lo_excl, const char *hi, uint32 hilen, bool hi_excl)
{
	ColSource  *src = impl;

	if (src->rd.sel.keep == NULL)
		sift_col_count_strs(src->rd.cs, leaf, collid, lo, lolen, lo_excl, hi, hilen, hi_excl);
}

static void
col_count_elem_src(void *impl)
{
	ColSource  *src = impl;

	if (src->rd.sel.keep == NULL)
		sift_col_count_elem(src->rd.cs);
}

static bool
col_unit_count(void *impl, const SiftSrcUnit *unit, int64 *rows)
{
	uint64		n;

	if (!sift_col_unit_counted(&((ColSource *) impl)->rd.unit, &n))
		return false;
	*rows = (int64) n;
	return true;
}

static uint32
col_super_span(void *impl)
{
	return sift_col_super_span(((ColSource *) impl)->rd.cs);
}

static void
col_split(void *impl, const SiftSrcUnit *unit)
{
	ColSource  *src = impl;

	sift_col_unit_split(src->rd.cs, &src->rd.unit);
}

static void
col_zone(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z)
{
	SiftColZone cz;

	sift_col_zone(&((ColSource *) impl)->rd.unit, leaf, &cz);
	*z = (SiftSrcZone) {.min = cz.min, .max = cz.max, .nvals = cz.nvals, .nnull = cz.nnull, .inexact = cz.has_ovf};
}

static uint32
col_zone_range(void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z)
{
	SiftColZone cz;
	uint32		n = sift_col_zone_range(((ColSource *) impl)->rd.cs, leaf, lo, hi, &cz);

	*z = (SiftSrcZone) {.min = cz.min, .max = cz.max, .nvals = cz.nvals, .nnull = cz.nnull, .inexact = cz.has_ovf};
	return n;
}

static uint32
col_zone_range_segs(void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z)
{
	SiftColZone cz;
	uint32		n = sift_col_zone_range_segs(((ColSource *) impl)->rd.cs, leaf, lo, hi, &cz);

	*z = (SiftSrcZone) {.min = cz.min, .max = cz.max, .nvals = cz.nvals, .nnull = cz.nnull, .inexact = cz.has_ovf};
	return n;
}

static bool
col_walk_stamp(void *impl, SiftSrcWalkStamp *st, uint32 *mem, int cap, int *nmem)
{
	SiftColWalkStamp cs;

	if (!sift_col_walk_stamp(((ColSource *) impl)->rd.cs, &cs, mem, cap, nmem))
		return false;
	*st = (SiftSrcWalkStamp) {.index = cs.index,.file = cs.file,.version = cs.version,.scope = cs.scope};
	return true;
}

/* the stored numbering: not a unit of a match set, of rows read again from the heap, or narrowed to a few rows */
static bool
col_run_info(void *impl, const SiftSrcUnit *unit, int leaf, bool walk, uint32 *nvals, uint32 *nnull, uint64 *id)
{
	ColSource  *src = impl;

	return !(src->rck && src->rck->active) && !sift_v1_idx_wide(&src->rd, leaf) && sift_aggpre_run_info(&src->rd.sel, unit, &src->rd.unit, leaf, nvals, nnull, id);
}

static int
col_key_order(void *impl, const SiftSrcUnit *unit, int leaf)
{
	ColSource  *src = impl;

	return src->rd.sel.compact || src->rd.sel.keep || (src->rck && src->rck->active) || sift_v1_idx_wide(&src->rd, leaf) || unit->nrow != src->rd.unit.nrows ? 0 : sift_col_key_order(&src->rd.unit, leaf);
}

static bool
col_sb_heads(void *impl, uint32 sb, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi)
{
	ColSource  *src = impl;
	SiftCHeadEnt e[2 * SIFT_CRUN_HEAD_MAX];

	if (src->rd.sel.compact || src->rd.sel.keep || (src->rck && src->rck->active) || sift_v1_idx_wide(&src->rd, leaf) || !sift_col_sb_heads(src->rd.cs, sb, leaf, e, nlo, nhi))
		return false;
	memcpy(lo, e, *nlo * sizeof(SiftSrcHead));
	memcpy(hi, e + *nlo, *nhi * sizeof(SiftSrcHead));
	return true;
}

static int
col_run_heads(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi)
{
	return sift_aggpre_run_heads(&((ColSource *) impl)->rd.unit, leaf, lo, nlo, hi, nhi);
}

static int
col_gorder_open(void *impl, int leaf, int rows, int npin, const int *pinleaf, const int64 *pinval, int *grouped, uint32 collid, const struct SiftCGOrdWant *want)
{
	ColSource  *src = impl;

	return sift_col_gorder_open(src->rd.cs, leaf, rows, npin, pinleaf, pinval, grouped, collid, want);
}

static bool
col_gorder_cuts(void *impl, int i, int leaf, struct SiftCGOrdCuts *cuts)
{
	return sift_col_gorder_cuts(((ColSource *) impl)->rd.cs, i, leaf, cuts);
}

static uint32
col_gorder_strrank(void *impl, int i, int leaf, const char *s, uint32 len, bool after)
{
	return sift_col_gorder_strrank(((ColSource *) impl)->rd.cs, i, leaf, s, len, after);
}

static const char *
col_gorder_str(void *impl, int i, int leaf, int64 key, uint32 *len)
{
	return sift_col_gorder_str(((ColSource *) impl)->rd.cs, i, leaf, key, len);
}

static bool
col_gorder_info(void *impl, int i, int leaf, uint32 *entries, uint32 *values)
{
	return sift_col_gorder_info(((ColSource *) impl)->rd.cs, i, leaf, entries, values);
}

static void
col_gorder_range(void *impl, int i, int leaf, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne)
{
	sift_col_gorder_range(((ColSource *) impl)->rd.cs, i, leaf, vs, ve, ns, ne);
}

static uint32
col_gorder_seek(void *impl, int i, int leaf, uint32 vs, uint32 ve, int64 key, bool after, struct SiftCGOrdBuf *keep)
{
	return sift_col_gorder_seek(((ColSource *) impl)->rd.cs, i, leaf, vs, ve, key, after, keep);
}

static bool
col_gorder_before(void *impl, int i, int leaf, uint32 vs, uint32 ve, int64 v, bool desc, uint32 *clo, uint32 *chi, struct SiftCGOrdBuf *memo)
{
	return sift_col_gorder_before(((ColSource *) impl)->rd.cs, i, leaf, vs, ve, v, desc, clo, chi, memo);
}

static int
col_gorder_exact(void *impl, int i, SiftKey tid)
{
	return sift_col_gorder_exact(((ColSource *) impl)->rd.cs, i, tid);
}

static void
col_gorder_values(void *impl, int leaf, const int *seg, const SiftKey *tid, uint32 n, int64 *val, uint8 *st)
{
	sift_col_gorder_values(((ColSource *) impl)->rd.cs, leaf, seg, tid, n, val, st);
}

static void
col_gorder_head(void *impl, int i, int leaf, uint32 p, uint32 w, struct SiftCGOrdBuf *buf)
{
	sift_col_gorder_head(((ColSource *) impl)->rd.cs, i, leaf, p, w, buf);
}

static void
col_run_read(void *impl, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows)
{
	sift_col_run_read(&((ColSource *) impl)->rd.unit, leaf, pos, cnt, rows);
}

bool
col_overflow(void *impl, const SiftSrcUnit *unit, int leaf)
{
	SiftColZone cz;

	if (sift_v1_idx_wide(impl, leaf) || ((ColSource *) impl)->rd.unit.nsu > 0)
		return false;				/* a NaN is a dictionary entry; a superunit holds no overflow value */
	sift_col_zone(&((ColSource *) impl)->rd.unit, leaf, &cz);
	return cz.has_ovf;
}

/* ---- pre-aggregates (src/col/v1/pagg.c, docs/COL-PREAGG.md) ---- */

static void
col_preagg(Relation index, SiftSrcPreagg *pa)
{
	SiftCPaggSpec spec;

	sift_col_pagg_spec(index, &spec);
	sift_aggpre_spec(&spec, pa);
}

static bool
col_unit_exact(void *impl, const SiftSrcUnit *unit)
{
	SiftColSegment *seg;

	/* a whole segment answers from its key table only (sift_col_unit_hist): every other question of it is refused */
	return sift_col_unit_whole(&((ColSource *) impl)->rd.unit, &seg) || sift_col_unit_preagg_ok(&((ColSource *) impl)->rd.unit);
}

static int
col_unit_dead(void *impl, const SiftSrcUnit *unit, int mode)
{
	if (mode == SSD_NET)
		return sift_col_unit_net(&((ColSource *) impl)->rd.unit);
	return sift_col_unit_dead(&((ColSource *) impl)->rd.unit, mode == SSD_OFF ? -1 : sift_agg_dead_delta, mode == SSD_BOUNDS);
}

static bool
col_unit_except(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_col_unit_except(&((ColSource *) impl)->rd.unit, leaf);
}

static bool
col_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	ColSource  *src = impl;

	return sift_aggpre_unit_agg(&src->rd.unit, leaf, sift_col_def(src->rd.cs, leaf), agg);
}

static bool
col_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, const SiftSrcRollupNeed *need, SiftSrcHist *h)
{
	ColSource  *src = impl;

	return sift_aggpre_unit_hist(&src->pre, &src->rd.unit, dim, sift_col_def(src->rd.cs, dim % SIFT_SRC_CAL), NULL, NULL, need, h);
}

static bool
col_unit_presence(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p)
{
	return sift_aggpre_unit_presence(&((ColSource *) impl)->rd.unit, leaf, p);
}

static bool
col_unit_hll(void *impl, const SiftSrcUnit *unit, int leaf, const uint8 **blob)
{
	return sift_col_unit_hll(&((ColSource *) impl)->rd.unit, leaf, blob);
}

static uint32
col_unit_rollup_tuples(void *impl, const SiftSrcUnit *unit, int list)
{
	return sift_col_unit_rollup_tuples(&((ColSource *) impl)->rd.unit, list);
}

static bool
col_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *h)
{
	ColSource  *src = impl;

	return sift_aggpre_unit_rollup(&src->pre, &src->rd.unit, sift_col_schema(src->rd.cs), list, need, NULL, NULL, h);
}

static bool
col_unit_rollup_miss(void *impl, const SiftSrcUnit *unit)
{
	return sift_col_unit_rollup_miss(&((ColSource *) impl)->rd.unit);
}

static const SiftSrcProvider col_provider = {
	.owns_index = col_owns_index,
	.reads_index = col_reads_index,
	.owns_table = col_owns_table,
	.layout = sift_v1_idx_layout,
	.stats = sift_v1_idx_stats,
	.scope_stats = sift_col_scope_stats,
	.miss = sift_col_miss,
	.stamp = col_stamp,
	.open = col_open,
	.key_range = col_key_range,
	.key_leaf = col_key_leaf,
	.open_part = col_open_part,
	.open_scope = sift_col_open_scope,
	.scope_delta = sift_col_open_scope_delta,
	.view_size = col_view_size,
	.view_export = col_view_export,
	.part_count = sift_col_count_view,
	.view_sb_heap = sift_col_view_sb_heap,
	.conn_proof = sift_col_conn_proof,
	.close = col_close,
	.unpin = col_unpin,
	.source_layout = sift_v1_idx_source_layout,
	.nsb = sift_v1_idx_nsb,
	.rescan = col_rescan,
	.begin = col_begin,
	.kept = col_kept,
	.next = col_next,
	.super = col_super,
	.whole = col_whole,
	.count_range = col_count_range,
	.count_cmp = col_count_cmp,
	.count_strs = col_count_strs,
	.count_elem = col_count_elem_src,
	.unit_count = col_unit_count,
	.point_exact = col_point_exact,
	.split = col_split,
	.super_align = col_super_align,
	.super_tests = col_super_tests,
	.super_span = col_super_span,
	.read = sift_v1_idx_read,
	.live = col_live,
	.vet = sift_v1_idx_vet,
	.vet_needed = sift_v1_idx_vet_needed,
	.counters = col_counters,
	.leaf_n = sift_v1_idx_leaf_n,
	.read_into = sift_v1_idx_read_into,
	.select = sift_v1_idx_select,
	.tids = sift_v1_idx_tids,
	.sb_heap = col_sb_heap,
	.zone = col_zone,
	.zone_range = col_zone_range,
	.zone_range_segs = col_zone_range_segs,
	.walk_stamp = col_walk_stamp,
	.overflow = col_overflow,
	.run_info = col_run_info,
	.run_heads = col_run_heads,
	.key_order = col_key_order,
	.sb_heads = col_sb_heads,
	.run_read = col_run_read,
	.gorder_open = col_gorder_open,
	.gorder_strrank = col_gorder_strrank,
	.gorder_str = col_gorder_str,
	.gorder_info = col_gorder_info,
	.gorder_compact = col_gorder_compact,
	.gorder_cuts = col_gorder_cuts,
	.gorder_before = col_gorder_before,
	.gorder_exact = col_gorder_exact,
	.gorder_clean = col_gorder_clean,
	.gorder_groups_open = col_gorder_groups_open,
	.gorder_groups_next = col_gorder_groups_next,
	.gorder_head = col_gorder_head,
	.gorder_values = col_gorder_values,
	.gorder_range = col_gorder_range,
	.gorder_groups = col_gorder_groups,
	.gorder_group_at = col_gorder_group_at,
	.gorder_seek = col_gorder_seek,
	.read_rows = sift_v1_idx_read_rows,
	.set_filters = col_set_filters,
	.filter = col_filter,
	.read_sel = sift_v1_idx_read_sel,
	.narrow = col_narrow,
	.filter_types = col_filter_types,
	.tolerate_ovf = col_tolerate_ovf,
	.set_raw = col_set_raw,
	.set_numsum = col_set_numsum,
	.set_recheck = col_set_recheck,
	.overflow_raise = col_overflow_raise,
	.recheck_rows = col_recheck_rows,
	.read_types = col_read_types,
	.read_jnull = col_read_jnull,
	.prune_str = col_prune_str,
	.filter_str = col_filter_str,
	.set_wide = col_set_wide,
	.filter_wide = col_filter_wide,
	.preagg = col_preagg,
	.unit_exact = col_unit_exact,
	.unit_dead = col_unit_dead,
	.unit_except = col_unit_except,
	.unit_agg = col_unit_agg,
	.unit_hist = col_unit_hist,
	.unit_presence = col_unit_presence,
	.unit_dset = col_unit_dset,
	.unit_hll = col_unit_hll,
	.unit_rollup_tuples = col_unit_rollup_tuples,
	.unit_rollup = col_unit_rollup,
	.unit_rollup_miss = col_unit_rollup_miss,
};

void
sift_aggcol_register(void)
{
	sift_colsrc_register(&col_provider);
}

#else

void
sift_aggcol_register(void)
{
}

#endif							/* __has_include("col/col.h") */
