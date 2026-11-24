/*
 * par_distinct.c - agg/v6/par/distinct.c and keytab.c on 1, 2, 3 and 4 threads (the trap shim flags the helpers: no palloc, no ereport), against the rows sorted and counted: every shape of key (none,
 * words, a text, both), integer and text values (a text of one part is another place than the same text of another), NULL values, the empty text and the value 0, mixed sums, skew, and arenas too small,
 * where the run says so and gives no answer. The key table is tried alone with a hash that every key shares.
 */
#include "postgres.h"

#include "agg/v6/par/keytab.h"
#include "core/par/pool.h"
#include "par_dist.h"
#include "par_host.h"
#include "pgshim.h"
#include "unit.h"

#define ROWS		20000
#define SCRATCH		(4 << 20)

static int	nchecks,
			nparity,
			noverflow;

#define PASS(cond) do { nchecks++; CHECK(cond); } while (0)

typedef struct Sizes
{
	size_t		state;
	size_t		work;
} Sizes;

static Sizes
roomy(uint32 workers)
{
	return (Sizes) {(size_t) ROWS * 80 / workers + (32 << 20), 16 << 20};
}

/* ---- the answer ---- */

typedef struct DRow
{
	uint64		key[SIFT_PAR_GROUP_MAX_KEYS];
	uint8	   *str;
	uint32		len;
	uint64		rows;
	uint64		sum[SIFT_PAR_GROUP_MAX_SUM];
	uint64		distinct;
	SiftStrRef	mm[SIFT_PAR_DISTINCT_MM_MAX];	/* copies: p NULL when no row had a text */
} DRow;

typedef struct DVec
{
	DRow	   *r;
	uint32		n;
	uint32		cap;
} DVec;

static DRow *
dvec_new(DVec *v)
{
	if (v->n == v->cap)
		v->r = realloc(v->r, sizeof(DRow) * (v->cap = v->cap ? v->cap * 2 : 256));
	memset(&v->r[v->n], 0, sizeof(DRow));
	return &v->r[v->n++];
}

static void
dvec_free(DVec *v)
{
	for (uint32 i = 0; i < v->n; i++)
	{
		free(v->r[i].str);
		for (int m = 0; m < SIFT_PAR_DISTINCT_MM_MAX; m++)
			free((void *) v->r[i].mm[m].p);
	}
	free(v->r);
	*v = (DVec) {0};
}

static const SiftParDistinctSpec *dcur_spec;

static int
drow_cmp(const void *a, const void *b)
{
	const DRow *x = a,
			   *y = b;
	int			c;

	for (int j = 0; j < dcur_spec->kw; j++)
		if (x->key[j] != y->key[j])
			return x->key[j] < y->key[j] ? -1 : 1;
	if (x->len != y->len)
		return x->len < y->len ? -1 : 1;
	c = x->len > 0 ? memcmp(x->str, y->str, x->len) : 0;
	return c < 0 ? -1 : c > 0;
}

static void
dgot_push(void *arg, const SiftParDistinctRow *row)
{
	DRow	   *r = dvec_new(arg);

	memcpy(r->key, row->key, sizeof(r->key));
	r->len = row->len;
	r->str = row->len > 0 ? memcpy(malloc(row->len), row->str, row->len) : NULL;
	r->rows = row->rows;
	memcpy(r->sum, row->sum, sizeof(r->sum));
	r->distinct = row->distinct;
	for (int m = 0; m < SIFT_PAR_DISTINCT_MM_MAX; m++)
		if (row->mm[m].p != NULL)
			r->mm[m] = (SiftStrRef) {memcpy(malloc(row->mm[m].len + 1), row->mm[m].p, row->mm[m].len), row->mm[m].len};
}

