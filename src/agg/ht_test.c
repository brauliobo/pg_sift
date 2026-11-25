/*
 * agg/ht_test.c - test-only SQL functions of the group table beyond its 16 byte keys, compiled only with SIFT_TESTING and
 * registered by the pg_sift_test extension: the growth threshold of the load factor (deterministic, with the 7/8 written out so that
 * a changed constant fails it), wide keys and string views with the state blocks of agg/group/state.h and the count(DISTINCT) set
 * against sorted references, and the distinct set alone. Every function returns the number of failed cases.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"

#include <math.h>

#include "utils/array.h"

#include "agg/group/dist.h"
#include "agg/group/ht.h"
#include "agg/group/rec.h"
#include "agg/group/state.h"
#include "core/memtrack.h"
#include "core/testrng.h"

PG_FUNCTION_INFO_V1(sift_test_ht_grow);
PG_FUNCTION_INFO_V1(sift_test_ht_ext);
PG_FUNCTION_INFO_V1(sift_test_dist_set);
PG_FUNCTION_INFO_V1(sift_test_ht_text_groups);

typedef __int128 i128;

static SiftRng rng;

static uint64
rnd(void)
{
	return sift_rng_next(&rng);
}

static SiftInlineKey16
key16(uint64 n)
{
	SiftInlineKey16 k;

	k.u64[0] = n;
	k.u64[1] = n * 7 + 1;
	return k;
}

static uint32
occupied(const SiftSaltedHT *ht)
{
	uint32		n = 0;

	for (uint32 i = 0; i < ht->capacity; i++)
		n += (ht->ctrl[i] & 0x80) != 0;
	return n;
}

/* The capacity of a table that holds n groups: the smallest power of 2 from 64 with n <= capacity * 7 / 8 (the load factor written out) */
static uint32
expected_capacity(uint32 n)
{
	uint32		cap = 64;

	while (n > cap / 8 * 7)
		cap *= 2;
	return cap;
}

static int64
grow_case(void)
{
	SiftSaltedHT ht;
	int64		bad = 0;

	sift_ht_init(&ht, 1);
	bad += ht.capacity != 64 || ht.grow_at != 56;
	for (uint32 n = 1; n <= 5000; n++)
	{
		const SiftInlineKey16 key = key16(n);
		bool		found;
		SiftAggSlot64 *s = sift_ht_upsert(&ht, sift_aggk6_hash1(n), &key, &found);
		const uint32 cap = expected_capacity(n);

		s->agg.i64[0] = n;
		bad += found;
		bad += ht.capacity != cap || ht.grow_at != cap / 8 * 7 || ht.count != n || ht.count > ht.capacity / 8 * 7;
		bad += occupied(&ht) != n;
		for (uint32 i = 0; i < SIFT_HT_GROUP_WIDTH; i++)
			bad += ht.ctrl[ht.capacity + i] != ht.ctrl[i];
	}
	for (uint32 n = 1; n <= 5000; n++)
	{
		const SiftInlineKey16 key = key16(n);
		bool		found;
		SiftAggSlot64 *s = sift_ht_upsert(&ht, sift_aggk6_hash1(n), &key, &found);

		bad += !found || s->agg.i64[0] != n || ht.count != 5000;
	}
	sift_ht_destroy(&ht);

	/* a room for n groups grows the table exactly when n passes the load factor */
	for (uint32 n = 1; n <= 600; n++)
	{
		sift_ht_init(&ht, 1);
		sift_ht_reserve(&ht, n);
		bad += ht.capacity != expected_capacity(n);
		sift_ht_destroy(&ht);
	}

	/* a batch of upserts reserves its groups first: 100 groups then 13 more pass 7/8 of 128 only at the 113th */
	{
		SiftInlineKey16 keys[113];
		uint64		hashes[113];
		SiftAggSlot64 *out[113];

		for (uint32 i = 0; i < 113; i++)
		{
			keys[i] = key16(i + 1);
			hashes[i] = sift_aggk6_hash1(i + 1);
		}
		sift_ht_init(&ht, 1);
		bad += sift_ht_upsert_batch(&ht, hashes, keys, 100, out) != 100;
		bad += ht.capacity != 128 || ht.count != 100;
		bad += sift_ht_upsert_batch(&ht, hashes + 100, keys + 100, 12, out) != 12;
		bad += ht.capacity != 128 || ht.count != 112;
		bad += sift_ht_upsert_batch(&ht, hashes + 112, keys + 112, 1, out) != 1;
		bad += ht.capacity != 256 || ht.count != 113;
		sift_ht_clear(&ht);
		bad += ht.capacity != 256 || ht.count != 0 || occupied(&ht) != 0;
		sift_ht_destroy(&ht);
	}
	return bad;
}

