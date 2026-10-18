/*
 * sift_aggcol.c - the column source of the aggregate scan over a sift_col index (sift_colsrc.h over the column engine of
 * docs/COL.md, src/sift_col.h): the leaf list becomes the layout, a unit's selection-based reads become whole-unit vectors of the
 * types the kernels take (int64 narrowed to int32 for the leaves of SCV_I32, NULL bitmaps expanded to flags), liveness and MVCC
 * vetting are the engine's. Compiled only when the engine is part of the build.
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
#include "sift_aggsel.h"
#include "sift_col.h"
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
	SiftAggSel	sel;			/* the match set the units are restricted to */
	int			nflt;			/* the filters set_filters accepted, as engine predicates */
	SiftColPred *fpred;
	SiftColPred *spred;			/* the text tests of prune_str, as column predicates */
	int			nspred;
	SiftColValue *fvals;		/* the constants of one filter_str call */
	int			fcap;
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

static void
col_stats(Relation index, SiftSrcStats *st)
{
	SiftColIndexMeta *meta = sift_col_index_meta(index);

	memset(st, 0, sizeof(*st));
	st->nrows = meta->stats.nrows;
	st->nunits = meta->stats.nunits;
	st->bytes = meta->stats.bytes;
	st->allvisible_frac = meta->stats.allvisible_frac;
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
	sift_aggsel_init(&src->sel, mcxt);
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
	SiftColPred	preds[2 * SIFT_COLSRC_MAX_LEAVES + COL_MAX_STR_PRUNE + SIFT_COLSRC_MAX_FILTERS];

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
	memcpy(preds + 2 * nprune + src->nspred, src->fpred, src->nflt * sizeof(SiftColPred));
	sift_col_begin(src->cs, preds, 2 * nprune + src->nspred + src->nflt, sb_lo, sb_hi);
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

static bool
col_next(void *impl, SiftSrcUnit *unit)
{
	ColSource  *src = impl;
	uint32		nrow;

	do
	{
		if (!sift_col_next(src->cs, &src->unit))
			return false;
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

static const size_t vtype_size[] = {sizeof(int32), sizeof(int64), sizeof(double), sizeof(uint32)};

/* the leaf's buffers for n elements; the tail up to SIFT_AGG_PAD elements is zero */
static ColLeafBuf *
leaf_buf(ColSource *src, int leaf, SiftSrcVecType vt, size_t n)
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
	memset((char *) b->data + n * vtype_size[vt], 0, SIFT_AGG_PAD * vtype_size[vt]);
	return b;
}

/* the only value a numeric(p <= 18, s) leaf cannot hold is NaN: the executor evaluates those rows as the numeric NaN they are */
static bool
nan_leaf(const SiftSrcLeaf *l)
{
	return l->typid == NUMERICOID && l->kind == SCL_SCALAR;
}

static void
overflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift: a column of the aggregate scan holds values its index cannot store"),
			 errhint("Set sift.enable_agg to off for this query.")));
}

/* The values of a whole-leaf read into the leaf's buffer */
static void
col_fill(ColSource *src, int leaf, const SiftColVec *v, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	size_t		n = v->n;
	ColLeafBuf *b;

	if (v->ovf && !nan_leaf(l))
		overflow_error();
	b = leaf_buf(src, leaf, l->vtype, n);
	switch (l->vtype)
	{
		case SCV_I32:
			for (size_t i = 0; i < n; i++)
				((int32 *) b->data)[i] = (int32) v->i[i];
			break;
		case SCV_I64:
			memcpy(b->data, v->i, n * sizeof(int64));
			break;
		case SCV_F64:
			memcpy(b->data, v->f, n * sizeof(double));
			break;
		case SCV_CODE:
			memcpy(b->data, v->codes, n * sizeof(uint32));
			break;
	}
	vec->data = b->data;
	vec->n = n;
	vec->isnull = NULL;
	vec->ovf = NULL;
	if (v->ovf)
	{
		memcpy(b->ovf, v->ovf, SIFT_COL_NWORDS(n) * sizeof(uint64));
		vec->ovf = b->ovf;
	}
	if (v->nulls)
	{
		for (size_t i = 0; i < n; i++)
			b->isnull[i] = sift_colsrc_bit(v->nulls, i);
		vec->isnull = b->isnull;
	}
}