/* The row i of the data as a DRow (rows 1, the sums of the row, no distinct yet) */
static void
drow_of(const DData *d, const SiftParDistinctSpec *sp, uint32 i, DRow *r)
{
	memset(r, 0, sizeof(*r));
	for (int j = 0; j < sp->kw; j++)
		r->key[j] = d->key[j][i];
	if (sp->kstr)
	{
		r->str = (uint8 *) d->kstr[i].p;
		r->len = d->kstr[i].len;
	}
	r->rows = 1;
	for (int m = 0; m < sp->nsum; m++)
		r->sum[m] = d->sum[m][i];
}

static const DData *dcur_data;

/* The rows by key, and with the equal keys by value: the NULLs first */
static int
dindex_cmp(const void *a, const void *b)
{
	const uint32 i = *(const uint32 *) a,
				j = *(const uint32 *) b;
	DRow		x,
				y;
	int			c;

	drow_of(dcur_data, dcur_spec, i, &x);
	drow_of(dcur_data, dcur_spec, j, &y);
	if ((c = drow_cmp(&x, &y)) != 0)
		return c;
	if (dcur_spec->vstr)
	{
		const SiftStrRef p = dcur_data->vstr[i],
					q = dcur_data->vstr[j];

		if (p.len != q.len)
			return p.len == SIFT_STR_NULL ? -1 : q.len == SIFT_STR_NULL ? 1 : p.len < q.len ? -1 : 1;
		c = p.len > 0 && p.len != SIFT_STR_NULL ? memcmp(p.p, q.p, p.len) : 0;
		return c < 0 ? -1 : c > 0;
	}
	if (dcur_data->vnull[i] != dcur_data->vnull[j])
		return dcur_data->vnull[i] ? -1 : 1;
	return dcur_data->val[i] < dcur_data->val[j] ? -1 : dcur_data->val[i] > dcur_data->val[j];
}

static bool
dvalue_null(const DData *d, const SiftParDistinctSpec *sp, uint32 i)
{
	return sp->vstr ? d->vstr[i].len == SIFT_STR_NULL : d->vnull[i];
}

static bool
dvalue_same(const DData *d, const SiftParDistinctSpec *sp, uint32 i, uint32 j)
{
	dcur_data = d;
	dcur_spec = sp;
	return dindex_cmp(&i, &j) == 0;
}

/* The text s into the minimum or maximum m of the row: by the bytes, then the length */
static void
dref_mm(DRow *r, int m, bool max, SiftStrRef s)
{
	int			c;

	if (s.len == SIFT_STR_NULL)
		return;
	if (r->mm[m].p != NULL)
	{
		c = memcmp(s.p, r->mm[m].p, Min(s.len, r->mm[m].len));
		if (c == 0)
			c = s.len < r->mm[m].len ? -1 : s.len > r->mm[m].len;
		if (max ? c <= 0 : c >= 0)
			return;
		free((void *) r->mm[m].p);
	}
	r->mm[m] = (SiftStrRef) {memcpy(malloc(s.len + 1), s.p, s.len), s.len};
}

/* The answer of the rows: one row per key (with no key, one, also with no rows), by sorting; the str of a row is the data's */
static DVec
dreference(const DData *d, const SiftParDistinctSpec *sp)
{
	uint32	   *idx = malloc(sizeof(uint32) * Max(d->n, 1));
	DVec		v = {0};
	DRow	   *cur = NULL;

	dcur_data = d;
	dcur_spec = sp;
	for (uint32 i = 0; i < d->n; i++)
		idx[i] = i;
	qsort(idx, d->n, sizeof(uint32), dindex_cmp);
	if (sp->kw == 0 && !sp->kstr)
		cur = dvec_new(&v);
	for (uint32 k = 0; k < d->n; k++)
	{
		DRow		row;
		bool		fresh_key;

		drow_of(d, sp, idx[k], &row);
		fresh_key = cur == NULL || drow_cmp(cur, &row) != 0;
		if (fresh_key)
		{
			cur = dvec_new(&v);
			memcpy(cur->key, row.key, sizeof(row.key));
			cur->len = row.len;
			cur->str = row.len > 0 ? memcpy(malloc(row.len), row.str, row.len) : NULL;
		}
		cur->rows++;
		for (int m = 0; m < sp->nsum; m++)
			cur->sum[m] += row.sum[m];
		for (int m = 0; m < sp->nmm; m++)
			dref_mm(cur, m, (sp->mm_max >> m) & 1, d->mm[m][idx[k]]);
		if (!dvalue_null(d, sp, idx[k]) && (fresh_key || k == 0 || !dvalue_same(d, sp, idx[k - 1], idx[k])))
			cur->distinct++;
	}
	free(idx);
	return v;
}

