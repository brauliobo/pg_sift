/*
 * col/v1/src_zone.c - the prune entries of a scan as zone predicates (col/v1/src_zone.h).
 */
/* check-memory: allow-allocs every array here follows the keys, predicates, slots or constants of one plan or query, not the rows of the table */
#include "postgres.h"

#include "col/v1/src_zone.h"

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
zone_preds_same(const SiftColPred *p, const SiftSrcPrune *z, int leaf, const int64 *known)
{
	if (z->nulltest)
		return p[0].col == leaf && p[0].op == (z->nulltest == SSN_ISNULL ? SIFT_COLOP_ISNULL : SIFT_COLOP_NOTNULL);
	if (p[0].col != leaf || p[0].op != SIFT_COLOP_GE || p[0].vals[0].v.i != z->lo)
		return false;
	if (z->nvals == 0)
		return p[1].op == SIFT_COLOP_LE && p[1].vals[0].v.i == z->hi;
	if (p[1].op != SIFT_COLOP_IN || p[1].nvals != z->nvals)
		return false;
	if (known == z->vals)
		return true;
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
zone_tests(SiftSrcZoneTests *z, MemoryContext cxt, const int *col, const SiftSrcPrune *prune, int nprune)
{
	int			n = 0;

	if (z->zpred == NULL)
		z->zpred = MemoryContextAllocZero(cxt, SIFT_SRCZONE_MAX * sizeof(SiftColPred));
	for (int k = 0; k < nprune; k++)
		if (prune[k].nkids == 0)
		{
			int			leaf = leaf_of(col, prune[k].leaf);
			int			len = prune[k].nulltest ? 1 : 2;

			if (leaf < 0)
				continue;
			if (n + len > z->nzpred || !zone_preds_same(&z->zpred[n], &prune[k], leaf, z->zvals[n]))
			{
				zone_preds(cxt, &prune[k], leaf, MemoryContextAllocZero(cxt, 2 * sizeof(SiftColValue)), &z->zpred[n]);
				z->zvals[n] = prune[k].nvals > 0 ? prune[k].vals : NULL;
			}
			n += len;
		}
	z->nzpred = n;
}

/* does the value every row of the source holds on a leaf it lacks pass the entry z (no junction)? */
static bool
const_passes(const SiftSrcPrune *z, const SiftSrcConst *c)
{
	if (z->nulltest)
		return c[z->leaf].isnull == (z->nulltest == SSN_ISNULL);
	if (c[z->leaf].isnull || c[z->leaf].v < z->lo || c[z->leaf].v > z->hi)
		return false;
	for (int i = 0; i < z->nvals; i++)
		if (z->vals[i] == c[z->leaf].v)
			return true;
	return z->nvals == 0;
}

typedef enum ZoneTree
{
	ZT_TEST,					/* the tree tests the zones of the units */
	ZT_ALL,						/* every row of the source passes the entry: it prunes nothing */
	ZT_NONE						/* no row of the source passes it: it prunes every unit */
} ZoneTree;

/*
 * The zone tree of the entry z (made once for the scans of the source: the entries are the executor's, the same at every begin). An entry on a leaf the source lacks
 * is decided by the value its rows hold there (absent), and decides its junction: a false kid of an AND or a true kid of an OR, else it is left out of it.
 */
static ZoneTree
zone_tree(MemoryContext cxt, const int *col, const SiftSrcConst *absent, const SiftSrcPrune *z, SiftColPrune *c)
{
	SiftColPrune *kids;

	*c = (SiftColPrune) {.any = z->any};
	if (z->nkids == 0)
	{
		SiftColPred *preds = MemoryContextAlloc(cxt, 2 * sizeof(SiftColPred));
		int			leaf = leaf_of(col, z->leaf);

		if (leaf < 0)
			return const_passes(z, absent) ? ZT_ALL : ZT_NONE;
		c->npreds = zone_preds(cxt, z, leaf, MemoryContextAllocZero(cxt, 2 * sizeof(SiftColValue)), preds);
		c->preds = preds;
		return ZT_TEST;
	}
	kids = MemoryContextAlloc(cxt, z->nkids * sizeof(SiftColPrune));
	for (int i = 0; i < z->nkids; i++)
		switch (zone_tree(cxt, col, absent, &z->kids[i], &kids[c->nkids]))
		{
			case ZT_TEST:
				c->nkids++;
				break;
			case ZT_ALL:
				if (z->any)
					return ZT_ALL;
				break;
			case ZT_NONE:
				if (!z->any)
					return ZT_NONE;
				break;
		}
	c->kids = kids;
	return c->nkids > 0 ? ZT_TEST : z->any ? ZT_NONE : ZT_ALL;
}

/* the trees of the junctions into z->zone; an entry no row of the source passes is an OR of no kid, which keeps no unit */
static void
zone_trees(SiftSrcZoneTests *z, MemoryContext cxt, const int *col, const SiftSrcConst *absent, const SiftSrcPrune *prune, int nprune)
{
	bool		none = false;

	z->zfor = prune;
	z->nzone = 0;
	z->zone = MemoryContextAlloc(cxt, (nprune + 1) * sizeof(SiftColPrune));
	for (int k = 0; k < nprune && !none; k++)
	{
		SiftColPrune c;

		if (prune[k].nkids == 0 && leaf_of(col, prune[k].leaf) >= 0)
			continue;
		switch (zone_tree(cxt, col, absent, &prune[k], &c))
		{
			case ZT_TEST:
				z->zone[z->nzone++] = c;
				break;
			case ZT_ALL:
				break;
			case ZT_NONE:
				none = true;
				break;
		}
	}
	if (none)
	{
		z->zone[0] = (SiftColPrune) {.any = true};
		z->nzone = 1;
	}
}

int
sift_srczone_preds(SiftSrcZoneTests *z, MemoryContext cxt, const int *col, const SiftSrcConst *absent, const SiftSrcPrune *prune, int nprune, SiftColPred *out)
{
	if (z->zfor != prune)
		zone_trees(z, cxt, col, absent, prune, nprune);
	zone_tests(z, cxt, col, prune, nprune);
	memcpy(out, z->zpred, z->nzpred * sizeof(SiftColPred));
	return z->nzpred;
}
