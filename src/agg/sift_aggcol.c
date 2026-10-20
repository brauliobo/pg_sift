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
#include "sift_aggpre.h"
#include "sift_aggsel.h"
#include "sift_aggwide.h"
#include "sift_popcount.h"
#include "sift_col.h"
#include "sift_col_gpu.h"
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
	bool	   *tol;			/* per leaf: its values the index cannot hold are read as NULL (tolerate_ovf) */
	SiftAggSel	sel;			/* the match set the units are restricted to */
	int			nflt;			/* the filters set_filters accepted, as engine predicates */
	SiftColPred *fpred;
	SiftColPred *spred;			/* the text tests of prune_str, as column predicates */
	int			nspred;
	SiftColWTest *wtest;		/* the conjuncts on wide leaves of set_wide */
	int			nwtest;
	SiftAggWide *wide;			/* per leaf: the dictionary of a wide leaf in the unit (wgen: the unit it was made for) */
	uint64	   *wgen;
	uint64		ugen;			/* the unit the source is at, counted */
	uint64		wseq;			/* dictionaries made, for their ids */
	SiftColValue *fvals;		/* the constants of one filter_str call */
	int			fcap;
	SiftAggPre	pre;			/* the histogram or rollup of the last unit_hist / unit_rollup */
	SiftSrcDict rdict[SIFT_SRC_MAX_RDIMS];	/* the dictionaries of the dims of the last unit_rollup */
} ColSource;

#define COL_MAX_STR_PRUNE	16
#define COL_MAX_WIDE		8		/* the wide conjuncts of a scan (SIFT_AGG_MAX_WIDE) */

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
	return def->lane == 0 && (def->lanes > 0 || def->type == SIFT_COLT_UUID || def->type == SIFT_COLT_BYTEA || def->type == SIFT_COLT_VARLENA);
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
		l->typid = leaf_hidden(def) ? InvalidOid : sift_col_def_typid(tupdesc, def);
		l->wide = leaf_wide(def);
		l->wtypid = sift_col_def_lane_tested(def) ? sift_col_def_typid(tupdesc, def) : InvalidOid;
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

		st->ovf[i] = (ls[i].has_ovf || (meta->stats.wbuf_ovf & SIFT_COL_OVF_BIT(i))) && def->type != SIFT_COLT_NUMERIC;
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

	memset(st, 0, sizeof(*st));
	st->nrows = meta->stats.nrows;
	st->nunits = meta->stats.nunits;
	st->bytes = meta->stats.bytes;
	st->allvisible_frac = meta->stats.allvisible_frac;
	st->exact_frac = st->allvisible_frac;
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
	src->tol = palloc0(sizeof(bool) * src->lay->nleaves);
	src->wide = palloc0(sizeof(SiftAggWide) * src->lay->nleaves);
	src->wgen = palloc0(sizeof(uint64) * src->lay->nleaves);
	src->ugen = 1;
	sift_aggsel_init(&src->sel, mcxt);
	sift_aggpre_init(&src->pre, mcxt);
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
	SiftColPred	preds[2 * SIFT_COLSRC_MAX_LEAVES + COL_MAX_STR_PRUNE + SIFT_COLSRC_MAX_FILTERS + COL_MAX_WIDE];
	int			n;

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

static void
filter_str_op(const SiftColUnit *cu, SiftColSel *sel, void *pred)
{
	sift_col_filter(cu, pred, 1, sel);
}

/* the rows whose value passes, on the encoded form of the unit: the strings are not decoded unless the prefixes cannot decide */
static bool
col_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	ColSource  *src = impl;
	SiftColPred pred;

	if (src->lay->leaf[t->leaf].lossy)
		return false;
	if (t->nvals > src->fcap)
	{
		src->fcap = Max(t->nvals, 2 * src->fcap);
		src->fvals = src->fvals ? repalloc(src->fvals, src->fcap * sizeof(SiftColValue)) : MemoryContextAlloc(src->cxt, src->fcap * sizeof(SiftColValue));
	}
	pred = str_pred(t, src->fvals);
	sift_aggsel_stored(&src->sel, src->cs, &src->unit, rows, filter_str_op, &pred);
	return true;
}

