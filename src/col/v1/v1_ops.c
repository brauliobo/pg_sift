/*
 * col/v1/v1_ops.c - the Format 1 table of the column access method (col/am/format_ops.h)
 */
#include "postgres.h"

#include "col/am.h"
#include "core/sql.h"

static Datum
v1_sql(SiftColSqlFn fn, FunctionCallInfo fcinfo)
{
	switch (fn)
	{
		case SIFT_SQL_SPACE:
			return sift_col_v1_space(fcinfo);
		case SIFT_SQL_EXTENTS:
			return sift_col_v1_extents(fcinfo);
		case SIFT_SQL_SHRINK_HISTORY:
			return sift_col_v1_shrink_history(fcinfo);
		case SIFT_SQL_INDEX_INFO:
			return sift_col_v1_index_info(fcinfo);
		case SIFT_SQL_SEGMENTS:
			return sift_col_v1_segments(fcinfo);
		case SIFT_SQL_LEAVES:
			return sift_col_v1_leaves(fcinfo);
		case SIFT_SQL_SECTIONS:
			return sift_col_v1_sections(fcinfo);
		case SIFT_SQL_VERIFY:
			return sift_col_v1_verify(fcinfo);
		case SIFT_SQL_MAINTAIN:
			return sift_col_v1_maintain(fcinfo);
		case SIFT_SQL_MAINTENANCE_DUE:
			return sift_col_v1_maintenance_due(fcinfo);
		case SIFT_SQL_MAINTENANCE_STATUS:
			return sift_col_v1_maintenance_status(fcinfo);
		case SIFT_SQL_TRACKING:
			return sift_col_v1_tracking(fcinfo);
		case SIFT_SQL_WORKER_STATE:
			return sift_worker_state_dir(fcinfo);
		case SIFT_SQL_WORKER_HISTORY:
			return sift_worker_history_dir(fcinfo);
		case SIFT_SQL_REFRESH:
			return sift_col_v1_refresh(fcinfo);
	}
	pg_unreachable();
}

const SiftColFormatOps sift_col_v1_ops = {
	.format_major = 1,
	.name = "v1",
	.ambuild = sift_col_ambuild,
	.ambuildempty = sift_col_ambuildempty,
	.ambuildphasename = sift_col_ambuildphasename,
	.aminsert = sift_col_aminsert,
	.ambulkdelete = sift_col_ambulkdelete,
	.amvacuumcleanup = sift_col_amvacuumcleanup,
	.amcostestimate = sift_col_amcostestimate,
	.ambeginscan = sift_col_ambeginscan,
	.amrescan = sift_col_amrescan,
	.amgetbitmap = sift_col_amgetbitmap,
	.amgettuple = sift_col_amgettuple,
	.amendscan = sift_col_amendscan,
	.amcanreturn = sift_col_amcanreturn,
	.ammarkpos = sift_col_ammarkpos,
	.amrestrpos = sift_col_amrestrpos,
	.flush_held = sift_col_v1_flush_held,
	.track_trigger = sift_col_v1_track_old_rows,
	.track_reconcile = sift_col_v1_track_reconcile,
	.tracks_addresses = false,
	.sql = v1_sql,
	.check = sift_col_v1_check,
};
