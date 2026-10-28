/*
 * agg/chunkfilter.c - the integer filters of a scan on the encoded chunks of a unit (agg/chunkfilter.h).
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/chunkfilter.h"
#include "agg/sel.h"

static const SiftColOp filter_ops[] = {
	[SSF_EQ] = SIFT_COLOP_EQ, [SSF_NE] = SIFT_COLOP_NE, [SSF_LT] = SIFT_COLOP_LT, [SSF_LE] = SIFT_COLOP_LE, [SSF_GT] = SIFT_COLOP_GT,
	[SSF_GE] = SIFT_COLOP_GE, [SSF_IN] = SIFT_COLOP_IN, [SSF_NOTIN] = SIFT_COLOP_NOTIN, [SSF_NOTNULL] = SIFT_COLOP_NOTNULL
};

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
		case SSTR_CONTAINS:
			return SIFT_COLOP_CONTAINS;
		case SSTR_NOTCONTAINS:
			return SIFT_COLOP_NOTCONTAINS;
		case SSTR_ICONTAINS:
			return SIFT_COLOP_ICONTAINS;
	}
	SIFT_INVARIANT(false, "sift: unknown text test %d", (int) op);
	pg_unreachable();
}

/* a text test as a column predicate over the constants in vals (room for t->nvals) */
SiftColPred
sift_aggfilt_str_pred(const SiftSrcStrTest *t, SiftColValue *vals, int col)
{
	for (int i = 0; i < t->nvals; i++)
		vals[i] = t->ints ? (SiftColValue) {.v.i = t->ints[i]} : (SiftColValue) {.v.s = t->vals[i], .len = t->lens[i]};
	return (SiftColPred) {col, str_op(t->op), t->nvals, vals};
}
bool
sift_aggfilt_int(const SiftColDef *def)
{
	return def->parent < 0 && def->store == SIFT_COLS_INT;
}

void
sift_aggfilt_add(SiftAggFilters *fl, MemoryContext cxt, int col, const SiftSrcFilter *f)
{
	bool		set = f->op == SSF_IN || f->op == SSF_NOTIN;
	int			nvals = f->op == SSF_NOTNULL ? 0 : set ? f->nset : 1;
	SiftColValue *vals;

	if (f->op == SSF_RANGES || f->op == SSF_RANGES_HINT)
	{
		if (fl->rng == NULL)
			fl->rng = MemoryContextAlloc(cxt, SIFT_COLSRC_MAX_FILTERS * sizeof(SiftAggRanges));
		fl->rng[fl->nrng++] = (SiftAggRanges) {col, f->leaf, f->nset / 2, f->set, f->op == SSF_RANGES_HINT};
		return;
	}
	vals = MemoryContextAllocZero(cxt, Max(nvals, 1) * sizeof(SiftColValue));
	if (fl->pred == NULL)
	{
		fl->pred = MemoryContextAlloc(cxt, SIFT_COLSRC_MAX_FILTERS * sizeof(SiftColPred));
		fl->leaf = MemoryContextAlloc(cxt, SIFT_COLSRC_MAX_FILTERS * sizeof(int));
	}
	for (int i = 0; i < nvals; i++)
		vals[i].v.i = set ? f->set[i] : f->val;
	fl->pred[fl->n] = (SiftColPred) {col, filter_ops[f->op], nvals, vals};
	fl->leaf[fl->n++] = f->leaf;
}

/*
 * The share of the unit's rows the predicate keeps, from the zone of its leaf (the values spread evenly between min and max): what orders
 * the filters of a unit, the most selective first, and tells nothing else
 */