/* ---- the conjuncts on wide leaves: tested by the engine, on the unit as stored ---- */

static bool rows_left(const SiftSrcUnit *unit, const uint64 *bits);

static int
col_set_wide(void *impl, const SiftSrcWideTest *t, int nt)
{
	ColSource  *src = impl;

	if (nt > COL_MAX_WIDE)
		elog(ERROR, "sift: %d conjuncts on wide leaves, at most %d", nt, COL_MAX_WIDE);
	src->wtest = MemoryContextAlloc(src->cxt, Max(nt, 1) * sizeof(SiftColWTest));
	for (int i = 0; i < nt; i++)
		sift_colw_test(sift_col_def(src->cs, t[i].leaf), t[i].op, t[i].nvals, t[i].vals, t[i].isnull, src->cxt, &src->wtest[i]);
	src->nwtest = nt;
	return nt;
}

static void
wide_op(const SiftColUnit *cu, SiftColSel *sel, void *test)
{
	sift_colw_apply(cu, test, sel);
}

static bool
col_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows)
{
	ColSource  *src = impl;

	sift_aggsel_stored(&src->sel, src->cs, &src->unit, rows, wide_op, &src->wtest[i]);
	return rows_left(unit, rows);
}

static bool
col_next(void *impl, SiftSrcUnit *unit)
{
	ColSource  *src = impl;
	uint32		nrow;

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

static bool
col_overflow(void *impl, const SiftSrcUnit *unit, int leaf)
{
	SiftColZone cz;

	if (((ColSource *) impl)->lay->leaf[leaf].wide)
		return false;				/* a NaN is a dictionary entry */
	sift_col_zone(&((ColSource *) impl)->unit, leaf, &cz);
	return cz.has_ovf;
}

static const size_t vtype_size[] = {sizeof(int32), sizeof(int64), sizeof(double), sizeof(uint32)};
static const SiftColLane vtype_lane[] = {SIFT_COLL_I32, SIFT_COLL_I64, SIFT_COLL_F64, SIFT_COLL_CODE};

/* of the unit as stored: a compacted unit of a match set yields fewer */
static int64
col_leaf_n(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_col_lane_n(&((ColSource *) impl)->unit, leaf);
}

static void
overflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift: a column of the aggregate scan holds values its index cannot store"),
			 errhint("Set sift.enable_agg to off for this query.")));
}

/*
 * A wide leaf of the unit as a dictionary (sift_aggwide.h), made from its lanes by the first read of the leaf in the unit (the rows the executor
 * sees: those of a unit restricted to a match set are the kept ones) and shared by the reads after it, so that the executor's maps of the
 * dictionary are made once. A dictionary has an id of its own: the executor keeps what it derives by the id.
 */
static const SiftAggWide *
wide_unit(ColSource *src, int leaf)
{
	SiftAggWide *w = &src->wide[leaf];

	if (src->wgen[leaf] != src->ugen)
	{
		const SiftColDef *def = sift_col_def(src->cs, leaf);
		SiftColVec	v[SIFT_COL_MAX_LANES];

		for (int k = 0; k < Max(def->lanes, 1); k++)
			sift_aggsel_read(&src->sel, &src->unit, leaf + k, &v[k]);
		w->cxt = src->cxt;
		sift_aggwide_build(w, def, v, v[0].n);
		src->wgen[leaf] = src->ugen;
		src->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) (UINT64CONST(0x7000000000000000) + ++src->wseq), w->nd, w->off, w->bytes};
	}
	return w;
}

