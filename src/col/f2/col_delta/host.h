/*
 * col/f2/col_delta/host.h - what the sift_col side hands the shared columnar engine (col/f2/store/store.h) as its host: sift_col_host_ops, and the context of its heap visibility filter.
 */
#ifndef SIFT_F2_COL_HOST_H
#define SIFT_F2_COL_HOST_H

#include "core/host.h"

#include "core/f2/vis.h"

/* The vis_ctx of SiftF2HostOps.filter_granule_vis for an index: the visibility the scan began and the guard of the parts it reads */
typedef struct SiftColHostVis
{
	SiftVis    *vis;
	SiftVisF2Guard *guard;
} SiftColHostVis;

#endif							/* SIFT_F2_COL_HOST_H */