Datum
sift_test_ht_grow(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(grow_case());
}

/* A group's wide key: 8 + id % 97 bytes that begin with the id, and its null flags */
static uint32
wide_key(uint64 id, uint8 *buf, uint64 *nulls)
{
	const uint32 len = 8 + id % 97;

	memcpy(buf, &id, 8);
	for (uint32 j = 8; j < len; j++)
		buf[j] = (uint8) (id * 131 + j * 7);
	*nulls = id % 4 == 0 ? (id * UINT64CONST(0x9E3779B97F4A7C15)) >> 40 : 0;
	return len;
}

static uint64
wide_hash(const uint8 *buf, uint32 len, uint64 nulls)
{
	return sift_hash_bytes(buf, len) ^ (nulls * UINT64CONST(0xBF58476D1CE4E5B9));
}

typedef struct ExtState
{
	SiftStateSum128 sum;
	SiftStateMinMax128 mm;
	SiftStateVar var;
	SiftDistSet *dist;
	int64		pad;
} ExtState;

typedef struct XRow
{
	uint64		id;
	int64		v;
	double		x;
	uint64		d;
} XRow;

static int
xrow_cmp(const void *a, const void *b)
{
	const XRow *x = a,
			   *y = b;

	if (x->id != y->id)
		return x->id < y->id ? -1 : 1;
	return x->d < y->d ? -1 : x->d > y->d;
}

static SiftI128
i128_of(i128 v)
{
	SiftI128	r = {(uint64) v, (int64) (v >> 64)};

	return r;
}

static bool
close_to(double a, double b)
{
	return fabs(a - b) <= 1e-7 * (fabs(b) + 1.0);
}

/* One group of the sorted rows [lo, hi) against its slot and state block */
static bool
group_matches(const XRow *rows, uint32 lo, uint32 hi, const SiftAggSlot64 *s, const ExtState *e)
{
	int64		sum = 0,
				mn = INT64_MAX,
				mx = INT64_MIN;
	i128		wide = 0,
				wmn = 0,
				wmx = 0;
	double		mean = 0,
				m2 = 0;
	uint64		dist = 0;

	for (uint32 i = lo; i < hi; i++)
	{
		const i128	w = (i128) rows[i].v << 32;

		sum = (int64) ((uint64) sum + (uint64) rows[i].v);
		mn = Min(mn, rows[i].v);
		mx = Max(mx, rows[i].v);
		wide += w;
		wmn = i == lo ? w : (w < wmn ? w : wmn);
		wmx = i == lo ? w : (w > wmx ? w : wmx);
		mean += rows[i].x / (double) (hi - lo);
		dist += i == lo || rows[i].d != rows[i - 1].d;
	}
	for (uint32 i = lo; i < hi; i++)
		m2 += (rows[i].x - mean) * (rows[i].x - mean);
	return s->agg.i64[0] == (int64) (hi - lo) && s->agg.i64[1] == sum && s->agg.i64[2] == mn && s->agg.i64[3] == mx &&
		e->sum.n == (int64) (hi - lo) && !memcmp(&e->sum.sum, &(SiftI128) {(uint64) wide, (int64) (wide >> 64)}, sizeof(SiftI128)) &&
		e->mm.n == (int64) (hi - lo) && e->mm.min.lo == (uint64) wmn && e->mm.min.hi == (int64) (wmn >> 64) &&
		e->mm.max.lo == (uint64) wmx && e->mm.max.hi == (int64) (wmx >> 64) &&
		e->var.n == (int64) (hi - lo) && close_to(e->var.mean, mean) && close_to(e->var.m2, m2) &&
		sift_dist_count(e->dist) == dist;
}

