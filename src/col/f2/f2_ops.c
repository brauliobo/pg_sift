/*
 * col/f2/f2_ops.c - the Format 2 table of the column access method (col/am/format_ops.h): what CREATE INDEX and REINDEX build, and what an index whose block 0 says format major 2 runs.
 */
#include "postgres.h"

#include "col/f2/f2.h"
#include "col/f2/f2_journal.h"
#include "col/f2/l0_stage.h"
#include "col/f2/scan/ord_stream.h"
#include "core/f2/f2_progress.h"

static void
f2_flush_held(void)
{
	sift_stage_flush_all();
	sift_f2_journal_flush();
}

const SiftColFormatOps sift_col_v2_ops = {
	.format_major = SIFT_FORMAT2_MAJOR,
	.name = "f2",
	.ambuild = sift_f2_ambuild,
	.ambuildempty = sift_f2_ambuildempty,
	.ambuildphasename = sift_f2_ambuildphasename,
	.aminsert = sift_f2_aminsert,
	.ambulkdelete = sift_f2_ambulkdelete,
	.amvacuumcleanup = sift_f2_amvacuumcleanup,
	.amcostestimate = sift_f2_amcostestimate,
	.ambeginscan = sift_f2_ambeginscan,
	.amrescan = sift_f2_amrescan,
	.amgetbitmap = sift_f2_amgetbitmap,
	.amgettuple = sift_f2_ord_gettuple,
	.amendscan = sift_f2_amendscan,
	.amcanreturn = sift_f2_amcanreturn,
	.ammarkpos = sift_f2_ord_markpos,
	.amrestrpos = sift_f2_ord_restrpos,
	.flush_held = f2_flush_held,
	.track_trigger = sift_f2_track_trigger,
	.track_reconcile = sift_f2_track_reconcile,
	.tracks_addresses = true,
	.deletes_heard = sift_f2_deletes_heard,
	.sql = sift_col_v2_sql,
	.check = sift_f2_check,
	.journal = &sift_f2_journal_ops,
};
