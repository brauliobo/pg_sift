/*
 * agg/live.c - the upper-path hook and the node registration of the operators of SiftAggV6 (v6_live.h).
 */
#include "postgres.h"

#include "nodes/extensible.h"
#include "optimizer/optimizer.h"

#include "agg/agg.h"
#include "agg/engine.h"
#include "agg/cost/card.h"
#include "agg/first/node.h"
#include "agg/live.h"
#include "agg/sip_scan.h"
#include "agg/stream.h"
#include "agg/topn/topn.h"
#include "agg/two/two.h"
#include "agg/win/window.h"
#include "core/catalog.h"

void
sift_agg_v6_live_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	if (!sift_store_ops_any())
		return;
	sift_agg_v6_topn_upper_paths(root, stage, input_rel, output_rel, extra);
	sift_agg_v6_window_upper_paths(root, stage, input_rel, output_rel, extra);
	sift_agg_v6_first_upper_paths(root, stage, input_rel, output_rel, extra);
	sift_agg_v6_stream_upper_paths(root, stage, input_rel, output_rel, extra);
}

void
sift_agg_v6_live_outer_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	if (sift_store_ops_any() && sift_db_has_extension())
		sift_agg_v6_two_upper_paths(root, stage, input_rel, output_rel, extra);
}

void
sift_agg_v6_live_register(void)
{
	RegisterCustomScanMethods(&sift_agg_v6_two_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_topn_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_window_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_first_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_stream_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_sip_plan_methods);
}
