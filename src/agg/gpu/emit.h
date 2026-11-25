/*
 * agg/gpu/emit.h - the programs of a query as the device runs them (agg/gpu/emit.c)
 */
#ifndef SIFT_AGG_GPU_EMIT_H
#define SIFT_AGG_GPU_EMIT_H

#include "agg/gpu/v6.h"
#include "agg/vk/layout.h"

#define SIFT_GPU_EMIT_OPS		64
#define SIFT_GPU_EMIT_POOL		16384

/* The operations and sets the job points at, and what the translation has used up of the device's registers and mask slots */
typedef struct SiftGpuEmit
{
	uint32		ops[SIFT_GPU_EMIT_OPS * SIFT_VK_OP_WORDS];
	uint32		nops;
	uint32		pool[SIFT_GPU_EMIT_POOL];
	uint32		npool;
	uint32		nregs;
	uint32		nmasks;
	int			col_reg[SIFT_VK_MAXCOLS];
	int			key_slot[SIFT_VK_MAXKEYS];	/* the column slot of a key that is a plain column, else -1 */
	int			amap[SIFT_VK_MAXAGGS];	/* the aggregate of the job that is the aggregate of the spec, -1 for a COUNT(DISTINCT), which the job does not hold */
	int			dist_slot;		/* the column slot of the value the COUNT(DISTINCT) aggregates count, -1 for none */
	int			dist_reg;		/* ... and its register */
	const char *why;			/* the first reason the device cannot run the query, else NULL */
} SiftGpuEmit;

extern void sift_gpu_emit_init(SiftGpuEmit *e);
/* Fills the program of job (ncols, ops, pool, where, keys, aggregates) for q's aggregate; NULL, or why the device cannot run it */
extern const char *sift_gpu_emit_agg(SiftGpuEmit *e, const SiftGpuQuery *q, SiftVkJob *job);
extern const char *sift_gpu_emit_topn(SiftGpuEmit *e, const SiftGpuQuery *q, SiftVkJob *job, uint32 k, bool desc, bool nulls_first);

#endif