/* the codes of the rows `rows` (n of them; NULL: all the unit's) of a wide leaf, in the caller's storage */
static void
wide_fill(ColSource *src, int leaf, const uint32 *rows, int n, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	const SiftAggWide *w = wide_unit(src, leaf);
	uint32	   *codes = buf->data;
	int			m = rows ? n : (int) w->n;

	for (int i = 0; i < m; i++)
	{
		uint32		r = rows ? rows[i] : (uint32) i;

		codes[i] = w->codes[r];
		buf->isnull[i] = w->isnull[r];
	}
	memset(codes + m, 0, SIFT_AGG_PAD * sizeof(uint32));
	*vec = (SiftSrcVec) {.data = codes, .isnull = w->has_null ? buf->isnull : NULL, .dict = &src->dict[leaf], .n = m};
}

/* the values of a read of the engine into the caller's storage, in the vector type of the leaf */
static void
col_fill(ColSource *src, int leaf, const SiftColVec *v, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	size_t		n = v->n;

	vec->ovf = NULL;
	if (v->ovf && !buf->ovf)
		overflow_error();
	switch (l->vtype)
	{
		case SCV_I32:
			for (size_t i = 0; i < n; i++)
				((int32 *) buf->data)[i] = (int32) v->i[i];
			break;
		case SCV_I64:
			memcpy(buf->data, v->i, n * sizeof(int64));
			break;
		case SCV_F64:
			memcpy(buf->data, v->f, n * sizeof(double));
			break;
		case SCV_CODE:
			memcpy(buf->data, v->codes, n * sizeof(uint32));
			src->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) v->dict->id, v->dict->n, v->dict->off, v->dict->bytes};
			break;
	}
	memset((char *) buf->data + n * vtype_size[l->vtype], 0, SIFT_AGG_PAD * vtype_size[l->vtype]);
	vec->data = buf->data;
	vec->dict = l->vtype == SCV_CODE ? &src->dict[leaf] : NULL;
	vec->n = n;
	vec->isnull = NULL;
	if (v->ovf && !src->tol[leaf])
	{
		memcpy(buf->ovf, v->ovf, SIFT_COL_NWORDS(n) * sizeof(uint64));
		vec->ovf = buf->ovf;
	}
	if (v->nulls)
	{
		for (size_t i = 0; i < n; i++)
			buf->isnull[i] = sift_colsrc_bit(v->nulls, i);
		vec->isnull = buf->isnull;
	}
	sift_colsrc_nan_values(l, buf->data, buf->isnull, vec);
}

/* A compacted unit of a match set (sift_aggsel.h): the kept rows only, which the engine reads by their positions, not by lane */
static void
col_read_selected(ColSource *src, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	SiftColVec	v;

	sift_aggsel_read(&src->sel, &src->unit, leaf, &v);
	col_fill(src, leaf, &v, buf, vec);
}

/* the leaf decoded by the engine into the caller's storage, in the lane of the leaf's vector type */
static void
col_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	SiftColLaneInfo info;

	if (l->wide)
	{
		wide_fill(src, leaf, NULL, 0, buf, vec);
		return;
	}
	if (src->sel.keep && src->sel.compact)
	{
		col_read_selected(src, leaf, buf, vec);
		return;
	}
	sift_col_read_lane(&src->unit, leaf, vtype_lane[l->vtype], buf->data, buf->isnull, &info);
	vec->ovf = NULL;
	info.ovf &= !src->tol[leaf];
	if (info.ovf && buf->ovf)
	{
		SiftColVec	v;

		sift_col_read(&src->unit, leaf, NULL, &v);
		memcpy(buf->ovf, v.ovf, SIFT_COL_NWORDS(info.n) * sizeof(uint64));
		vec->ovf = buf->ovf;
	}
	else if (info.ovf)
		overflow_error();
	memset((char *) buf->data + info.n * vtype_size[l->vtype], 0, SIFT_AGG_PAD * vtype_size[l->vtype]);
	vec->data = buf->data;
	vec->n = info.n;
	vec->isnull = info.nulls ? buf->isnull : NULL;
	vec->dict = NULL;
	sift_colsrc_nan_values(l, buf->data, buf->isnull, vec);
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

