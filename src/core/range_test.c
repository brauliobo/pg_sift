/*
 * core/range_test.c - test-only SQL function sift._test_range_check, compiled only with SIFT_TESTING and registered by the pg_sift_test extension: the bounds of core/range.h against an
 * oracle that compares key tuples element by element and never their encoding. Keys are drawn from the extremes of int8, NULL, values that no bound uses (so that every gap between two
 * bounds holds a key) and heap addresses; the properties are the order of the encoding, the depth, the successor of a prefix, the overlap and containment of ranges and the span of the
 * partitions of a random tiling that hold a prefix, the last two against a walk over every key of the domain.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"

#include "core/range.h"
#include "core/testrng.h"

PG_FUNCTION_INFO_V1(sift_test_range_check);

enum
{
	NBV = 6,					/* the values a bound uses: the extremes, three integers and NULL */
	NKV = 10,					/* the values of a key: those and four that no bound has, one in each gap */
	NTB = 3,					/* the addresses a bound uses */
	NTK = 5,					/* the addresses of a key: those and two in the gaps */
	TEST_PARTS = 24,
	MAX_KEYS = NKV * NKV * NKV * NTK,
	MAX_BOUNDS = 1 + NKV * NTK + NKV * NKV * NTK + NKV * NKV * NKV * NTK + NKV + NKV * NKV + NKV * NKV * NKV
};

static const int64_t bound_val[NBV] = {INT64_MIN, -5, 0, 7, INT64_MAX, 0};
static const int64_t key_val[NKV] = {INT64_MIN, -100, -5, -1, 0, 3, 7, 100, INT64_MAX, 0};
static const uint64_t bound_tid[NTB] = {0, 5, SIFT_BOUND_TID_MAX};
static const uint64_t key_tid[NTK] = {0, 1, 5, 100, SIFT_BOUND_TID_MAX};

enum
{
	KIND_INT, KIND_NULL, KIND_TID
};

typedef struct Elem
{
	int			kind;
	int64_t		v;
} Elem;

typedef struct Key
{
	int			n;
	Elem		e[SIFT_BOUND_COMPS + 1];
} Key;

typedef struct Counts
{
	int64_t		order,
				order_bad,
				next,
				next_bad,
				span,
				span_bad,
				range,
				range_bad,
				depth,
				depth_bad;
} Counts;

static int
elem_cmp(const Elem *a, const Elem *b)
{
	if (a->kind != b->kind)
		return a->kind < b->kind ? -1 : 1;
	if (a->kind == KIND_NULL)
		return 0;
	if (a->kind == KIND_TID)
		return (uint64_t) a->v < (uint64_t) b->v ? -1 : (uint64_t) a->v > (uint64_t) b->v;
	return a->v < b->v ? -1 : a->v > b->v;
}

static int
key_cmp(const Key *a, const Key *b)
{
	for (int i = 0; i < a->n && i < b->n; i++)
	{
		int			c = elem_cmp(&a->e[i], &b->e[i]);

		if (c != 0)
			return c;
	}
	return a->n < b->n ? -1 : a->n > b->n;
}

static bool
key_has_prefix(const Key *k, const Key *p)
{
	if (p->n > k->n)
		return false;
	for (int i = 0; i < p->n; i++)
		if (elem_cmp(&k->e[i], &p->e[i]) != 0)
			return false;
	return true;
}

static uint32_t
key_depth(const Key *a, const Key *b)
{
	uint32_t	d = 0;

	while (d < (uint32_t) a->n && d < (uint32_t) b->n && a->e[d].kind != KIND_TID && b->e[d].kind != KIND_TID && elem_cmp(&a->e[d], &b->e[d]) == 0)
		d++;
	return d;
}

