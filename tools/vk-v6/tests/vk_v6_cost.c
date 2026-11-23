/*
 * tests/vk_v6_cost.c - the cost model of sift.gpu = auto (agg/gpu_cost.c) against the measurements it was fitted to: every prediction within a factor of two, and the choice between the CPU and the
 * device the one the measurements make wherever they are not within a third of the margin. No device is used.
 *   the first ten points: BENCHMARK-RESULTS.md, GPU 1 on PCIe 3.0 x16, 4 CPU workers, the mean of two runs of each point (rows of 6 columns of the real codec);
 *   the others: the 1 million rows of the synthetic ClickBench `hits` after the aggregates of Spec 63E, on an RTX 3060 on PCIe 3.0 x16, the CPU scan with sift.gpu = off against the device with its chunks
 *   resident and its clocks up (EXPLAIN ANALYZE, the best of eight runs); what the device runs of them is the job of the pairs of COUNT(DISTINCT) or the hash job that ends with the best K of its groups.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "agg/gpu_cost.h"

typedef struct Point
{
	const char *name;
	SiftGpuCostIn cpu_in;		/* what the CPU scan does */
	SiftGpuCostIn gpu_in;		/* the job of the device */
	double		cpu_ms,
				warm_ms,
				cold_ms;		/* 0: not measured */
	bool		cpu_bound;		/* the CPU time is only bounded from above: the pairs of a COUNT(DISTINCT) are as many as the rows, the distinct values of the column are not known */
} Point;

#define MB (1e6)
#define SCAN(m, f, r, e, g, c, o, k, a) .mode = m, .fast = f, .rows = r, .scan_rows = r, .enc_bytes = e, .pcie_gbps = 12.29, .groups = g, .ncols = c, .nops = o, .nkeys = k, .naggs = a
#define WORKERS3 .parallel_workers = 3

#define TABLE(k, a)	((double) 2097152 * ((k) + 3 + 3 * (a)))

static const Point points[] = {
	{"ungrouped 10M", {SCAN(SIFT_VK_UNGROUPED, true, 10002432, 67 * MB, 1, 3, 4, 0, 5), WORKERS3}, {SCAN(SIFT_VK_UNGROUPED, true, 10002432, 67 * MB, 1, 3, 4, 0, 5), WORKERS3}, 20.2, 0.9, 47.7, false},
	{"dense 10M", {SCAN(SIFT_VK_DENSE, true, 10002432, 68 * MB, 101, 3, 3, 1, 3), WORKERS3}, {SCAN(SIFT_VK_DENSE, true, 10002432, 68 * MB, 101, 3, 3, 1, 3), WORKERS3}, 21.0, 1.8, 43.2, false},
	{"hash 10M", {SCAN(SIFT_VK_HASH, true, 10002432, 63 * MB, 1e6, 4, 4, 2, 3), WORKERS3}, {SCAN(SIFT_VK_HASH, true, 10002432, 63 * MB, 1e6, 4, 4, 2, 3), .table_words = TABLE(2, 3), WORKERS3}, 444, 44.1, 85.7, false},
	{"expression 10M", {SCAN(SIFT_VK_UNGROUPED, false, 10002432, 67 * MB, 1, 3, 10, 0, 4), WORKERS3}, {SCAN(SIFT_VK_UNGROUPED, false, 10002432, 67 * MB, 1, 3, 10, 0, 4), WORKERS3}, 22.0, 9.3, 52.7, false},
	{"top-100 10M", {SCAN(SIFT_VK_TOPN, false, 10002432, 29 * MB, 1, 1, 1, 0, 0), WORKERS3}, {SCAN(SIFT_VK_TOPN, false, 10002432, 29 * MB, 1, 1, 1, 0, 0), WORKERS3}, 8.0, 4.0, 32.2, false},
	{"ungrouped 50M", {SCAN(SIFT_VK_UNGROUPED, true, 50003968, 332 * MB, 1, 3, 4, 0, 5), WORKERS3}, {SCAN(SIFT_VK_UNGROUPED, true, 50003968, 332 * MB, 1, 3, 4, 0, 5), WORKERS3}, 91.4, 2.8, 261, false},
	{"dense 50M", {SCAN(SIFT_VK_DENSE, true, 50003968, 339 * MB, 101, 3, 3, 1, 3), WORKERS3}, {SCAN(SIFT_VK_DENSE, true, 50003968, 339 * MB, 101, 3, 3, 1, 3), WORKERS3}, 85.8, 5.9, 192, false},
	{"hash 50M", {SCAN(SIFT_VK_HASH, true, 50003968, 314 * MB, 1e6, 4, 4, 2, 3), WORKERS3}, {SCAN(SIFT_VK_HASH, true, 50003968, 314 * MB, 1e6, 4, 4, 2, 3), .table_words = TABLE(2, 3), WORKERS3}, 1253, 131, 277, false},
	{"expression 50M", {SCAN(SIFT_VK_UNGROUPED, false, 50003968, 332 * MB, 1, 3, 10, 0, 4), WORKERS3}, {SCAN(SIFT_VK_UNGROUPED, false, 50003968, 332 * MB, 1, 3, 10, 0, 4), WORKERS3}, 100.3, 40.2, 212, false},
	{"top-100 50M", {SCAN(SIFT_VK_TOPN, false, 50003968, 144 * MB, 1, 1, 1, 0, 0), WORKERS3}, {SCAN(SIFT_VK_TOPN, false, 50003968, 144 * MB, 1, 1, 1, 0, 0), WORKERS3}, 40.9, 16.1, 111, false},
	{"count distinct 1M (Q4)", {SCAN(SIFT_VK_UNGROUPED, true, 1000000, 6.7 * MB, 1, 1, 1, 0, 0), .dist_pairs = 1e6},
	 {SCAN(SIFT_VK_HASH, true, 1000000, 6.7 * MB, 1e6, 1, 1, 1, 0), .finish = SIFT_VK_FIN_COUNT, .table_words = TABLE(1, 0)}, 18.7, 3.2, 0, true},
	{"count distinct by group 1M (Q8)", {SCAN(SIFT_VK_DENSE, true, 1000000, 13 * MB, 5000, 2, 2, 1, 0), .dist_pairs = 1e6},
	 {SCAN(SIFT_VK_HASH, true, 1000000, 13 * MB, 1e6, 2, 2, 2, 0), .finish = SIFT_VK_FIN_DENSE_COUNT, .table_words = TABLE(2, 0)}, 91.3, 8.1, 0, false},
	{"top-10 of 100 thousand groups 1M (Q15)", {SCAN(SIFT_VK_HASH, true, 1000000, 6.7 * MB, 1e5, 1, 1, 1, 1)},
	 {SCAN(SIFT_VK_HASH, true, 1000000, 6.7 * MB, 1e5, 1, 1, 1, 1), .finish = SIFT_VK_FIN_TOPK, .table_words = TABLE(1, 1)}, 25.0, 7.0, 0, false},
	{"top-10 of a million groups 1M (Q35)", {SCAN(SIFT_VK_HASH, true, 1000000, 6.7 * MB, 1e6, 1, 1, 1, 1)},
	 {SCAN(SIFT_VK_HASH, true, 1000000, 6.7 * MB, 1e6, 1, 1, 1, 1), .finish = SIFT_VK_FIN_TOPK, .table_words = TABLE(1, 1)}, 106.9, 14.5, 0, false},
	{"top-10 of a million groups of two keys 1M (Q32)", {SCAN(SIFT_VK_HASH, false, 1000000, 22 * MB, 1e6, 4, 4, 2, 3)},
	 {SCAN(SIFT_VK_HASH, false, 1000000, 22 * MB, 1e6, 4, 4, 2, 3), .finish = SIFT_VK_FIN_TOPK, .table_words = TABLE(2, 3)}, 247.5, 22.8, 0, false},
	{"filtered strings of 34 granules (Q40)", {SCAN(SIFT_VK_HASH, false, 278528, 4 * MB, 1e4, 5, 6, 5, 1)},
	 {SCAN(SIFT_VK_HASH, false, 278528, 4 * MB, 1e4, 5, 6, 5, 1), .table_words = TABLE(5, 1) / 128}, 3.1, 5.4, 0, false},
	{"filtered pair of 11 granules (Q42)", {SCAN(SIFT_VK_HASH, false, 90112, 1 * MB, 1e3, 3, 4, 2, 1)},
	 {SCAN(SIFT_VK_HASH, false, 90112, 1 * MB, 1e3, 3, 4, 2, 1), .table_words = TABLE(2, 1) / 128}, 1.2, 5.4, 0, false},
};

