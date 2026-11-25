/*
 * agg/gpu/cfg.h - the four settings of the Vulkan path (agg/gpu/scan.h): sift.gpu, sift.gpu_min_rows, sift.gpu_device and sift.gpu_cache_mb
 */
#ifndef SIFT_AGG_GPU_CFG_H
#define SIFT_AGG_GPU_CFG_H

#include "postgres.h"

extern PGDLLIMPORT int sift_gpu_mode;
extern PGDLLIMPORT int sift_gpu_min_rows;
extern PGDLLIMPORT char *sift_gpu_device;
extern PGDLLIMPORT int sift_gpu_cache_mb;

extern void sift_gpu_v6_init_gucs(void);

#endif							/* SIFT_AGG_GPU_CFG_H */
