/*
 * agg/acc_layout.h - the per-group arrays of AggAcc (agg/accum_int.h) an aggregate holds: acc_alloc (agg/group/table.c) allocates them and agg_cost
 * (agg/cost/) prices them, from this one answer.
 */
#ifndef SIFT_AGG_ACC_LAYOUT_H
#define SIFT_AGG_ACC_LAYOUT_H

#include "agg/spec.h"

#define ACCL_I		0x01
#define ACCL_W		0x02
#define ACCL_D		0x04
#define ACCL_DT		0x08
#define ACCL_NF		0x10
#define ACCL_V		0x20
#define ACCL_R		0x40
#define ACCL_LAST	0x80

/* the arrays besides the count (AggAcc.n, always); lane_serial: the float sums of the scan go through the lanes in one table (AggAcc.dt) */
extern unsigned sift_agg_acc_layout(const SiftAggAgg *a, bool lane_serial);
/* the bytes of a group in the arrays of the layout, the count included */
extern double sift_agg_acc_layout_bytes(unsigned layout);

#endif							/* SIFT_AGG_ACC_LAYOUT_H */
