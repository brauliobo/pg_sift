/*
 * par_radix.c - agg/v6/par/group.c and distinct.c on 1, 2, 3 and 4 threads (the trap shim flags the helpers: no palloc, no ereport), against the rows sorted and merged: every shape of key (words,
 * a text, words and texts in any order, several texts), the two regimes, the K best, skew, and arenas too small, where what the run folded and the items it hands back must still make the same answer.
 */
#include "postgres.h"

#include "agg/v6/par/distinct.h"
#include "agg/v6/par/group.h"
#include "core/par/pool.h"
#include "core/popcount.h"
#include "par_ref.h"
#include "par_host.h"
#include "pgshim.h"
#include "unit.h"

#define ROWS		100000
#define MANY		30000
#define FEW			40
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
roomy(uint32 workers, uint32 rec_words)
{
	return (Sizes) {(size_t) ROWS * (rec_words * 8 + 32) / workers + (16 << 20), 16 << 20};
}

typedef struct Run
{
	bool		overflowed;
	bool		created;
	uint32		left_items;
} Run;

/* ---- the grouped aggregation ---- */

typedef struct GCtx
{
	Src			src;
	Vec			out[SIFT_PAR_MAX_THREADS];
	SiftParRange *left;
	uint32		nleft;
} GCtx;

static void
g_emit(void *arg, int worker, const SiftParGroupRow *row)
{
	GCtx	   *c = arg;

	vec_push(&c->out[worker], c->src.sp, row);
}

static void
g_overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
	GCtx	   *c = arg;

	c->left = memcpy(malloc(sizeof(SiftParRange) * Max(nleft, 1)), left, sizeof(SiftParRange) * nleft);
	c->nleft = nleft;
}

/* The answer of a run: its groups, and the rows of the items it handed back as groups of one row; the K best of them */
static Vec
g_answer(GCtx *c)
{
	Vec			v = {0};

	for (int t = 0; t < SIFT_PAR_MAX_THREADS; t++)
		vec_append(&v, &c->out[t]);
	for (uint32 k = 0; k < c->nleft; k++)
		for (uint32 i = c->left[k].lo; i < c->left[k].hi; i++)
			vec_push_row(&v, c->src.sp, c->src.d, i);
	free(c->left);
	vec_normalize(&v, c->src.sp);
	vec_best(&v, c->src.sp);
	return v;
}

static Run
run_group(const Data *d, const SiftParGroupSpec *sp, bool high, uint32 workers, Sizes sz, Vec *answer)
{
	GCtx		c = {.src = {d, sp}};
	SiftParGroupHooks hooks = {.fill = src_group_fill,.emit = g_emit,.overflow = g_overflow,.arg = &c};
	SiftParArenas *ar;
	SiftParGroup *g;
	SiftErr		err = {0};
	Run			r = {0};

	pgshim_reset();
	ar = sift_par_arenas_create(CurrentMemoryContext, workers, SCRATCH, sz.state, sz.work);
	g = sift_par_group_create(ar, sp, high, d->n, &hooks, &err);
	if (g == NULL)
		return r;
	r.created = true;
	r.overflowed = !sift_par_run(&(SiftParCfg) {.nthreads = workers,.items = d->n}, &sift_par_group_ops, g, ar, NULL);
	r.left_items = 0;
	for (uint32 k = 0; k < c.nleft; k++)
		r.left_items += c.left[k].hi - c.left[k].lo;
	PASS(!sift_par_pool_busy());
	*answer = g_answer(&c);
	return r;
}

static SiftParGroupSpec
int_spec(int kw, int nsum, bool u32)
{
	return (SiftParGroupSpec) {.nk = kw,.nsum = nsum,.key1_u32 = kw == 2 && u32};
}

static SiftParGroupSpec
str_spec(int nsum)
{
	return (SiftParGroupSpec) {.nk = 1,.strs = 1,.nsum = nsum};
}