static uint32
group_start(const XRow *rows, uint32 n, uint64 id)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		const uint32 mid = (lo + hi) / 2;

		if (rows[mid].id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* A table of wide keys with a state block of sums, minima, variance and a distinct set per group, over rows that make it grow many times, against the sorted rows */
static bool
ext_case(void)
{
	const uint32 n = 1 + rnd() % 20000,
				pool = 1 + rnd() % n,
				dpool = 1 + rnd() % 50;
	XRow	   *rows = palloc(sizeof(XRow) * n);
	SiftSaltedHT ht;
	SiftAggSlot64 *s;
	uint32		seen = 0;
	bool		ok = true;

	sift_ht_init_ex(&ht, 1, true, sizeof(ExtState), offsetof(ExtState, dist));
	for (uint32 i = 0; i < n; i++)
	{
		uint8		kb[8 + 97];
		uint64		nulls;
		uint32		len;
		bool		found;
		ExtState   *e;
		SiftI128	w;

		rows[i].id = 1 + rnd() % pool;
		rows[i].v = (int64) rnd() >> (rnd() % 64);
		rows[i].x = (double) (rows[i].v >> 20);
		rows[i].d = rnd() % dpool;
		len = wide_key(rows[i].id, kb, &nulls);
		s = sift_ht_upsert_wide(&ht, wide_hash(kb, len, nulls), kb, len, nulls, &found);
		if (!found)
			sift_slot_cmsm_init(s);
		sift_slot_cmsm_add(s, rows[i].v);
		e = (ExtState *) sift_ht_ext(&ht, s);
		w = i128_of((i128) rows[i].v << 32);
		sift_state_sum128_add(&e->sum, w);
		sift_state_minmax128_add(&e->mm, w);
		sift_state_var_add(&e->var, rows[i].x);
		if (e->dist == NULL)
			e->dist = sift_dist_new();
		sift_dist_add(e->dist, rows[i].d);
	}
	qsort(rows, n, sizeof(XRow), xrow_cmp);
	SIFT_HT_FOREACH(&ht, s)
	{
		uint8		kb[8 + 97];
		uint64		id,
					nulls;
		uint32		len;
		const uint8 *key = sift_ht_wide_key(&ht, s, &len);
		uint32		lo,
					hi;

		memcpy(&id, key, 8);
		ok = ok && len == wide_key(id, kb, &nulls) && !memcmp(key, kb, len) && s->key.wide.key_nulls == nulls &&
			s->hash64 == wide_hash(kb, len, nulls);
		lo = group_start(rows, n, id);
		for (hi = lo; hi < n && rows[hi].id == id; hi++)
			;
		ok = ok && hi > lo && group_matches(rows, lo, hi, s, (const ExtState *) sift_ht_ext(&ht, s));
		seen++;
	}
	{
		uint32		groups = 0;

		for (uint32 i = 0; i < n; i++)
			groups += i == 0 || rows[i].id != rows[i - 1].id;
		ok = ok && ht.count == groups && seen == groups;
	}
	/* cleared, the table gives back its sets and keys and takes new groups */
	sift_ht_clear(&ht);
	ok = ok && ht.count == 0 && ht.ext_used == 0 && ht.arena_used == 0;
	{
		uint8		kb[8 + 97];
		uint64		nulls;
		const uint32 len = wide_key(7, kb, &nulls);
		bool		found;

		s = sift_ht_upsert_wide(&ht, wide_hash(kb, len, nulls), kb, len, nulls, &found);
		ok = ok && !found && ht.count == 1 && ((ExtState *) sift_ht_ext(&ht, s))->dist == NULL &&
			((ExtState *) sift_ht_ext(&ht, s))->sum.n == 0;
	}
	sift_ht_destroy(&ht);
	pfree(rows);
	return ok;
}

/* The states merged from two halves of the rows equal the states of all of them: the sums and minima exactly, the variance to float rounding */
static bool
merge_case(void)
{
	const uint32 n = rnd() % 3000;
	const uint32 cut = n ? rnd() % (n + 1) : 0;
	SiftStateSum128 a = {0},
				b = {0},
				all = {0};
	SiftStateMinMax128 ma = {0},
				mb = {0},
				mall = {0};
	SiftStateVar va = {0},
				vb = {0},
				vall = {0};
	bool		ok;

	for (uint32 i = 0; i < n; i++)
	{
		const int64 v = (int64) rnd() >> (rnd() % 64);
		const SiftI128 w = i128_of((i128) v * ((i128) 1 << (rnd() % 60)));
		const double x = (double) (v >> 20);

		sift_state_sum128_add(i < cut ? &a : &b, w);
		sift_state_sum128_add(&all, w);
		sift_state_minmax128_add(i < cut ? &ma : &mb, w);
		sift_state_minmax128_add(&mall, w);
		sift_state_var_add(i < cut ? &va : &vb, x);
		sift_state_var_add(&vall, x);
	}
	sift_state_sum128_merge(&a, &b);
	sift_state_minmax128_merge(&ma, &mb);
	sift_state_var_merge(&va, &vb);
	ok = !memcmp(&a, &all, sizeof(a)) && ma.n == mall.n && (n == 0 || (!memcmp(&ma.min, &mall.min, 16) && !memcmp(&ma.max, &mall.max, 16))) &&
		va.n == vall.n && close_to(va.mean, vall.mean) && close_to(va.m2, vall.m2);
	return ok;
}

typedef struct SRow
{
	char	   *s;
	uint32		len;
} SRow;

static int
srow_cmp(const void *a, const void *b)
{
	const SRow *x = a,
			   *y = b;
	const int	c = memcmp(x->s, y->s, Min(x->len, y->len));

	return c ? c : (x->len < y->len ? -1 : x->len > y->len);
}

/* Strings of 0 to 39 bytes as string views, the short ones inline and the others by pointer, counted per string */
static bool
str_case(void)
{
	const uint32 n = 1 + rnd() % 5000;
	SRow	   *rows = palloc(sizeof(SRow) * n);
	SiftSaltedHT ht;
	SiftAggSlot64 *s;
	uint32		groups = 0;
	bool		ok = true;

	sift_ht_init_str(&ht, 1, C_COLLATION_OID, 0, SIFT_HT_NO_EXT);
	for (uint32 i = 0; i < n; i++)
	{
		SiftStrView v;
		bool		found;

		memset(&v, 0, sizeof(v));
		rows[i].len = v.len = rnd() % 40;
		rows[i].s = palloc(v.len + 1);
		for (uint32 j = 0; j < v.len; j++)
			rows[i].s[j] = 'a' + (char) (rnd() % 2);
		if (v.len <= 12)
			memcpy((uint8 *) &v + offsetof(SiftStrView, prefix), rows[i].s, v.len);
		else
		{
			memcpy(v.prefix, rows[i].s, 4);
			v.u.ptr = rows[i].s;
		}
		s = sift_ht_upsert_str(&ht, &v, &found);
		if (!found)
			sift_slot_cmsm_init(s);
		sift_slot_cmsm_add(s, 1);
	}
	qsort(rows, n, sizeof(SRow), srow_cmp);
	for (uint32 lo = 0, hi; lo < n; lo = hi)
	{
		bool		found;
		SiftStrView v;

		for (hi = lo; hi < n && !srow_cmp(&rows[lo], &rows[hi]); hi++)
			;
		memset(&v, 0, sizeof(v));
		v.len = rows[lo].len;
		if (v.len <= 12)
			memcpy((uint8 *) &v + offsetof(SiftStrView, prefix), rows[lo].s, v.len);
		else
		{
			memcpy(v.prefix, rows[lo].s, 4);
			v.u.ptr = rows[lo].s;
		}
		s = sift_ht_upsert_str(&ht, &v, &found);
		ok = ok && found && s->agg.i64[0] == (int64) (hi - lo);
		groups++;
	}
	ok = ok && ht.count == groups;
	sift_ht_destroy(&ht);
	for (uint32 i = 0; i < n; i++)
		pfree(rows[i].s);
	pfree(rows);
	return ok;
}

/* Number of failed cases: wide keys with state blocks, merged states, string views */
Datum
sift_test_ht_ext(PG_FUNCTION_ARGS)
{
	int64		bad = 0;

	sift_rng_seed(&rng, PG_GETARG_INT64(0));
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		CHECK_FOR_INTERRUPTS();
		bad += !ext_case();
		bad += !merge_case();
		bad += !str_case();
	}
	PG_RETURN_INT64(bad);
}