static bool
same_answer(DVec *got, const DVec *want, const SiftParDistinctSpec *sp)
{
	dcur_spec = sp;
	qsort(got->r, got->n, sizeof(DRow), drow_cmp);
	if (got->n != want->n)
		return false;
	for (uint32 i = 0; i < got->n; i++)
	{
		const DRow *g = &got->r[i],
				   *w = &want->r[i];

		if (drow_cmp(g, w) != 0 || g->rows != w->rows || g->distinct != w->distinct || memcmp(g->sum, w->sum, sizeof(uint64) * sp->nsum) != 0)
			return false;
		for (int m = 0; m < sp->nmm; m++)
			if (g->mm[m].len != w->mm[m].len || (g->mm[m].p == NULL) != (w->mm[m].p == NULL) || (w->mm[m].len > 0 && memcmp(g->mm[m].p, w->mm[m].p, w->mm[m].len) != 0))
				return false;
	}
	return true;
}

/* true when the run said it had no room for the rows (and then it gives no answer) */
static bool
run_one(const DData *d, const SiftParDistinctSpec *sp, uint32 workers, Sizes sz, DVec *got)
{
	DSrc		c = {.d = d,.sp = sp};
	SiftParDistinctHooks hooks = {.fill = dsrc_fill,.overflow = dsrc_overflow,.arg = &c};
	SiftParArenas *ar;
	SiftParDistinct *dd;
	SiftErr		err = {0};
	bool		done;

	pgshim_reset();
	ar = sift_par_arenas_create(CurrentMemoryContext, workers, SCRATCH, sz.state, sz.work);
	if ((dd = sift_par_distinct_create(ar, sp, &hooks, &err)) == NULL)
		return true;
	done = sift_par_run(&(SiftParCfg) {.nthreads = workers,.items = d->n}, &sift_par_distinct_ops, dd, ar, NULL);
	PASS(!sift_par_pool_busy());
	PASS(done != c.overflowed);
	if (!done)
		return true;
	sift_par_distinct_each(dd, dgot_push, got);
	return false;
}

static bool
check(const DData *d, const DVec *want, const SiftParDistinctSpec *sp, uint32 workers, Sizes sz)
{
	DVec		got = {0};
	const bool	overflowed = run_one(d, sp, workers, sz, &got);

	if (!overflowed && !same_answer(&got, want, sp))
		fprintf(stderr, "kw=%d kstr=%d nsum=%d vstr=%d w=%u n=%u: %u groups, want %u\n", sp->kw, sp->kstr, sp->nsum, sp->vstr, workers, d->n, got.n, want->n);
	PASS(overflowed || same_answer(&got, want, sp));
	nparity += !overflowed;
	noverflow += overflowed;
	dvec_free(&got);
	return overflowed;
}

static void
check_all_threads(const DData *d, const SiftParDistinctSpec *sp)
{
	DVec		want = dreference(d, sp);

	for (uint32 w = 1; w <= 4; w++)
		PASS(!check(d, &want, sp, w, roomy(w)));
	dvec_free(&want);
}

