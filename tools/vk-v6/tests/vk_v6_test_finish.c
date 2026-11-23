/*
 * tests/vk_v6_test_finish.c - T12: the finishes of a hash job (agg/vk/shaders/vk_ht_compact.comp, vk_ht_topk.comp) against the scalar oracle over the same table of pairs or groups:
 *   COUNT(DISTINCT x)                       the number of pairs (the keys, x) whose x is not NULL (SIFT_VK_FIN_COUNT);
 *   ... by the dense group of the keys      the count of each group, and the pairs whose x is NULL apart (SIFT_VK_FIN_DENSE_COUNT);
 *   ORDER BY a count or a key LIMIT K       every group of the K best is a candidate, each candidate is a group of the table with its rows, and the ties with the K-th are all candidates
 *                                           or, when the columns are all the sort columns, only as many as make K (SIFT_VK_FIN_TOPK).
 */
#include <stdlib.h>
#include <string.h>

#include "vk_v6_agg_case.h"

static SiftVk *vk;

#define COUNT_MASK (((int64_t) 1 << SIFT_VK_COUNT_BITS) - 1)

static void
ld(Case *c, uint32_t reg, uint32_t col)
{
	P_COL(&c->p, reg, col, SIFT_VK_T_I64);
}

static SiftVkStatus
run(Case *c, SiftVkResult *res)
{
	c->j.nops = c->p.n;
	c->j.ops = c->p.w;
	return sift_vk_run(vk, &c->j, NULL, res);
}

/* the groups of the job as the oracle makes them (the finish does not change them) */
static bool
reference(const TData *d, const Case *c, RefResult *ref)
{
	SiftVkJob	j = c->j;

	j.finish = SIFT_VK_FIN_GROUPS;
	j.nops = c->p.n;
	j.ops = c->p.w;
	return ref_run(d, &j, ref);
}

static void
distinct_ungrouped(const TData *d, const char *name, uint32_t col)
{
	Case		c;
	RefResult	ref;
	SiftVkResult res;
	uint64_t	want = 0;
	SiftVkStatus st;

	case_init(&c, d, SIFT_VK_HASH);
	ld(&c, 0, col);
	c.j.nkeys = 1;
	c.j.key_reg[0] = 0;
	c.j.est_groups = 8192;
	c.j.finish = SIFT_VK_FIN_COUNT;
	CHECK(reference(d, &c, &ref), "T12 %s: the oracle does not know the job", name);
	for (uint32_t i = 0; i < ref.n; i++)
		want += (ref.g[i].nullmask & 1) == 0;
	st = run(&c, &res);
	CHECK(st == SIFT_VK_OK, "T12 %s: status %d", name, st);
	CHECK(st != SIFT_VK_OK || res.distinct == want, "T12 %s: %llu distinct values, the oracle says %llu", name, (unsigned long long) res.distinct, (unsigned long long) want);
	printf("   %-40s %llu distinct values of %u pairs\n", name, (unsigned long long) want, ref.n);
	ref_free(&ref);
}

/* the dense number of the group of the first nk keys of a pair, the digits as a DENSE job counts them */
static uint32_t
dense_of(const RGroup *g, uint32_t nk, const SiftVkJob *j)
{
	uint32_t	n = 0;

	for (uint32_t i = 0; i < nk; i++)
		n = n * j->dense_span[i] + ((g->nullmask >> i) & 1 ? j->dense_span[i] - 1 : (uint32_t) (g->key[i] - j->dense_kmin[i]));
	return n;
}