/* The values of a read of the rows idx[0..m) of a unit of nrow rows, at their rows in the leaf's buffer; the other rows are zero */
static void
col_scatter(ColSource *src, int leaf, const SiftColVec *v, const uint32 *idx, size_t nrow, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	size_t		m = v->n;
	ColLeafBuf *b;

	if (v->ovf && !nan_leaf(l))
		overflow_error();
	b = leaf_buf(src, leaf, l->vtype, nrow);
	memset(b->data, 0, nrow * vtype_size[l->vtype]);
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
			break;
	}
	vec->data = b->data;
	vec->n = nrow;
	vec->isnull = NULL;
	vec->ovf = NULL;
	if (v->ovf)
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
}

/* the dictionary of a text leaf read last */
static void
col_dict(ColSource *src, int leaf, const SiftColVec *v, SiftSrcVec *vec)
{
	SiftSrcDict *d = &src->dict[leaf];

	vec->dict = NULL;
	if (src->lay->leaf[leaf].vtype != SCV_CODE)
		return;
	d->id = (const void *) (uintptr_t) v->dict->id;
	d->n = v->dict->n;
	d->off = v->dict->off;
	d->bytes = v->dict->bytes;
	vec->dict = d;
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

	col_sel(src, unit, bits, &sel);
	idx = MemoryContextAlloc(src->cxt, (sel.count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(&sel, idx);
	sift_col_read(&src->unit, leaf, &sel, &v);
	col_scatter(src, leaf, &v, idx, unit->nrow, vec);
	col_dict(src, leaf, &v, vec);
	pfree(idx);
}

static void
col_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	SiftColVec	v;

	sift_aggsel_read(&src->sel, &src->unit, leaf, &v);
	col_fill(src, leaf, &v, vec);
	col_dict(src, leaf, &v, vec);
}

/* the rows ascend in the numbering the executor sees; those of a compacted unit are the idx-th rows of the unit as stored */
static void
col_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	uint32	   *ords = palloc((n + SIFT_COL_IDXPAD) * sizeof(uint32));
	SiftColVec	v;

	for (int i = 0; i < n; i++)
		ords[i] = src->sel.keep && src->sel.compact ? src->sel.idx[rows[i]] : rows[i];
	memset(ords + n, 0, SIFT_COL_IDXPAD * sizeof(uint32));
	sift_col_read_idx(&src->unit, leaf, ords, n, &v);
	col_fill(src, leaf, &v, vec);
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

static bool
col_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;
	SiftColSel	sel;
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
	col_sel(src, unit, bits, &sel);
	for (int i = 0; i < src->nflt; i++)
	{
		const SiftColPred *p = &src->fpred[order[i]];
		SiftColZone z;

		sift_col_zone(&src->unit, p->col, &z);
		if (z.has_ovf && !nan_leaf(&src->lay->leaf[p->col]))
			overflow_error();
		if (!sift_col_filter(&src->unit, p, 1, &sel))
			break;
		if (z.has_ovf)
			clear_nan_rows(src, p, &sel);
	}
	memcpy(bits, sel.bits, sel.nwords * sizeof(uint64));
	return sel.count > 0;
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
	.select = col_select,
	.tids = col_tids,
	.zone = col_zone,
	.read_rows = col_read_rows,
	.set_filters = col_set_filters,
	.filter = col_filter,
	.read_sel = col_read_sel,
	.prune_str = col_prune_str,
	.filter_str = col_filter_str,
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