static int
u64_cmp(const void *a, const void *b)
{
	const uint64 x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

static bool
dist_case(void)
{
	const uint32 n = rnd() % 20000,
				range = 1 + rnd() % (rnd() % 2 ? 40 : 100000);
	uint64	   *vals = palloc(sizeof(uint64) * (n + 1)),
			   *got = palloc(sizeof(uint64) * (n + 1));
	SiftDistSet *a = sift_dist_new(),
			   *b = sift_dist_new();
	uint32		uniq = 0,
				pos = 0,
				k = 0;
	uint64		v;
	bool		ok = true;

	for (uint32 i = 0; i < n; i++)
	{
		vals[i] = rnd() % range;
		sift_dist_add(i % 2 ? a : b, vals[i]);
	}
	qsort(vals, n, sizeof(uint64), u64_cmp);
	for (uint32 i = 0; i < n; i++)
		uniq += i == 0 || vals[i] != vals[i - 1];
	sift_dist_merge(a, b);
	ok = sift_dist_count(a) == uniq;
	while (sift_dist_next(a, &pos, &v) && k < n + 1)
		got[k++] = v;
	ok = ok && k == uniq;
	qsort(got, k, sizeof(uint64), u64_cmp);
	for (uint32 i = 0, j = 0; ok && i < n; i++)
		if (i == 0 || vals[i] != vals[i - 1])
			ok = got[j++] == vals[i];
	for (uint32 i = 0; i < n; i++)
		ok = ok && !sift_dist_add(a, vals[i]);
	ok = ok && sift_dist_count(a) == uniq;
	sift_dist_free(a);
	sift_dist_free(b);
	pfree(vals);
	pfree(got);
	return ok;
}

/* A hash with four values: strings collide in it as they could not in sift_hash_bytes, and equal strings still have equal hashes */
static uint64
colliding_hash(const void *bytes, uint32 len)
{
	return sift_hash_bytes(bytes, len) % 4;
}

/* The distinct strings of rows, sorted: the bytes the set has to give back */
static uint32
distinct_rows(SRow *rows, uint32 n)
{
	uint32		u = 0;

	qsort(rows, n, sizeof(SRow), srow_cmp);
	for (uint32 i = 0; i < n; i++)
		if (i == 0 || srow_cmp(&rows[i], &rows[i - 1]))
			rows[u++] = rows[i];
	return u;
}

/* The values of a set of bytes are exactly the rows, each once */
static bool
dist_holds(const SiftDistSet *set, const SRow *want, uint32 nwant)
{
	SRow	   *got = palloc(sizeof(SRow) * (nwant + 1));
	const uint8 *bytes;
	uint32		pos = 0,
				len,
				k = 0;
	bool		ok = sift_dist_count(set) == nwant;

	while (sift_dist_next_bytes(set, &pos, &bytes, &len) && k <= nwant)
		got[k++] = (SRow) {(char *) bytes, len};
	ok = ok && k == nwant;
	if (ok)
		qsort(got, k, sizeof(SRow), srow_cmp);
	for (uint32 i = 0; ok && i < nwant; i++)
		ok = !srow_cmp(&got[i], &want[i]);
	pfree(got);
	return ok;
}

/*
 * count(DISTINCT) of strings of 0 to 39 bytes over a two letter alphabet, so that the repeats are many and the strings that differ in one byte are many too: in a set
 * whose hash is the real one, in a set whose hash is one of four values (every string collides with most others), merged in halves, and shipped as records
 * between tables. Every set holds exactly the distinct strings: a collision never makes two strings one.
 */
static bool
dist_bytes_case(void)
{
	const uint32 n = 1 + rnd() % 3000;
	SRow	   *rows = palloc(sizeof(SRow) * n);
	SiftDistSet *real = sift_dist_new(),
			   *coll = sift_dist_new(),
			   *half_a = sift_dist_new(),
			   *half_b = sift_dist_new();
	uint32		u;
	bool		ok = true;

	for (uint32 i = 0; i < n; i++)
	{
		rows[i].len = rnd() % 40;
		rows[i].s = palloc(rows[i].len + 1);
		for (uint32 j = 0; j < rows[i].len; j++)
			rows[i].s[j] = 'a' + (char) (rnd() % 2);
		sift_dist_add_bytes(real, rows[i].s, rows[i].len);
		sift_dist_add_hashed(coll, colliding_hash(rows[i].s, rows[i].len), rows[i].s, rows[i].len);
	}
	u = distinct_rows(rows, n);
	for (uint32 i = 0; i < u; i++)
		sift_dist_add_hashed(i % 2 ? half_a : half_b, colliding_hash(rows[i].s, rows[i].len), rows[i].s, rows[i].len);
	ok = dist_holds(real, rows, u);
	for (uint32 i = 0; i < u; i++)
		ok = ok && !sift_dist_add_hashed(real, sift_hash_bytes(rows[i].s, rows[i].len), rows[i].s, rows[i].len);
	ok = ok && sift_dist_count(real) == u && dist_holds(coll, rows, u);
	sift_dist_merge(half_a, half_b);
	ok = ok && dist_holds(half_a, rows, u) && sift_dist_bytes(half_a) > sizeof(SiftDistSet);
	sift_dist_free(real);
	sift_dist_free(coll);
	sift_dist_free(half_a);
	sift_dist_free(half_b);
	return ok;
}

typedef struct RecSink
{
	uint8	   *buf;
	uint32		used;
} RecSink;

static void
rec_collect(void *arg, uint64 hash, const uint8 *rec, uint32 len)
{
	RecSink    *k = arg;

	memcpy(k->buf + k->used, rec, len);
	k->used += len;
}

/* The wide distinct sets of the groups of a table through the records that flush and merge them (the way of a spill and a parallel merge): every group's count is kept */
static bool
dist_bytes_records_case(void)
{
	const uint32 n = 1 + rnd() % 2000,
				ngroups = 1 + rnd() % 5;
	char	  (*vals)[16] = palloc(sizeof(char[16]) * n);
	SiftSaltedHT from,
				to;
	SiftRadixShape shape = {.wide_key = false,.ext_stride = 16,.ext_dist_off = 0};
	SiftRecBuf	buf = {0};
	RecSink		sink = {palloc(sizeof(char) * (n + ngroups) * 128), 0};
	SiftRecMerge m = {&shape, &to, 0};
	uint32		pos = 0;
	bool		ok = true;
	SiftAggSlot64 *s;

	sift_ht_init_ex(&from, 1, false, 16, 0);
	sift_ht_init_ex(&to, 1, false, 16, 0);
	for (uint32 i = 0; i < n; i++)
	{
		SiftInlineKey16 k = {0};
		const uint32 len = 1 + rnd() % 15;
		bool		found;
		const uint32 group = rnd() % ngroups;
		SiftDistSet **set;

		memset(vals[i], 0, 16);
		for (uint32 j = 0; j < len; j++)
			vals[i][j] = 'a' + (char) (rnd() % 2);
		k.u64[0] = group + 1;
		s = sift_ht_upsert(&from, group * UINT64CONST(0x9E3779B97F4A7C15), &k, &found);
		set = (SiftDistSet **) sift_ht_ext(&from, s);
		if (*set == NULL)
			*set = sift_dist_new();
		sift_dist_add_hashed(*set, colliding_hash(vals[i], 16), vals[i], 16);
	}
	sift_rec_flush_ht(&from, &shape, &buf, rec_collect, &sink);
	while (pos < sink.used)
	{
		sift_rec_merge(&m, sink.buf + pos);
		pos += sift_rec_len(sink.buf + pos);
	}
	ok = to.count == from.count;
	SIFT_HT_FOREACH(&from, s)
	{
		bool		found;
		SiftAggSlot64 *d = sift_ht_upsert(&to, s->hash64, &s->key, &found);
		const SiftDistSet *a = *(SiftDistSet **) sift_ht_ext(&from, s),
				   *b = *(SiftDistSet **) sift_ht_ext(&to, d);

		ok = ok && found && sift_dist_count(a) == sift_dist_count(b) && m.dist_bytes > 0;
	}
	sift_ht_destroy(&from);
	sift_ht_destroy(&to);
	return ok;
}

/* The groups the table of strings of the collation makes of the values (the error of sift_ht_init_str when the collation is not deterministic) */
Datum
sift_test_ht_text_groups(PG_FUNCTION_ARGS)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(0);
	Datum	   *vals;
	bool	   *nulls;
	int			n;
	SiftSaltedHT ht;
	int64		groups;

	deconstruct_array(a, TEXTOID, -1, false, TYPALIGN_INT, &vals, &nulls, &n);
	sift_ht_init_str(&ht, 1, PG_GETARG_OID(1), 0, SIFT_HT_NO_EXT);
	for (int i = 0; i < n; i++)
	{
		const text *t = DatumGetTextPP(vals[i]);
		SiftStrView v;
		bool		found;

		memset(&v, 0, sizeof(v));
		v.len = VARSIZE_ANY_EXHDR(t);
		if (v.len <= 12)
			memcpy((uint8 *) &v + offsetof(SiftStrView, prefix), VARDATA_ANY(t), v.len);
		else
		{
			memcpy(v.prefix, VARDATA_ANY(t), 4);
			v.u.ptr = VARDATA_ANY(t);
		}
		sift_ht_upsert_str(&ht, &v, &found);
	}
	groups = ht.count;
	sift_ht_destroy(&ht);
	PG_RETURN_INT64(groups);
}

/* Number of failed cases */
Datum
sift_test_dist_set(PG_FUNCTION_ARGS)
{
	int64		bad = 0;

	sift_rng_seed(&rng, PG_GETARG_INT64(0));
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		MemoryContext cxt = sift_mem_allocset_create(CurrentMemoryContext, "dist case");
		MemoryContext old = MemoryContextSwitchTo(cxt);

		CHECK_FOR_INTERRUPTS();
		bad += !dist_case();
		bad += !dist_bytes_case();
		bad += !dist_bytes_records_case();
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cxt);
	}
	PG_RETURN_INT64(bad);
}

#endif							/* SIFT_TESTING */
