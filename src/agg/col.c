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
#include "col/col.h"
#include "col/gpu.h"
#include "col/pagg.h"
#include "col/am.h"
#include "col/enc.h"
#include "col/pg.h"
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
			elog(ERROR, "sift: leaf of storage %d has no column vector type", def->store);
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
	if (strncmp(path, att, alen) != 0)
		elog(ERROR, "sift: leaf path \"%s\" does not start with its attribute \"%s\"", path, att);
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
	const SiftColIndexMeta *meta = sift_col_index_defs(index);
	TupleDesc	tupdesc = RelationGetDescr(index);
	SiftSrcLayout *lay = palloc0(sizeof(SiftSrcLayout));

	if (meta->partial)
		elog(ERROR, "sift: a partial sift_col index has no column source for the aggregate scan");
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
sift_colsrc_leaf_stats(const SiftColDef *cols, int ncols, const SiftColLeafStats *ls, const bool *wovf, SiftSrcStats *st)
{
	for (int i = 0; i < ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
	{
		const SiftColDef *def = &cols[i];
		double		nonnull = ls[i].nvals - ls[i].nnull;

		st->ovf[i] = (ls[i].has_ovf || wovf[i]) && def->type != SIFT_COLT_NUMERIC;
		st->nokinds[i] = ls[i].nokinds;
		if (def->kind == SIFT_COLK_GROUP)
		{
			st->elems_per_row[i] = ls[i].has_sum && ls[i].nvals > 0 ? ls[i].sum / ls[i].nvals : 0;
			continue;
		}
		st->ndistinct[i] = nonnull;
		if (def->store == SIFT_COLS_STR && !ls[i].inl)
			st->ndistinct[i] = Min(nonnull, ls[i].dict_n);
		else if (def->store == SIFT_COLS_INT && ls[i].has_range && nonnull > 0 && !leaf_wide(def))
		{
			st->ndistinct[i] = Min(nonnull, (double) ls[i].max - (double) ls[i].min + 1);
			st->min[i] = (double) ls[i].min;
			st->max[i] = (double) ls[i].max;
		}
	}
}

static void
col_stats(Relation index, SiftSrcStats *st)
{
	SiftColIndexMeta *meta = sift_col_index_meta(index);
	bool		wovf[SIFT_COLSRC_MAX_LEAVES];

	memset(st, 0, sizeof(*st));
	st->nrows = meta->stats.nrows;
	st->nunits = meta->stats.nunits;
	st->bytes = meta->stats.bytes;
	st->allvisible_frac = meta->stats.allvisible_frac;
	st->exact_frac = st->allvisible_frac;
	for (int i = 0; i < meta->ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
		wovf[i] = (meta->stats.wbuf_ovf & SIFT_COL_OVF_BIT(i)) != 0;
	sift_colsrc_leaf_stats(meta->cols, meta->ncols, sift_col_leaf_stats(index), wovf, st);
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
	src->wide = palloc0(sizeof(SiftAggWide) * src->lay->nleaves);
	src->wgen = palloc0(sizeof(uint64) * src->lay->nleaves);
	src->ugen = 1;
	sift_aggsel_init(&src->sel, mcxt);
	sift_aggpre_init(&src->pre, mcxt);
	MemoryContextSwitchTo(old);
	return src;
}

static void *
col_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	return col_open_flags(heap, index, snapshot, mcxt, 0);
}

/* a partition read through its partitioned table: the privileges and row security of the table were checked, not those of the partition */
static void *
col_open_part(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	return col_open_flags(heap, index, snapshot, mcxt, SIFT_COLO_NOCHECKS);
}

static void
col_close(void *impl)
{
	ColSource  *src = impl;

	if (src->rck)
		recheck_close(src->rck);
	sift_col_close(src->cs);
}

static const SiftSrcLayout *
col_source_layout(const void *impl)
{
	return ((const ColSource *) impl)->lay;
}

static uint32
col_nsb(void *impl)
{
	return sift_col_heap_sbs(((ColSource *) impl)->cs);
}

static void
col_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	ColSource  *src = impl;
	SiftColValue vals[2 * SIFT_COLSRC_MAX_LEAVES] = {{{0}}};
	SiftColPred	preds[2 * SIFT_COLSRC_MAX_LEAVES + COL_MAX_STR_PRUNE + SIFT_COLSRC_MAX_FILTERS + COL_MAX_WIDE];
	int			n;

	if (src->rck)
		src->rck->taken = src->rck->active = false, src->rck->npend = 0;
	/* a zone [lo, hi] is two comparisons: the unit is skipped when its min/max exclude either */
	for (int k = 0; k < nprune; k++)
	{
		vals[2 * k].v.i = prune[k].lo;
		vals[2 * k + 1].v.i = prune[k].hi;
		preds[2 * k] = (SiftColPred) {prune[k].leaf, SIFT_COLOP_GE, 1, &vals[2 * k]};
		preds[2 * k + 1] = (SiftColPred) {prune[k].leaf, SIFT_COLOP_LE, 1, &vals[2 * k + 1]};
	}
	for (int i = 0; i < src->nspred; i++)
		preds[2 * nprune + i] = src->spred[i];
	for (int i = 0; i < src->nflt; i++)
		preds[2 * nprune + src->nspred + i] = src->fpred[i];
	n = 2 * nprune + src->nspred + src->nflt;
	for (int i = 0; i < src->nwtest; i++)
		if (src->wtest[i].verdict == SIFT_COLW_PRED)
			preds[n++] = src->wtest[i].pred;
	sift_col_begin(src->cs, preds, n, sb_lo, sb_hi);
}

