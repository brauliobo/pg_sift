/*
 * agg/col.c - the column source of the aggregate scan over a sift_col index: the leaf list becomes the layout, a unit's selection-based reads become whole-unit vectors of the
 * types the kernels take (the engine decodes a chunk straight into the lane of the leaf's vector type, NULL bitmaps expanded to flags),
 * liveness and MVCC vetting are the engine's. Compiled only when the engine is part of the build.
 */
#include "postgres.h"

#include "agg/agg.h"

#if __has_include("col/col.h")

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/kern.h"
#include "agg/pre.h"
#include "agg/sel.h"
#include "agg/wide.h"
#include "core/popcount.h"
#include "core/store.h"
#include "core/visibility.h"
#include "col/col.h"
#include "col/gpu.h"
#include "col/pagg.h"
#include "col/am.h"
#include "col/enc.h"
#include "col/pg.h"
#include "col/runs.h"
#include "agg/colsrc.h"
#include "col/shred_pg.h"

#include "agg/col_int.h"

static bool
col_owns_index(Oid indexrelam)
{
	return OidIsValid(indexrelam) && indexrelam == sift_col_am_oid();
}

static bool
col_owns_table(Oid tablerelam)
{
	return false;
}

/* a head of two or three lanes (a numeric of more than 18 digits or of any other shape, an interval) or a uuid, bytea or other varlena image:
 * read through a dictionary of its datums */
static bool
leaf_wide(const SiftColDef *def)
{
	return def->lane == 0 && (def->lanes > 0 || sift_col_def_is_image(def));
}

static SiftSrcVecType
leaf_vtype(const SiftColDef *def)
{
	if (leaf_wide(def))
		return SCV_CODE;
	switch (def->store)
	{
		case SIFT_COLS_INT:
			return (def->type == SIFT_COLT_INT8 || def->type == SIFT_COLT_TIMESTAMP || def->type == SIFT_COLT_TIMESTAMPTZ || def->type == SIFT_COLT_NUMERIC ||
					def->type == SIFT_COLT_MONEY || def->type == SIFT_COLT_TIME) ? SCV_I64 : SCV_I32;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			return SCV_F64;
		case SIFT_COLS_STR:
			return SCV_CODE;
		default:
			SIFT_INVARIANT(false, "sift: leaf of storage %d has no column vector type", def->store);
	}
	return SCV_I32;
}

/*
 * A leaf the aggregate scan does not read: a lane of a wide value and the heads that are not one machine word;
 * with no SQL type no expression matches it. money is one int64 and is read as such.
 */
static bool
leaf_hidden(const SiftColDef *def)
{
	return def->lane > 0;
}

/* "a.b" below the index attribute `att` of a leaf path "att.a.b" / "att->a->b" / "att.a[]" / "att.*": the keys, as String nodes */
static List *
path_below(const char *path, const char *att)
{
	List	   *keys = NIL;
	size_t		alen = strlen(att);
	const char *p = path + alen;
	StringInfoData key;

	if (*path == '\0')
		return NIL;
	SIFT_INVARIANT(strncmp(path, att, alen) == 0, "sift: leaf path \"%s\" does not start with its attribute \"%s\"", path, att);
	initStringInfo(&key);
	while (*p)
	{
		if (*p == '.' || (p[0] == '-' && p[1] == '>') || *p == '[' || *p == '#')
		{
			if (key.len)
				keys = lappend(keys, makeString(pstrdup(key.data)));
			resetStringInfo(&key);
			if (*p == '[' || *p == '#')
				break;
			p += (*p == '.') ? 1 : 2;
		}
		else if (*p == '*')
			break;
		else
			appendStringInfoChar(&key, *p++);
	}
	if (key.len)
		keys = lappend(keys, makeString(pstrdup(key.data)));
	return keys;
}

