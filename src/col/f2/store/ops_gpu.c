/*
 * col/f2/store/ops_gpu.c - SiftGpuOps of format major 2: the parts of the relation the device reads (f2_gpu_plan.c), for a sift_col index and a sift_table alike.
 */
#include "postgres.h"

#include "col/f2/store/ops_int.h"

const SiftGpuOps sift_f2_gpu_ops = {
	.plan = sift_gpu_f2_plan,
	.free = sift_gpu_f2_free,
	.load = sift_gpu_f2_load,
	.unload = sift_gpu_f2_unload,
	.keys = sift_gpu_f2_keys,
	.tab_key = sift_gpu_f2_tab_key,
	.part = sift_gpu_f2_part,
	.dict = sift_gpu_f2_dict,
	.locate = sift_gpu_f2_locate,
	.covers = sift_gpu_f2_covers
};
