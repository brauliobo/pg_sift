/*
 * par_tseries.c - agg/v6/par/tseries.c on 1 to 4 threads (the trap shim flags the helpers: no palloc, no ereport): GROUP BY date_bin(..) of a time series with an integer key beside it, against the same
 * rows binned one at a time by the kernel of the serial scan and merged. Strides of every kind (the constant ones of a dashboard, a stride with no fast path, the month), origins before and after the data,
 * timestamps before 2000, NULL timestamps and NULL keys; and the rows the threads must leave to the scan: an infinite bin (the word of the NULL key), a bin that overflows, a NULL in a summed column.
 */
#include "postgres.h"

#include "agg/v6/par/tseries.h"
#include "core/hash.h"
#include "core/par/pool.h"
#include "core/popcount.h"
#include "par_host.h"
#include "pgshim.h"
#include "unit.h"

#define ROWS		(1u << 18)
#define GRAN		8192
#define SCRATCH		(4 << 20)
#define SEC			INT64CONST(1000000)

static int	nchecks;

#define PASS(cond) do { nchecks++; CHECK(cond); } while (0)

typedef struct Data
{
	int64	   *ts;
	bool	   *ts_null;
	int64	   *dev;
	bool	   *dev_null;
	int64	   *val;
	bool	   *val_null;
	uint64	   *alive;			/* the rows a predicate leaves, a bitmap per granule */
} Data;

static uint64 rng_state = 7;

static uint64
rng(void)
{
	return sift_fmix64(++rng_state);
}

typedef struct Gen
{
	int64		t0;				/* the first timestamp */
	int64		step;			/* the largest step between two rows, in microseconds */
	uint32		ndev;
	bool		ts_nulls;
	bool		dev_nulls;
	bool		val_nulls;
	bool		sparse;			/* a predicate leaves a quarter of the rows */
} Gen;

static Data *
make_data(const Gen *g)
{
	Data	   *d = calloc(1, sizeof(Data));
	int64		t = g->t0;

	d->ts = malloc(sizeof(int64) * ROWS);
	d->ts_null = calloc(ROWS, 1);
	d->dev = malloc(sizeof(int64) * ROWS);
	d->dev_null = calloc(ROWS, 1);
	d->val = malloc(sizeof(int64) * ROWS);
	d->val_null = calloc(ROWS, 1);
	d->alive = calloc(ROWS / 64, sizeof(uint64));
	for (uint32 i = 0; i < ROWS; i++)
	{
		const uint64 h = rng();

		t += (int64) (h % (uint64) g->step);
		d->ts[i] = t;
		d->ts_null[i] = g->ts_nulls && h % 53 == 1;
		d->dev[i] = (int64) ((h >> 20) % g->ndev) - 3;
		d->dev_null[i] = g->dev_nulls && (h >> 40) % 41 == 2;
		d->val[i] = (int64) ((h >> 8) % 20001) - 10000;
		d->val_null[i] = g->val_nulls && (h >> 50) % 4001 == 7;
		if (!g->sparse || (h >> 33) % 4 == 0)
			d->alive[i / 64] |= UINT64CONST(1) << (i % 64);
	}
	return d;
}

static void
free_data(Data *d)
{
	free(d->ts);
	free(d->ts_null);
	free(d->dev);
	free(d->dev_null);
	free(d->val);
	free(d->val_null);
	free(d->alive);
	free(d);
}

static SiftTsShape
make_shape(const SiftTsKey *bin)
{
	SiftTsShape sh = {.nk = 2,.nsum = 1,.ncount = 0,.ncols = 3};

	sh.key[0] = *bin;
	sh.key[0].at = 0;
	sh.key[0].binned = true;
	sh.key[1] = (SiftTsKey) {.at = 1};
	sh.sum_at[0] = 2;
	sh.count_at[sh.ncount++] = 2;
	sh.gs = (SiftParGroupSpec) {.nk = 2,.nsum = 1};
	return sh;
}

