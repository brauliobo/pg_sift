/*
 * agg/v6/v6_gpu.h - SiftAggV6 asks the device for the parts it can read (agg/gpu_v6.h) before the CPU scans the rest.
 */
#ifndef SIFT_AGG_V6_GPU_H
#define SIFT_AGG_V6_GPU_H

#include "commands/explain.h"

#include "agg/v6/v6.h"

/* What the device took: the run it made (NULL when the query has no shape for the device) and the generations of the parts the CPU scan leaves out, ascending */
typedef struct SiftV6Gpu
{
	struct SiftGpuRun *run;
	uint64	   *skip;
	int			nskip;
} SiftV6Gpu;

/*
 * The groups of the parts the device read, or NULL when the CPU runs everything. index is the sift_col index of heap; NULL for a sift_table, which heap is and whose attributes are
 * the columns. The quals of spec are compiled for the device here: those it cannot run leave the scan to the CPU.
 * est_groups is the planner's count of groups.
 */
extern SiftV6Group *sift_v6_gpu_agg(const SiftV6Spec *spec, Relation heap, Relation index, Snapshot snap, double est_groups, MemoryContext gcx, TupleDesc desc, SiftV6Gpu *gpu);

/* The lines of EXPLAIN; nothing when the device was not asked */
extern void sift_v6_gpu_explain(const SiftV6Gpu *gpu, ExplainState *es);
/* Lets go of the parts pinned for the scan */
extern void sift_v6_gpu_end(SiftV6Gpu *gpu);

#endif							/* SIFT_AGG_V6_GPU_H */