static void
distinct_grouped(const TData *d, const char *name, uint32_t nk, const uint32_t *keycol, const int64_t *kmin, const uint32_t *span)
{
	Case		c;
	RefResult	ref;
	SiftVkResult res;
	SiftVkStatus st;
	uint32_t	groups = 1;
	int64_t	   *low,
			   *nulls;

	case_init(&c, d, SIFT_VK_HASH);
	for (uint32_t i = 0; i < nk; i++)
	{
		ld(&c, i, keycol[i]);
		c.j.key_reg[i] = (uint8_t) i;
		c.j.dense_kmin[i] = kmin[i];
		c.j.dense_span[i] = span[i];
		groups *= span[i];
	}
	ld(&c, nk, C_V32);
	c.j.key_reg[nk] = (uint8_t) nk;
	c.j.nkeys = nk + 1;
	c.j.est_groups = 65536;
	c.j.finish = SIFT_VK_FIN_DENSE_COUNT;
	low = calloc(groups, sizeof(int64_t));
	nulls = calloc(groups, sizeof(int64_t));
	CHECK(reference(d, &c, &ref), "T12 %s: the oracle does not know the job", name);
	for (uint32_t i = 0; i < ref.n; i++)
	{
		const uint32_t g = dense_of(&ref.g[i], nk, &c.j);

		if ((ref.g[i].nullmask >> nk) & 1)
			nulls[g]++;
		else
			low[g]++;
	}
	st = run(&c, &res);
	CHECK(st == SIFT_VK_OK, "T12 %s: status %d", name, st);
	for (uint32_t g = 0; st == SIFT_VK_OK && g < groups; g++)
		CHECK((res.acc[g] & COUNT_MASK) == low[g] && (res.acc[g] >> SIFT_VK_COUNT_BITS) == nulls[g], "T12 %s: group %u counts %lld and %lld NULL pairs, the oracle %lld and %lld", name, g,
			  (long long) (res.acc[g] & COUNT_MASK), (long long) (res.acc[g] >> SIFT_VK_COUNT_BITS), (long long) low[g], (long long) nulls[g]);
	printf("   %-40s %u groups of %u pairs\n", name, groups, ref.n);
	free(low);
	free(nulls);
	ref_free(&ref);
}

typedef struct Order
{
	const SiftVkTopk *t;
	const SiftVkJob *j;
} Order;

static int64_t
comp_val(const RGroup *g, uint32_t w, bool *isnull)
{
	*isnull = (w & 1) == 0 && ((g->nullmask >> ((w >> 1) & 255)) & 1);
	return (w & 1) ? g->a[(w >> 1) & 255].cnt : g->key[(w >> 1) & 255];
}

/* the order of two groups in the first columns of the top K: negative when a is first */
static int
order_cmp(const void *pa, const void *pb, void *arg)
{
	const Order *o = arg;

	for (uint32_t c = 0; c < o->t->ncomp; c++)
	{
		const uint32_t w = o->t->comp[c];
		bool		an,
					bn;
		int64_t		av = comp_val(pa, w, &an),
					bv = comp_val(pb, w, &bn);

		if (an != bn)
			return an == (((w >> 10) & 1) != 0) ? -1 : 1;
		if (!an && av != bv)
			return (av < bv) == (((w >> 9) & 1) == 0) ? -1 : 1;
	}
	return 0;
}

