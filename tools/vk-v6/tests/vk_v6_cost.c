/*
 * tests/vk_v6_cost.c - the cost model of sift.gpu = auto (agg/gpu_cost.c) against the measurements it was fitted to (BENCHMARK-RESULTS.md, GPU 1 on
 * PCIe 3.0 x16, 4 CPU workers; the mean of two runs of each point): every prediction within a factor of two, and the choice between the CPU and the
 * device the one the measurements make wherever they are not within a third of the margin. No device is used.
 */
#include <stdio.h>
#include <string.h>

#include "agg/gpu_cost.h"

typedef struct Point
{
	const char *name;
	SiftGpuCostIn in;
	double		cpu_ms,
				warm_ms,
				cold_ms;
} Point;

#define MB (1e6)

static const Point points[] = {
	{"ungrouped 10M", {SIFT_VK_UNGROUPED, true, 10002432, 67 * MB, 0, 12.29, 1, 3, 4, 0, 5, 3}, 20.2, 0.9, 47.7},
	{"dense 10M", {SIFT_VK_DENSE, true, 10002432, 68 * MB, 0, 12.29, 101, 3, 3, 1, 3, 3}, 21.0, 1.8, 43.2},
	{"hash 10M", {SIFT_VK_HASH, true, 10002432, 63 * MB, 0, 12.29, 1e6, 4, 4, 2, 3, 3}, 444, 44.1, 85.7},
	{"expression 10M", {SIFT_VK_UNGROUPED, false, 10002432, 67 * MB, 0, 12.29, 1, 3, 10, 0, 4, 3}, 22.0, 9.3, 52.7},
	{"top-100 10M", {SIFT_VK_TOPN, false, 10002432, 29 * MB, 0, 12.29, 1, 1, 1, 0, 0, 3}, 8.0, 4.0, 32.2},
	{"ungrouped 50M", {SIFT_VK_UNGROUPED, true, 50003968, 332 * MB, 0, 12.29, 1, 3, 4, 0, 5, 3}, 91.4, 2.8, 261},
	{"dense 50M", {SIFT_VK_DENSE, true, 50003968, 339 * MB, 0, 12.29, 101, 3, 3, 1, 3, 3}, 85.8, 5.9, 192},
	{"hash 50M", {SIFT_VK_HASH, true, 50003968, 314 * MB, 0, 12.29, 1e6, 4, 4, 2, 3, 3}, 1253, 131, 277},
	{"expression 50M", {SIFT_VK_UNGROUPED, false, 50003968, 332 * MB, 0, 12.29, 1, 3, 10, 0, 4, 3}, 100.3, 40.2, 212},
	{"top-100 50M", {SIFT_VK_TOPN, false, 50003968, 144 * MB, 0, 12.29, 1, 1, 1, 0, 0, 3}, 40.9, 16.1, 111},
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
		SiftGpuCostIn warm = p->in,
					cold = p->in;
		double		cpu = sift_gpu_cost_cpu_ms(&p->in);

		cold.cold_share = 1;
		printf("%s\n", p->name);
		within("cpu", p->name, cpu, p->cpu_ms);
		within("warm", p->name, sift_gpu_cost_gpu_ms(&warm), p->warm_ms);
		within("cold", p->name, sift_gpu_cost_gpu_ms(&cold), p->cold_ms);
		choice("warm", p->name, cpu, sift_gpu_cost_gpu_ms(&warm), p->cpu_ms, p->warm_ms);
		choice("cold", p->name, cpu, sift_gpu_cost_gpu_ms(&cold), p->cpu_ms, p->cold_ms);
	}
	printf(bad ? "FAILED: %d\n" : "PASSED: 0 failures\n", bad);
	return bad != 0;
}