/* the field name of a child of a group: after "[]." (list) or the member / value of a map */
static void
child_name(const char *path, bool is_map, bool is_key, char *name)
{
	const char *f = is_map ? NULL : strstr(path, "[].");

	if (is_map)
		strlcpy(name, is_key ? "key" : "value", NAMEDATALEN);
	else if (f)
		strlcpy(name, f + 3, NAMEDATALEN);
	else
		name[0] = '\0';
}

/* The leaf of column def i of a shredded attribute: its place in the group tree, its path below the attribute, its kind and vector type, its SQL type in desc */
void
sift_colsrc_def_leaf(const SiftColDef *cols, int i, TupleDesc desc, SiftSrcLeaf *l)
{
	const SiftColDef *def = &cols[i];
	const char *att = NameStr(TupleDescAttr(desc, def->attno - 1)->attname);
	bool		is_map = def->parent >= 0 && strstr(cols[def->parent].path, ".*") != NULL;

	l->id = def->id;
	l->attno = def->attno;
	l->parent = def->parent;
	l->typid = leaf_hidden(def) ? InvalidOid : sift_col_def_typid(desc, def);
	l->wide = leaf_wide(def);
	l->wtypid = sift_col_def_lane_tested(def) ? sift_col_def_typid(desc, def) : InvalidOid;
	l->scale = def->scale;
	l->lossy = (def->flags & SIFT_COLD_LOSSY) != 0;
	l->astext = (def->flags & SIFT_COLD_ASTEXT) != 0;
	if (def->kind == SIFT_COLK_GROUP)
	{
		l->kind = strstr(def->path, ".*") ? SCL_OBJECT : SCL_ARRAY;
		l->vtype = SCV_I32;
		l->path = path_below(def->path, att);
	}
	else
	{
		l->kind = SCL_SCALAR;
		l->vtype = def->store == SIFT_COLS_VAR ? SCV_I64 : leaf_vtype(def);
		l->lossy |= def->store == SIFT_COLS_VAR;
		if (def->parent < 0)
			l->path = path_below(def->path, att);
		else
			child_name(def->path, is_map, strstr(def->path, "#key") != NULL, l->name);
	}
}

static SiftSrcLayout *
col_layout(Relation index)
{
	SiftColIndexMeta m = *sift_col_index_defs(index);	/* a copy: the cached state it points into is freed when reading an index expression reloads the entry */
	const SiftColIndexMeta *meta = &m;
	TupleDesc	tupdesc = RelationGetDescr(index);
	SiftSrcLayout *lay = palloc0(sizeof(SiftSrcLayout));

	lay->heapoid = meta->heapoid;
	lay->natts = meta->natts;
	for (int a = 1; a <= meta->natts; a++)
		lay->attexprs = lappend(lay->attexprs, sift_col_att_expr(index, a));
	lay->nleaves = meta->ncols;
	lay->leaf = palloc0(sizeof(SiftSrcLeaf) * meta->ncols);
	for (int i = 0; i < meta->ncols; i++)
	{
		sift_colsrc_def_leaf(meta->cols, i, tupdesc, &lay->leaf[i]);
		elog(DEBUG1, "sift_aggcol: leaf %d path \"%s\" attno %d parent %d kind %d", i, meta->cols[i].path, meta->cols[i].attno, meta->cols[i].parent,
			 lay->leaf[i].kind);
	}
	return lay;
}

/*
 * The per-leaf numbers of SiftSrcStats from the zones and dictionaries, counted over the non-NULL values
 * of the leaf (elements for a child). ndistinct: a text leaf with no inline unit is its largest segment dictionary, exact for one segment
 * and a lower bound for several that hold different values; an integer leaf is bounded by its range (max - min + 1); any leaf by its
 * non-NULL values. min and max: integer leaves, as double. elems_per_row: a group's total count over its rows.
 */
