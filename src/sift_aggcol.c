/*
 * sift_aggcol.c - the column source of the aggregate scan over a sift_col index: the leaf list becomes the layout, a unit's selection-based reads become whole-unit vectors of the
 * types the kernels take (the engine decodes a chunk straight into the lane of the leaf's vector type, NULL bitmaps expanded to flags),
 * liveness and MVCC vetting are the engine's. Compiled only when the engine is part of the build.
 */
#include "postgres.h"

#include "sift_agg.h"

#if __has_include("sift_col.h")

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "sift_aggkern.h"
#include "sift_col.h"
#include "sift_col_pagg.h"
#include "sift_col_pg.h"
#include "sift_colsrc.h"

typedef struct ColLeafBuf
{
	size_t		cap;			/* elements the buffers hold */
	void	   *data;
	uint8	   *isnull;
	uint64	   *ovf;			/* numeric leaves: the NaNs of the heap */
} ColLeafBuf;

typedef struct ColSource
{
	SiftColSource *cs;
	SiftColUnit unit;
	SiftSrcLayout *lay;
	MemoryContext cxt;
	SiftSrcDict *dict;			/* per leaf: the dictionary of the last read */
	ColLeafBuf *buf;			/* per leaf */
	SiftColPred *spred;			/* the text tests of prune_str, as column predicates */
	int			nspred;
	SiftColValue *fvals;		/* the constants of one filter_str call */
	int			fcap;
	MemoryContext pcx;			/* the histogram of the last unit_hist */
	SiftSrcDict hdict;			/* its dictionary */
} ColSource;

#define COL_MAX_STR_PRUNE	16

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

static SiftSrcVecType
leaf_vtype(const SiftColDef *def)
{
	switch (def->store)
	{
		case SIFT_COLS_INT:
			return (def->type == SIFT_COLT_INT8 || def->type == SIFT_COLT_TIMESTAMP || def->type == SIFT_COLT_TIMESTAMPTZ || def->type == SIFT_COLT_NUMERIC) ? SCV_I64 : SCV_I32;
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
		const SiftColDef *def = &meta->cols[i];
		SiftSrcLeaf *l = &lay->leaf[i];
		const char *att = NameStr(TupleDescAttr(tupdesc, def->attno - 1)->attname);
		bool		is_map = def->parent >= 0 && strstr(meta->cols[def->parent].path, ".*") != NULL;

		l->id = def->id;
		l->attno = def->attno;
		l->parent = def->parent;
		l->typid = sift_col_def_typid(tupdesc, def);
		l->scale = def->scale;
		l->lossy = (def->flags & SIFT_COLD_LOSSY) != 0;
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
		elog(DEBUG1, "sift_aggcol: leaf %d path \"%s\" attno %d parent %d kind %d", i, def->path, def->attno, def->parent, l->kind);
	}
	return lay;
}

/*
 * The per-leaf numbers of SiftSrcStats from the zones and dictionaries, counted over the non-NULL values
 * of the leaf (elements for a child). ndistinct: a text leaf with no inline unit is its largest segment dictionary, exact for one segment
 * and a lower bound for several that hold different values; an integer leaf is bounded by its range (max - min + 1); any leaf by its
 * non-NULL values. min and max: integer leaves, as double. elems_per_row: a group's total count over its rows.
 */