/* The values of a read of the rows idx[0..m) of a unit of nrow rows, at their rows in the leaf's buffer; the other rows are zero */
static void
col_scatter(ColSource *src, int leaf, const SiftColVec *v, const uint32 *idx, size_t nrow, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	size_t		m = v->n;
	ColLeafBuf *b = leaf_buf(src, leaf, nrow);

	memset(b->data, 0, (nrow + SIFT_AGG_PAD) * vtype_size[l->vtype]);
	switch (l->vtype)
	{
		case SCV_I32:
			for (size_t k = 0; k < m; k++)
				((int32 *) b->data)[idx[k]] = (int32) v->i[k];
			break;
		case SCV_I64:
			for (size_t k = 0; k < m; k++)
				((int64 *) b->data)[idx[k]] = v->i[k];
			break;
		case SCV_F64:
			for (size_t k = 0; k < m; k++)
				((double *) b->data)[idx[k]] = v->f[k];
			break;
		case SCV_CODE:
			for (size_t k = 0; k < m; k++)
				((uint32 *) b->data)[idx[k]] = v->codes[k];
			src->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) v->dict->id, v->dict->n, v->dict->off, v->dict->bytes};
			break;
	}
	vec->data = b->data;
	vec->dict = l->vtype == SCV_CODE ? &src->dict[leaf] : NULL;
	vec->n = nrow;
	vec->isnull = NULL;
	vec->ovf = NULL;
	if (v->ovf && !src->tol[leaf])
	{
		memset(b->ovf, 0, SIFT_COL_NWORDS(nrow) * sizeof(uint64));
		for (size_t k = 0; k < m; k++)
			if (sift_colsrc_bit(v->ovf, k))
				b->ovf[idx[k] >> 6] |= UINT64CONST(1) << (idx[k] & 63);
		vec->ovf = b->ovf;
	}
	if (v->nulls)
	{
		memset(b->isnull, 0, nrow);
		for (size_t k = 0; k < m; k++)
			b->isnull[idx[k]] = sift_colsrc_bit(v->nulls, k);
		vec->isnull = b->isnull;
	}
	sift_colsrc_nan_values(l, b->data, b->isnull, vec);
}

/* A selection of the unit's rows, of the bits of the executor */
static void
col_sel(ColSource *src, const SiftSrcUnit *unit, const uint64 *bits, SiftColSel *sel)
{
	sift_col_sel_alloc(src->cs, unit->nrow, sel);
	memcpy(sel->bits, bits, sel->nwords * sizeof(uint64));
	sift_col_sel_count(sel);
}

static void
col_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	SiftColSel	sel;
	SiftColVec	v;
	uint32	   *idx;

	if ((src->sel.keep && src->sel.compact) || src->lay->leaf[leaf].wide)	/* the rows of a compacted unit are not the engine's: the whole leaf */
	{
		col_read(impl, unit, leaf, vec);
		return;
	}
	col_sel(src, unit, bits, &sel);
	idx = MemoryContextAlloc(src->cxt, (sel.count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(&sel, idx);
	sift_col_read(&src->unit, leaf, &sel, &v);
	col_scatter(src, leaf, &v, idx, unit->nrow, vec);
	pfree(idx);
}

/* the rows ascend in the numbering the executor sees; those of a compacted unit are the idx-th rows of the unit as stored */
static void
col_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	uint32	   *ords;
	SiftColVec	v;
	ColLeafBuf *b;

	if (src->lay->leaf[leaf].wide)
	{
		b = leaf_buf(src, leaf, n);
		wide_fill(src, leaf, rows, n, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
		return;
	}
	ords = palloc((n + SIFT_COL_IDXPAD) * sizeof(uint32));
	for (int i = 0; i < n; i++)
		ords[i] = src->sel.keep && src->sel.compact ? src->sel.idx[rows[i]] : rows[i];
	memset(ords + n, 0, SIFT_COL_IDXPAD * sizeof(uint32));
	sift_col_read_idx(&src->unit, leaf, ords, n, &v);
	b = leaf_buf(src, leaf, n);
	col_fill(src, leaf, &v, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
	pfree(ords);
}

static void
col_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;

	sift_aggsel_live(&src->sel, &src->unit, bits);
}

static void
col_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;

	sift_aggsel_vet(&src->sel, src->cs, &src->unit, bits);
}

