/*
 * agg/zoneprune.c - the prune entries of a scan as zone predicates (agg/zoneprune.h).
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/zoneprune.h"

static int
leaf_of(const int *col, int leaf)
{
	return col ? col[leaf] : leaf;
}

/* a zone [lo, hi] is two comparisons: the unit is skipped when its min/max exclude either; a set of values is one IN, which skips a unit whose zone holds none; a NULL test is one predicate */
static int
zone_preds(MemoryContext cxt, const SiftSrcPrune *z, int leaf, SiftColValue *vals, SiftColPred *out)
{
	if (z->nulltest)
	{
		out[0] = (SiftColPred) {leaf, z->nulltest == SSN_ISNULL ? SIFT_COLOP_ISNULL : SIFT_COLOP_NOTNULL, 0, NULL};
		return 1;
	}
	vals[0].v.i = z->lo;
	vals[1].v.i = z->hi;
	out[0] = (SiftColPred) {leaf, SIFT_COLOP_GE, 1, &vals[0]};
	out[1] = (SiftColPred) {leaf, SIFT_COLOP_LE, 1, &vals[1]};
	if (z->nvals > 0)
	{
		SiftColValue *set = MemoryContextAllocZero(cxt, z->nvals * sizeof(SiftColValue));

		for (int i = 0; i < z->nvals; i++)
			set[i].v.i = z->vals[i];
		out[1] = (SiftColPred) {leaf, SIFT_COLOP_IN, z->nvals, set};
	}
	return 2;
}

/* are the predicates p made of the entry z (the entry's range, and its set of values when it has one)? */
static bool
zone_preds_same(const SiftColPred *p, const SiftSrcPrune *z, int leaf)
{
	if (z->nulltest)
		return p[0].col == leaf && p[0].op == (z->nulltest == SSN_ISNULL ? SIFT_COLOP_ISNULL : SIFT_COLOP_NOTNULL);
	if (p[0].col != leaf || p[0].op != SIFT_COLOP_GE || p[0].vals[0].v.i != z->lo)
		return false;
	if (z->nvals == 0)
		return p[1].op == SIFT_COLOP_LE && p[1].vals[0].v.i == z->hi;
	if (p[1].op != SIFT_COLOP_IN || p[1].nvals != z->nvals)
		return false;
	for (int i = 0; i < z->nvals; i++)
		if (p[1].vals[i].v.i != z->vals[i])
			return false;
	return true;
}

/*
 * The zone tests of the entries that are no junction, into z->zpred. A begin comes for every range of units a worker takes, and a set of many values
 * is converted to column values and copied for each: the tests of the last begin stay while the entries hold the same ranges and values.
 */
static void
zone_tests(SiftAggZones *z, MemoryContext cxt, const int *col, const SiftSrcPrune *prune, int nprune)
{
	int			n = 0;

	if (z->zpred == NULL)
		z->zpred = MemoryContextAllocZero(cxt, SIFT_AGGZONE_MAX * sizeof(SiftColPred));
	for (int k = 0; k < nprune; k++)
		if (prune[k].nkids == 0)
		{
			int			leaf = leaf_of(col, prune[k].leaf);
			int			len = prune[k].nulltest ? 1 : 2;

			if (leaf < 0)
				continue;
			if (n + len > z->nzpred || !zone_preds_same(&z->zpred[n], &prune[k], leaf))
				zone_preds(cxt, &prune[k], leaf, MemoryContextAllocZero(cxt, 2 * sizeof(SiftColValue)), &z->zpred[n]);
			n += len;
		}
	z->nzpred = n;
}

/* the zone tree of a junction of the prune entries (kept for the scans of the source: the entries are the executor's, the same at every begin); false when an entry is on a leaf the source lacks */
static bool
zone_tree(MemoryContext cxt, const int *col, const SiftSrcPrune *z, SiftColPrune *c)
{
	*c = (SiftColPrune) {.any = z->any,.nkids = z->nkids};
	if (z->nkids == 0)
	{
		SiftColPred *preds = MemoryContextAlloc(cxt, 2 * sizeof(SiftColPred));
		int			leaf = leaf_of(col, z->leaf);

		if (leaf < 0)
			return false;
		c->npreds = zone_preds(cxt, z, leaf, MemoryContextAllocZero(cxt, 2 * sizeof(SiftColValue)), preds);
		c->preds = preds;
		return true;
	}
	c->kids = MemoryContextAlloc(cxt, z->nkids * sizeof(SiftColPrune));
	for (int i = 0; i < z->nkids; i++)
		if (!zone_tree(cxt, col, &z->kids[i], &((SiftColPrune *) c->kids)[i]))
			return false;
	return true;
}

int
sift_aggzone_preds(SiftAggZones *z, MemoryContext cxt, const int *col, const SiftSrcPrune *prune, int nprune, SiftColPred *out)
{
	if (z->zfor != prune)
	{
		z->zfor = prune;
		z->nzone = 0;
		z->zone = MemoryContextAlloc(cxt, Max(nprune, 1) * sizeof(SiftColPrune));
		for (int k = 0; k < nprune; k++)
			if (prune[k].nkids > 0 && zone_tree(cxt, col, &prune[k], &z->zone[z->nzone]))
				z->nzone++;
	}
	zone_tests(z, cxt, col, prune, nprune);
	memcpy(out, z->zpred, z->nzpred * sizeof(SiftColPred));
	return z->nzpred;
}