static void
leaf_stats(const SiftColIndexMeta *meta, const SiftColLeafStats *ls, SiftSrcStats *st)
{
	for (int i = 0; i < meta->ncols && i < SIFT_COLSRC_MAX_LEAVES; i++)
	{
		const SiftColDef *def = &meta->cols[i];
		double		nonnull = ls[i].nvals - ls[i].nnull;

		if (def->kind == SIFT_COLK_GROUP)
		{
			st->elems_per_row[i] = ls[i].has_sum && ls[i].nvals > 0 ? ls[i].sum / ls[i].nvals : 0;
			continue;
		}
		st->ndistinct[i] = nonnull;
		if (def->store == SIFT_COLS_STR && !ls[i].inl)
			st->ndistinct[i] = Min(nonnull, ls[i].dict_n);
		else if (def->store == SIFT_COLS_INT && ls[i].has_range && nonnull > 0)
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

	memset(st, 0, sizeof(*st));
	st->nrows = meta->stats.nrows;
	st->nunits = meta->stats.nunits;
	st->allvisible_frac = meta->stats.allvisible_frac;
	leaf_stats(meta, sift_col_leaf_stats(index), st);
}

static void *
col_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	ColSource  *src = palloc0(sizeof(ColSource));

	src->cxt = mcxt;
	src->cs = sift_col_open(heap, index, snapshot, mcxt, 0);
	src->lay = col_layout(index);
	src->dict = palloc0(sizeof(SiftSrcDict) * src->lay->nleaves);
	src->buf = palloc0(sizeof(ColLeafBuf) * src->lay->nleaves);
	MemoryContextSwitchTo(old);
	return src;
}

static void
col_close(void *impl)
{
	sift_col_close(((ColSource *) impl)->cs);
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
	SiftColPred	preds[2 * SIFT_COLSRC_MAX_LEAVES + COL_MAX_STR_PRUNE];

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
	sift_col_begin(src->cs, preds, 2 * nprune + src->nspred, sb_lo, sb_hi);
}

static SiftColOp
str_op(SiftSrcStrOp op)
{
	switch (op)
	{
		case SSTR_EQ:
			return SIFT_COLOP_EQ;
		case SSTR_NE:
			return SIFT_COLOP_NE;
		case SSTR_LT:
			return SIFT_COLOP_LT;
		case SSTR_LE:
			return SIFT_COLOP_LE;
		case SSTR_GT:
			return SIFT_COLOP_GT;
		case SSTR_GE:
			return SIFT_COLOP_GE;
		case SSTR_IN:
			return SIFT_COLOP_IN;
		case SSTR_NOTIN:
			return SIFT_COLOP_NOTIN;
		case SSTR_PREFIX:
			return SIFT_COLOP_PREFIX;
	}
	elog(ERROR, "sift: unknown text test %d", (int) op);
}

/* a text test as a column predicate over the constants in vals (room for t->nvals) */
static SiftColPred
str_pred(const SiftSrcStrTest *t, SiftColValue *vals)
{
	for (int i = 0; i < t->nvals; i++)
		vals[i] = (SiftColValue) {.v.s = t->vals[i], .len = t->lens[i]};
	return (SiftColPred) {t->leaf, str_op(t->op), t->nvals, vals};
}

/* the tests join the zone pruning of every begin: the segment dictionary's, or the prefix zone of an inline unit */
static void
col_prune_str(void *impl, const SiftSrcStrTest *t, int nt)
{
	ColSource  *src = impl;

	src->spred = MemoryContextAlloc(src->cxt, Min(nt, COL_MAX_STR_PRUNE) * sizeof(SiftColPred));
	for (int i = 0; i < nt && src->nspred < COL_MAX_STR_PRUNE; i++)
		if (!src->lay->leaf[t[i].leaf].lossy)
			src->spred[src->nspred++] = str_pred(&t[i], MemoryContextAlloc(src->cxt, t[i].nvals * sizeof(SiftColValue)));
}

/* the rows whose value passes, on the encoded form of the unit: the strings are not decoded unless the prefixes cannot decide */
static bool
col_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	ColSource  *src = impl;
	SiftColSel	sel;
	SiftColPred pred;

	if (src->lay->leaf[t->leaf].lossy)
		return false;
	if (t->nvals > src->fcap)
	{
		src->fcap = Max(t->nvals, 2 * src->fcap);
		src->fvals = src->fvals ? repalloc(src->fvals, src->fcap * sizeof(SiftColValue)) : MemoryContextAlloc(src->cxt, src->fcap * sizeof(SiftColValue));
	}
	pred = str_pred(t, src->fvals);
	sift_col_sel_alloc(src->cs, unit->nrow, &sel);
	memcpy(sel.bits, rows, sel.nwords * sizeof(uint64));
	sel.count = sift_col_sel_count(&sel);
	sift_col_filter(&src->unit, &pred, 1, &sel);
	memcpy(rows, sel.bits, sel.nwords * sizeof(uint64));
	return true;
}

