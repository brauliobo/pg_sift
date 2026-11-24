/*
 * col/v1/v1_store_ops.h - Format 1 behind the storage seam of the unified engine (core/store_ops.h): a sift_col index or a sift_table of Format 1 is read, never written, through it.
 */
#ifndef SIFT_COL_V1_STORE_OPS_H
#define SIFT_COL_V1_STORE_OPS_H

#include "postgres.h"

#include "utils/snapshot.h"

#include "agg/batch_source.h"
#include "core/filter.h"
#include "core/store_ops.h"

/* Registers Format 1 with the storage seam (from the load of the library) */
extern void sift_v1_store_ops_init(void);

/* The zone-pruned aggregate scan, the dictionary answer and the parallel scan of Format 1 (v1_store_ops_zone.c, v1_store_ops_par.c) */
extern const SiftZoneOps sift_v1_zone_ops;
extern const SiftDictOps sift_v1_dict_ops;
extern const SiftParOps sift_v1_par_ops;

/* The superblocks [sb_lo, sb_hi) of the directory view the leader of a parallel scan exported (the view_export of the reader); NULL: the whole relation as it is now */
typedef struct SiftV1Range
{
	const void *view;
	uint32		sb_lo;
	uint32		sb_hi;
} SiftV1Range;

/* The batches of the columns of in over the snapshot, only the rows that pass quals (NULL: all) and, with keep, only the units that hold a row of it; the source and what it holds open live in parent until its close */
extern SiftV6Source *sift_v1_source_open(MemoryContext parent, Snapshot snap, const SiftV6Input *in, const SiftV6Vexpr *quals, const SiftV1Range *range,
										 const SiftFilter *keep);

#endif							/* SIFT_COL_V1_STORE_OPS_H */
