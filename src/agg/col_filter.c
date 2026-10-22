/*
 * agg/col_filter.c - the filters, text tests and JSON type tests of the column source, run on the encoded chunks of the engine
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
	SIFT_INVARIANT(false, "sift: unknown text test %d", (int) op);
	pg_unreachable();
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
void
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
bool
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

/* ---- the conjuncts on wide leaves (docs/COL-LEAFTYPES-32.md 5): tested by the engine, on the unit as stored ---- */

static bool rows_left(const SiftSrcUnit *unit, const uint64 *bits);

int
col_set_wide(void *impl, const SiftSrcWideTest *t, int nt)
{
	ColSource  *src = impl;

	SIFT_INVARIANT(nt <= COL_MAX_WIDE, "sift: %d conjuncts on wide leaves, at most %d", nt, COL_MAX_WIDE);
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

bool
col_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows)
{
	ColSource  *src = impl;

	sift_aggsel_stored(&src->sel, src->cs, &src->unit, rows, wide_op, &src->wtest[i]);
	return rows_left(unit, rows);
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

uint64
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
	return sift_aggsel_rows_left(bits, unit->nrow);
}

bool
col_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;

	sift_aggsel_stored(&src->sel, src->cs, &src->unit, bits, filter_op, src);
	return rows_left(unit, bits);
}

/* ---- the JSON type of the values of a leaf (docs/COL-PLAN.md, "The JSON type of a path"): the tags of the overflow values of the chunks ---- */

void
col_tolerate_ovf(void *impl, int leaf)
{
	((ColSource *) impl)->tol[leaf] = true;
}

bool
col_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits)
{
	ColSource  *src = impl;
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];

	sift_aggsel_filter_types(&src->sel, src->cs, &src->unit, leaf, l->typid, l->astext, types, bits);
	return rows_left(unit, bits);
}

/* the JSON type of the value of the leaf in each row of the unit; the rows of a compacted unit of a match set are the kept ones */
void
col_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags)
{
	ColSource  *src = impl;
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];

	sift_aggsel_read_types(&src->sel, &src->unit, leaf, l->typid, l->astext, unit->nrow, tags);
}

/* the JSON nulls among the slots of the leaf, in the numbering of the unit the executor sees (the elements of the kept rows in a compacted one) */
void
col_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits)
{
	ColSource  *src = impl;
	bool		compact = src->sel.compact,
				known;

	sift_col_jnull_bits(&src->unit, leaf, compact ? src->sel.idx : NULL, src->sel.sel.count, bits, &known);
	if (!known)
		sift_aggsel_types_unknown_error();
}

void
col_counters(const void *impl, SiftSrcCounters *c)
{
	const SiftColCounters *cc = sift_col_counters(((const ColSource *) impl)->cs);

	c->units = cc->units;
	c->units_search_pruned = ((const ColSource *) impl)->sel.units_skipped;
	c->units_pruned = cc->units_pruned + c->units_search_pruned;
	c->units_set_pruned = cc->units_set;
	c->rows = cc->rows_scanned;
	c->bytes = cc->bytes_decoded;
	c->rows_vetted = cc->rows_vetted;
	c->heap_pages = cc->heap_pages;
	c->rechecked = ((const ColSource *) impl)->rck ? ((const ColSource *) impl)->rck->nrechecked : 0;
	c->recheck_units = ((const ColSource *) impl)->rck ? ((const ColSource *) impl)->rck->nunits : 0;
}

#endif							/* __has_include("col/col.h") */