static bool
col_next(void *impl, SiftSrcUnit *unit)
{
	ColSource  *src = impl;

	if (!sift_col_next(src->cs, &src->unit))
		return false;
	unit->sb = src->unit.sb;
	unit->nrow = src->unit.nrows;
	unit->priv = src;
	return true;
}

static const size_t vtype_size[] = {sizeof(int32), sizeof(int64), sizeof(double), sizeof(uint32)};
static const SiftColLane vtype_lane[] = {SIFT_COLL_I32, SIFT_COLL_I64, SIFT_COLL_F64, SIFT_COLL_CODE};

static int64
col_leaf_n(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_col_lane_n(&((ColSource *) impl)->unit, leaf);
}

/* the leaf decoded by the engine into the caller's storage, in the lane of the leaf's vector type */
static void
col_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	SiftColLaneInfo info;

	sift_col_read_lane(&src->unit, leaf, vtype_lane[l->vtype], buf->data, buf->isnull, &info);
	vec->ovf = NULL;
	/* the only value a numeric(p <= 18, s) leaf cannot hold is NaN: the executor evaluates those rows as the numeric NaN they are */
	if (info.ovf && buf->ovf && l->typid == NUMERICOID && l->kind == SCL_SCALAR)
	{
		SiftColVec	v;

		sift_col_read(&src->unit, leaf, NULL, &v);
		memcpy(buf->ovf, v.ovf, SIFT_COL_NWORDS(info.n) * sizeof(uint64));
		vec->ovf = buf->ovf;
	}
	else if (info.ovf)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift: a column of the aggregate scan holds values its index cannot store"),
				 errhint("Set sift.enable_agg to off for this query.")));
	memset((char *) buf->data + info.n * vtype_size[l->vtype], 0, SIFT_AGG_PAD * vtype_size[l->vtype]);
	vec->data = buf->data;
	vec->n = info.n;
	vec->isnull = info.nulls ? buf->isnull : NULL;
	vec->dict = NULL;
	if (l->vtype == SCV_CODE)
	{
		src->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) info.dict.id, info.dict.n, info.dict.off, info.dict.bytes};
		vec->dict = &src->dict[leaf];
	}
}

/* the leaf's buffers for n elements and SIFT_AGG_PAD more */
static ColLeafBuf *
leaf_buf(ColSource *src, int leaf, size_t n)
{
	ColLeafBuf *b = &src->buf[leaf];
	size_t		need = n + SIFT_AGG_PAD;

	if (b->cap < need)
	{
		if (b->data)
		{
			pfree(b->data);
			pfree(b->isnull);
			pfree(b->ovf);
		}
		b->cap = Max(need, 4096);
		b->data = MemoryContextAlloc(src->cxt, b->cap * 8);
		b->isnull = MemoryContextAlloc(src->cxt, b->cap);
		b->ovf = MemoryContextAlloc(src->cxt, (b->cap / 64 + 1) * sizeof(uint64));
	}
	return b;
}

/* read_into over the source's own buffers of the leaf: valid until the next read of the leaf or the next unit */
static void
col_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	ColLeafBuf *b = leaf_buf(src, leaf, sift_col_lane_n(&src->unit, leaf));

	col_read_into(impl, unit, leaf, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
}

static void
col_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;
	SiftColSel	sel;

	sift_col_sel_all(&src->unit, &sel);
	memcpy(bits, sel.bits, sel.nwords * sizeof(uint64));
}

static void
col_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;
	SiftColSel	sel;

	sift_col_sel_alloc(src->cs, unit->nrow, &sel);
	memcpy(sel.bits, bits, sel.nwords * sizeof(uint64));
	sel.count = sift_col_sel_count(&sel);
	sift_col_vet(&src->unit, &sel);
	memcpy(bits, sel.bits, sel.nwords * sizeof(uint64));
}

static void
col_counters(const void *impl, SiftSrcCounters *c)
{
	const SiftColCounters *cc = sift_col_counters(((const ColSource *) impl)->cs);

	c->units = cc->units;
	c->units_pruned = cc->units_pruned;
	c->rows = cc->rows_scanned;
	c->bytes = cc->bytes_decoded;
	c->rows_vetted = cc->rows_vetted;
	c->heap_pages = cc->heap_pages;
}