static void
encode(const Key *k, SiftBound *b)
{
	sift_bound_clear(b);
	for (int i = 0; i < k->n; i++)
	{
		const Elem *e = &k->e[i];
		bool		ok = e->kind == KIND_INT ? sift_bound_push_i64(b, e->v) :
			e->kind == KIND_NULL ? sift_bound_push_null(b) : sift_bound_push_tid(b, (uint64_t) e->v);

		if (!ok)
			elog(ERROR, "sift._test_range_check: a key of the test does not fit a bound");
	}
}

static int
sign(int c)
{
	return c < 0 ? -1 : c > 0;
}

static uint32_t
below(SiftRng *r, uint32_t n)
{
	return (uint32_t) (sift_rng_next(r) % n);
}

static void
add_comp(SiftRng *r, Key *k, bool keys)
{
	uint32_t	v = below(r, keys ? NKV : NBV);
	bool		null = v == (keys ? NKV : NBV) - 1;

	k->e[k->n].kind = null ? KIND_NULL : KIND_INT;
	k->e[k->n].v = (keys ? key_val : bound_val)[v];
	k->n++;
}

static void
add_tid(SiftRng *r, Key *k, bool keys)
{
	k->e[k->n].kind = KIND_TID;
	k->e[k->n].v = (int64_t) (keys ? key_tid : bound_tid)[below(r, keys ? NTK : NTB)];
	k->n++;
}

/* ncomp components of the values of bounds (or of keys), an address after them when with_tid */
static void
random_key(SiftRng *r, Key *k, int ncomp, bool keys, bool with_tid)
{
	k->n = 0;
	for (int i = 0; i < ncomp; i++)
		add_comp(r, k, keys);
	if (with_tid)
		add_tid(r, k, keys);
}

static void
random_any(SiftRng *r, Key *k, bool keys)
{
	int			ncomp = (int) below(r, SIFT_BOUND_COMPS + 1);

	random_key(r, k, ncomp, keys, ncomp > 0 && below(r, 3) == 0);
}

static const SiftBound *
bound_at(const void *refs, uint32_t i)
{
	return &((const SiftBound *) refs)[i];
}

/* A key that is p, an extension of it, a prefix of it or unrelated */
static void
related_key(SiftRng *r, Key *k, const Key *p)
{
	switch (below(r, 4))
	{
		case 0:
			random_any(r, k, true);
			break;
		case 1:
			*k = *p;
			break;
		case 2:
			*k = *p;
			if (k->n == 0 || k->e[k->n - 1].kind != KIND_TID)
			{
				while (k->n < SIFT_BOUND_COMPS && below(r, 3) != 0)
					add_comp(r, k, true);
				if (k->n > 0 && below(r, 2) == 0)
					add_tid(r, k, true);
			}
			break;
		default:
			*k = *p;
			k->n = (int) below(r, p->n + 1);
			break;
	}
}

static void
check_order(SiftRng *r, Counts *c)
{
	Key			a,
				b;
	SiftBound	ea,
				eb;
	int			want;

	random_any(r, &a, below(r, 2) == 0);
	if (below(r, 2) == 0)
		related_key(r, &b, &a);
	else
		random_any(r, &b, below(r, 2) == 0);
	encode(&a, &ea);
	encode(&b, &eb);
	want = sign(key_cmp(&a, &b));
	c->order++;
	c->order_bad += sign(sift_bound_cmp(&ea, &eb)) != want || sign(sift_bound_cmp(&eb, &ea)) != -want;
	c->depth++;
	c->depth_bad += sift_bound_depth(&ea, &eb) != key_depth(&a, &b);
}

static void
check_next(SiftRng *r, Counts *c)
{
	Key			p,
				k;
	SiftBound	ep,
				ek,
				next;
	bool		has_next;

	random_any(r, &p, true);
	related_key(r, &k, &p);
	encode(&p, &ep);
	encode(&k, &ek);
	has_next = sift_bound_prefix_next(&ep, &next);
	c->next++;
	c->next_bad += key_has_prefix(&k, &p) != (sift_bound_cmp(&ep, &ek) <= 0 && (!has_next || sift_bound_cmp(&ek, &next) < 0));
}