static SiftParGroupSpec
mixed_spec(int nk, int strs, int nsum)
{
	return (SiftParGroupSpec) {.nk = nk,.strs = strs,.nsum = nsum};
}

/* The words of a record of the shape: the keys, the sums, and the bytes of its texts */
static uint32
mixed_words(const SiftParGroupSpec *sp)
{
	return sp->nk + sp->nsum + 4 * __builtin_popcount(sp->strs) + 1;
}

static void
check_group(const Data *d, const SiftParGroupSpec *sp, bool high, uint32 workers, Sizes sz, Run *out)
{
	Vec			want = reference(d, sp),
				got = {0};
	Run			r = run_group(d, sp, high, workers, sz, &got);

	PASS(r.created || out != NULL);
	if (r.created && !vec_same(&got, &want, sp))
		fprintf(stderr, "nk=%d strs=%d nsum=%d u32=%d topk=%d order=%d high=%d w=%u state=%zu work=%zu overflowed=%d: %u groups, want %u\n", sp->nk, sp->strs, sp->nsum,
				sp->key1_u32, sp->topk, sp->order, high, workers, sz.state, sz.work, r.overflowed, got.n, want.n);
	PASS(!r.created || vec_same(&got, &want, sp));
	nparity += r.created;
	noverflow += r.overflowed;
	vec_free(&got);
	vec_free(&want);
	if (out != NULL)
		*out = r;
}

static void
test_group_ints(void)
{
	for (int kw = 1; kw <= SIFT_PAR_GROUP_MAX_KEYS; kw++)
		for (int nsum = 0; nsum <= 3; nsum += 3)
			for (uint32 ngroups = FEW; ngroups != 0; ngroups = ngroups == FEW ? MANY : 0)
				for (int high = 0; high <= 1; high++)
					for (int u32 = 0; u32 <= (kw == 2); u32++)
					{
						const SiftParGroupSpec sp = int_spec(kw, nsum, u32);
						Data	   *d = gen_int(kw, ROWS, ngroups, (kw + nsum + high) % 3 == 0, nsum, u32);

						for (uint32 w = 1; w <= 4; w++)
							check_group(d, &sp, high, w, roomy(w, kw + nsum + 1), NULL);
						data_free(d);
					}
}

static void
test_group_strs(void)
{
	for (int nsum = 0; nsum <= 2; nsum += 2)
		for (uint32 ngroups = FEW; ngroups != 0; ngroups = ngroups == FEW ? MANY : 0)
			for (int high = 0; high <= 1; high++)
			{
				const SiftParGroupSpec sp = str_spec(nsum);
				Data	   *d = gen_str(ROWS, ngroups, (nsum + high) % 2 == 0, nsum, true);

				for (uint32 w = 1; w <= 4; w++)
					check_group(d, &sp, high, w, roomy(w, 4 + nsum), NULL);
				data_free(d);
			}
}

/* The columns in every order of words and texts, the first one shared by a few groups: the groups that the words and the texts tell apart together */
static void
test_group_mixed(void)
{
	static const int shapes[][2] = {{2, 2}, {2, 1}, {3, 2}, {3, 5}, {2, 3}, {4, 10}};

	for (size_t sh = 0; sh < sizeof(shapes) / sizeof(shapes[0]); sh++)
		for (int nsum = 0; nsum <= 2; nsum += 2)
			for (uint32 ngroups = FEW; ngroups != 0; ngroups = ngroups == FEW ? MANY : 0)
				for (int high = 0; high <= 1; high++)
				{
					const SiftParGroupSpec sp = mixed_spec(shapes[sh][0], shapes[sh][1], nsum);
					Data	   *d = gen_mixed(sp.nk, sp.strs, ROWS, ngroups, (sh + nsum + high) % 3 == 0, nsum, true);

					for (uint32 w = 1; w <= 4; w++)
						check_group(d, &sp, high, w, roomy(w, mixed_words(&sp)), NULL);
					data_free(d);
				}
}