/* ---- the run ---- */

typedef struct Rec
{
	uint64		key[2];
	uint64		count;
	uint64		sum;
} Rec;

typedef struct Vec
{
	Rec		   *r;
	uint32		n;
	uint32		cap;
} Vec;

typedef struct Ctx
{
	const SiftTsShape *sh;
	const Data *d;
	Vec			out[SIFT_PAR_MAX_THREADS];
} Ctx;

static bool
fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	const Ctx  *c = arg;
	const uint32 at = lo * GRAN;
	const uint64 *alive = c->d->alive + at / 64;
	SiftParCol	col[3] = {{c->d->ts + at, c->d->ts_null + at}, {c->d->dev + at, c->d->dev_null + at}, {c->d->val + at, c->d->val_null + at}};
	uint32		nact = 0;

	for (int w = 0; w < GRAN / 64; w++)
		nact += sift_pc64(alive[w]);
	if (nact == 0)
		return true;
	return sift_tseries_batch(c->sh, col, alive, nact, GRAN, scratch, err, out);
}

static void
emit(void *arg, int worker, const SiftParGroupRow *row)
{
	Ctx		   *c = arg;
	Vec		   *v = &c->out[worker];

	if (v->n == v->cap)
		v->r = realloc(v->r, sizeof(Rec) * (v->cap = v->cap ? v->cap * 2 : 4096));
	v->r[v->n++] = (Rec) {{row->key[0], row->key[1]}, row->count, row->sum[0]};
}

static void
overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
	CHECK(nleft == 0);
}

/* The ring of granules: every item is a granule of the data, made by the leader with nothing to read */
static bool
feed(void *arg, uint32 item)
{
	return item < ROWS / GRAN;
}

static void
release(void *arg, uint32 item)
{
}

static void
fallback(void *arg, uint32 item)
{
	CHECK(false);
}


static int
cmp_rec(const void *a, const void *b)
{
	const Rec  *x = a,
			   *y = b;

	if (x->key[0] != y->key[0])
		return x->key[0] < y->key[0] ? -1 : 1;
	return x->key[1] == y->key[1] ? 0 : x->key[1] < y->key[1] ? -1 : 1;
}

/* The records sorted by key, the equal keys merged */
static Vec
merged(Vec *in, int n)
{
	Vec			all = {0};
	Vec			m = {0};

	for (int t = 0; t < n; t++)
		for (uint32 i = 0; i < in[t].n; i++)
		{
			if (all.n == all.cap)
				all.r = realloc(all.r, sizeof(Rec) * (all.cap = all.cap ? all.cap * 2 : 4096));
			all.r[all.n++] = in[t].r[i];
		}
	qsort(all.r, all.n, sizeof(Rec), cmp_rec);
	m.r = malloc(sizeof(Rec) * Max(all.n, 1));
	for (uint32 i = 0; i < all.n; i++)
	{
		if (m.n > 0 && cmp_rec(&m.r[m.n - 1], &all.r[i]) == 0)
		{
			m.r[m.n - 1].count += all.r[i].count;
			m.r[m.n - 1].sum += all.r[i].sum;
		}
		else
			m.r[m.n++] = all.r[i];
	}
	free(all.r);
	return m;
}

