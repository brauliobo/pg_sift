/*
 * agg/gpu_cost.c - the cost model of sift.gpu = auto. Every constant is fitted to tests/vk_v6_bench on two GeForce RTX 3060 12 GiB (PCIe 3.0 x16 for
 * the device the model is fitted on), a 16 core / 32 thread CPU, 10 and 50 million rows of six columns stored by the real codec (results/ and
 * BENCHMARK-RESULTS.md hold the runs; the host was shared, load average 35, which is why the host copy is slow and the CPU does not scale past 8).
 *
 *   warm: the decoded columns are in VRAM, only the aggregation runs. A fast kernel (agg/vk/fast.c) is bound by the memory of the device:
 *         ungrouped 13 bytes a row in 0.044 ns (295 GB/s of 304 GB/s measured), dense keys adds the atomics of the groups, hashed keys the
 *         random access to the table, more of it with more groups (0.98 ns at 100 groups, 1.36 at 10 thousand, 2.38 at a million).
 *   cold: the stored chunks are copied by the host into the staging buffer, cross PCIe and are decoded on the device; the three add up
 *         (67 MB of 10 million rows take 41 to 53 ms).
 *   CPU:  the scan of the stored chunks by worker processes: 7.6 ns a row and process for three columns, and 130 ns more for a hash table of
 *         a million groups, which does not scale past eight processes on this host. The scan of the executor is not faster than this.
 */
#include <math.h>

#include "agg/gpu_cost.h"

/* the kernels that run any program: the base, a decoded column, an operation beyond the loads (measured: expression 0.85 ns for 10 operations) */
#define GPU_VM_NS_ROW_BASE			0.03
#define GPU_VM_NS_ROW_COL			0.146
#define GPU_VM_NS_ROW_OP			0.054
#define GPU_VM_NS_ROW_HASH_1M		4.8		/* hashed keys, a million groups; linear in the groups up to GPU_HASH_REF_GROUPS */
/* the fast kernels */
#define GPU_FAST_NS_ROW_BASE		0.003
#define GPU_FAST_NS_ROW_COL			0.014
#define GPU_FAST_NS_ROW_DENSE		0.065
#define GPU_FAST_NS_ROW_HASH_100	0.92	/* the table at 100 groups, and the extra of a million: log10 of the groups in between */
#define GPU_FAST_NS_ROW_HASH_1M		1.4
#define GPU_HASH_MIN_GROUPS			100.0
#define GPU_HASH_REF_GROUPS			1e6
#define GPU_NS_ROW_TOPN				0.45	/* the whole warm scan: the sample pass, the candidates (1 column, 10 and 50 million rows) */
/* the host and the link, for every kernel */
#define GPU_FIXED_MS				0.45		/* a submission: the part of the warm wall time that does not grow with the rows */
#define GPU_NS_ROW_HOST				0.002	/* the host of a warm scan whose descriptor tables are on the device: the granule table of each batch */
#define GPU_NS_GROUP				12		/* the host reading and folding a group of a hash result, beyond its transfer */
#define GPU_STAGE_GBPS				2.0		/* the copy of the stored chunks into the staging buffer (1.5 to 2.6 per thread on the loaded host) */
#define GPU_DECODE_GBPS				5.5		/* LZ4 and the node trees on the device: 12 ms for 67 MB */
/* the CPU scan */
#define CPU_NS_ROW_BASE				1.0
#define CPU_NS_ROW_COL				2.2
#define CPU_NS_ROW_OP				0.11
#define CPU_PAR_MAX					8		/* processes after which more do not help: 4 workers 1.9 ns a row, 32 workers 0.9 */
#define CPU_MANY_GROUPS				65536	/* more groups than this miss the cache of the CPU on every row */
#define CPU_NS_ROW_MANY_GROUPS		120

static double
min_d(double a, double b)
{
	return a < b ? a : b;
}

double
sift_gpu_cost_cpu_ms(const SiftGpuCostIn *in)
{
	double		ns = CPU_NS_ROW_BASE + CPU_NS_ROW_COL * in->ncols + CPU_NS_ROW_OP * (in->nops > in->ncols ? in->nops - in->ncols : 0);
	double		par = min_d(CPU_PAR_MAX, in->parallel_workers + 1.0);

	if (in->groups > CPU_MANY_GROUPS)
		ns += CPU_NS_ROW_MANY_GROUPS;
	return in->scan_rows * ns / par / 1e6;
}

/* the warm kernel: nanoseconds a row */
static double
kernel_ns(const SiftGpuCostIn *in)
{
	double		share = min_d(1.0, in->groups / GPU_HASH_REF_GROUPS),
				decades = in->groups > GPU_HASH_MIN_GROUPS ? log10(in->groups / GPU_HASH_MIN_GROUPS) / log10(GPU_HASH_REF_GROUPS / GPU_HASH_MIN_GROUPS) : 0;
	double		ns;

	if (in->mode == SIFT_VK_TOPN)
		return GPU_NS_ROW_TOPN;
	if (!in->fast)
		ns = GPU_VM_NS_ROW_BASE + GPU_VM_NS_ROW_COL * in->ncols + GPU_VM_NS_ROW_OP * (in->nops > in->ncols ? in->nops - in->ncols : 0);
	else
		ns = GPU_FAST_NS_ROW_BASE + GPU_FAST_NS_ROW_COL * in->ncols;
	if (in->mode == SIFT_VK_DENSE && in->fast)
		ns += GPU_FAST_NS_ROW_DENSE;
	else if (in->mode == SIFT_VK_HASH)
		ns += in->fast ? GPU_FAST_NS_ROW_HASH_100 + GPU_FAST_NS_ROW_HASH_1M * min_d(1.0, decades) : GPU_VM_NS_ROW_HASH_1M * share;
	return ns;
}

double
sift_gpu_cost_gpu_ms(const SiftGpuCostIn *in)
{
	double		cold = in->enc_bytes * in->cold_share,
				ns = kernel_ns(in) + (in->mode == SIFT_VK_TOPN ? 0 : GPU_NS_ROW_HOST),
				ms = GPU_FIXED_MS + in->rows * ns / 1e6,
				readback = 0;

	if (in->mode == SIFT_VK_HASH)
	{
		readback = in->groups * (in->nkeys + 2 + 3 * in->naggs) * 8;
		ms += in->groups * GPU_NS_GROUP / 1e6;
	}
	return ms + (readback + cold) / (in->pcie_gbps * 1e6) + cold / (GPU_STAGE_GBPS * 1e6) + cold / (GPU_DECODE_GBPS * 1e6);
}
