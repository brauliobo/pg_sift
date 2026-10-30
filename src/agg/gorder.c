/*
 * agg/gorder.c - the groups in the order the parent sorts by
 *
 * A path that carries pathkeys promises its parent rows in that order, and PostgreSQL then adds no Sort. The keys of a group are ids of the distinct values
 * the scan met, and there are far fewer distinct values of a key than groups. So the distinct values of each ordered key are sorted once with the
 * comparator of the pathkey (its sort operator, collation and nulls order, which is what the Sort would have used), a rank is given to each id, and the
 * groups are sorted by the tuple of their ranks: integer compares, whatever the type of the keys.
 */
#include "postgres.h"

#include <math.h>

#include "lib/stringinfo.h"
#include "port/pg_bitutils.h"
#include "utils/jsonb.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
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


/*
 * jsonb keys. Comparing two jsonb values takes the comparator of the type about 400 ns (an iterator for each, allocated), and ranking the 38,000 distinct
 * values of a dimension by it was 240 ms of a scan of 224,000 groups. Each value is written once as a string of bytes whose memcmp order follows the rules of
 * compareJsonbContainers (the tokens of an iterator in turn: the kind, the type, and the length, number of elements or bytes; a number as a double), the values
 * are sorted by it, and the order is then CHECKED with the comparator of the pathkey and repaired by insertion where it differs (a string ordered by a collation, two
 * numbers that are one double), so the ranks are those of the comparator whatever the bytes got wrong. A repair that moves more than JKEY_REPAIR_MOVES values for each
 * value gives up for the full sort.
 */
#define JKEY_REPAIR_MOVES 4

typedef struct JKey
{
	const unsigned char *p;
	uint32		len;
	uint32		idx;
} JKey;

static void
jkey_put_u32(StringInfo b, uint32 v)
{
	unsigned char c[4] = {v >> 24, v >> 16, v >> 8, v};

	appendBinaryStringInfo(b, (char *) c, 4);
}

/* the order-preserving bytes of a double: the sign bit flipped for a positive number, all the bits for a negative one; NaN last */
static void
jkey_put_double(StringInfo b, double d)
{
	uint64		u;
	unsigned char c[8];

	if (isnan(d))
		u = PG_UINT64_MAX;
	else
	{
		memcpy(&u, &d, sizeof(u));
		u = (u >> 63) ? ~u : (u | ((uint64) 1 << 63));
	}
	for (int i = 0; i < 8; i++)
		c[i] = u >> (56 - 8 * i);
	appendBinaryStringInfo(b, (char *) c, 8);
}

static void
jkey_make(StringInfo b, Jsonb *jb)
{
	JsonbIterator *it = JsonbIteratorInit(&jb->root);
	JsonbValue	v;
	JsonbIteratorToken tok;

	while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		appendStringInfoChar(b, (char) tok);
		if (tok == WJB_END_ARRAY || tok == WJB_END_OBJECT)
			continue;
		appendStringInfoChar(b, (char) v.type);
		switch (v.type)
		{
			case jbvString:
				appendBinaryStringInfo(b, v.val.string.val, v.val.string.len);
				appendStringInfoChar(b, '\0');
				break;
			case jbvNumeric:
				jkey_put_double(b, DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow, NumericGetDatum(v.val.numeric))));
				break;
			case jbvBool:
				appendStringInfoChar(b, v.val.boolean ? 1 : 0);
				break;
			case jbvArray:
				appendStringInfoChar(b, v.val.array.rawScalar ? 0 : 1);
				jkey_put_u32(b, v.val.array.nElems);
				break;
			case jbvObject:
				jkey_put_u32(b, v.val.object.nPairs);
				break;
			default:
				break;
		}
	}
}

static int
jkey_cmp(const void *a, const void *b)
{
	const JKey *x = a,
			   *y = b;
	int			r = memcmp(x->p, y->p, Min(x->len, y->len));

	return r != 0 ? r : (x->len > y->len) - (x->len < y->len);
}