static bool
col_next(void *impl, SiftSrcUnit *unit)
{
	ColSource  *src = impl;
	uint32		nrow;

	if (src->rck)
	{
		bool		again = src->rck->taken && src->rck->npend > 0;

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
	unit->priv = src;
	return true;
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

	if (src->sel.keep && src->sel.compact)
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

bool
col_overflow(void *impl, const SiftSrcUnit *unit, int leaf)
{
	SiftColZone cz;

	if (((ColSource *) impl)->lay->leaf[leaf].wide)
		return false;				/* a NaN is a dictionary entry */
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

	return sift_aggpre_unit_hist(&src->pre, &src->unit, dim, sift_col_def(src->cs, dim), NULL, h);
}

/* ---- the Vulkan path (src/col/gpu.h, docs/GPU.md) ---- */

static bool
col_gpu_unit(void *impl, const SiftSrcUnit *unit, SiftSrcGpuUnit *g)
{
	SiftColGpuUnit cg;

	if (((ColSource *) impl)->sel.keep || !sift_col_unit_gpu(&((ColSource *) impl)->unit, &cg))
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

static bool
col_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, SiftSrcRollup *h)
{
	ColSource  *src = impl;
	SiftCRollup r;
	SiftColDict dict[SIFT_PAGG_MAX_RDIMS];

	if (src->pre.pcx == NULL)
		src->pre.pcx = AllocSetContextCreate(src->pre.cxt, "sift_aggpre histogram", ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(src->pre.pcx);
	if (!sift_col_unit_rollup(&src->unit, list, src->pre.pcx, &r, dict))
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
	.open = col_open,
	.open_part = col_open_part,
	.close = col_close,
	.source_layout = col_source_layout,
	.nsb = col_nsb,
	.begin = col_begin,
	.next = col_next,
	.read = col_read,
	.live = col_live,
	.vet = col_vet,
	.vet_needed = col_vet_needed,
	.counters = col_counters,
	.leaf_n = col_leaf_n,
	.read_into = col_read_into,
	.select = col_select,
	.tids = col_tids,
	.zone = col_zone,
	.zone_range = col_zone_range,
	.overflow = col_overflow,
	.read_rows = col_read_rows,
	.set_filters = col_set_filters,
	.filter = col_filter,
	.read_sel = col_read_sel,
	.filter_types = col_filter_types,
	.tolerate_ovf = col_tolerate_ovf,
	.set_recheck = col_set_recheck,
	.overflow_raise = col_overflow_raise,
	.read_types = col_read_types,
	.read_jnull = col_read_jnull,
	.prune_str = col_prune_str,
	.filter_str = col_filter_str,
	.set_wide = col_set_wide,
	.filter_wide = col_filter_wide,
	.preagg = col_preagg,
	.unit_exact = col_unit_exact,
	.unit_agg = col_unit_agg,
	.unit_hist = col_unit_hist,
	.gpu_unit = col_gpu_unit,
	.gpu_dense = col_gpu_dense,
	.gpu_chunk = col_gpu_chunk,
	.gpu_dict = col_gpu_dict,
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