static bool
col_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	ColSource  *src = impl;

	return sift_aggsel_vet_needed(&src->unit);
}

/* ---- integer filters in the source: the conjuncts run on the encoded chunks (docs/COL-PLAN.md, "Filters in the source") ---- */

static const SiftColOp filter_ops[] = {
	[SSF_EQ] = SIFT_COLOP_EQ, [SSF_NE] = SIFT_COLOP_NE, [SSF_LT] = SIFT_COLOP_LT, [SSF_LE] = SIFT_COLOP_LE, [SSF_GT] = SIFT_COLOP_GT,
	[SSF_GE] = SIFT_COLOP_GE, [SSF_IN] = SIFT_COLOP_IN, [SSF_NOTIN] = SIFT_COLOP_NOTIN, [SSF_NOTNULL] = SIFT_COLOP_NOTNULL
};

/* the engine predicate of a filter; false when the leaf is not one the engine tests as integers on its chunks */
static bool
filter_pred(ColSource *src, const SiftSrcFilter *f, SiftColPred *p)
{
	const SiftColDef *def = sift_col_def(src->cs, f->leaf);
	bool		set = f->op == SSF_IN || f->op == SSF_NOTIN;
	int			nvals = f->op == SSF_NOTNULL ? 0 : set ? f->nset : 1;
	SiftColValue *vals;

	if (def->parent >= 0 || def->store != SIFT_COLS_INT)
		return false;
	vals = MemoryContextAllocZero(src->cxt, Max(nvals, 1) * sizeof(SiftColValue));
	for (int i = 0; i < nvals; i++)
		vals[i].v.i = set ? f->set[i] : f->val;
	*p = (SiftColPred) {f->leaf, filter_ops[f->op], nvals, vals};
	return true;
}

static uint64
col_set_filters(void *impl, const SiftSrcFilter *f, int nf)
{
	ColSource  *src = impl;
	uint64		ok = 0;

	src->fpred = MemoryContextAlloc(src->cxt, Max(nf, 1) * sizeof(SiftColPred));
	src->nflt = 0;
	for (int i = 0; i < nf; i++)
		if (filter_pred(src, &f[i], &src->fpred[src->nflt]))
		{
			src->nflt++;
			ok |= UINT64CONST(1) << i;
		}
	return ok;
}

/*
 * The share of the unit's rows the predicate keeps, from the zone of its leaf (the values spread evenly between min and max): what orders
 * the filters of a unit, the most selective first, and tells nothing else
 */
static double
filter_share(ColSource *src, const SiftColPred *p)
{
	SiftColZone z;
	double		span,
				live,
				c = p->nvals > 0 ? (double) p->vals[0].v.i : 0.0;

	sift_col_zone(&src->unit, p->col, &z);
	live = z.nvals > 0 ? (double) (z.nvals - z.nnull) / z.nvals : 0.0;
	if (p->op == SIFT_COLOP_NOTNULL || z.nnull == z.nvals || z.has_ovf)
		return live;
	span = (double) z.max - (double) z.min + 1.0;
	switch (p->op)
	{
		case SIFT_COLOP_EQ:
			return live / span;
		case SIFT_COLOP_NE:
			return live * (1.0 - 1.0 / span);
		case SIFT_COLOP_IN:
			return live * Min(1.0, p->nvals / span);
		case SIFT_COLOP_NOTIN:
			return live * Max(0.0, 1.0 - p->nvals / span);
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
			return live * Min(1.0, Max(0.0, (c - z.min + (p->op == SIFT_COLOP_LE)) / span));
		default:
			return live * Min(1.0, Max(0.0, (z.max - c + (p->op == SIFT_COLOP_GE)) / span));
	}
}