/* vals sorted as the comparator of ssup sorts them, by the bytes and a checked repair; false: it was left for the full sort */
static bool
jsonb_presort(IdVal *vals, uint32 nd, SortSupport ssup)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift agg jsonb ranks", ALLOCSET_DEFAULT_SIZES),
				old = MemoryContextSwitchTo(tmp);
	StringInfoData bytes;
	uint32	   *at = palloc((nd + 1) * sizeof(uint32));
	JKey	   *key = palloc(nd * sizeof(JKey));
	IdVal	   *sorted = palloc(nd * sizeof(IdVal));
	uint64		moves = 0;
	bool		ok = true;

	initStringInfo(&bytes);
	for (uint32 i = 0; i < nd; i++)
	{
		at[i] = bytes.len;
		jkey_make(&bytes, DatumGetJsonbP(vals[i].val));
	}
	at[nd] = bytes.len;
	for (uint32 i = 0; i < nd; i++)
	{
		key[i].p = (const unsigned char *) bytes.data + at[i];
		key[i].len = at[i + 1] - at[i];
		key[i].idx = i;
	}
	qsort(key, nd, sizeof(JKey), jkey_cmp);
	for (uint32 i = 0; i < nd; i++)
		sorted[i] = vals[ssup->ssup_reverse ? key[nd - 1 - i].idx : key[i].idx];
	/* the comparator of the pathkey decides: an insertion sort moves a value only past the ones it is out of order with */
	for (uint32 i = 1; i < nd && ok; i++)
	{
		IdVal		v = sorted[i];
		uint32		j = i;

		while (j > 0 && idval_cmp(&sorted[j - 1], &v, ssup) > 0)
		{
			sorted[j] = sorted[j - 1];
			j--;
			if (++moves > (uint64) JKEY_REPAIR_MOVES * nd)
			{
				ok = false;
				break;
			}
		}
		sorted[j] = v;
	}
	MemoryContextSwitchTo(old);
	if (ok)
		memcpy(vals, sorted, nd * sizeof(IdVal));
	MemoryContextDelete(tmp);
	return ok;
}

typedef struct RankTuples
{
	int			nord;
	const uint32 *rank;			/* per group, nord ranks */
} RankTuples;

/* the ranks of a group packed into one integer, the first key in the high bits: one compare for the whole tuple */
typedef struct PackedGroup
{
	uint64		key;
	uint32		group;
} PackedGroup;

static int
packed_cmp(const void *a, const void *b)
{
	const PackedGroup *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->group < y->group ? -1 : x->group > y->group;
}

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
	if (!(ex->spec->key[o->key].typid == JSONBOID && nd > 64 && jsonb_presort(vals, nd, &ssup)))
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
		int			bits = 0;
		uint32		top[SIFT_AGG_MAX_KEYS] = {0};

		for (uint32 i = 0; i < n; i++)
			for (uint32 o = 0; o < nord; o++)
				top[o] = Max(top[o], tup[(Size) i * nord + o]);
		for (uint32 o = 0; o < nord; o++)
			bits += pg_leftmost_one_pos32(top[o] | 1) + 1;
		perm = palloc(n * sizeof(uint32));
		if (bits <= 64)
		{
			PackedGroup *pk = palloc(n * sizeof(PackedGroup));

			for (uint32 i = 0; i < n; i++)
			{
				uint64		k = 0;

				for (uint32 o = 0; o < nord; o++)
					k = (k << (pg_leftmost_one_pos32(top[o] | 1) + 1)) | tup[(Size) i * nord + o];
				pk[i].key = k;
				pk[i].group = i;
			}
			qsort(pk, n, sizeof(PackedGroup), packed_cmp);
			for (uint32 i = 0; i < n; i++)
				perm[i] = pk[i].group;
			pfree(pk);
		}
		else
		{
			for (uint32 i = 0; i < n; i++)
				perm[i] = i;
			qsort_arg(perm, n, sizeof(uint32), tuple_cmp, &t);
		}
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