static void
topk_case(const TData *d, const char *name, uint32_t keycol, uint32_t k, uint32_t ncomp, uint32_t c0, uint32_t c1, bool cover)
{
	Case		c;
	RefResult	ref;
	SiftVkResult res;
	SiftVkStatus st;
	Order		o;
	uint32_t	before = 0,
				ties = 0,
				found_before = 0,
				nk = 1;

	case_init(&c, d, SIFT_VK_HASH);
	ld(&c, 0, keycol);
	ld(&c, 1, C_V32);
	c.j.nkeys = 1;
	c.j.key_reg[0] = 0;
	add_agg(&c, SIFT_VK_AK_COUNT_STAR, 255, 255);
	add_agg(&c, SIFT_VK_AK_SUM_I, 1, 255);
	c.j.est_groups = 65536;
	c.j.finish = SIFT_VK_FIN_TOPK;
	c.j.topk = (SiftVkTopk) {.k = k, .ncomp = ncomp, .comp = {c0, c1}, .cover = cover};
	o = (Order) {&c.j.topk, &c.j};
	CHECK(reference(d, &c, &ref), "T12 %s: the oracle does not know the job", name);
	for (uint32_t i = 0; i < ref.n; i++)
		ref.g[i].a[0].cnt = ref.g[i].rows;
	qsort_r(ref.g, ref.n, sizeof(RGroup), order_cmp, &o);
	if (ref.n > 0)
		for (uint32_t i = 0; i < ref.n; i++)
		{
			const int	cmp = order_cmp(&ref.g[i], &ref.g[(k < ref.n ? k : ref.n) - 1], &o);

			before += cmp < 0;
			ties += cmp == 0;
		}
	st = run(&c, &res);
	CHECK(st == SIFT_VK_OK, "T12 %s: status %d", name, st);
	for (uint32_t i = 0; st == SIFT_VK_OK && i < res.ngroups; i++)
	{
		const int64_t *rec = res.acc + (uint64_t) i * res.rec_words;
		uint32_t	g = 0;

		while (g < ref.n && !((ref.g[g].nullmask & 1) == (uint64_t) (rec[nk] & 1) && ((rec[nk] & 1) || ref.g[g].key[0] == rec[0])))
			g++;
		CHECK(g < ref.n, "T12 %s: candidate %u is not a group of the table", name, i);
		if (g == ref.n)
			continue;
		CHECK(rec[nk + 1] == ref.g[g].rows, "T12 %s: candidate %u has %lld rows, the group %lld", name, i, (long long) rec[nk + 1], (long long) ref.g[g].rows);
		found_before += order_cmp(&ref.g[g], &ref.g[(k < ref.n ? k : ref.n) - 1], &o) < 0;
	}
	if (st == SIFT_VK_OK)
	{
		const uint32_t want = cover ? (k < ref.n ? k : ref.n) : before + ties;

		CHECK(found_before == before, "T12 %s: %u of the %u groups before the K-th are candidates", name, found_before, before);
		CHECK(res.ngroups == want, "T12 %s: %u candidates, expected %u (%u before the K-th, %u tie with it)", name, res.ngroups, want, before, ties);
	}
	printf("   %-40s K %u of %u groups: %u candidates (%u before the K-th, %u tie with it)\n", name, k, ref.n, st == SIFT_VK_OK ? res.ngroups : 0, before, ties);
	ref_free(&ref);
}

#define AGG_DESC(a)		(1u | (a) << 1 | 1u << 9)
#define KEY_ASC(nf)		(0u | (nf) << 10)

void
test_finish(SiftVk *dev, Rng *rng)
{
	TData		d;
	const uint32_t keys1[] = {C_K0},
				keys2[] = {C_K0, C_K1};
	const int64_t kmin1[] = {0},
				kmin2[] = {0, 100};
	const uint32_t span1[] = {11},
				span2[] = {11, 41};

	vk = dev;
	build_base(&d, 7, rng, 5000);
	printf("T12 hash finishes\n");
	distinct_ungrouped(&d, "COUNT(DISTINCT v32)", C_V32);
	distinct_ungrouped(&d, "COUNT(DISTINCT big)", C_BIG);
	distinct_grouped(&d, "COUNT(DISTINCT v32) by k0", 1, keys1, kmin1, span1);
	distinct_grouped(&d, "COUNT(DISTINCT v32) by k0, k1", 2, keys2, kmin2, span2);
	topk_case(&d, "top 5 by count", C_KL, 5, 1, AGG_DESC(0), 0, false);
	topk_case(&d, "top 5 by count, ties capped", C_KL, 5, 1, AGG_DESC(0), 0, true);
	topk_case(&d, "top 1 by key, NULLs first", C_KL, 1, 1, KEY_ASC(1), 0, false);
	topk_case(&d, "top 7 by count then key", C_KL, 7, 2, AGG_DESC(0), KEY_ASC(0), true);
	topk_case(&d, "top 300 of 301 groups", C_KL, 300, 1, AGG_DESC(0), 0, false);
	topk_case(&d, "top 1000 of 301 groups", C_KL, 1000, 1, AGG_DESC(0), 0, true);
	topk_case(&d, "top 5 by count of ties of 1", C_NUM, 5, 1, AGG_DESC(0), 0, false);
	topk_case(&d, "top 5 by count of ties of 1, capped", C_NUM, 5, 1, AGG_DESC(0), 0, true);
	topk_case(&d, "top 5 by count then key of ties", C_NUM, 5, 2, AGG_DESC(0), KEY_ASC(0), false);
	td_free(&d);
}