void
sift_colsrc_leaf_stats(const SiftColDef *cols, int ncols, const SiftColLeafStats *ls, const bool *wovf, const bool *wjnull, SiftSrcStats *st)
{
	for (int i = 0; i < ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
	{
		const SiftColDef *def = &cols[i];
		double		nonnull = ls[i].nvals - ls[i].nnull;

		st->ovf[i] = (ls[i].has_ovf || wovf[i]) && def->type != SIFT_COLT_NUMERIC;
		st->ovf_units[i] = st->ovf[i] ? Max(ls[i].novf, 1.0) / Max(ls[i].nunits, 1.0) : 0;
		st->ovf_frac[i] = ls[i].ovf_vals > 0 ? ls[i].ovf_slots / ls[i].ovf_vals : -1;
		st->nokinds[i] = ls[i].nokinds;
		st->jnull[i] = ls[i].has_jnull || ls[i].nokinds || wjnull[i];
		if (def->kind == SIFT_COLK_GROUP)
		{
			st->elems_per_row[i] = ls[i].has_sum && ls[i].nvals > 0 ? ls[i].sum / ls[i].nvals : 0;
			continue;
		}
		if (ls[i].point_units > 0 && ls[i].nunits > 0)
			st->point_cover[i] = Min(ls[i].point_units / ls[i].nunits, 1.0);
		if (ls[i].set_units > 0)
		{
			st->set_cover[i] = ls[i].set_units / ls[i].set_sampled;
			st->set_keys[i] = ls[i].set_keys / ls[i].set_units;
		}
		st->ndistinct[i] = nonnull;
		if (def->store == SIFT_COLS_STR && !ls[i].inl)
			st->ndistinct[i] = Min(nonnull, ls[i].dict_n);
		else if (def->store == SIFT_COLS_INT && ls[i].has_range && nonnull > 0 && !leaf_wide(def))
		{
			st->ndistinct[i] = Min(nonnull, (double) ls[i].max - (double) ls[i].min + 1);
			st->min[i] = (double) ls[i].min;
			st->max[i] = (double) ls[i].max;
			st->zone_span[i] = Min(ls[i].span / Max(ls[i].nzone, 1.0) / ((double) ls[i].max - (double) ls[i].min + 1.0), 1.0);
		}
	}	/* an element that overflowed has its row read again: the share of rows is the elements' times those of a row */
	for (int i = 0; i < ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
		if (cols[i].parent >= 0 && st->ovf_frac[i] > 0)
			st->ovf_frac[i] = Min(1.0, st->ovf_frac[i] * Max(st->elems_per_row[cols[i].parent], 1.0));
}

/* A copy of the sample of the unit zones the planner may read (the memo of the backend can be replaced while the plan is made); only the integer scalar leaves that are not wide */
static const SiftSrcZones *
col_zones(const SiftColZones *zs, const SiftColIndexMeta *meta)
{
	SiftSrcZones *z;

	if (zs->n == 0 || meta->stats.nunits <= 0)
		return NULL;
	z = palloc0(sizeof(SiftSrcZones));
	z->n = zs->n;
	z->covered = Min((double) zs->units / meta->stats.nunits, 1.0);
	for (int i = 0; i < meta->ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
		if (zs->lo[i] != NULL && !leaf_wide(&meta->cols[i]))
		{
			z->lo[i] = memcpy(palloc(zs->n * sizeof(int64)), zs->lo[i], zs->n * sizeof(int64));
			z->hi[i] = memcpy(palloc(zs->n * sizeof(int64)), zs->hi[i], zs->n * sizeof(int64));
		}
	return z;
}

static void
col_stats(Relation index, SiftSrcStats *st)
{
	SiftColLeafMemo memo;
	SiftColIndexMeta *meta = sift_col_index_meta(index, &memo);
	bool		wovf[SIFT_COLSRC_MAX_LEAVES],
				wjnull[SIFT_COLSRC_MAX_LEAVES];

	memset(st, 0, sizeof(*st));
	st->nrows = meta->stats.nrows;
	st->nunits = meta->stats.nunits;
	st->bytes = meta->stats.bytes;
	st->allvisible_frac = meta->stats.allvisible_frac;
	st->exact_frac = st->allvisible_frac;
	for (int i = 0; i < meta->ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
	{
		wovf[i] = (meta->stats.wbuf_ovf & SIFT_COL_OVF_BIT(i)) != 0;
		wjnull[i] = (meta->stats.wbuf_jnull & SIFT_COL_OVF_BIT(i)) != 0;
	}
	sift_colsrc_leaf_stats(meta->cols, meta->ncols, memo.leaves, wovf, wjnull, st);
	st->zones = col_zones(memo.zones, meta);
	st->rows_miss = memo.pre->units > 0 ? 1.0 - Min(memo.pre->rows / memo.pre->units, 1.0) : 0.0;
	for (int i = 0; memo.pre->units > 0 && i < meta->ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
	{
		st->dim_miss[i] = 1.0 - Min(memo.pre->dim[i] / memo.pre->units, 1.0);
		st->mm_miss[i] = 1.0 - Min(memo.pre->mm[i] / memo.pre->units, 1.0);
		st->roll_miss[i] = 1.0 - Min(memo.pre->roll[i] / memo.pre->units, 1.0);
	}
}

static void
col_stamp(Relation index, SiftSrcStamp *st)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);

	st->file = index->rd_locator.relNumber;
	st->options = sift_col_pagg_hash(index);
	st->version = sift_dir_version(index);
	st->pages = RelationGetNumberOfBlocks(heap);
	st->visible = sift_vis_count_allvisible(heap, st->pages);
	table_close(heap, AccessShareLock);
}

static void *
col_open_flags(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	ColSource  *src = palloc0(sizeof(ColSource));

	src->cxt = mcxt;
	src->heap = heap;
	src->index = index;
	src->snapshot = snapshot;
	src->cs = sift_col_open(heap, index, snapshot, mcxt, flags);
	src->lay = col_layout(index);
	src->dict = palloc0(sizeof(SiftSrcDict) * src->lay->nleaves);
	src->buf = palloc0(sizeof(ColLeafBuf) * src->lay->nleaves);
	src->tol = palloc0(sizeof(bool) * src->lay->nleaves);
	src->raw = palloc0(sizeof(bool) * src->lay->nleaves);
	src->wide = palloc0(sizeof(SiftAggWide) * src->lay->nleaves);
	src->wgen = palloc0(sizeof(uint64) * src->lay->nleaves);
	src->ugen = 1;
	sift_aggsel_init(&src->sel, mcxt, RelationGetRelid(heap));
	sift_aggpre_init(&src->pre, mcxt);
	MemoryContextSwitchTo(old);
	return src;
}

static void *
col_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	return col_open_flags(heap, index, snapshot, mcxt, SIFT_COLO_NOACL | SIFT_COLO_POINTS | SIFT_COLO_POINT_ROWS);
}

/* a partition read through its partitioned table: the privileges and row security of the table were checked, not those of the partition */
static void *
col_open_part(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	ColSource  *src = col_open_flags(heap, index, snapshot, mcxt, SIFT_COLO_NOCHECKS | SIFT_COLO_SNAP_HELD | SIFT_COLO_POINTS | SIFT_COLO_POINT_ROWS);

	/* the open left the header page of each segment pinned, and a scan of thousands of partitions opens them all before it reads one */
	sift_col_unpin(src->cs);
	return src;
}

static void
col_close(void *impl)
{
	ColSource  *src = impl;

	if (src->rck)
		recheck_close(src->rck);
	sift_col_close(src->cs);
}

static void
col_unpin(void *impl)
{
	ColSource  *src = impl;

	if (src->rck)
		recheck_unpin(src->rck);
	sift_col_unpin(src->cs);
}

static const SiftSrcLayout *
col_source_layout(const void *impl)
{
	return ((const ColSource *) impl)->lay;
}

static bool
col_sb_heap(void *impl)
{
	return sift_col_sb_heap(((ColSource *) impl)->cs);
}

static uint32
col_nsb(void *impl)
{
	return sift_col_heap_sbs(((ColSource *) impl)->cs);
}

#define COL_PREDS_MAX	(2 * SIFT_COLSRC_MAX_LEAVES + COL_MAX_STR_PRUNE + SIFT_COLSRC_MAX_FILTERS + COL_MAX_WIDE)

/* The predicates a begin over the prune entries tests (the zone trees and the tests behind them are made once for the entries) */
static int
col_preds(ColSource *src, const SiftSrcPrune *prune, int nprune, SiftColPred *preds)
{
	int			n = sift_aggzone_preds(&src->zones, src->cxt, NULL, prune, nprune, preds);

	for (int i = 0; i < src->nspred; i++)
		preds[n++] = src->spred[i];
	for (int i = 0; i < src->flt.n; i++)
		preds[n++] = src->flt.pred[i];
	for (int i = 0; i < src->nwtest; i++)
		if (src->wtest[i].verdict == SIFT_COLW_PRED)
			preds[n++] = src->wtest[i].pred;
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
	sift_col_begin_zones(src->cs, preds, n, src->zones.zone, src->zones.nzone, sb_lo, sb_hi);
}

static bool
col_kept(void *impl, const SiftSrcPrune *prune, int nprune, uint32 nspace, uint64 *kept)
{
	ColSource  *src = impl;
	SiftColPred	preds[COL_PREDS_MAX];
	int			n = col_preds(src, prune, nprune, preds);

	return sift_col_kept_map(src->cs, preds, n, src->zones.zone, src->zones.nzone, nspace, kept);
}

static bool
col_next(void *impl, SiftSrcUnit *unit)
{
	ColSource  *src = impl;
	uint32		nrow;

	src->sel.compact = false;
	if (src->rck)
	{
		bool		again = src->rck->taken && recheck_pending(src);

		src->rck->active = false;
		src->rck->taken = false;
		if (again && recheck_next(src, unit))
			return true;
	}
	do
	{
		if (!sift_col_next(src->cs, &src->unit))
			return false;
		src->ugen++;
		nrow = src->unit.nrows;
	} while (src->sel.keep && !sift_aggsel_unit(&src->sel, &src->unit, &nrow));
	unit->sb = src->unit.sb;
	unit->nrow = nrow;
	unit->nsu = src->unit.nsu;
	unit->priv = src;
	return true;
}

/* The rows of bits are few: the unit goes on as them alone. Not the unit of rows read again from the heap, which has no stored numbering to go back to. */
static bool
col_narrow(void *impl, SiftSrcUnit *unit, const uint64 *bits)
{
	ColSource  *src = impl;
	uint32		nrow;

	if (src->sel.compact || (src->rck && src->rck->active))
		return false;
	sift_aggsel_narrow(&src->sel, src->cs, &src->unit, bits, &nrow);
	unit->nrow = nrow;
	return true;
}

static void
col_super(void *impl, bool on)
{
	sift_col_super(((ColSource *) impl)->cs, on);
}

static uint32
col_super_span(void *impl)
{
	return sift_col_super_span(((ColSource *) impl)->cs);
}

static void
col_split(void *impl, const SiftSrcUnit *unit)
{
	ColSource  *src = impl;

	sift_col_unit_split(src->cs, &src->unit);
}

static void
col_select(void *impl, const SiftFilter *keep)
{
	((ColSource *) impl)->sel.keep = keep;
}

/* rows ascend in the numbering the executor sees; those of a compacted unit are the idx-th rows of the unit as stored */
static void
col_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids)
{
	ColSource  *src = impl;

	if (src->sel.compact)
	{
		uint32	   *ords = palloc(Max(n, 1) * sizeof(uint32));

		for (int i = 0; i < n; i++)
			ords[i] = src->sel.idx[rows[i]];
		sift_col_row_tids(&src->unit, ords, n, tids);
		pfree(ords);
	}
	else
		sift_col_row_tids(&src->unit, rows, n, tids);
}

static void
col_zone(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z)
{
	SiftColZone cz;

	sift_col_zone(&((ColSource *) impl)->unit, leaf, &cz);
	*z = (SiftSrcZone) {.min = cz.min, .max = cz.max, .nvals = cz.nvals, .nnull = cz.nnull, .inexact = cz.has_ovf};
}

static uint32
col_zone_range(void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z)
{
	SiftColZone cz;
	uint32		n = sift_col_zone_range(((ColSource *) impl)->cs, leaf, lo, hi, &cz);

	*z = (SiftSrcZone) {.min = cz.min, .max = cz.max, .nvals = cz.nvals, .nnull = cz.nnull, .inexact = cz.has_ovf};
	return n;
}

static uint32
col_zone_range_segs(void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z)
{
	SiftColZone cz;
	uint32		n = sift_col_zone_range_segs(((ColSource *) impl)->cs, leaf, lo, hi, &cz);

	*z = (SiftSrcZone) {.min = cz.min, .max = cz.max, .nvals = cz.nvals, .nnull = cz.nnull, .inexact = cz.has_ovf};
	return n;
}

static bool
col_walk_stamp(void *impl, SiftSrcWalkStamp *st, uint32 *mem, int cap, int *nmem)
{
	SiftColWalkStamp cs;

	if (!sift_col_walk_stamp(((ColSource *) impl)->cs, &cs, mem, cap, nmem))
		return false;
	*st = (SiftSrcWalkStamp) {.index = cs.index,.file = cs.file,.version = cs.version};
	return true;
}

/* the stored numbering: not a unit of a match set, of rows read again from the heap, or narrowed to a few rows */
static bool
col_run_info(void *impl, const SiftSrcUnit *unit, int leaf, uint32 *nvals, uint32 *nnull, uint64 *id)
{
	ColSource  *src = impl;

	
	if (src->sel.compact || src->sel.keep || (src->rck && src->rck->active) || col_wide(src, leaf) || unit->nrow != src->unit.nrows)
		return false;
	return sift_col_run_info(&src->unit, leaf, nvals, nnull, id);
}

static bool
col_sb_heads(void *impl, uint32 sb, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi)
{
	ColSource  *src = impl;
	SiftCHeadEnt e[2 * SIFT_CRUN_HEAD_MAX];

	if (src->sel.compact || src->sel.keep || (src->rck && src->rck->active) || col_wide(src, leaf) || !sift_col_sb_heads(src->cs, sb, leaf, e, nlo, nhi))
		return false;
	memcpy(lo, e, *nlo * sizeof(SiftSrcHead));
	memcpy(hi, e + *nlo, *nhi * sizeof(SiftSrcHead));
	return true;
}

static int
col_run_heads(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi)
{
	SiftCHeadEnt e[2 * SIFT_CRUN_HEAD_MAX];
	int			h = sift_col_run_heads(&((ColSource *) impl)->unit, leaf, e, nlo, nhi);

	StaticAssertStmt(sizeof(SiftSrcHead) == sizeof(SiftCHeadEnt) && SIFT_SRC_HEAD_MAX == SIFT_CRUN_HEAD_MAX, "the run head entries of the engine and the source differ");
	memcpy(lo, e, *nlo * sizeof(SiftSrcHead));
	memcpy(hi, e + *nlo, *nhi * sizeof(SiftSrcHead));
	return h;
}

static int
col_gorder_open(void *impl, int leaf, int rows)
{
	ColSource  *src = impl;

	return sift_col_gorder_open(src->cs, leaf, rows);
}

static bool
col_gorder_info(void *impl, int i, int leaf, uint32 *entries, uint32 *values)
{
	return sift_col_gorder_info(((ColSource *) impl)->cs, i, leaf, entries, values);
}

static uint32
col_gorder_chunk(void *impl, int i, int leaf, uint32 c, int64 *key, uint64 *addr)
{
	return sift_col_gorder_chunk(((ColSource *) impl)->cs, i, leaf, c, key, addr);
}

static void
col_run_read(void *impl, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows)
{
	sift_col_run_read(&((ColSource *) impl)->unit, leaf, pos, cnt, rows);
}

bool
col_overflow(void *impl, const SiftSrcUnit *unit, int leaf)
{
	SiftColZone cz;

	if (col_wide(impl, leaf) || ((ColSource *) impl)->unit.nsu > 0)
		return false;				/* a NaN is a dictionary entry; a superunit holds no overflow value */
	sift_col_zone(&((ColSource *) impl)->unit, leaf, &cz);
	return cz.has_ovf;
}

/* ---- pre-aggregates (src/col/pagg.c, docs/COL-PREAGG.md) ---- */

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
	return sift_col_unit_preagg_ok(&((ColSource *) impl)->unit);
}