/* ---- pre-aggregates (src/sift_col_pagg.c, docs/COL-PREAGG.md) ---- */

static void
col_preagg(Relation index, SiftSrcPreagg *pa)
{
	SiftCPaggSpec spec;

	sift_col_pagg_spec(index, &spec);
	if (spec.off)
		return;
	for (int d = 0; d < spec.ndims; d++)
	{
		pa->dim[pa->ndims].leaf = spec.dim[d].leaf;
		pa->dim[pa->ndims].nmeas = spec.dim[d].nmeas;
		for (int m = 0; m < spec.dim[d].nmeas; m++)
			pa->dim[pa->ndims].meas[m] = spec.dim[d].meas[m];
		pa->ndims++;
	}
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
	const SiftColDef *def = sift_col_def(src->cs, leaf);
	SiftCAgg	a;

	if (!sift_col_unit_agg(&src->unit, leaf, &a))
		return false;
	memset(agg, 0, sizeof(*agg));
	agg->nvals = a.nvals;
	agg->nnull = a.nnull;
	agg->has_nan = (a.zflags & SIFT_COLZ_NAN) != 0;
	agg->is_const = (a.zflags & SIFT_COLZ_CONST) != 0;
	agg->has_minmax = a.nnull < a.nvals && def->store != SIFT_COLS_STR && !(a.zflags & SIFT_COLZ_INLINE);
	if (def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8)
	{
		agg->min_f = sift_col_f8_from_key(a.min);
		agg->max_f = sift_col_f8_from_key(a.max);
		agg->sum_f = a.sum_f;
	}
	else
	{
		agg->min = a.min;
		agg->max = a.max;
	}
	if (a.has_sums)
	{
		agg->has_sums = !(a.zflags & SIFT_COLZ_OVERFLOW);
		agg->has_sumsq = !(a.flags & SIFT_CAGG_SUMSQ_OVF);
		agg->sum = a.sum;
		agg->sumsq = a.sumsq;
	}
	return true;
}

static bool
col_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	ColSource  *src = impl;
	SiftCHistDim d;
	SiftColDict dict;

	if (src->pcx == NULL)
		src->pcx = AllocSetContextCreate(src->cxt, "sift_aggcol histogram", ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(src->pcx);
	if (!sift_col_unit_hist(&src->unit, dim, src->pcx, &d, &dict))
		return false;
	memset(h, 0, sizeof(*h));
	h->ngroups = d.ngroups;
	h->has_null = d.has_null;
	h->key = d.key;
	h->count = d.count;
	h->nmeas = d.nmeas;
	for (int m = 0; m < d.nmeas; m++)
	{
		h->meas[m] = d.meas[m];
		h->nn[m] = d.nn[m];
		h->sum[m] = d.sum[m];
		h->sumf[m] = d.sumf[m];
	}
	if (sift_col_def(src->cs, dim)->store == SIFT_COLS_STR)
	{
		src->hdict = (SiftSrcDict) {(const void *) (uintptr_t) dict.id, dict.n, dict.off, dict.bytes};
		h->dict = &src->hdict;
	}
	return true;
}

static const SiftSrcProvider col_provider = {
	.owns_index = col_owns_index,
	.owns_table = col_owns_table,
	.layout = col_layout,
	.stats = col_stats,
	.open = col_open,
	.close = col_close,
	.source_layout = col_source_layout,
	.nsb = col_nsb,
	.begin = col_begin,
	.next = col_next,
	.read = col_read,
	.live = col_live,
	.vet = col_vet,
	.counters = col_counters,
	.leaf_n = col_leaf_n,
	.read_into = col_read_into,
	.prune_str = col_prune_str,
	.filter_str = col_filter_str,
	.preagg = col_preagg,
	.unit_exact = col_unit_exact,
	.unit_agg = col_unit_agg,
	.unit_hist = col_unit_hist,
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

#endif							/* __has_include("sift_col.h") */
