/*
 * col/v1/store_prune.c - what a source of Format 1 asks its reader to leave unread: the units whose zones lie outside a bound of the quals, or outside every range of an OR of
 * them (col/v1/src_exec.h SiftSrcPrune). The program still tests every row of the units that are read.
 */
#include "postgres.h"

#include "col/source_fold.h"
#include "col/v1/reader.h"
#include "col/v1/store_src.h"

/* the leaf of the input column the source holds as plain integers, NULL for any other */
static const SiftSrcLeaf *
int_leaf(const SiftV1Src *s, int slot)
{
	const SiftSrcLeaf *l = s->col_leaf[slot];

	return l == NULL || l->wide || (l->vtype != SCV_I32 && l->vtype != SCV_I64) ? NULL : l;
}

/* the entry of a bound: a single value is a set of one, which the summaries of key columns test too */
static SiftSrcPrune
bound_entry(const SiftSrcLeaf *l, bool has_lo, int64 lo, bool has_hi, int64 hi, int nvals, const int64 *vals)
{
	SiftSrcPrune e = {.leaf = l->id,.lo = has_lo ? lo : PG_INT64_MIN,.hi = has_hi ? hi : PG_INT64_MAX,.nvals = nvals,.vals = vals};

	if (nvals == 0 && has_lo && has_hi && lo == hi)
	{
		int64	   *one = palloc(sizeof(int64));

		*one = lo;
		e.nvals = 1;
		e.vals = one;
	}
	return e;
}

/* the junction of OR set of the quals: a unit is skipped when every range skips it, each range an AND of its bounds; false when a range bounds no leaf the source holds */
static bool
set_entry(const SiftV1Src *s, int set, SiftSrcPrune *e)
{
	const int	nranges = sift_srcfold_nranges(s->quals, set);
	SiftSrcPrune *kids = palloc(Max(nranges, 1) * sizeof(SiftSrcPrune));

	for (int i = 0; i < nranges; i++)
	{
		SiftSrcBound bd[SIFT_SRC_MAX_BOUNDS];
		const int	nb = sift_srcfold_range(s->quals, set, i, bd);
		SiftSrcPrune *ands = palloc(Max(nb, 1) * sizeof(SiftSrcPrune));
		int			n = 0;

		for (int b = 0; b < nb; b++)
		{
			const SiftSrcLeaf *l = int_leaf(s, bd[b].slot);

			if (l != NULL)
				ands[n++] = bd[b].isnull ? (SiftSrcPrune) {.leaf = l->id,.nulltest = SSN_ISNULL} : bound_entry(l, bd[b].has_lo, bd[b].lo, bd[b].has_hi, bd[b].hi, 0, NULL);
		}
		if (n == 0)
			return false;
		kids[i] = n == 1 ? ands[0] : (SiftSrcPrune) {.nkids = n,.kids = ands};
	}
	*e = (SiftSrcPrune) {.any = true,.nkids = nranges,.kids = kids};
	return nranges > 0;
}

/* A unit whose zone of an integer column lies wholly outside the bounds the program puts on it, or outside every range of an OR, is not read; the program still tests every row of the units that are */
/* The bound of a row Top-N to the column source of an index, when the leaf of its attribute is a plain integer whose zones hold every value */
static void
bind_bound(SiftV1Src *s, const SiftV1Scan *scan)
{
	for (int i = 0; i < s->lay->nleaves; i++)
	{
		const SiftSrcLeaf *l = &s->lay->leaf[i];

		if (l->attno == scan->bound_att && l->parent < 0 && l->path == NIL && !l->lossy && !l->wide && l->scale == 0 && (l->vtype == SCV_I32 || l->vtype == SCV_I64))
		{
			sift_col_set_bound(sift_v1_idx_colsource(s->src), l->id, scan->bound);
			return;
		}
	}
}

void
sift_v1_src_prune_begin(SiftV1Src *s, const SiftV1Scan *scan)
{
	const SiftV1Range *range = scan->range;
	const uint32 nsb = s->rd->nsb(s->src);
	SiftSrcBound bound[SIFT_SRC_MAX_BOUNDS];
	SiftSrcPrune *prune = palloc0((SIFT_SRC_MAX_BOUNDS + SIFT_SRC_MAX_SETS) * sizeof(SiftSrcPrune));
	const int	nb = s->quals != NULL ? sift_srcfold_bounds(s->quals, bound) : 0;
	int			np = 0;

	for (int i = 0; i < nb; i++)
	{
		const SiftSrcLeaf *l = int_leaf(s, bound[i].slot);

		if (l != NULL)
			prune[np++] = bound_entry(l, bound[i].has_lo, bound[i].lo, bound[i].has_hi, bound[i].hi, bound[i].nvals, bound[i].vals);
	}
	for (int i = 0; s->quals != NULL && i < sift_srcfold_nsets(s->quals); i++)
		np += set_entry(s, i, &prune[np]);
	if (scan->bound != NULL && !s->in.is_table)
		bind_bound(s, scan);
	s->rd->begin(s->src, range ? range->sb_lo : 0, range ? Min(range->sb_hi, nsb) : nsb, prune, np);
}
