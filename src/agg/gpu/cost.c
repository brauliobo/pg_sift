/*
 * agg/gpu/cost.c - the cost model of sift.gpu = auto. Every constant is fitted to tests/vk_v6_bench on two GeForce RTX 3060 12 GiB (PCIe 3.0 x16 for
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
 *
 * Calibrated again after the aggregates of Spec 63E made the CPU faster and the device got COUNT(DISTINCT) and the top K of the groups (1 million rows of the synthetic ClickBench `hits`, the same RTX 3060 with
 * its clocks up, the best of eight runs, tools/vk-v6 for the kernels): the kernel of a hash job costs by the bytes its table touches, not by the groups (1.0 ns a row for the 3 MB of 100 thousand pairs, 2.4 ns
 * for the 56 MB of a million groups), and its table costs 0.3 to 0.5 ns a word clearing and reading it (Q32 224 MiB 9.5 ms, Q35 117 MiB 6.8 ms, Q4 64 MiB 2.2 ms); the CPU takes 25 ns a row for the
 * 5.6 MB of a table, 107 ns for 56 MB, and 91 ns for the pairs of a COUNT(DISTINCT) of a million of them.
 */
#include <math.h>

#include "agg/gpu/cost.h"

/* the kernels that run any program: the base, a decoded column, an operation beyond the loads (measured: expression 0.85 ns for 10 operations) */
#define GPU_VM_NS_ROW_BASE			0.03
#define GPU_VM_NS_ROW_COL			0.146
#define GPU_VM_NS_ROW_OP			0.054
#define GPU_VM_NS_ROW_HASH_1M		4.8		/* hashed keys, a million groups; linear in the groups up to GPU_HASH_REF_GROUPS */
/* the fast kernels */
#define GPU_FAST_NS_ROW_BASE		0.003
#define GPU_FAST_NS_ROW_COL			0.014
#define GPU_FAST_NS_ROW_DENSE		0.065
#define GPU_FAST_NS_ROW_HASH_100	0.92	/* a table that the 3 MB cache of the device holds, and the extra of one of 64 MB and more: log10 of the bytes touched in between */
#define GPU_FAST_NS_ROW_HASH_1M		1.4
#define GPU_CACHE_BYTES				4e6
#define GPU_MISS_BYTES				64e6
#define GPU_HASH_REF_GROUPS			1e6
#define GPU_NS_ROW_TOPN				0.45	/* the whole warm scan: the sample pass, the candidates (1 column, 10 and 50 million rows) */
/* the host and the link, for every kernel */
#define GPU_FIXED_MS				0.45		/* a submission: the part of the warm wall time that does not grow with the rows */
#define GPU_VM_GRANULE_MS			5.0		/* a job of the VM kernel that does not fill the device is as long as the granule of one workgroup: 5.4 and 9.2 ms of wall for 34 and 11 granules */
#define GPU_NS_ROW_HOST			0.002	/* the host of a warm scan whose descriptor tables are on the device: the granule table of each batch */
#define GPU_NS_TABLE_WORD			0.4		/* a word of the table of a hash job: cleared, then read by its finish */
#define GPU_TOPK_GROUPS				4096	/* the groups the host reads back of a top-K finish (agg/vk/run.c reads back what is not after the K-th) */
#define GPU_NS_GROUP				12		/* the host reading and folding a group of a hash result, beyond its transfer */
#define GPU_STAGE_GBPS				2.0		/* the copy of the stored chunks into the staging buffer (1.5 to 2.6 per thread on the loaded host) */
#define GPU_DECODE_GBPS				5.5		/* LZ4 and the node trees on the device: 12 ms for 67 MB */
/* the CPU scan */
#define CPU_NS_ROW_BASE				1.0
#define CPU_NS_ROW_COL				2.2
#define CPU_NS_ROW_OP				0.11
#define CPU_PAR_MAX					8		/* processes after which more do not help: 4 workers 1.9 ns a row, 32 workers 0.9 */
#define CPU_CACHE_BYTES				1e6		/* a hash table up to this size stays in the cache of the CPU */
#define CPU_MISS_BYTES				64e6	/* ... and one of this size misses it on every row */
#define CPU_NS_ROW_MISS_WORD		11		/* a word of the slot of a table that misses the cache: 75 to 100 ns a row for the 7 words of a count by one key, 150 to 250 ns for the 14 of two keys and three aggregates */