static int
col_unit_dead(void *impl, const SiftSrcUnit *unit, int mode)
{
	return sift_col_unit_dead(&((ColSource *) impl)->unit, mode == SSD_OFF ? -1 : sift_agg_dead_delta, mode == SSD_BOUNDS);
}

static bool
col_unit_except(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_col_unit_except(&((ColSource *) impl)->unit, leaf);
}

static bool
col_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	ColSource  *src = impl;

	return sift_aggpre_unit_agg(&src->unit, leaf, sift_col_def(src->cs, leaf), agg);
}

static bool
col_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	ColSource  *src = impl;

	return sift_aggpre_unit_hist(&src->pre, &src->unit, dim, sift_col_def(src->cs, dim % SIFT_SRC_CAL), NULL, h);
}

static bool
col_unit_presence(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p)
{
	SiftCPresence c;

	if (!sift_col_unit_presence(&((ColSource *) impl)->unit, leaf, &c))
		return false;
	*p = (SiftSrcPresence) {c.wbase, c.nwords, c.words, c.nkeys, c.keys};
	return true;
}

static bool
col_unit_hll(void *impl, const SiftSrcUnit *unit, int leaf, const uint8 **blob)
{
	return sift_col_unit_hll(&((ColSource *) impl)->unit, leaf, blob);
}

