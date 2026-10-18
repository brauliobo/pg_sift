/*
 * sift_aggcol.c - the column source of the aggregate scan over a sift_col index: the leaf list becomes the layout, a unit's selection-based reads become whole-unit vectors of the
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
} ColSource;

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
	SiftColValue vals[2 * SIFT_COLSRC_MAX_LEAVES] = {{{0}}};
	SiftColPred	preds[2 * SIFT_COLSRC_MAX_LEAVES];

	/* a zone [lo, hi] is two comparisons: the unit is skipped when its min/max exclude either */
	for (int k = 0; k < nprune; k++)
	{
		vals[2 * k].v.i = prune[k].lo;
		vals[2 * k + 1].v.i = prune[k].hi;
		preds[2 * k] = (SiftColPred) {prune[k].leaf, SIFT_COLOP_GE, 1, &vals[2 * k]};
		preds[2 * k + 1] = (SiftColPred) {prune[k].leaf, SIFT_COLOP_LE, 1, &vals[2 * k + 1]};
	}
	sift_col_begin(((ColSource *) impl)->cs, preds, 2 * nprune, sb_lo, sb_hi);
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
		}
		b->cap = Max(need, 4096);
		b->data = MemoryContextAlloc(src->cxt, b->cap * 8);
		b->isnull = MemoryContextAlloc(src->cxt, b->cap);
	}
	memset((char *) b->data + n * vtype_size[vt], 0, SIFT_AGG_PAD * vtype_size[vt]);
	return b;
}

static void
col_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	SiftColVec	v;
	ColLeafBuf *b;
	size_t		n;

	sift_aggsel_read(&src->sel, &src->unit, leaf, &v);
	n = v.n;
	if (v.ovf)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift: a column of the aggregate scan holds values its index cannot store"),
				 errhint("Set sift.enable_agg to off for this query.")));
	b = leaf_buf(src, leaf, l->vtype, n);
	switch (l->vtype)
	{
		case SCV_I32:
			for (size_t i = 0; i < n; i++)
				((int32 *) b->data)[i] = (int32) v.i[i];
			break;
		case SCV_I64:
			memcpy(b->data, v.i, n * sizeof(int64));
			break;
		case SCV_F64:
			memcpy(b->data, v.f, n * sizeof(double));
			break;
		case SCV_CODE:
			memcpy(b->data, v.codes, n * sizeof(uint32));
			src->dict[leaf].id = (const void *) (uintptr_t) v.dict->id;
			src->dict[leaf].n = v.dict->n;
			src->dict[leaf].off = v.dict->off;
			src->dict[leaf].bytes = v.dict->bytes;
			break;
	}
	vec->data = b->data;
	vec->dict = l->vtype == SCV_CODE ? &src->dict[leaf] : NULL;
	vec->n = n;
	vec->isnull = NULL;
	if (v.nulls)
	{
		for (size_t i = 0; i < n; i++)
			b->isnull[i] = sift_colsrc_bit(v.nulls, i);
		vec->isnull = b->isnull;
	}
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