/*
 * The engine keeps the rows whose value overflowed (stored as NULL, flagged): for a numeric leaf those are the NaNs, which pass the
 * comparisons that a number greater than all others passes (> >= <> and "not null") and no other. The others are cleared from sel.
 */
static void
clear_nan_rows(ColSource *src, const SiftColPred *p, SiftColSel *sel)
{
	SiftColVec	v;

	if (p->op == SIFT_COLOP_GT || p->op == SIFT_COLOP_GE || p->op == SIFT_COLOP_NE || p->op == SIFT_COLOP_NOTNULL || p->op == SIFT_COLOP_NOTIN)
		return;
	sift_col_read(&src->unit, p->col, NULL, &v);
	for (uint32 w = 0; v.ovf && w < sel->nwords; w++)
		sel->bits[w] &= ~v.ovf[w];
	sift_col_sel_count(sel);
}

/* the filters of the source on the unit as stored, the most selective first */
static void
filter_op(const SiftColUnit *cu, SiftColSel *sel, void *arg)
{
	ColSource  *src = arg;
	double		share[SIFT_COLSRC_MAX_FILTERS];
	int			order[SIFT_COLSRC_MAX_FILTERS];

	for (int i = 0; i < src->nflt; i++)
	{
		int			j = i;

		share[i] = filter_share(src, &src->fpred[i]);
		for (; j > 0 && share[order[j - 1]] > share[i]; j--)
			order[j] = order[j - 1];
		order[j] = i;
	}
	for (int i = 0; i < src->nflt; i++)
	{
		const SiftColPred *p = &src->fpred[order[i]];
		SiftColZone z;

		sift_col_zone(cu, p->col, &z);
		z.has_ovf &= !src->tol[p->col];
		if (!sift_col_filter(cu, p, 1, sel))
			break;
		if (z.has_ovf && sift_colsrc_nan_leaf(&src->lay->leaf[p->col]))
			clear_nan_rows(src, p, sel);
	}
}

static bool
rows_left(const SiftSrcUnit *unit, const uint64 *bits)
{
	uint64		left = 0;

	for (int w = 0; w < (unit->nrow + 63) / 64; w++)
		left += sift_pc64(bits[w]);
	return left > 0;
}

static bool
col_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;

	sift_aggsel_stored(&src->sel, src->cs, &src->unit, bits, filter_op, src);
	return rows_left(unit, bits);
}

/* ---- the JSON type of the values of a leaf (docs/COL-PLAN.md, "The JSON type of a path"): the tags of the overflow values of the chunks ---- */

StaticAssertDecl(SIFT_COLTAG_STRING == 1 && SIFT_COLTAG_JNULL == 7, "SIFT_SRC_JT_* are the chunk tags shifted by one");

static void
col_tolerate_ovf(void *impl, int leaf)
{
	((ColSource *) impl)->tol[leaf] = true;
}

/* the tag of the values a leaf of this type stores */
static uint8
stored_tag(Oid typid)
{
	return typid == BOOLOID ? SIFT_COLTAG_BOOLEAN : typid == TEXTOID ? SIFT_COLTAG_STRING : SIFT_COLTAG_NUMBER;
}

static void
types_unknown_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift: a column of the aggregate scan holds values whose JSON type its index did not store"),
			 errhint("REINDEX the index to store them, or set sift.enable_agg to off for this query.")));
}

