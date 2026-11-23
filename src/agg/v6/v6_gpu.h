/*
 * agg/v6/v6_gpu.h - SiftAggV6 asks the device for the parts it can read (agg/gpu_v6.h) before the CPU scans the rest.
 */
#ifndef SIFT_AGG_V6_GPU_H
#define SIFT_AGG_V6_GPU_H

#include "commands/explain.h"

#include "agg/v6/v6.h"

struct SiftV6Input;
struct SiftV6TopNDevice;
struct SiftV6TopNSpec;

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
extern SiftV6Group *sift_v6_gpu_agg(const SiftV6Spec *spec, Relation heap, Relation index, Snapshot snap, double est_groups, bool having, MemoryContext gcx, TupleDesc desc, SiftV6Gpu *gpu);

/*
 * The best spec->rows rows of the parts the device read, for the one key of spec ordered over the rows that pass quals (the clauses of the scan of in), or false when the CPU runs the whole
 * tournament; with true the generations of the parts the device read are in dev->skip. prunable: the order of the parts lets the tournament stop early, and it wins. heap is the table of the
 * sift_col index.
 */
extern bool sift_v6_gpu_topn(const struct SiftV6TopNSpec *spec, struct SiftV6Input *in, const List *quals, Relation heap, Relation index, Snapshot snap, bool prunable,
							 MemoryContext cx, SiftV6Gpu *gpu, struct SiftV6TopNDevice *dev);

/* sift.gpu = on and a top-N the device ranks: the planner prices the scan as the device runs it */
extern bool sift_v6_gpu_topn_forced(const struct SiftV6TopNSpec *spec);

/* The lines of EXPLAIN; nothing when the device was not asked */
extern void sift_v6_gpu_explain(const SiftV6Gpu *gpu, ExplainState *es);
/* Lets go of the parts pinned for the scan */
extern void sift_v6_gpu_end(SiftV6Gpu *gpu);

#endif							/* SIFT_AGG_V6_GPU_H */