/* ---- the Vulkan path (src/col/gpu.h, docs/GPU.md) ---- */

static bool
col_gpu_unit(void *impl, const SiftSrcUnit *unit, SiftSrcGpuUnit *g)
{
	SiftColGpuUnit cg;

	if (((ColSource *) impl)->sel.keep || ((ColSource *) impl)->sel.compact || !sift_col_unit_gpu(&((ColSource *) impl)->unit, &cg))
		return false;
	*g = (SiftSrcGpuUnit) {cg.store, cg.gen, cg.unit, cg.nrows};
	return true;
}

static bool
col_gpu_dense(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_col_unit_gpu_dense(&((ColSource *) impl)->unit, leaf);
}

static bool
col_gpu_chunk(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcGpuChunk *c)
{
	SiftColGpuChunk cg;

	if (!sift_col_unit_gpu_chunk(&((ColSource *) impl)->unit, leaf, &cg))
		return false;
	*c = (SiftSrcGpuChunk) {cg.blob, cg.len, cg.n, cg.is_const, cg.cval};
	return true;
}

static void
col_gpu_dict(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcDict *dict)
{
	ColSource  *src = impl;
	SiftColDict d;

	sift_col_unit_gpu_dict(&src->unit, leaf, &d);
	*dict = (SiftSrcDict) {(const void *) (uintptr_t) d.id, d.n, d.off, d.bytes};
}

