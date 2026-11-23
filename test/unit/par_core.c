/*
 * par_core.c - core/par: the radix streams, the top-K heap and its bound, the pool, and the morsel driver, on four threads that the trap shim flags as workers (no palloc, no ereport).
 */
#include "postgres.h"

#include <math.h>

#include "core/hash.h"
#include "core/par/radix.h"
#include "core/par/topk.h"
#include "unit.h"

#define WORKERS		4
#define SLAB		(8 << 20)

static uint64
mix(uint64 i)
{
	return sift_fmix64(i + 1);
}

/* ---- radix: a record is its value, the bytes that follow it (a multiple of 8) and those bytes, all equal to the low byte of the value ---- */

typedef struct RadixJob
{
	SiftRadixOut out[WORKERS];
	SiftArena	arena[WORKERS];
	uint32		per_worker;
} RadixJob;

static uint32
payload_of(uint64 i, uint64 v)
{
	return i % 5 == 0 ? 8 * (1 + v % 40) : 0;
}

static void
radix_scatter(void *arg, int t)
{
	RadixJob   *j = arg;

	for (uint32 i = 0; i < j->per_worker; i++)
	{
		const uint64 v = mix((uint64) t * j->per_worker + i);
		const uint64 n = payload_of(i, v);
		uint8	   *rec = sift_radix_reserve(&j->out[t], sift_radix_part(v), 16 + n);

		CHECK(rec != NULL);
		memcpy(rec, &v, 8);
		memcpy(rec + 8, &n, 8);
		memset(rec + 16, (int) (v & 0xff), n);
	}
}

typedef struct RadixSeen
{
	uint64		sum;
	uint64		n;
	uint32		part;
} RadixSeen;

static bool
radix_chunk(void *arg, const uint8 *data, uint32 len)
{
	RadixSeen  *s = arg;

	for (uint32 off = 0; off < len;)
	{
		uint64		v,
					n;

		CHECK(len - off >= 16);
		memcpy(&v, data + off, 8);
		memcpy(&n, data + off + 8, 8);
		CHECK(sift_radix_part(v) == s->part && len - off >= 16 + n);
		for (uint64 b = 0; b < n; b++)
			CHECK(data[off + 16 + b] == (uint8) (v & 0xff));
		s->sum += v;
		s->n++;
		off += 16 + n;
	}
	return true;
}

static void
test_radix_scatter(void)
{
	static RadixJob job;
	uint8	   *slab[WORKERS];
	uint64		expect = 0;
	RadixSeen	all = {0};

	job.per_worker = 10000;
	for (int t = 0; t < WORKERS; t++)
	{
		slab[t] = aligned_alloc(SIFT_ARENA_ALIGN, SLAB);
		CHECK(slab[t] != NULL);
		sift_arena_init(&job.arena[t], slab[t], SLAB);
		sift_radix_init(&job.out[t], &job.arena[t]);
	}
	unit_run_workers(WORKERS, radix_scatter, &job);
	for (uint64 i = 0; i < (uint64) WORKERS * job.per_worker; i++)
		expect += mix(i);
	for (uint32 p = 0; p < SIFT_RADIX_PARTS; p++)
		for (int t = 0; t < WORKERS; t++)
		{
			RadixSeen	s = {.part = p};

			CHECK(sift_radix_scan(&job.out[t], p, radix_chunk, &s));
			CHECK(s.n == sift_radix_nrec(&job.out[t], p));
			all.sum += s.sum;
			all.n += s.n;
		}
	CHECK(all.n == (uint64) WORKERS * job.per_worker && all.sum == expect);
	for (int t = 0; t < WORKERS; t++)
		free(slab[t]);
}

static bool
radix_stop(void *arg, const uint8 *data, uint32 len)
{
	return false;
}

static void
test_radix_full_arena(void)
{
	static uint8 slab[8192] __attribute__((aligned(SIFT_ARENA_ALIGN)));
	SiftArena	a;
	SiftRadixOut o;
	const uint64 v = 7;
	uint64		n;

	sift_arena_init(&a, slab, sizeof(slab));
	sift_radix_init(&o, &a);
	for (int i = 0; i < 8; i++)
		CHECK(sift_radix_put(&o, 3, &v, 8));
	CHECK(sift_radix_nrec(&o, 3) == 8);
	CHECK(sift_radix_reserve(&o, 3, 60000) == NULL && sift_radix_nrec(&o, 3) == 8);
	CHECK(!sift_radix_scan(&o, 3, radix_stop, NULL));
	while (sift_radix_put(&o, 5, &v, 8))
		;
	n = sift_radix_nrec(&o, 5);
	CHECK(n > 0 && !sift_radix_put(&o, 5, &v, 8) && sift_radix_nrec(&o, 5) == n && a.used <= a.cap);
	CHECK(sift_radix_scan(&o, 0, radix_stop, NULL));
}

/* ---- top-K: the K best by metric, then by the key (a total order), from four heaps, equal to the first K of a sort of everything ---- */

typedef struct TkElem
{
	int64		metric;
	uint64		key;
	char	   *txt;
	uint32		len;
} TkElem;

