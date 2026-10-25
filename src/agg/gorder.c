/*
 * agg/gorder.c - the groups in the order the parent sorts by
 *
 * A path that carries pathkeys promises its parent rows in that order, and PostgreSQL then adds no Sort. The keys of a group are ids of the distinct values
 * the scan met, and there are far fewer distinct values of a key than groups. So the distinct values of each ordered key are sorted once with the
 * comparator of the pathkey (its sort operator, collation and nulls order, which is what the Sort would have used), a rank is given to each id, and the
 * groups are sorted by the tuple of their ranks: integer compares, whatever the type of the keys.
 */
#include "postgres.h"

#include "utils/memutils.h"
#include "utils/sortsupport.h"

#include "agg/agg.h"
#include "agg/spec.h"
#include "agg/agg_int.h"

typedef struct IdVal
{
	uint32		id;
	Datum		val;
} IdVal;

static int
idval_cmp(const void *a, const void *b, void *arg)
{
	return ApplySortComparator(((const IdVal *) a)->val, false, ((const IdVal *) b)->val, false, (SortSupport) arg);
}

typedef struct RankTuples
{
	int			nord;
	const uint32 *rank;			/* per group, nord ranks */
} RankTuples;

static int
tuple_cmp(const void *a, const void *b, void *arg)
{
	const RankTuples *t = arg;
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;
	const uint32 *rx = t->rank + (Size) x * t->nord,
			   *ry = t->rank + (Size) y * t->nord;

	for (int o = 0; o < t->nord; o++)
		if (rx[o] != ry[o])
			return rx[o] < ry[o] ? -1 : 1;
	/* groups that tie keep the order the scan gave them */
	return x < y ? -1 : x > y;
}

/* the rank of every id of key o among the groups of the list (0: NULL; ranks of equal values are equal) */
static uint32 *
key_ranks(AggExec *ex, const SiftAggOrd *o, const uint32 *ids, uint32 n, uint32 nord, uint32 col, uint32 *maxid_out)
{
	uint32		maxid = 0,
				nd = 0;
	uint32	   *rank;
	bool	   *seen;
	IdVal	   *vals;
	SortSupportData ssup = {0};
	uint32		r = 0;

	for (uint32 i = 0; i < n; i++)
		maxid = Max(maxid, ids[(Size) i * nord + col]);
	rank = palloc0((maxid + 1) * sizeof(uint32));
	seen = palloc0((maxid + 1) * sizeof(bool));
	vals = palloc((Size) Min((uint64) maxid, (uint64) n) * sizeof(IdVal) + sizeof(IdVal));
	for (uint32 i = 0; i < n; i++)
	{
		uint32		id = ids[(Size) i * nord + col];

		if (id != 0 && !seen[id])
		{
			bool		isnull;

			seen[id] = true;
			vals[nd].id = id;
			vals[nd].val = key_id_datum(ex, o->key, id, &isnull);
			nd++;
		}
	}
	/* COALESCE(key, constant): the groups with no value sort as the constant */
	if (o->subst != NULL && !o->subst->constisnull)
	{
		for (uint32 i = 0; i < n; i++)
			if (ids[(Size) i * nord + col] == 0)
			{
				vals[nd].id = 0;
				vals[nd].val = o->subst->constvalue;
				nd++;
				break;
			}
	}
	ssup.ssup_cxt = CurrentMemoryContext;
	ssup.ssup_collation = o->collation;
	ssup.ssup_nulls_first = o->nulls_first;
	PrepareSortSupportFromOrderingOp(o->sortop, &ssup);
	qsort_arg(vals, nd, sizeof(IdVal), idval_cmp, &ssup);
	/* NULLs rank before every value or after all of them; the values from 1 */
	for (uint32 i = 0; i < nd; i++)
	{
		if (i == 0 || idval_cmp(&vals[i - 1], &vals[i], &ssup) != 0)
			r++;
		rank[vals[i].id] = r;
	}
	if (o->subst == NULL || o->subst->constisnull)
		rank[0] = o->nulls_first ? 0 : r + 1;
	pfree(seen);
	pfree(vals);
	*maxid_out = maxid;
	return rank;
}

void
groups_order(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	uint32		n = ex->nglist,
				nord = s->nord;
	MemoryContext tmp,
				old;
	uint32	   *ids,
			   *tup,
			   *perm,
			   *out;
	RankTuples	t;
	bool		sorted = true;

	if (n < 2)
		return;
	tmp = AllocSetContextCreate(ex->cxt, "sift agg group order", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(tmp);
	ids = palloc((Size) n * nord * sizeof(uint32));
	tup = palloc((Size) n * nord * sizeof(uint32));
	for (uint32 o = 0; o < nord; o++)
		for (uint32 i = 0; i < n; i++)
			ids[(Size) i * nord + o] = key_id(ex, s->ord[o].key, ex->glist[i]);
	for (uint32 o = 0; o < nord; o++)
	{
		uint32		maxid;
		uint32	   *rank = key_ranks(ex, &s->ord[o], ids, n, nord, o, &maxid);

		for (uint32 i = 0; i < n; i++)
			tup[(Size) i * nord + o] = rank[ids[(Size) i * nord + o]];
		pfree(rank);
	}
	t.nord = nord;
	t.rank = tup;
	for (uint32 i = 1; i < n && sorted; i++)
		sorted = tuple_cmp(&(uint32) {i - 1}, &(uint32) {i}, &t) <= 0;
	if (!sorted)
	{
		perm = palloc(n * sizeof(uint32));
		for (uint32 i = 0; i < n; i++)
			perm[i] = i;
		qsort_arg(perm, n, sizeof(uint32), tuple_cmp, &t);
		MemoryContextSwitchTo(old);
		out = MemoryContextAlloc(ex->cxt, n * sizeof(uint32));
		for (uint32 i = 0; i < n; i++)
			out[i] = ex->glist[perm[i]];
		pfree(ex->glist);
		ex->glist = out;
	}
	else
		MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
}