static double
filter_share(const SiftColUnit *cu, const SiftColPred *p)
{
	SiftColZone z;
	double		span,
				live,
				c = p->nvals > 0 ? (double) p->vals[0].v.i : 0.0;

	sift_col_zone(cu, p->col, &z);
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
clear_nan_rows(const SiftColUnit *cu, const SiftColPred *p, SiftColSel *sel)
{
	SiftColVec	v;

	if (p->op == SIFT_COLOP_GT || p->op == SIFT_COLOP_GE || p->op == SIFT_COLOP_NE || p->op == SIFT_COLOP_NOTNULL || p->op == SIFT_COLOP_NOTIN)
		return;
	sift_col_read(cu, p->col, NULL, &v);
	for (uint32 w = 0; v.ovf && w < sel->nwords; w++)
		sel->bits[w] &= ~v.ovf[w];
	sift_col_sel_count(sel);
}

/* a unit with more of the ranges of a prefilter than this is not narrowed by it: a search for each costs more than the test it saves */
#define RANGES_UNIT_ARMS	32

/* the arms [*first, *end) of a union of ranges that a zone [min, max] meets (the arms are ascending and apart) */
static void
zone_arms(const int64 *arm, int narms, int64 min, int64 max, int *first, int *end)
{
	int			lo = 0,
				hi = narms;

	while (lo < hi)
	{
		int			m = (lo + hi) / 2;

		if (arm[2 * m + 1] < min)
			lo = m + 1;
		else
			hi = m;
	}
	*first = lo;
	for (hi = lo; hi < narms && arm[2 * hi] <= max; hi++)
		;
	*end = hi;
}

/* the prefilter of a hint does not narrow a unit whose zone is unknown (NULLs only, an overflow) or that meets too many arms */
static bool
hint_narrows(bool zoned, int first, int end)
{
	return zoned && end - first <= RANGES_UNIT_ARMS;
}

/* does filter_ranges narrow, with this zone of the unit, the rows of a hint filter to the rows of its ranges? Then the executor need not test them again */
bool
sift_aggfilt_hint_narrows(const SiftSrcFilter *f, const SiftSrcZone *z)
{
	int			first,
				end;

	zone_arms(f->set, f->nset / 2, z->min, z->max, &first, &end);
	return hint_narrows(z->nnull < z->nvals && !z->inexact, first, end);
}

/*
 * A union of ranges on one leaf: each arm narrows a copy of sel the way a single range does (two bounds, the sorted run window), and sel becomes the rows
 * of any arm. An arm without a bound keeps the rows with a value.
 */
static void
filter_ranges(const SiftColUnit *cu, const SiftAggRanges *r, SiftColSel *sel)
{
	SiftColZone z;
	int			first = 0,
				hi = r->narms;
	bool		zoned;
	uint64	   *from,
			   *any;

	/* the arms are ascending and apart: those of the unit's zone are a run of them */
	sift_col_zone(cu, r->col, &z);
	zoned = z.nnull < z.nvals && !z.has_ovf;
	if (zoned)
		zone_arms(r->arm, r->narms, z.min, z.max, &first, &hi);
	if (r->hint && !hint_narrows(zoned, first, hi))
		return;
	from = palloc(sel->nwords * sizeof(uint64));
	any = palloc0(sel->nwords * sizeof(uint64));
	memcpy(from, sel->bits, sel->nwords * sizeof(uint64));
	for (int a = first; a < hi; a++)
	{
		SiftColValue lov = {.v.i = r->arm[2 * a]},
					hiv = {.v.i = r->arm[2 * a + 1]};
		SiftColPred	p[2];
		int			np = 0;

		if (lov.v.i != PG_INT64_MIN)
			p[np++] = (SiftColPred) {r->col, SIFT_COLOP_GE, 1, &lov};
		if (hiv.v.i != PG_INT64_MAX)
			p[np++] = (SiftColPred) {r->col, SIFT_COLOP_LE, 1, &hiv};
		if (np == 0)
			p[np++] = (SiftColPred) {r->col, SIFT_COLOP_NOTNULL, 0, NULL};
		memcpy(sel->bits, from, sel->nwords * sizeof(uint64));
		sift_col_sel_count(sel);
		if (sift_col_filter(cu, p, np, sel))
			for (uint32 w = 0; w < sel->nwords; w++)
				any[w] |= sel->bits[w];
	}
	memcpy(sel->bits, any, sel->nwords * sizeof(uint64));
	pfree(from);
	pfree(any);
	sift_col_sel_count(sel);
}

/*
 * The filters of the source on the unit as stored, the most selective first. The two bounds of a range on one leaf are one filter, as selective as the
 * rows between them, and go to sift_col_filter together: a unit with a sorted run for the leaf takes the range from the run.
 */
void
sift_aggfilt_op(const SiftColUnit *cu, SiftColSel *sel, void *arg)
{
	const SiftAggFiltRun *run = arg;
	const SiftAggFilters *fl = run->fl;
	SiftColPred	pair[SIFT_COLSRC_MAX_FILTERS][2];
	int			np[SIFT_COLSRC_MAX_FILTERS];
	int			leaf[SIFT_COLSRC_MAX_FILTERS];
	double		share[SIFT_COLSRC_MAX_FILTERS];
	int			order[SIFT_COLSRC_MAX_FILTERS];
	bool		used[SIFT_COLSRC_MAX_FILTERS] = {false};
	int			nf = 0;

	for (int i = 0; i < fl->n; i++)
	{
		int			j;

		if (used[i])
			continue;
		pair[nf][0] = fl->pred[i];
		np[nf] = 1;
		leaf[nf] = fl->leaf[i];
		share[nf] = filter_share(cu, &fl->pred[i]);
		if (sift_col_pred_is_bound(&fl->pred[i]))
			for (j = i + 1; j < fl->n; j++)
				if (!used[j] && fl->pred[j].col == fl->pred[i].col && sift_col_pred_is_bound(&fl->pred[j]))
				{
					pair[nf][np[nf]++] = fl->pred[j];
					share[nf] = Max(0.0, share[nf] + filter_share(cu, &fl->pred[j]) - 1.0);
					used[j] = true;
					break;
				}
		for (j = nf; j > 0 && share[order[j - 1]] > share[nf]; j--)
			order[j] = order[j - 1];
		order[j] = nf++;
	}
	for (int i = 0; i < nf; i++)
	{
		const SiftColPred *p = pair[order[i]];
		SiftColZone z;

		sift_col_zone(cu, p->col, &z);
		z.has_ovf &= !run->tol[leaf[order[i]]];
		if (!sift_col_filter(cu, p, np[order[i]], sel))
			break;
		if (z.has_ovf && sift_colsrc_nan_leaf(&run->lay->leaf[leaf[order[i]]]))
			for (int k = 0; k < np[order[i]]; k++)
				clear_nan_rows(cu, &p[k], sel);
	}
	for (int i = 0; i < fl->nrng && sel->count > 0; i++)
		filter_ranges(cu, &fl->rng[i], sel);
}

