/*
 * agg/vk/fast.h - the shape of a job the fast kernels take (agg/vk/fast.c, agg/vk/shaders/common_fast.glsl). Plain C, no Vulkan types.
 */
#ifndef SIFT_AGG_VK_FAST_H
#define SIFT_AGG_VK_FAST_H

#include "agg/vk/vk.h"

#define FAST_PRED_MAX	8							/* comparisons with constants of a fast kernel: 8 nibbles of SC_PCOL and SC_POP, 8 words of constants at J_FAST_IMM */
#define FAST_NSC		13							/* specialization constants of a fast kernel (shaders/common_fast.glsl) */

/* the specialization constants of a job and the constants of its comparisons */
typedef struct FastShape
{
	uint32_t	sc[FAST_NSC];
	uint64_t	imm[FAST_PRED_MAX];
	uint32_t	cnt_rows;		/* bit a: the counts of aggregate a are the rows of its group */
} FastShape;

/* False when the job needs the kernels that run the program; job->gran may be NULL (the counts of nullable columns are then not taken from the rows) */
extern bool fast_shape(const SiftVkJob *job, FastShape *fs);

#endif