static void
test_group_topk(void)
{
	static const uint16 ks[] = {1, 10, 100};

	static const uint8 orders[] = {0, 1, SIFT_PAR_GROUP_BY_KEY};

	for (int shape = 0; shape <= 2; shape++)
	{
		const SiftParGroupSpec base = shape == 0 ? int_spec(2, 1, true) : shape == 1 ? str_spec(1) : mixed_spec(3, 2, 1);
		Data	   *d = shape == 0 ? gen_int(2, ROWS, MANY, false, 1, true) : gen_mixed(base.nk, base.strs, ROWS, MANY, false, 1, true);

		for (int ik = 0; ik < 3; ik++)
			for (int io = 0; io < 3; io++)
				for (int desc = 0; desc <= 1; desc++)
					for (int high = 0; high <= 1; high++)
					{
						SiftParGroupSpec sp = base;

						sp.topk = ks[ik];
						sp.order = orders[io];
						sp.desc = desc;
						for (uint32 w = 1; w <= 4; w += 3)
							check_group(d, &sp, high, w, roomy(w, mixed_words(&sp)), NULL);
					}
		data_free(d);
	}
}

/* Arenas from room for everything down to room for nothing: every run, whether it overflowed or not, gives the answer of the rows */
static void
sweep_group(const Data *d, const SiftParGroupSpec *sp, bool high, uint32 workers, uint32 rec_words)
{
	const Sizes full = roomy(workers, rec_words);
	uint32		overflows = 0,
				left = 0,
				drained = 0;

	for (int k = 0; k <= 12; k++)
		for (int what = 0; what < 2; what++)
		{
			Sizes		sz = full;
			Run			r;

			if (what == 0)
				sz.state = Max(full.state >> k, 64 << 10);
			else
				sz.work = Max(full.work >> (k + 2), 8 << 10);
			check_group(d, sp, high, workers, sz, &r);
			overflows += r.overflowed;
			left += r.left_items > 0;
			drained += r.overflowed && r.left_items == 0;
		}
	PASS(overflows > 0 && left > 0 && drained > 0);
}

static void
test_group_overflow(void)
{
	Data	   *ints = gen_int(2, ROWS, MANY, false, 2, true),
			   *strs = gen_str(ROWS, MANY, false, 2, true),
			   *mixed = gen_mixed(3, 2, ROWS, MANY, false, 2, true);

	for (uint32 w = 1; w <= 4; w += (w == 1 ? 1 : 2))
	{
		SiftParGroupSpec sp = int_spec(2, 2, true);

		sweep_group(ints, &sp, true, w, 5);
		sweep_group(ints, &sp, false, w, 5);
		sp.topk = 10;
		sp.order = 1;
		sp.desc = true;
		sweep_group(ints, &sp, true, w, 5);
		sp = str_spec(2);
		sweep_group(strs, &sp, true, w, 6);
		sp.topk = 100;
		sweep_group(strs, &sp, true, w, 6);
		sp = mixed_spec(3, 2, 2);
		sweep_group(mixed, &sp, true, w, mixed_words(&sp));
		sweep_group(mixed, &sp, false, w, mixed_words(&sp));
		sp.topk = 50;
		sp.order = SIFT_PAR_GROUP_BY_KEY;
		sweep_group(mixed, &sp, true, w, mixed_words(&sp));
	}
	data_free(ints);
	data_free(strs);
	data_free(mixed);
}

/* ---- COUNT(DISTINCT) ---- */

typedef struct DCtx
{
	Src			src;
	uint32		ngroups;
	uint64	   *seen_val;
	uint32	   *seen_gid;
	uint32		nseen;
	uint32		cap;
	SiftParRange *left;
	uint32		nleft;
} DCtx;