typedef struct Tiling
{
	int			n;
	Key			lo[TEST_PARTS];
	SiftBound	enc[TEST_PARTS];
} Tiling;

static int
cmp_key_ptr(const void *a, const void *b)
{
	return key_cmp(a, b);
}

/* The first partition has no lower bound, the others strictly ascending bounds of one to ncomp components and, for some, an address after ncomp */
static void
random_tiling(SiftRng *r, Tiling *t, int ncomp)
{
	int			want = 1 + (int) below(r, TEST_PARTS - 4);
	Key			cand[2 * TEST_PARTS];
	int			nc = 0;

	for (int i = 0; i < want; i++)
	{
		random_key(r, &cand[nc++], (int) below(r, ncomp) + 1, false, false);
		if (below(r, 3) == 0)
			random_key(r, &cand[nc++], ncomp, false, true);
	}
	qsort(cand, nc, sizeof(Key), cmp_key_ptr);
	t->n = 1;
	t->lo[0].n = 0;
	for (int i = 0; i < nc && t->n < TEST_PARTS; i++)
		if (key_cmp(&cand[i], &t->lo[t->n - 1]) > 0)
			t->lo[t->n++] = cand[i];
	for (int i = 0; i < t->n; i++)
		encode(&t->lo[i], &t->enc[i]);
}

/* The partition a key belongs to: the last whose lower bound is at or below it */
static int
home(const Tiling *t, const Key *k)
{
	int			at = 0;

	for (int i = 1; i < t->n; i++)
		if (key_cmp(&t->lo[i], k) <= 0)
			at = i;
	return at;
}

/* Every key of ncomp components and an address that the index can hold, in the order of the loops */
static int
all_keys(int ncomp, Key *out)
{
	int			n = 0,
				total = 1;

	for (int i = 0; i < ncomp; i++)
		total *= NKV;
	for (int code = 0; code < total; code++)
		for (int ti = 0; ti < NTK; ti++)
		{
			Key		   *k = &out[n++];
			int			rest = code;

			k->n = ncomp + 1;
			for (int i = ncomp - 1; i >= 0; i--)
			{
				int			v = rest % NKV;

				rest /= NKV;
				k->e[i].kind = v == NKV - 1 ? KIND_NULL : KIND_INT;
				k->e[i].v = key_val[v];
			}
			k->e[ncomp].kind = KIND_TID;
			k->e[ncomp].v = (int64_t) key_tid[ti];
		}
	return n;
}

static void
check_span(SiftRng *r, Counts *c, const Key *keys, int nkeys, int ncomp)
{
	Tiling		t;
	Key			p;
	SiftBound	ep;
	bool		hit[TEST_PARTS] = {false};
	uint32_t	first,
				last;
	int			lowest = -1,
				highest = -1;

	random_tiling(r, &t, ncomp);
	random_key(r, &p, 1 + (int) below(r, ncomp), true, false);
	if (p.n == ncomp && below(r, 4) == 0)
		add_tid(r, &p, true);
	encode(&p, &ep);
	for (int i = 0; i < nkeys; i++)
		if (key_has_prefix(&keys[i], &p))
			hit[home(&t, &keys[i])] = true;
	for (int i = 0; i < t.n; i++)
		if (hit[i])
		{
			lowest = lowest < 0 ? i : lowest;
			highest = i;
		}
	sift_prefix_span(t.enc, bound_at, t.n, ncomp, &ep, &first, &last);
	c->span++;
	c->span_bad += lowest < 0 || (int) first != lowest || (int) last != highest + 1;
}

static bool
in_range(const Key *lo, const Key *hi, const Key *k)
{
	return key_cmp(lo, k) <= 0 && (hi->n == 0 || key_cmp(k, hi) < 0);
}

