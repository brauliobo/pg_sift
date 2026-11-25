/*
 * agg/gpu/cost.h - what the choice of sift.gpu = auto weighs (agg/gpu/v6.c): the time a scan takes on the CPU and on the Vulkan device, from the
 * shape of the query and the data. Plain C, no PostgreSQL headers; tests/cost.c holds the model to the measurements it was fitted to.
 */
#ifndef SIFT_AGG_GPU_COST_H
#define SIFT_AGG_GPU_COST_H

#include <stdbool.h>

#include "agg/vk/vk.h"

#define SIFT_GPU_PCIE_DEFAULT_GBPS	12.3	/* PCIe 3.0 x16, when the device cannot say */
#define SIFT_GPU_MARGIN				1.25	/* the device must be this much faster to be chosen */

typedef struct SiftGpuCostIn
{
	SiftVkMode	mode;
	SiftVkFinish finish;		/* HASH: what is read back of the table */
	bool		fast;			/* sift_vk_job_is_fast: a program of columns and comparisons with constants */
	double		rows;
	double		scan_rows;		/* the rows the scan of the CPU reads: those of the granules the zones do not rule out */
	double		enc_bytes;		/* the stored bytes of the columns the query reads */
	double		cold_share;		/* the share of them that is not on the device, 0..1 */
	double		pcie_gbps;
	double		groups;			/* the groups of the result (dense: the slots, hash: the estimate) */
	double		table_words;	/* HASH: the words of the table of the device, which the job clears and the finish reads */
	double		dist_pairs;		/* COUNT(DISTINCT): the pairs of a group and a value the CPU keeps in a hash table (an upper bound: 0 for none) */
	int			ncols;			/* the columns the query reads */
	int			nops;			/* the operations of the program, loads included */
	int			nkeys;
	int			naggs;
	int			njobs;			/* the jobs the scan runs as, each with its own submission (the pieces the dictionaries of the parts cut it in); 0: one */
	int			parallel_workers;	/* the planner's workers; the leader is one more */
} SiftGpuCostIn;

extern double sift_gpu_cost_cpu_ms(const SiftGpuCostIn *in);
extern double sift_gpu_cost_gpu_ms(const SiftGpuCostIn *in);

#endif