static uint32
col_unit_rollup_tuples(void *impl, const SiftSrcUnit *unit, int list)
{
	return sift_col_unit_rollup_tuples(&((ColSource *) impl)->unit, list);
}

static bool
col_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *h)
{
	ColSource  *src = impl;
	SiftCRollup r;
	SiftCRollupNeed cn;
	SiftColDict dict[SIFT_PAGG_MAX_RDIMS];

	cn.ndims = need->ndims;
	cn.nmeas = need->nmeas;
	for (int j = 0; j < need->ndims; j++)
		cn.dim[j] = need->dim[j];
	for (int m = 0; m < need->nmeas; m++)
	{
		cn.meas[m].leaf = need->meas[m].leaf;
		cn.meas[m].sums = need->meas[m].sums;
		cn.meas[m].minmax = need->meas[m].minmax;
	}
	if (src->pre.pcx == NULL)
		src->pre.pcx = AllocSetContextCreate(src->pre.cxt, "sift_aggpre histogram", ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(src->pre.pcx);
	if (!sift_col_unit_rollup(&src->unit, list, src->pre.pcx, need->all ? NULL : &cn, &r, dict))
		return false;
	memset(h, 0, sizeof(*h));
	h->ntuples = r.ntuples;
	h->ndims = r.ndims;
	h->count = r.count;
	h->nmeas = r.nmeas;
	for (int j = 0; j < r.ndims; j++)
	{
		h->dim[j] = r.dim[j];
		h->key[j] = r.key[j];
		h->isnull[j] = r.isnull[j];
		if (sift_col_def(src->cs, r.dim[j])->store == SIFT_COLS_STR)
		{
			src->rdict[j] = (SiftSrcDict) {(const void *) (uintptr_t) dict[j].id, dict[j].n, dict[j].off, dict[j].bytes};
			h->dict[j] = &src->rdict[j];
		}
	}
	for (int m = 0; m < r.nmeas; m++)
	{
		h->meas[m] = r.meas[m];
		h->nn[m] = r.nn[m];
		h->sum[m] = r.sum[m];
		h->sumf[m] = r.sumf[m];
		h->min[m] = r.min[m];
		h->max[m] = r.max[m];
		h->minf[m] = r.minf[m];
		h->maxf[m] = r.maxf[m];
	}
	return true;
}

static bool
col_unit_rollup_miss(void *impl, const SiftSrcUnit *unit)
{
	return sift_col_unit_rollup_miss(&((ColSource *) impl)->unit);
}

static const SiftSrcProvider col_provider = {
	.owns_index = col_owns_index,
	.owns_table = col_owns_table,
	.layout = col_layout,
	.stats = col_stats,
	.stamp = col_stamp,
	.open = col_open,
	.open_part = col_open_part,
	.close = col_close,
	.unpin = col_unpin,
	.source_layout = col_source_layout,
	.nsb = col_nsb,
	.begin = col_begin,
	.kept = col_kept,
	.next = col_next,
	.super = col_super,
	.split = col_split,
	.super_span = col_super_span,
	.read = col_read,
	.live = col_live,
	.vet = col_vet,
	.vet_needed = col_vet_needed,
	.counters = col_counters,
	.leaf_n = col_leaf_n,
	.read_into = col_read_into,
	.select = col_select,
	.tids = col_tids,
	.sb_heap = col_sb_heap,
	.zone = col_zone,
	.zone_range = col_zone_range,
	.zone_range_segs = col_zone_range_segs,
	.walk_stamp = col_walk_stamp,
	.overflow = col_overflow,
	.run_info = col_run_info,
	.run_heads = col_run_heads,
	.sb_heads = col_sb_heads,
	.run_read = col_run_read,
	.gorder_open = col_gorder_open,
	.gorder_info = col_gorder_info,
	.gorder_chunk = col_gorder_chunk,
	.read_rows = col_read_rows,
	.set_filters = col_set_filters,
	.filter = col_filter,
	.read_sel = col_read_sel,
	.narrow = col_narrow,
	.filter_types = col_filter_types,
	.tolerate_ovf = col_tolerate_ovf,
	.set_raw = col_set_raw,
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
	.unit_hll = col_unit_hll,
	.gpu_unit = col_gpu_unit,
	.gpu_dense = col_gpu_dense,
	.gpu_chunk = col_gpu_chunk,
	.gpu_dict = col_gpu_dict,
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