static void
test_shapes(void)
{
	static const SiftParDistinctSpec keys[] = {{0, false}, {1, false}, {2, false}, {4, false}, {0, true}, {1, true}, {2, true}, {.kw = 1,.direct_n = 6000,.direct_lo = -300}};
	static const uint32 ngroups[] = {1, 40, 5000};
	static const uint32 ndist[] = {7, 4000};
	int			variant = 0;

	for (int ik = 0; ik < lengthof(keys); ik++)
		for (int vstr = 0; vstr <= 1; vstr++)
			for (int nsum = 0; nsum <= 3; nsum += 3)
				for (int ig = 0; ig < lengthof(ngroups); ig++)
					for (int in = 0; in < lengthof(ndist); in++)
					{
						SiftParDistinctSpec sp = keys[ik];
						DData	   *d;

						if (ig > 0 && sp.kw == 0 && !sp.kstr)
							continue;
						sp.vstr = vstr;
						sp.nsum = nsum;
						sp.nmm = variant % 3 == 0 ? 2 : variant % 3 == 1 ? 1 : 0;
						sp.mm_max = 2;
						d = dgen(&sp, ROWS, ngroups[ig], ndist[in], variant % 2, (variant / 2) % 2);
						variant++;
						check_all_threads(d, &sp);
						dfree(d);
					}
}

static void
all_null_values(DData *d, const SiftParDistinctSpec *sp)
{
	for (uint32 i = 0; i < d->n; i++)
		if (sp->vstr)
			d->vstr[i] = (SiftStrRef) {NULL, SIFT_STR_NULL};
		else
			d->vnull[i] = 1;
}

static void
test_edges(void)
{
	static const SiftParDistinctSpec shapes[] = {{0, false, 1, false}, {0, false, 1, true}, {1, true, 2, false}, {1, true, 2, true}, {2, false, 0, true}, {0, true, 0, false},
												 {.kw = 1,.nsum = 1,.nmm = 1,.direct_n = 40,.direct_lo = 7}, {.kw = 1,.kstr = true,.nsum = 1,.vstr = true,.nmm = 2,.mm_max = 1},
												 {.nmm = 2,.mm_max = 2}};
	static const uint32 sizes[] = {0, 1, 3, 17, ROWS};

	for (int is = 0; is < lengthof(shapes); is++)
		for (int in = 0; in < lengthof(sizes); in++)
		{
			DData	   *d = dgen(&shapes[is], sizes[in], 30, 1, false, false);

			check_all_threads(d, &shapes[is]);
			all_null_values(d, &shapes[is]);
			check_all_threads(d, &shapes[is]);
			dfree(d);
		}
}

/* Arenas from room for everything down to room for nothing: a run that has no room says so, and every other gives the answer of the rows */
static void
sweep(const DData *d, const SiftParDistinctSpec *sp, uint32 workers)
{
	const Sizes full = roomy(workers);
	DVec		want = dreference(d, sp);
	uint32		overflows = 0,
				answers = 0;

	for (int k = 0; k <= 14; k++)
		for (int what = 0; what < 2; what++)
		{
			Sizes		sz = full;
			bool		overflowed;

			if (what == 0)
				sz.state = Max(full.state >> k, 64 << 10);
			else
				sz.work = Max(full.work >> (k + 2), 8 << 10);
			overflowed = check(d, &want, sp, workers, sz);
			overflows += overflowed;
			answers += !overflowed;
		}
	PASS(overflows > 0 && answers > 0);
	dvec_free(&want);
}

static void
test_overflow(void)
{
	static const SiftParDistinctSpec shapes[] = {{0, false, 0, false}, {0, false, 0, true}, {1, false, 2, false}, {1, true, 2, true}, {0, true, 1, true},
												 {.kw = 1,.nsum = 1,.vstr = true,.nmm = 1,.direct_n = 3000,.direct_lo = -1}, {.kw = 1,.kstr = true,.nmm = 2,.mm_max = 1}};

	for (int is = 0; is < lengthof(shapes); is++)
	{
		DData	   *d = dgen(&shapes[is], ROWS, 3000, ROWS, false, true);

		for (uint32 w = 1; w <= 4; w += (w == 1 ? 1 : 2))
			sweep(d, &shapes[is], w);
		dfree(d);
	}
}

/* ---- the key table, with one hash for every key: only the bytes and the words tell them apart ---- */

typedef struct KeyIn
{
	uint64		w;
	char		buf[24];
	uint32		len;
} KeyIn;