/* Every bound of the domain: the keys of every length up to SIFT_BOUND_COMPS, with and without an address after the components */
static int
all_bounds(Key *out)
{
	int			n = 0;

	for (int len = 0; len <= SIFT_BOUND_COMPS; len++)
	{
		int			total = 1;

		for (int i = 0; i < len; i++)
			total *= NKV;
		for (int code = 0; code < total; code++)
			for (int ti = -1; ti < (len > 0 ? NTK : 0); ti++)
			{
				Key		   *k = &out[n++];
				int			rest = code;

				k->n = len;
				for (int i = len - 1; i >= 0; i--)
				{
					int			v = rest % NKV;

					rest /= NKV;
					k->e[i].kind = v == NKV - 1 ? KIND_NULL : KIND_INT;
					k->e[i].v = key_val[v];
				}
				if (ti >= 0)
				{
					k->e[len].kind = KIND_TID;
					k->e[len].v = (int64_t) key_tid[ti];
					k->n++;
				}
			}
	}
	return n;
}

/* Two ranges of bounds overlap when some bound is in both: a walk over every bound of the domain; contains against the comparison of the tuples */
static void
check_range(SiftRng *r, Counts *c, const Key *bounds, int nbounds)
{
	Key			k[4];
	SiftRange	a,
				b;
	bool		want = false,
				bad;

	for (int i = 0; i < 4; i++)
		random_any(r, &k[i], false);
	encode(&k[0], &a.lo);
	encode(&k[1], &a.hi);
	encode(&k[2], &b.lo);
	encode(&k[3], &b.hi);
	for (int i = 0; i < nbounds && !want; i++)
		want = in_range(&k[0], &k[1], &bounds[i]) && in_range(&k[2], &k[3], &bounds[i]);
	bad = sift_range_overlap(&a, &b) != want || sift_range_overlap(&b, &a) != want;
	for (int i = 0; i < 24; i++)
	{
		const Key  *probe = i < 4 ? &k[i] : &bounds[below(r, nbounds)];
		SiftBound	ep;

		encode(probe, &ep);
		bad |= sift_range_contains(&a, &ep) != in_range(&k[0], &k[1], probe);
	}
	c->range++;
	c->range_bad += bad;
}

/*
 * (seed int8, rounds int4) returns int8[]: for each of order, successor, span, range, depth the checks made and the checks that failed, in that order: failures are 0 when the bounds are right
 */
Datum
sift_test_range_check(PG_FUNCTION_ARGS)
{
	SiftRng		rng;
	Counts		c = {0};
	int32		rounds = PG_GETARG_INT32(1);
	Key		   *keys[SIFT_BOUND_COMPS + 1];
	Key		   *bounds = palloc(MAX_BOUNDS * sizeof(Key));
	int			nbounds;
	int			nkeys[SIFT_BOUND_COMPS + 1];
	Datum		out[10];

	sift_rng_seed(&rng, (uint64_t) PG_GETARG_INT64(0));
	for (int a = 1; a <= SIFT_BOUND_COMPS; a++)
	{
		keys[a] = palloc(MAX_KEYS * sizeof(Key));
		nkeys[a] = all_keys(a, keys[a]);
	}
	nbounds = all_bounds(bounds);
	for (int32 i = 0; i < rounds; i++)
	{
		int			ncomp = 1 + (int) below(&rng, SIFT_BOUND_COMPS);

		check_order(&rng, &c);
		check_next(&rng, &c);
		check_span(&rng, &c, keys[ncomp], nkeys[ncomp], ncomp);
		check_range(&rng, &c, bounds, nbounds);
	}
	out[0] = Int64GetDatum(c.order);
	out[1] = Int64GetDatum(c.order_bad);
	out[2] = Int64GetDatum(c.next);
	out[3] = Int64GetDatum(c.next_bad);
	out[4] = Int64GetDatum(c.span);
	out[5] = Int64GetDatum(c.span_bad);
	out[6] = Int64GetDatum(c.range);
	out[7] = Int64GetDatum(c.range_bad);
	out[8] = Int64GetDatum(c.depth);
	out[9] = Int64GetDatum(c.depth_bad);
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(out, 10, INT8OID));
}

#endif							/* SIFT_TESTING */
