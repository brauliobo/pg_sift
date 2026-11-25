/*
 * tests/bench_probe.c - the kernel probes of vk_v6_bench, run when asked for: VKB_PROBE (the ungrouped kernel, one more column each),
 * VKB_SWEEP (the hash kernel at 100, 10 thousand and a million groups) and VKB_DPROBE (the dense kernel, one aggregate set at a time).
 */
#include "postgres.h"

#include "bench.h"

/* the least kernel time of four runs, the first (which makes the pipeline) left out */
static double
best_kernel(SiftVk *vk, const SiftVkJob *j, SiftVkResult *res)
{
	double		best = 1e9;

	for (int k = 0; k < 4 && sift_vk_run(vk, j, NULL, res) == SIFT_VK_OK; k++)
		best = k > 0 && res->times.kernel_ms < best ? res->times.kernel_ms : best;
	return best;
}

/* the ungrouped kernel with no WHERE and one more column each: what a column costs */
static void
ungrouped_probe(SiftVk *vk, const Bench *b)
{
	static const char *const names[] = {"count(*) only, no columns", "qty: sum", "qty, amount: sum", "qty, amount, price: sums"};
	static const uint8_t cols[] = {B_QTY, B_AMOUNT, B_PRICE};

	for (int ncol = 0; ncol < 4; ncol++)
	{
		Job		   *j = malloc(sizeof(Job));
		SiftVkResult res;

		job_init(j, b, SIFT_VK_UNGROUPED);
		j->vg = calloc(b->ngran, sizeof(SiftVkGranule));
		for (uint32_t g = 0; g < b->ngran; g++)
		{
			j->vg[g] = b->vg[g];
			memset(j->vg[g].col, 0, sizeof(j->vg[g].col));
			for (int c = 0; c < ncol; c++)
				j->vg[g].col[cols[c]] = b->vg[g].col[cols[c]];
		}
		j->j.gran = j->vg;
		jagg(j, SIFT_VK_AK_COUNT_STAR, 255);
		for (int c = 0; c < ncol; c++)
		{
			P_COL(&j->p, c, cols[c], c == 2 ? SIFT_VK_T_F64 : SIFT_VK_T_I64);
			jagg(j, c == 2 ? SIFT_VK_AK_SUM_F : SIFT_VK_AK_SUM_I, c);
		}
		job_seal(j);
		printf("probe %d (%s): kernel %.2f ms\n", ncol, names[ncol], best_kernel(vk, &j->j, &res));
		free(j->vg);
		free(j);
	}
}

/* the grouped kernels at other group counts and with one aggregate at a time: the router's curves */
static void
grouped_probe(SiftVk *vk, const Bench *b, bool hash)
{
	static const struct
	{
		const char *name;
		uint32_t	nkeys;
		uint8_t		k0,
					k1,
					kinds[3],
					vregs[3];
		uint64_t	groups;
	}			hcfg[] = {{"ks", 1, 4, 0, {0}, {0}, 100}, {"km", 1, 0, 0, {0}, {0}, 10000}, {"km, kl", 2, 0, 1, {0}, {0}, 1000000}},
				dcfg[] = {{"count(*)", 1, 0, 0, {SIFT_VK_AK_COUNT_STAR}, {255}, 100},
	{"count(*), sum(int)", 1, 0, 0, {SIFT_VK_AK_COUNT_STAR, SIFT_VK_AK_SUM_I}, {255, 1}, 100},
	{"count(*), sum(double)", 1, 0, 0, {SIFT_VK_AK_COUNT_STAR, SIFT_VK_AK_SUM_F}, {255, 2}, 100},
	{"count(*), sum(int), sum(double)", 1, 0, 0, {SIFT_VK_AK_COUNT_STAR, SIFT_VK_AK_SUM_I, SIFT_VK_AK_SUM_F}, {255, 1, 2}, 100}};
	size_t		n = hash ? sizeof(hcfg) / sizeof(hcfg[0]) : sizeof(dcfg) / sizeof(dcfg[0]);

	for (size_t i = 0; i < n; i++)
	{
		const typeof(hcfg[0]) *c = hash ? &hcfg[i] : &dcfg[i];
		Job		   *j = malloc(sizeof(Job));
		SiftVkResult res;
		double		best;

		make_job(j, b, hash ? 2 : 1);
		if (hash)
		{
			P_COL(&j->p, 4, B_KS, SIFT_VK_T_I64);
			for (uint32_t g = 0; g < b->ngran; g++)
				j->vg[g].col[B_KS] = b->vg[g].col[B_KS];
			j->j.nkeys = c->nkeys;
			j->j.key_reg[0] = c->k0;
			j->j.key_reg[1] = c->k1;
			j->j.est_groups = c->groups;
		}
		else
		{
			j->j.naggs = 0;
			for (int a = 0; a < 3 && c->vregs[a]; a++)
				jagg(j, c->kinds[a], c->vregs[a]);
		}
		job_seal(j);
		best = best_kernel(vk, &j->j, &res);
		printf("%s probe (%s), %llu groups: kernel %.2f ms, %.2f ns per row\n", hash ? "hash" : "dense", c->name,
			   (unsigned long long) res.ngroups, best, best * 1e6 / (double) b->rows);
		free(j->vg);
		free(j);
	}
}

void
bench_probes(SiftVk *vk, const Bench *b)
{
	if (getenv("VKB_SWEEP"))
		grouped_probe(vk, b, true);
	if (getenv("VKB_DPROBE"))
		grouped_probe(vk, b, false);
	if (getenv("VKB_PROBE"))
		ungrouped_probe(vk, b);
}
