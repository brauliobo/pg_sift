/*
 * agg/v6/v6_live.c - the upper-path hook and the node registration of the operators of SiftAggV6 (v6_live.h).
 */
#include "postgres.h"

#include "nodes/extensible.h"
#include "optimizer/optimizer.h"

#include "agg/agg.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_live.h"
#include "agg/v6/v6_sip.h"
#include "agg/v6/v6_stream.h"
#include "agg/v6/v6_topn.h"
#include "agg/v6/v6_window.h"
#include "col/f2/f2_v6_bridge.h"

void
sift_agg_v6_live_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	if (!sift_v6_format_bound())
		return;
	sift_agg_v6_topn_upper_paths(root, stage, input_rel, output_rel, extra);
	sift_agg_v6_window_upper_paths(root, stage, input_rel, output_rel, extra);
	sift_agg_v6_stream_upper_paths(root, stage, input_rel, output_rel, extra);
}

void
sift_agg_v6_live_register(void)
{
	RegisterCustomScanMethods(&sift_agg_v6_topn_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_window_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_stream_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_sip_plan_methods);
}

bool
sift_agg_v6_live_scan(const Plan *plan, Oid *indexoid)
{
	const CustomScan *cs;
	const List *priv;

	if (!IsA(plan, CustomScan))
		return false;
	cs = (const CustomScan *) plan;
	priv = cs->custom_private;
	if (cs->methods == &sift_agg_v6_plan_methods || cs->methods == &sift_agg_v6_stream_plan_methods || cs->methods == &sift_agg_v6_window_plan_methods)
		*indexoid = (Oid) linitial_int(linitial(priv));
	else if (cs->methods == &sift_agg_v6_topn_plan_methods)
		*indexoid = (Oid) linitial_int(linitial(linitial(priv)));
	else
		return false;
	return true;
}

bool
sift_agg_v6_live_owns_index(Oid indexoid)
{
	return sift_v6_format_bound() && sift_v6_format()->owns_rel(indexoid, false);
}
