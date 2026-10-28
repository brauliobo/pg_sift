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

/* the tests join the zone pruning of every begin: the segment dictionary's, or the prefix zone of an inline unit */
void
col_prune_str(void *impl, const SiftSrcStrTest *t, int nt)
{
	ColSource  *src = impl;

	src->spred = MemoryContextAlloc(src->cxt, Min(nt, COL_MAX_STR_PRUNE) * sizeof(SiftColPred));
	src->spmap = MemoryContextAlloc(src->cxt, Min(nt, COL_MAX_STR_PRUNE) * sizeof(int));
	for (int i = 0; i < nt && src->nspred < COL_MAX_STR_PRUNE; i++)
		if (!src->lay->leaf[t[i].leaf].lossy)
		{
			src->spmap[src->nspred] = i;
			src->spred[src->nspred++] = sift_aggfilt_str_pred(&t[i], MemoryContextAlloc(src->cxt, t[i].nvals * sizeof(SiftColValue)), t[i].leaf);
		}
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
	pred = sift_aggfilt_str_pred(t, src->fvals, t->leaf);
	sift_aggsel_stored(&src->sel, src->cs, &src->unit, rows, filter_str_op, &pred);
	return true;
}

/* ---- the conjuncts on wide leaves: tested by the engine, on the unit as stored ---- */

static bool rows_left(const SiftSrcUnit *unit, const uint64 *bits);

int
col_set_wide(void *impl, const SiftSrcWideTest *t, int nt)
{
	ColSource  *src = impl;

	SIFT_INVARIANT(nt <= COL_MAX_WIDE, "sift: %d conjuncts on wide leaves, at most %d", nt, COL_MAX_WIDE);
	src->wtest = MemoryContextAlloc(src->cxt, Max(nt, 1) * sizeof(SiftColWTest));
	src->wpred = MemoryContextAlloc(src->cxt, Max(nt, 1) * sizeof(SiftColPred));
	src->nwpred = 0;
	for (int i = 0; i < nt; i++)
	{
		const SiftColPred *alt[2];
		int			npred[2];
		SiftColValue *v = MemoryContextAllocZero(src->cxt, sizeof(SiftColValue));

		sift_colw_test(sift_col_def(src->cs, t[i].leaf), src->lay->leaf[t[i].leaf].typid, t[i].op, t[i].nvals, t[i].vals, t[i].isnull, src->cxt, &src->wtest[i]);
		v->v.s = (const char *) &src->wtest[i];
		if (src->wtest[i].verdict == SIFT_COLW_PRED)
			src->wpred[src->nwpred++] = src->wtest[i].pred;
		else if (sift_colw_alts(&src->wtest[i], alt, npred) > 0)
			src->wpred[src->nwpred++] = (SiftColPred) {src->wtest[i].pred.col, SIFT_COLOP_SHAPE, 1, v};
	}
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

/* ---- integer filters in the source: the conjuncts run on the encoded chunks (docs/COL-PLAN.md, "Filters in the source") ---- */

uint64
col_set_filters(void *impl, const SiftSrcFilter *f, int nf)
{
	ColSource  *src = impl;
	uint64		ok = 0;

	src->flt = (SiftAggFilters) {0};
	for (int i = 0; i < nf; i++)
		if (sift_aggfilt_int(sift_col_def(src->cs, f[i].leaf)))
		{
			sift_aggfilt_add(&src->flt, src->cxt, f[i].leaf, &f[i]);
			ok |= UINT64CONST(1) << i;
		}
	return ok;
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

	SiftAggFiltRun run = {&src->flt, src->lay, src->tol};

	sift_aggsel_stored(&src->sel, src->cs, &src->unit, bits, sift_aggfilt_op, &run);
	return rows_left(unit, bits);
}

/* ---- the JSON type of the values of a leaf (docs/COL-PLAN.md, "The JSON type of a path"): the tags of the overflow values of the chunks ---- */

void
col_tolerate_ovf(void *impl, int leaf)
{
	((ColSource *) impl)->tol[leaf] = true;
}

void
col_set_raw(void *impl, int leaf)
{
	((ColSource *) impl)->raw[leaf] = true;
}

void
col_set_numsum(void *impl, int leaf)
{
	((ColSource *) impl)->numsum[leaf] = true;
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
	c->point_units = cc->point_units;
	c->point_rows = cc->point_rows;
	c->run_windows = cc->run_windows;
}

#endif							/* __has_include("col/col.h") */
