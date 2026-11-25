/*
 * col/f2/scan/fetch.c - the scan of an index of format major 2 handed to the executor: the collected addresses as a bitmap or one tuple at a time (col/f2/f2_scan.c collects them).
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/itup.h"
#include "nodes/tidbitmap.h"
#include "utils/memutils.h"

#include "col/f2/f2_scope_route.h"
#include "col/f2/scan/rescan_cache.h"
#include "col/f2/scan/scan_int.h"
#include "col/f2/scan/stream.h"
#include "core/errctx.h"

static F2Scan *
collected(IndexScanDesc scan, TIDBitmap *tbm)
{
	F2Scan	   *s = scan->opaque;
	SiftErrCtx	ctx;

	sift_errctx_push(&ctx, RelationGetRelid(scan->indexRelation));
	if (!s->prepared)
		sift_f2_scan_prepare(scan, s);
	if (!s->collected)
	{
		if (tbm != NULL)
		{
			s->tbm = MemoryContextAllocZero(s->cx, sizeof(F2TbmSink));
			s->tbm->tbm = tbm;
		}
		sift_f2_scan_collect(scan, s);
	}
	sift_errctx_pop(&ctx);
	return s;
}

static void
scan_sink(IndexScanDesc scan, SiftF2Sink sink, void *arg, const int *leaves, int nleaves, bool maybe)
{
	F2Scan	   *s = scan->opaque;
	SiftErrCtx	ctx;

	s->sink_maybe = maybe;
	s->sink = sink;
	s->sink_arg = arg;
	s->sink_leaves = leaves;
	s->nsink_leaves = nleaves;
	sift_errctx_push(&ctx, RelationGetRelid(scan->indexRelation));
	sift_f2_scan_prepare(scan, s);
	if (s->recheck_all)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a qual of the aggregate cannot be decided on the stored values"),
						errhint("Run the statement with sift.enable_agg = off: the heap decides such a qual.")));
	sift_f2_scan_collect(scan, s);
	sift_errctx_pop(&ctx);
}

void
sift_f2_scan_sink(IndexScanDesc scan, SiftF2Sink sink, void *arg, const int *leaves, int nleaves)
{
	scan_sink(scan, sink, arg, leaves, nleaves, false);
}

void
sift_f2_scan_sink_maybe(IndexScanDesc scan, SiftF2Sink sink, void *arg, const int *leaves, int nleaves)
{
	scan_sink(scan, sink, arg, leaves, nleaves, true);
}

void
sift_f2_scan_set_part_sink(IndexScanDesc scan, SiftF2PartSink part_sink)
{
	((F2Scan *) scan->opaque)->part_sink = part_sink;
}

void
sift_f2_scan_set_part_dict_sink(IndexScanDesc scan, SiftF2PartDictSink sink)
{
	((F2Scan *) scan->opaque)->part_dict_sink = sink;
}

void
sift_f2_scan_set_gran_sink(IndexScanDesc scan, SiftF2GranSink gran_sink, void *gran_arg)
{
	F2Scan	   *s = scan->opaque;

	s->gran_sink = gran_sink;
	s->gran_arg = gran_arg;
}

void
sift_f2_scan_set_pin_win(IndexScanDesc scan, struct SiftPinWin *win)
{
	((F2Scan *) scan->opaque)->pinwin = win;
}

void
sift_f2_scan_set_dict_leaves(IndexScanDesc scan, const bool *dict_leaf)
{
	((F2Scan *) scan->opaque)->sink_dict = dict_leaf;
}

int64
sift_f2_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	return collected(scan, tbm)->ntids;
}

bool
sift_f2_amgettuple(IndexScanDesc scan, ScanDirection dir)
{
	F2Scan	   *s = collected(scan, NULL);
	uint64		t;

	if (s->stream != NULL)
	{
		if (!sift_f2_stream_next(s, dir, &t))
			return false;
	}
	else
	{
		if (s->sort == NULL || ScanDirectionIsNoMovement(dir) || !tuplesort_gettupleslot(s->sort, ScanDirectionIsForward(dir), false, s->get, NULL))
			return false;
		slot_getallattrs(s->get);
		t = (uint64) DatumGetInt64(s->get->tts_values[0]);
	}
	sift_f2_tid_set(&scan->xs_heaptid, t >> 1);
	scan->xs_recheck = (t & 1) != 0;
	if (scan->xs_want_itup)
		scan->xs_hitup = sift_f2_returned_tuple(scan, s);
	return true;
}

void
sift_f2_scan_set_bridge(IndexScanDesc scan, const SiftTidBridge *bridge, uint64 *seen, uint64 *skipped)
{
	F2Scan	   *s = scan->opaque;

	s->bridge = bridge;
	s->bridge_seen = seen;
	s->bridge_skipped = skipped;
}

void
sift_f2_scan_set_scope_router(IndexScanDesc scan, const SiftScopeRouter *router)
{
	F2Scan	   *s = scan->opaque;

	s->router = router;
}

void
sift_f2_scan_skip_parts(IndexScanDesc scan, const uint64 *gens, int ngens)
{
	F2Scan	   *s = scan->opaque;

	s->skip_gens = gens;
	s->nskip_gens = ngens;
}

void
sift_f2_scan_set_plan(IndexScanDesc scan, const struct SiftPlanSnapshot *plan)
{
	F2Scan	   *s = scan->opaque;

	s->pinned_plan = plan;
}

void
sift_f2_amendscan(IndexScanDesc scan)
{
	F2Scan	   *s = scan->opaque;

	sift_f2_scan_sort_end(s);
	sift_rescan_end(s);
	MemoryContextDelete(s->cx);
	pfree(s);
}