static int bad;

static void
within(const char *what, const char *name, double predicted, double measured)
{
	double		r = predicted / measured;

	printf("   %-8s predicted %8.1f ms  measured %8.1f ms  %.2fx\n", what, predicted, measured, r);
	if (r < 0.5 || r > 2.0)
	{
		printf("   FAIL %s %s: the prediction is off by more than a factor of two\n", name, what);
		bad++;
	}
}

static void
choice(const char *what, const char *name, double cpu, double gpu, double cpu_m, double gpu_m)
{
	double		measured = cpu_m / (gpu_m * SIFT_GPU_MARGIN);

	if (measured > 0.75 && measured < 1.33)
		return;
	if ((cpu > gpu * SIFT_GPU_MARGIN) != (measured > 1))
	{
		printf("   FAIL %s %s: the model chooses the %s, the measurements the %s\n", name, what, measured > 1 ? "CPU" : "device", measured > 1 ? "device" : "CPU");
		bad++;
	}
}

int
main(void)
{
	for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++)
	{
		const Point *p = &points[i];
		SiftGpuCostIn warm = p->gpu_in,
					cold = p->gpu_in;
		double		cpu = sift_gpu_cost_cpu_ms(&p->cpu_in);

		cold.cold_share = 1;
		printf("%s\n", p->name);
		if (p->cpu_bound)
			printf("   cpu      predicted %8.1f ms  measured %8.1f ms  %.2fx (an upper bound)\n", cpu, p->cpu_ms, cpu / p->cpu_ms);
		else
			within("cpu", p->name, cpu, p->cpu_ms);
		if (p->cpu_bound && cpu < p->cpu_ms)
		{
			printf("   FAIL %s cpu: the bound is below the measurement\n", p->name);
			bad++;
		}
		within("warm", p->name, sift_gpu_cost_gpu_ms(&warm), p->warm_ms);
		choice("warm", p->name, cpu, sift_gpu_cost_gpu_ms(&warm), p->cpu_ms, p->warm_ms);
		if (p->cold_ms == 0)
			continue;
		within("cold", p->name, sift_gpu_cost_gpu_ms(&cold), p->cold_ms);
		choice("cold", p->name, cpu, sift_gpu_cost_gpu_ms(&cold), p->cpu_ms, p->cold_ms);
	}
	printf(bad ? "FAILED: %d\n" : "PASSED: 0 failures\n", bad);
	return bad != 0;
}