typedef struct TkJob
{
	SiftParTopK tk[WORKERS];
	SiftArena	arena[WORKERS];
	SiftAtomicBound bound;
	uint32		n;
	uint32		k;
} TkJob;

static bool
tk_better(const void *a, const void *b, const void *ctx)
{
	const TkElem *x = a,
			   *y = b;

	if (x->metric != y->metric)
		return x->metric > y->metric;
	return x->key < y->key;
}

static bool
tk_store(void *dst, const void *cand, SiftArena *str, const void *ctx)
{
	const TkElem *c = cand;
	TkElem	   *d = dst;
	char	   *buf = sift_arena_alloc(str, c->len, 1);

	if (buf == NULL)
		return false;
	memcpy(buf, c->txt, c->len);
	*d = *c;
	d->txt = buf;
	return true;
}

static void
tk_value(uint64 i, TkElem *e, char *txt)
{
	const uint64 v = mix(i);

	e->metric = (int64) (v % 50);
	e->key = i;
	e->len = 1 + v % 12;
	memset(txt, 'a' + (int) (i % 26), e->len);
	e->txt = txt;
}

static void
tk_offer(void *arg, int t)
{
	TkJob	   *j = arg;

	for (uint32 i = t; i < j->n; i += WORKERS)
	{
		TkElem		e;
		char		txt[16];

		tk_value(i, &e, txt);
		if (sift_bound_key_i64(e.metric, true) < sift_bound_load(&j->bound))
			continue;
		CHECK(sift_par_topk_offer(&j->tk[t], &e));
		if (sift_par_topk_worst(&j->tk[t]) != NULL)
			sift_bound_raise(&j->bound, sift_bound_key_i64(((const TkElem *) sift_par_topk_worst(&j->tk[t]))->metric, true));
	}
}

static int
tk_cmp_ref(const void *a, const void *b)
{
	return tk_better(a, b, NULL) ? -1 : tk_better(b, a, NULL) ? 1 : 0;
}

static void
test_topk(uint32 n, uint32 k)
{
	static TkJob job;
	static uint8 slab[WORKERS][1 << 20] __attribute__((aligned(SIFT_ARENA_ALIGN)));
	SiftArena	out;
	static uint8 outslab[1 << 20] __attribute__((aligned(SIFT_ARENA_ALIGN)));
	SiftParTopK *heaps[WORKERS];
	TkElem	   *ref = malloc(sizeof(TkElem) * n);
	const void **sorted;
	uint32		got;

	job.n = n;
	job.k = k;
	sift_bound_init(&job.bound, 0);
	for (int t = 0; t < WORKERS; t++)
	{
		sift_arena_init(&job.arena[t], slab[t], sizeof(slab[t]));
		CHECK(sift_par_topk_init(&job.tk[t], &job.arena[t], k, sizeof(TkElem), 64 << 10, tk_better, tk_store, NULL));
		heaps[t] = &job.tk[t];
	}
	unit_run_workers(WORKERS, tk_offer, &job);
	for (uint32 i = 0; i < n; i++)
	{
		static char txt[1 << 16][16];

		tk_value(i, &ref[i], txt[i % (1 << 16)]);
		ref[i].txt = NULL;
	}
	qsort(ref, n, sizeof(TkElem), tk_cmp_ref);
	sift_arena_init(&out, outslab, sizeof(outslab));
	CHECK(sift_par_topk_merge(heaps, WORKERS, &out, &sorted, &got));
	CHECK(got == Min(k, n));
	for (uint32 i = 0; i < got; i++)
	{
		const TkElem *e = sorted[i];

		CHECK(e->metric == ref[i].metric && e->key == ref[i].key);
		CHECK(e->len == 1 + mix(e->key) % 12 && e->txt[0] == 'a' + (int) (e->key % 26));
	}
	free(ref);
}

static void
test_bound_keys(void)
{
	static const int64 vals[] = {INT64_MIN, -5, -1, 0, 1, 7, INT64_MAX};
	static const float fl[] = {-INFINITY, -1e30f, -2.5f, -0.0f, 0.0f, 1e-30f, 3.5f, 1e30f, INFINITY};
	SiftAtomicBound b;

	for (int i = 1; i < 7; i++)
		CHECK(sift_bound_key_i64(vals[i - 1], true) < sift_bound_key_i64(vals[i], true) &&
			  sift_bound_key_i64(vals[i - 1], false) > sift_bound_key_i64(vals[i], false));
	for (int i = 1; i < 9; i++)
		CHECK(sift_bound_key_f32(fl[i - 1]) <= sift_bound_key_f32(fl[i]));
	for (int i = 0; i < 9; i++)
		CHECK(sift_bound_f32(sift_bound_key_f32(fl[i])) == fl[i]);
	sift_bound_init(&b, 10);
	sift_bound_raise(&b, 5);
	CHECK(sift_bound_load(&b) == 10);
	sift_bound_raise(&b, 11);
	CHECK(sift_bound_load(&b) == 11);
}

int
main(void)
{
	test_radix_scatter();
	test_radix_full_arena();
	test_bound_keys();
	test_topk(200000, 10);
	test_topk(200000, 1000);
	test_topk(300, 1000);
	test_topk(5000, 1);
	puts("par_core: ok");
	return 0;
}