static void
d_value(void *arg, uint32 gid, uint64 value)
{
	DCtx	   *c = arg;

	if (c->nseen == c->cap)
	{
		c->cap = c->cap ? c->cap * 2 : 1 << 16;
		c->seen_val = realloc(c->seen_val, sizeof(uint64) * c->cap);
		c->seen_gid = realloc(c->seen_gid, sizeof(uint32) * c->cap);
	}
	c->seen_val[c->nseen] = value;
	c->seen_gid[c->nseen++] = gid;
}

static void
d_overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
	DCtx	   *c = arg;

	c->left = memcpy(malloc(sizeof(SiftParRange) * Max(nleft, 1)), left, sizeof(SiftParRange) * nleft);
	c->nleft = nleft;
}

typedef struct Pair
{
	uint32		gid;
	uint64		val;
} Pair;

static int
pair_cmp(const void *a, const void *b)
{
	const Pair *x = a,
			   *y = b;

	return x->gid != y->gid ? (x->gid < y->gid ? -1 : 1) : x->val != y->val ? (x->val < y->val ? -1 : 1) : 0;
}

/* The distinct values of each group of the pairs (gid, val), sorted: counts[g]; the number of pairs is n (they are freed here) */
static void
count_pairs(Pair *p, uint32 n, uint64 *counts)
{
	qsort(p, n, sizeof(Pair), pair_cmp);
	for (uint32 i = 0; i < n; i++)
		counts[p[i].gid] += i == 0 || pair_cmp(&p[i - 1], &p[i]) != 0;
	free(p);
}

static void
count_rows(const Data *d, uint64 *counts)
{
	Pair	   *p = malloc(sizeof(Pair) * d->n);

	for (uint32 i = 0; i < d->n; i++)
		p[i] = (Pair) {d->gid != NULL ? d->gid[i] : 0, d->val[i]};
	count_pairs(p, d->n, counts);
}

static void
count_answer(DCtx *c, bool overflowed, SiftParDistinct *dd, uint64 *counts)
{
	if (!overflowed)
	{
		if (c->ngroups == 0)
			counts[0] = sift_par_distinct_count(dd);
		else
			sift_par_distinct_groups(dd, counts);
		return;
	}
	{
		uint32		n = c->nseen;
		Pair	   *p = malloc(sizeof(Pair) * (n + c->src.d->n));

		for (uint32 i = 0; i < n; i++)
			p[i] = (Pair) {c->seen_gid[i], c->seen_val[i]};
		for (uint32 k = 0; k < c->nleft; k++)
			for (uint32 i = c->left[k].lo; i < c->left[k].hi; i++)
				p[n++] = (Pair) {c->src.d->gid != NULL ? c->src.d->gid[i] : 0, c->src.d->val[i]};
		count_pairs(p, n, counts);
	}
}

static bool
check_distinct(const Data *d, uint32 ngroups, uint32 workers, Sizes sz)
{
	const uint32 ng = Max(ngroups, 1);
	DCtx		c = {.src = {d, NULL},.ngroups = ngroups};
	SiftParDistinctHooks hooks = {.fill = src_distinct_fill,.value = d_value,.overflow = d_overflow,.arg = &c};
	uint64	   *want = calloc(ng, sizeof(uint64)),
			   *got = calloc(ng, sizeof(uint64));
	SiftParArenas *ar;
	SiftParDistinct *dd;
	SiftErr		err = {0};
	bool		overflowed;

	pgshim_reset();
	ar = sift_par_arenas_create(CurrentMemoryContext, workers, SCRATCH, sz.state, sz.work);
	dd = sift_par_distinct_create(ar, ngroups, &hooks, &err);
	if (dd == NULL)
	{
		free(want);
		free(got);
		return false;
	}
	overflowed = !sift_par_run(&(SiftParCfg) {.nthreads = workers,.items = d->n}, &sift_par_distinct_ops, dd, ar, NULL);
	PASS(!sift_par_pool_busy());
	count_rows(d, want);
	count_answer(&c, overflowed, dd, got);
	PASS(memcmp(want, got, sizeof(uint64) * ng) == 0);
	nparity++;
	noverflow += overflowed;
	free(want);
	free(got);
	free(c.seen_val);
	free(c.seen_gid);
	free(c.left);
	return overflowed;
}