/* The rows bin by bin, with the kernel of the scan one row at a time */
static Vec
reference(const Data *d, const SiftTsShape *sh, bool *serial)
{
	Vec			v = {0};

	*serial = false;
	for (uint32 i = 0; i < ROWS; i++)
	{
		Rec			r = {.count = 1};
		int64		b = 0;

		if (!((d->alive[i / 64] >> (i % 64)) & 1))
			continue;
		if (d->val_null[i])
		{
			*serial = true;
			continue;
		}
		if (!d->ts_null[i])
		{
			if (sh->key[0].month)
			{
				SiftBatchSel bs = {.n_raw = 1,.n_active = 1,.all_active = true};
				SiftVec		in = {.data = &d->ts[i],.all_valid = true,.len = 1,.type = SVEC_I64};

				sift_datemonth_batch(&bs, &in, &b);
			}
			else
				CHECK(sift_datebin_one(&sh->key[0].bin, d->ts[i], &b) == SDB_OK);
			r.key[0] = (uint64) b ^ SIFT_TS_SIGN;
			if (r.key[0] == SIFT_TS_NULL_WORD)
			{
				*serial = true;
				continue;
			}
		}
		else
			r.key[0] = SIFT_TS_NULL_WORD;
		r.key[1] = d->dev_null[i] ? SIFT_TS_NULL_WORD : (uint64) d->dev[i] ^ SIFT_TS_SIGN;
		r.sum = (uint64) d->val[i];
		if (v.n == v.cap)
			v.r = realloc(v.r, sizeof(Rec) * (v.cap = v.cap ? v.cap * 2 : 4096));
		v.r[v.n++] = r;
	}
	{
		Vec			one = v;
		Vec			m = merged(&one, 1);

		free(v.r);
		return m;
	}
}

static bool
same(const Vec *a, const Vec *b)
{
	if (a->n != b->n)
		return false;
	for (uint32 i = 0; i < a->n; i++)
		if (cmp_rec(&a->r[i], &b->r[i]) != 0 || a->r[i].count != b->r[i].count || a->r[i].sum != b->r[i].sum)
			return false;
	return true;
}

/* One run on `workers` threads: false when the run stopped on the thread's SERIAL (RANGE mode raises it: the threads do not take what the scan must) */
static Vec
run(const Data *d, const SiftTsShape *sh, uint32 workers)
{
	Ctx			c = {.sh = sh,.d = d};
	SiftParGroupHooks hooks = {.fill = fill,.emit = emit,.overflow = overflow,.arg = &c};
	SiftParArenas *ar;
	SiftParGroup *g;
	SiftErr		err = {0};
	Vec			v;

	pgshim_reset();
	ar = sift_par_arenas_create(CurrentMemoryContext, workers, SCRATCH, 64 << 20, 16 << 20);
	g = sift_par_group_create(ar, &sh->gs, false, ROWS, &hooks, &err);
	CHECK(g != NULL);
	SiftParOps	ops = sift_par_group_ops;

	ops.feed_slot = feed;
	ops.release_slot = release;
	ops.fallback_item = fallback;
	CHECK(sift_par_run(&(SiftParCfg) {.mode = SIFT_PAR_RING,.nthreads = workers}, &ops, g, ar, NULL));
	PASS(!sift_par_pool_busy());
	v = merged(c.out, SIFT_PAR_MAX_THREADS);
	for (int t = 0; t < SIFT_PAR_MAX_THREADS; t++)
		free(c.out[t].r);
	return v;
}

static SiftTsKey
bin_of(int64 stride, int64 origin)
{
	SiftTsKey	k = {.binned = true,.bin = {.stride_us = stride,.origin = origin,.stride_status = SDB_OK}};

	if (stride == SIFT_DATEBIN_MONTH)
		k.month = true;
	return k;
}

static void
check_parity(const Gen *g, const SiftTsKey *bin, const char *what)
{
	Data	   *d = make_data(g);
	SiftTsShape sh = make_shape(bin);
	bool		serial;
	Vec			want = reference(d, &sh, &serial);

	PASS(!serial);
	for (uint32 w = 1; w <= 4; w++)
	{
		Vec			got = run(d, &sh, w);

		if (!same(&got, &want))
			fprintf(stderr, "%s: %u threads: %u groups, want %u\n", what, w, got.n, want.n);
		PASS(same(&got, &want));
		free(got.r);
	}
	free(want.r);
	free_data(d);
}