static double
min_d(double a, double b)
{
	return a < b ? a : b;
}

/* how much of the way from a table that the cache holds to one that misses it on every row a table of bytes is: log10 in between */
static double
miss_share(double bytes, double cache, double miss)
{
	return bytes <= cache ? 0 : min_d(1.0, log10(bytes / cache) / log10(miss / cache));
}

/* the words of a slot of a hash table: its state, its keys, their NULL mask, its rows and three words of each aggregate */
static double
slot_words(const SiftGpuCostIn *in)
{
	return in->nkeys + 3 + 3 * in->naggs;
}

/* the words of a slot of the pairs of COUNT(DISTINCT): the state, the keys and the value, the NULL mask and the rows */
static double
pair_words(const SiftGpuCostIn *in)
{
	return in->nkeys + 4;
}

double
sift_gpu_cost_cpu_ms(const SiftGpuCostIn *in)
{
	double		ns = CPU_NS_ROW_BASE + CPU_NS_ROW_COL * in->ncols + CPU_NS_ROW_OP * (in->nops > in->ncols ? in->nops - in->ncols : 0);
	double		par = min_d(CPU_PAR_MAX, in->parallel_workers + 1.0);

	ns += CPU_NS_ROW_MISS_WORD * (slot_words(in) * miss_share(in->groups * slot_words(in) * 8, CPU_CACHE_BYTES, CPU_MISS_BYTES) +
								  pair_words(in) * miss_share(in->dist_pairs * pair_words(in) * 8, CPU_CACHE_BYTES, CPU_MISS_BYTES));
	return in->scan_rows * ns / par / 1e6;
}

/* the warm kernel: nanoseconds a row */
static double
kernel_ns(const SiftGpuCostIn *in)
{
	const double share = min_d(1.0, in->groups / GPU_HASH_REF_GROUPS);
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
		ns += in->fast ? GPU_FAST_NS_ROW_HASH_100 + GPU_FAST_NS_ROW_HASH_1M * miss_share(in->groups * slot_words(in) * 8, GPU_CACHE_BYTES, GPU_MISS_BYTES) : GPU_VM_NS_ROW_HASH_1M * share;
	return ns;
}

double
sift_gpu_cost_gpu_ms(const SiftGpuCostIn *in)
{
	double		cold = in->enc_bytes * in->cold_share,
				ns = kernel_ns(in) + (in->mode == SIFT_VK_TOPN ? 0 : GPU_NS_ROW_HOST),
				ms = GPU_FIXED_MS * (in->njobs > 1 ? in->njobs : 1) + in->rows * ns / 1e6,
				readback = 0;

	if (!in->fast && in->mode != SIFT_VK_TOPN && ms < GPU_FIXED_MS + GPU_VM_GRANULE_MS)
		ms = GPU_FIXED_MS + GPU_VM_GRANULE_MS;

	if (in->mode == SIFT_VK_HASH)
	{
		const double read = in->finish == SIFT_VK_FIN_GROUPS ? in->groups : in->finish == SIFT_VK_FIN_TOPK ? GPU_TOPK_GROUPS : 0;

		readback = read * (in->nkeys + 2 + 3 * in->naggs) * 8;
		ms += read * GPU_NS_GROUP / 1e6 + in->table_words * GPU_NS_TABLE_WORD / 1e6;
	}
	return ms + (readback + cold) / (in->pcie_gbps * 1e6) + cold / (GPU_STAGE_GBPS * 1e6) + cold / (GPU_DECODE_GBPS * 1e6);
}