typedef struct TypesTest
{
	ColSource  *src;
	int			leaf;
	uint32		mask;			/* bit t: the type of tag t */
	bool		read;			/* the query reads the leaf elsewhere */
} TypesTest;

/* the rows of the unit whose value has one of the types; a row whose value of those types the leaf cannot hold is an error if a snapshot sees it and the query reads the leaf */
static void
types_op(const SiftColUnit *cu, SiftColSel *vs, void *arg)
{
	TypesTest  *t = arg;
	bool		known,
				left;
	uint64	   *ovf;
	SiftColSel	bad;

	left = sift_col_filter_types(cu, t->leaf, t->mask, stored_tag(t->src->lay->leaf[t->leaf].typid), t->src->lay->leaf[t->leaf].astext, vs, &known);
	if (!known)
		types_unknown_error();
	ovf = palloc(SIFT_COL_NWORDS(cu->nrows) * sizeof(uint64));
	if (!left || !t->read || !sift_col_ovf_bits(cu, t->leaf, ovf))
		return;
	sift_col_sel_alloc(t->src->cs, cu->nrows, &bad);
	for (uint32 w = 0; w < bad.nwords; w++)
		bad.bits[w] = vs->bits[w] & ovf[w];
	if (sift_col_sel_count(&bad) > 0 && sift_col_vet(cu, &bad))
		overflow_error();
}

static bool
col_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits)
{
	ColSource  *src = impl;
	TypesTest	t = {src, leaf, (types & SIFT_SRC_JT_ALL) << 1, (types & SIFT_SRC_JT_READ) != 0};

	sift_aggsel_stored(&src->sel, src->cs, &src->unit, bits, types_op, &t);
	return rows_left(unit, bits);
}

/* a chunk tag as read_types gives it: the index of the name jsonb_typeof gives, or none */
static uint8
src_type(uint8 tag)
{
	return tag == SIFT_COLTAG_NONE ? SIFT_SRC_TYPE_NONE : tag == SIFT_COLTAG_JNULL ? 5 : tag - 1;
}

/* the JSON type of the value of the leaf in each row of the unit; the rows of a compacted unit of a match set are the kept ones */
static void
col_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags)
{
	ColSource  *src = impl;
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	bool		compact = src->sel.keep && src->sel.compact;
	uint8	   *all = compact ? palloc(src->sel.nrow) : tags;

	if (!sift_col_row_types(&src->unit, leaf, stored_tag(l->typid), l->astext, all))
		types_unknown_error();
	for (uint32 i = 0; i < unit->nrow; i++)
		tags[i] = src_type(all[compact ? src->sel.idx[i] : i]);
}

/* the JSON nulls among the slots of the leaf, in the numbering of the unit the executor sees (the elements of the kept rows in a compacted one) */
static void
col_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits)
{
	ColSource  *src = impl;
	bool		compact = src->sel.keep && src->sel.compact,
				known;

	sift_col_jnull_bits(&src->unit, leaf, compact ? src->sel.idx : NULL, src->sel.sel.count, bits, &known);
	if (!known)
		types_unknown_error();
}

static void
col_counters(const void *impl, SiftSrcCounters *c)
{
	const SiftColCounters *cc = sift_col_counters(((const ColSource *) impl)->cs);

	c->units = cc->units;
	c->units_pruned = cc->units_pruned + ((const ColSource *) impl)->sel.units_skipped;
	c->rows = cc->rows_scanned;
	c->bytes = cc->bytes_decoded;
	c->rows_vetted = cc->rows_vetted;
	c->heap_pages = cc->heap_pages;
}

/* ---- pre-aggregates (src/col/sift_col_pagg.c, docs/COL-PREAGG.md) ---- */

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

/* ---- the Vulkan path (src/col/sift_col_gpu.h, docs/GPU.md) ---- */

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

#endif							/* __has_include("sift_col.h") */