static KeyIn
key_in(uint32 i)
{
	KeyIn		k = {.w = i % 7};

	k.len = snprintf(k.buf, sizeof(k.buf), "%.*s%u", (int) (i / 7 % 3), "ab", i / 21);
	return k;
}

static uint32
intern_in(SiftKeyTab *t, const KeyIn *in, uint64 h)
{
	const SiftTabKey k = {.h = h,.w = &in->w,.p = (const uint8 *) in->buf,.len = in->len};

	return sift_keytab_intern(t, &k);
}

static void
test_keytab(void)
{
	enum {KEYS = 5000};
	SiftArena  *a = sift_arena_create(CurrentMemoryContext, 16 << 20);
	const SiftKeyShape sh = {.kw = 1,.str = true,.npay = 1};
	SiftKeyTab	t,
				borrower;
	KeyIn		in[KEYS];
	const uint64 word = 5;
	uint8		bytes[3] = {1, 2, 3};
	SiftTabKey		raw = {.h = 7,.w = &word,.p = bytes,.len = 3};

	PASS(sift_keytab_init(&t, a, sh, false) && sift_keytab_init(&borrower, a, sh, true));
	for (uint32 i = 0; i < KEYS; i++)
	{
		in[i] = key_in(i);
		PASS(intern_in(&t, &in[i], 42) == i);
	}
	for (uint32 i = 0; i < KEYS; i++)
	{
		sift_keytab_pay(&t, sift_keytab_at(&t, i))[0] = i;
		memset(in[i].buf, 'z', in[i].len);
	}
	PASS(t.count == KEYS);
	for (uint32 i = 0; i < KEYS; i++)
	{
		const KeyIn	again = key_in(i);

		PASS(intern_in(&t, &again, 42) == i && sift_keytab_pay(&t, sift_keytab_at(&t, i))[0] == i);
	}
	PASS(t.count == KEYS);
	for (uint32 i = 0; i < KEYS; i++)
		PASS(sift_keytab_intern_of(&borrower, sift_keytab_at(&t, i)) == i && sift_keytab_key(sift_keytab_at(&borrower, i)).p == sift_keytab_key(sift_keytab_at(&t, i)).p);
	raw.h = sift_key_hash(&sh, raw.w, raw.p, raw.len);
	{
		uint8		copy[3] = {1, 2, 3};
		const SiftTabKey same = {.h = raw.h,.w = raw.w,.p = copy,.len = 3};
		const uint32 id = sift_keytab_intern(&t, &raw);

		PASS(id == KEYS && sift_keytab_intern(&t, &same) == id && sift_key_hash(&sh, same.w, same.p, same.len) == raw.h);
	}
}

static void
test_keytab_direct(void)
{
	SiftArena  *a = sift_arena_create(CurrentMemoryContext, 1 << 20);
	const SiftKeyShape sh = {.kw = 1,.npay = 1};
	SiftKeyTab	t;
	const uint64 in[] = {(uint64) -5, (uint64) 0, 5, 6, (uint64) -6};
	const uint32 id[] = {0, 5, 10, SIFT_KEYTAB_NONE, SIFT_KEYTAB_NONE};

	PASS(sift_keytab_init_direct(&t, a, sh, -5, 11) && t.count == 11);
	for (int i = 0; i < 5; i++)
	{
		const SiftTabKey k = {.h = 0,.w = &in[i]};

		PASS(sift_keytab_intern(&t, &k) == id[i]);
	}
	for (uint32 i = 0; i < 11; i++)
	{
		PASS(sift_keytab_pay(&t, sift_keytab_at(&t, i))[0] == 0);
	}
	PASS(!sift_keytab_init_direct(&t, a, sh, 0, 1 << 20));
}

int
main(void)
{
	pgshim_keep();
	test_keytab();
	test_keytab_direct();
	test_shapes();
	test_edges();
	test_overflow();
	printf("par_distinct: ok (%d parity comparisons, %d runs with no room; %d checks)\n", nparity, noverflow, nchecks);
	return 0;
}