/* What the threads must leave to the scan, one batch on one thread */
static void
check_serial(const Gen *g, const SiftTsKey *bin, void (*poke) (Data *), bool expect_serial)
{
	Data	   *d = make_data(g);
	SiftTsShape sh = make_shape(bin);
	SiftArena  *a = sift_arena_create(CurrentMemoryContext, 4 << 20);
	SiftParGroupBatch out = {0};
	SiftErr		err = {0};
	SiftParCol	col[3] = {{d->ts, d->ts_null}, {d->dev, d->dev_null}, {d->val, d->val_null}};
	uint64		active[GRAN / 64];
	bool		ok;

	if (poke != NULL)
		poke(d);
	memset(active, 0xFF, sizeof(active));
	ok = sift_tseries_batch(&sh, col, active, GRAN, GRAN, a, &err, &out);
	PASS(ok != expect_serial);
	PASS(ok || err.code == SIFT_ERR_SERIAL);
	free_data(d);
}

static void
poke_infinite(Data *d)
{
	d->ts[17] = PG_INT64_MAX;
	d->ts_null[17] = false;
}

static void
poke_overflow(Data *d)
{
	d->ts[100] = PG_INT64_MIN + 1;
	d->ts_null[100] = false;
}

static void
poke_null_sum(Data *d)
{
	d->val_null[9] = true;
}

static void
poke_null_ts(Data *d)
{
	d->ts_null[9] = true;
	d->dev_null[9] = true;
}

int
main(void)
{
	const int64 y2000 = INT64CONST(0);
	const int64 strides[] = {SEC, 60 * SEC, 300 * SEC, 900 * SEC, 3600 * SEC, 86400 * SEC, 7 * 86400 * SEC, 13370000, 3 * 3600 * SEC + 1};
	const int64 origins[] = {0, INT64CONST(-946771200000000), 5 * SEC + 17, INT64CONST(1700000000000000)};
	Gen			g = {.t0 = y2000 + INT64CONST(86400000000) * 9000,.step = 3 * SEC,.ndev = 50};

	pgshim_keep();
	for (size_t s = 0; s < lengthof(strides); s++)
		for (size_t o = 0; o < lengthof(origins); o++)
		{
			SiftTsKey	k = bin_of(strides[s], origins[o]);
			char		what[64];

			g.ts_nulls = g.dev_nulls = (s + o) % 2 == 0;
			g.sparse = (s + o) % 3 == 0;
			g.ndev = s % 2 == 0 ? 50 : 3000;
			snprintf(what, sizeof(what), "stride %ld origin %ld", (long) strides[s], (long) origins[o]);
			check_parity(&g, &k, what);
		}
	g = (Gen) {.t0 = INT64CONST(-90000000000000000),.step = 40000 * SEC,.ndev = 20,.ts_nulls = true,.dev_nulls = true};
	{
		SiftTsKey	k = bin_of(SIFT_DATEBIN_MONTH, 0);

		check_parity(&g, &k, "month, before 2000");
		g.t0 = INT64CONST(700000000000000000);
		check_parity(&g, &k, "month, far after 2000");
	}
	g = (Gen) {.t0 = INT64CONST(-1000000000000),.step = 7 * SEC,.ndev = 300,.ts_nulls = true};
	{
		SiftTsKey	k = bin_of(3600 * SEC, INT64CONST(-946771200000000) - 123);

		check_parity(&g, &k, "negative timestamps");
		g.sparse = true;
		check_parity(&g, &k, "negative timestamps, sparse");
	}
	g = (Gen) {.t0 = 0,.step = 5 * SEC,.ndev = 8};
	{
		SiftTsKey	k = bin_of(3600 * SEC, 0);

		check_serial(&g, &k, NULL, false);
		check_serial(&g, &k, poke_null_ts, false);
		check_serial(&g, &k, poke_infinite, true);
		check_serial(&g, &k, poke_overflow, true);
		check_serial(&g, &k, poke_null_sum, true);
	}
	printf("par_tseries: ok (%d checks)\n", nchecks);
	return 0;
}
