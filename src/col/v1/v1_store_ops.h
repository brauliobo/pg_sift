/*
 * col/v1/v1_store_ops.h - Format 1 behind the storage seam of the unified engine (core/store_ops.h): a sift_col index or a sift_table of Format 1 is read, never written, through it.
 */
#ifndef SIFT_COL_V1_STORE_OPS_H
#define SIFT_COL_V1_STORE_OPS_H

/* Registers Format 1 with the storage seam (from the load of the library) */
extern void sift_v1_store_ops_init(void);

#endif							/* SIFT_COL_V1_STORE_OPS_H */
