/*
 * col/f2/scan/survey.c - the granules a scan of an index of format major 2 would read, counted from the zones and the skipping indexes of its parts before any is read: what a hybrid
 * scan (agg/hybrid_exec.c) weighs the structured side by once its values are bound.
 */
#include "postgres.h"

#include "col/f2/scan/scan_int.h"
#include "core/errctx.h"

uint64
sift_f2_scan_survey(IndexScanDesc scan)
{
	F2Scan	   *s = scan->opaque;
	SiftErrCtx	ctx;

	s->survey = true;
	sift_errctx_push(&ctx, RelationGetRelid(scan->indexRelation));
	sift_f2_scan_prepare(scan, s);
	sift_f2_scan_collect(scan, s);
	sift_errctx_pop(&ctx);
	return s->survey_granules;
}
