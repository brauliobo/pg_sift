/*
 * col/v1_engine/store_ops_ext.c - SiftPreaggOps of Format 1 (core/store/ops.h): the first dimension of the stored cube (col/v1/preagg/preagg.h) that keeps a measure, as the leaves of the Format 1
 * layout. The files of col/v1_engine/ are the part of the Format 1 store that reads the engine's headers (agg/v6/), which a file of col/v1/ may not include
 * (scripts/check-format-isolation.sh); they move into col/v1/ once those headers are format-neutral. The order ops are in v1_store_ops_order.c.
 */
#include "postgres.h"

#include "col/v1/reader.h"
#include "col/v1/store_ops.h"
#include "core/store/ops.h"
#include "table/v1/tbl_ops.h"

static bool
preagg_keeps_cubes(Oid relid, bool is_table)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	SiftSrcPreagg pa;

	memset(&pa, 0, sizeof(pa));
	sift_v1_reader_of(is_table)->preagg(r.rel, &pa);
	sift_v1_rel_close(&r);
	return pa.aggs && pa.ndims > 0;
}

const SiftPreaggOps v1_preagg_ops = {
	.keeps_cubes = preagg_keeps_cubes
};