static void
test_distinct(void)
{
	static const uint32 ndist[] = {1, 5000, ROWS};
	static const uint32 groups[] = {0, 1, 50, 3000};

	for (int in = 0; in < 3; in++)
		for (int ig = 0; ig < 4; ig++)
			for (int zero = 0; zero <= 1; zero++)
			{
				Data	   *d = gen_dist(ROWS, ndist[in], groups[ig], zero);

				for (uint32 w = 1; w <= 4; w++)
					PASS(!check_distinct(d, groups[ig], w, roomy(w, 2)));
				data_free(d);
			}
}

static void
test_distinct_overflow(void)
{
	for (int grouped = 0; grouped <= 1; grouped++)
	{
		Data	   *d = gen_dist(ROWS, ROWS, grouped ? 50 : 0, true);

		for (uint32 w = 1; w <= 4; w += (w == 1 ? 1 : 2))
		{
			const Sizes full = roomy(w, 2);
			uint32		overflows = 0;

			for (int k = 0; k <= 12; k++)
			{
				Sizes		sz = full;

				sz.state = Max(full.state >> k, 64 << 10);
				overflows += check_distinct(d, grouped ? 50 : 0, w, sz);
				sz = full;
				sz.work = Max(full.work >> (k + 2), 8 << 10);
				overflows += check_distinct(d, grouped ? 50 : 0, w, sz);
			}
			PASS(overflows > 0);
		}
		data_free(d);
	}
}

typedef struct OrJob
{
	const uint64 *const *bits;
	uint32		nwords;
	uint64	   *out;
	uint64		set[SIFT_PAR_MAX_THREADS];
	int			nthreads;
} OrJob;

static void
or_part(void *arg, int t)
{
	OrJob	   *j = arg;

	j->set[t] = sift_par_distinct_bits_or(j->bits, 3, j->nwords, t, j->nthreads, j->out);
}

static void
test_bits_or(void)
{
	enum {NWORDS = 4099};
	uint64	   *bits[3],
			   *want = calloc(NWORDS, sizeof(uint64));
	uint64		set = 0;

	for (int b = 0; b < 3; b++)
	{
		bits[b] = malloc(sizeof(uint64) * NWORDS);
		for (uint32 i = 0; i < NWORDS; i++)
		{
			bits[b][i] = gen_next() & gen_next() & gen_next();
			want[i] |= bits[b][i];
		}
	}
	for (uint32 i = 0; i < NWORDS; i++)
		set += sift_pc64(want[i]);
	for (int w = 1; w <= 4; w++)
	{
		OrJob		j = {.bits = (const uint64 *const *) bits,.nwords = NWORDS,.out = calloc(NWORDS, sizeof(uint64)),.nthreads = w};
		uint64		sum = 0;

		unit_run_workers(w, or_part, &j);
		for (int t = 0; t < w; t++)
			sum += j.set[t];
		PASS(sum == set && memcmp(j.out, want, sizeof(uint64) * NWORDS) == 0);
		free(j.out);
	}
	for (int b = 0; b < 3; b++)
		free(bits[b]);
	free(want);
}

int
main(void)
{
	pgshim_keep();
	test_group_ints();
	test_group_strs();
	test_group_mixed();
	test_group_topk();
	test_group_overflow();
	test_distinct();
	test_distinct_overflow();
	test_bits_or();
	printf("par_radix: ok (%d parity comparisons, %d of them after an overflow; %d checks)\n", nparity, noverflow, nchecks);
	return 0;
}
