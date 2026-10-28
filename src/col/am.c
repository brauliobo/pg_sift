/*
 * col/am.c - the sift_col index access method: handler, options, the per-relation leaf list, tuple to row conversion and
 * aminsert. The build is in col/build.c, VACUUM and maintenance in col/maint.c, the read side in col/scan.c.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/varlena.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/hsearch.h"
#include "utils/numeric.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/sift.h"
#include "bm25/am.h"
#include "agg/stats.h"
#include "col/am.h"
#include "col/dictshm.h"
#include "col/leafshm.h"
#include "col/build.h"
#include "col/seg.h"
#include "col/point.h"
#include "col/hll.h"
#include "col/gorder.h"
#include "col/runs.h"
#include "col/setsum.h"
#include "core/cpu.h"
#include "core/limits.h"
#include "core/ops.h"
#include "core/visibility.h"

bool		sift_enable_col = true;
int			sift_col_format_minor = SIFT_COL_FORMAT_MINOR;
#ifdef SIFT_TESTING
bool		sift_test_col_rel_inval = false;
#endif

/* 5 is the minor of the wide leaves, which a schema takes by itself, and 11 the format of the write buffer of a table with a wide row: the setting is 3, 4, 6, 7, 8, 9, 10, 12, 13, 14 or 15 */
static bool
check_col_format_minor(int *newval, void **extra, GucSource source)
{
	if (*newval == SIFT_COL_FORMAT_WIDE || *newval == SIFT_COL_FORMAT_WIDE_ROWS)
	{
		GUC_check_errdetail("Column format minor %d is written for %s only, whatever the setting.", *newval,
							*newval == SIFT_COL_FORMAT_WIDE ? "wide leaves" : "the write buffer of a table with a wide row");
		GUC_check_errhint("Use 3 (pg_sift 3.0), 4 (pg_sift 3.1), 6, 7, 8, 9, 10, 12, 13, 14 or 15.");
		return false;
	}
	return true;
}

static relopt_kind col_relopt_kind;

/* Every entry of the option is a policy, optionally prefixed by "column =" */
void
sift_col_validate_compression(const char *value)
{
	char	   *list = pstrdup(value),
			   *save = NULL;

	for (char *e = strtok_r(list, ";", &save); e; e = strtok_r(NULL, ";", &save))
	{
		SiftCcPolicy p;
		const char *eq = strchr(e, '='),
				   *err = sift_cc_policy_parse(eq ? eq + 1 : e, &p);

		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid compression option \"%s\": %s", value, err)));
	}
	pfree(list);
}

void
sift_col_validate_preagg(const char *value)
{
	const char *err = sift_cpagg_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid preagg option \"%s\": %s", value, err)));
}

void
sift_col_validate_rollup(const char *value)
{
	const char *err = sift_cpagg_rollup_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid rollup option \"%s\": %s", value, err)));
}

void
sift_col_validate_rollup_dims(const char *value)
{
	const char *err = sift_cpagg_rows_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid rollup_dims option \"%s\": %s", value, err)));
}

void
sift_col_validate_calendar_columns(const char *value)
{
	const char *err = sift_cpagg_calendar_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid calendar_columns option \"%s\": %s", value, err)));
}

/* 'a, b': the names of the text, varchar and uuid columns a tuple scan returns; r NULL: just the syntax. Empty or 'off': none */
static const char *
return_walk(const char *opt, SiftColRel *r, TupleDesc desc)
{
	const char *p = opt;

	while (*p == ' ')
		p++;
	if (*p == '\0' || strcmp(p, "off") == 0)
		return NULL;
	for (;;)
	{
		const char *end = strchr(p, ',');
		size_t		len = end ? (size_t) (end - p) : strlen(p);
		int			a = 0;

		while (len > 0 && p[len - 1] == ' ')
			len--;
		if (len == 0)
			return "empty column name";
		if (r != NULL)
		{
			for (int i = 1; i <= r->natts && a == 0; i++)
				if (strlen(NameStr(TupleDescAttr(desc, i - 1)->attname)) == len && strncmp(NameStr(TupleDescAttr(desc, i - 1)->attname), p, len) == 0)
					a = i;
			if (a == 0)
				return "unknown column";
			if (!sift_col_returns_str(desc, a, r->nleaves[a], r->json[a]))
				return "not a text, varchar or uuid column";
			r->ret[a] = true;
		}
		if (end == NULL)
			return NULL;
		p = end + 1;
		while (*p == ' ')
			p++;
	}
}

void
sift_col_validate_return_columns(const char *value)
{
	const char *err = return_walk(value, NULL, NULL);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid return_columns option \"%s\": %s", value, err)));
}

void
sift_col_validate_key_columns(const char *value)
{
	const char *err;

	/* the validator also checks the default, which is NULL: the option is not set until CREATE INDEX chooses it */
	if (value == NULL)
		return;
	err = sift_cset_keys_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid key_columns option \"%s\": %s", value, err)));
}

void
sift_col_validate_order_columns(const char *value)
{
	const char *err = sift_crun_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_columns option \"%s\": %s", value, err)));
}

void
sift_col_validate_order_array_group(const char *value)
{
	bool		autolist;
	const char *err = sift_crun_syntax(sift_cpagg_auto_list(value, &autolist));

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_group option \"%s\": %s", value, err)));
}

void
sift_col_validate_order_array_columns(const char *value)
{
	bool		autolist;
	const char *err = sift_crun_syntax(sift_cpagg_auto_list(value, &autolist));

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_columns option \"%s\": %s", value, err)));
}

void
sift_col_validate_hll_columns(const char *value)
{
	const char *err = sift_hll_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid hll_columns option \"%s\": %s", value, err)));
}

void
sift_col_validate_point_columns(const char *value)
{
	const char *err = sift_cpoint_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid point_columns option \"%s\": %s", value, err)));
}

void
sift_col_validate_count_columns(const char *value)
{
	const char *err = sift_cpoint_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid count_columns option \"%s\": %s", value, err)));
}

void
sift_col_validate_point_tid_columns(const char *value)
{
	const char *err = sift_cpoint_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid point_tid_columns option \"%s\": %s", value, err)));
}

static relopt_enum_elt_def array_lengths_names[] = {
	{"auto", SIFT_COL_ARRLEN_AUTO},
	{"on", SIFT_COL_ARRLEN_ON},
	{"true", SIFT_COL_ARRLEN_ON},
	{"off", SIFT_COL_ARRLEN_OFF},
	{"false", SIFT_COL_ARRLEN_OFF},
	{(const char *) NULL}
};

static relopt_enum_elt_def dict_lookup_names[] = {
	{"auto", SIFT_COL_DICT_AUTO},
	{"on", SIFT_COL_DICT_ON},
	{"true", SIFT_COL_DICT_ON},
	{"off", SIFT_COL_DICT_OFF},
	{"false", SIFT_COL_DICT_OFF},
	{(const char *) NULL}
};

void
sift_col_init(void)
{
	DefineCustomBoolVariable("sift.enable_col",
							 "Enables the column scans over sift_col indexes.",
							 NULL, &sift_enable_col, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.enable_col_scan",
							 "Lets the planner use a sift_col index with the sift.col_*_ops operator classes as a bitmap index scan.",
							 "On by default: the planner chooses it by cost against the b-trees and the sequential scan; off keeps the aggregate scans of SiftAgg only, see docs/COL-SCAN.md.",
							 &sift_enable_col_scan, true, PGC_USERSET, 0, NULL, NULL, NULL);
	sift_col_scan_init();
	DefineCustomRealVariable("sift.plan_scale",
							 "Plans as if the tables with a sift_col index had this many times the pages and rows (1: as they are).",
							 "The tables without one, and the statistics of the columns, stay as they are. For the planner's choice at a larger size: compare the plans of EXPLAIN and sift.explain_why at 10 and 100 without the data. PostgreSQL up to 18.",
							 &sift_plan_scale, 1.0, 1.0, 1e6, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.col_rollup_on_demand",
							 "A scan that reads units for lack of their rollups asks the background worker to rebuild them.",
							 "Nothing is written by the query; the next one answers from the rollups. Off: the units stay as they are until a merge or REINDEX.",
							 &sift_col_rollup_on_demand, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.cluster_advice",
							 "sift.cluster_advice() recommends a physical order for the tables of the sift_col indexes.",
							 "Off: it answers 'not recommended' for every index, and CREATE INDEX does not suggest key_cut. The index option cluster_advice switches one index off.",
							 &sift_cluster_advice, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.col_rollup_worker",
							 "A request for a rollup refresh starts the background worker that makes it.",
							 "Off: the requests wait in the queue until sift.col_rollup_refresh(index) is called.",
							 &sift_col_rollup_worker, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.col_auto_merge",
							 "Keeps the sift_col indexes answerable after churn: a background worker vacuums the heap table, and after a VACUUM that left an index scattered it seals the write buffer and merges all its segments into one.",
							 "Rows on a page the visibility map does not call all-visible (any page with an UPDATE or DELETE since the last VACUUM) are checked one by one in the heap, "
							 "and a unit holding a dead row answers from its rows; the VACUUM sets the map, and the merge after it makes the units answer from their pre-aggregates again. "
							 "A backend that inserts a row version, or reads many rows in the heap, looks at the statistics of the table once per sift.maintenance_naptime and starts the worker "
							 "when the rows changed since the last VACUUM are a thousandth of the table (50 rows at least, 100,000 at most). It vacuums as autovacuum does (its reloptions, cost delay, skipped when the table is locked), "
							 "at most once per five sift.maintenance_naptime (ten times the average VACUUM of the table on PostgreSQL 18), and not a table with autovacuum_enabled = false. "
							 "The merge rule is sift.col_auto_merge_dirty_pct, _fragmentation and _buffer_pct, and it stays within sift.col_auto_merge_wal_pct of the WAL the server writes; the rewrite sleeps as sift.maintenance_cost_delay says and is skipped "
							 "when the maintenance lock is held. An index over sift.vacuum_rewrite_limit is rewritten by partitions of that size (sift.col_auto_merge_partition_kb sets another), one step each, so that the lock is let go between them. Needs sift.async_maintenance; sift.col_maintain(index) applies the same rule from a job. "
							 "A DELETE alone calls nothing and waits for a scan or an insert; a scan of a partitioned table does not ask, an insert into a partition does. "
							 "Set it for the server or the database: the worker reads no session setting.",
							 &sift_col_auto_merge, true, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_auto_merge_dirty_pct",
							"Percent of the units of a sift_col index holding a dead row above which the automatic rewrite runs.",
							"Such a unit answers from its rows, not from its pre-aggregates, however few rows are dead.",
							&sift_col_auto_merge_dirty_pct, 30, 1, 100, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.col_auto_merge_fragmentation",
							 "Units listed per superblock they cover from which the automatic rewrite of a sift_col index runs.",
							 "Each promotion of the write buffer adds a segment over the superblocks the churn touched.",
							 &sift_col_auto_merge_fragmentation, 2.0, 1.1, 1000.0, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.col_auto_merge_buffer_pct",
							 "Percent of its rows the write buffer of a sift_col index holds above which the automatic rewrite runs.",
							 "The rows of the buffer are read as one in-memory unit per superblock they touch, on every query, until a promotion.",
							 &sift_col_auto_merge_buffer_pct, 1.0, 0.01, 100.0, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.col_auto_merge_wal_pct",
							 "Percent of the WAL the server writes that the automatic rewrite of a sift_col index may add.",
							 "The rewrite writes the whole index, so it waits until the server has written 100 / this percent times the bytes of the index since the index was last written "
							 "(its build or its last rewrite): 5 bounds the rewrites at a twentieth of the WAL, and the churn that makes one worth it is the churn that wrote that WAL. "
							 "The segments of a build that overlap each other (a parallel build leaves one set per worker) are not churn: their rewrite does not wait. "
							 "0 does not wait. sift.col_maintain(index) ignores it.",
							 &sift_col_auto_merge_wal_pct, 5.0, 0.0, 100.0, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_auto_merge_max_active",
							"Client backends of the database running a statement above which the automatic rewrite of a sift_col index waits for a quieter moment.",
							"The worker looks again after sift.maintenance_naptime, then twice as late each time, 8 times in all; the next VACUUM asks again. 0: never waits.",
							&sift_col_auto_merge_max_active, 0, 0, 10000, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_auto_merge_partition_kb",
							"Size of the segments the automatic rewrite of a sift_col index writes, which then rewrites only the ones the churn touched; 0 rewrites the whole index into one.",
							"A step rewrites the partition with most dirty units (over sift.col_auto_merge_dirty_pct of its own), or the overlays the promotions left over the partitions, "
							"and holds the maintenance lock and needs the free space of one partition, not of the index. A segment under half of this size is an overlay; "
							"the merge tiers leave the partitions alone. An index under half of it is rewritten whole. 0: partitions of sift.vacuum_rewrite_limit for an index over it, or of a third of maintenance_work_mem where that is less, which is what the writer of a segment holds in memory.",
							&sift_col_auto_merge_partition_kb, 0, 0, MAX_KILOBYTES, PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_unit_floor",
							"Rows per unit below which the rows of a sift_col promotion or overlay merge are cut by TID range, not one unit per superblock; 0 keeps a unit per superblock.",
							"Churn puts a few dozen rows in each of the superblocks it touches, and a unit costs a scan about as much as a thousand rows however few it holds, so a promotion that reaches "
							"16 superblocks or more with under half this many rows each is written as units of this many rows in TID order, and a merge that stays that sparse does the same. "
							"A merge that reaches this many rows per superblock cuts by superblock again. Needs a table of 16 superblocks (4,096 pages) or more.",
							&sift_col_unit_floor, 2048, 0, 32768, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_auto_merge_max_workers",
							"Rewrite workers of sift_col indexes that may run at once in a database, 0 for no limit.",
							"A worker above the limit looks again after sift.maintenance_naptime, then twice as late each time, 8 times in all; the next VACUUM asks again.",
							&sift_col_auto_merge_max_workers, 0, 0, 64, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_leaf_cache_kb",
							"Shared memory, for all backends together, that keeps what the zones of the units of a sift_col index say to the planner (a record for each leaf and a sample of the unit zones), so that a new backend does not read them all (0: off).",
							"The table is allocated, at the size of this setting, by the first backend that plans a statement over a sift_col index after the server starts; a later change turns the use of it on and off (0), not its size. A record is about 33 kB for each integer leaf and 0.4 kB for the others; a record that does not fit is not kept, and the oldest goes when the table is full.",
							&sift_col_leaf_cache_kb, 4096, 0, 262144,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_format_minor",
							"Column format minor of the segments written (15, the newest 3.3 writes, by default): 6 keeps them readable by pg_sift 3.2, 4 by 3.1, 3 by 3.0.",
							"Segments already written keep their minor; a 3.2 library refuses the ones above 6. A segment of a schema with wide leaves is minor 5 at least, and the minor 6 "
							"of the complete kinds is written only for a schema with jsonb paths, from rows whose kinds are known. The minor 7 is written only for an index with the option cube_minmax, the minor 8 only for a sift_table with a date or timestamp column, the minor 9 only for an index with the option rollup_dims, the minor 10 only for one with the option key_columns, the minor 12 only for one with the option superunit, the minor 13 only for one with the option order_columns, the minor 14 only for one with a dictionary of 1,024 strings or more (the option dict_lookup), the minor 15 only for one whose key_columns keep a key list or a bitmap.",
							&sift_col_format_minor, SIFT_COL_FORMAT_MINOR, SIFT_COL_FORMAT_MIN_READ, SIFT_COL_FORMAT_MINOR,
							PGC_SUSET, 0, check_col_format_minor, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomIntVariable("sift.test_col_rollup_panic",
							"Testing: the rollup refresh worker PANICs after it wrote the new segment, before it replaces the old one (1 = yes). Set it in postgresql.conf: the worker reads no session setting.",
							NULL, &sift_test_col_rollup_panic, 0, 0, 1, PGC_SIGHUP, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_merge_panic",
							 "Testing: a column merge (promotion excepted) PANICs after it wrote the new segment, before it replaces the old ones.",
							 NULL, &sift_test_col_merge_panic, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_gorder_rows",
							"Testing: 1024 writes the global order with the chunks of column format minor 21 (1,024 entries), 0 the current ones (512).",
							NULL, &sift_test_col_gorder_rows, 0, 0, 1024, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_floor_sbs",
							"Testing: the superblocks a promotion or merge must reach for sift.col_unit_floor to apply (0 = 16).",
							NULL, &sift_test_col_floor_sbs, 0, 0, 1000, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_refresh_sleep_ms",
							"Testing: the refresh of a segment (sift.col_refresh, the rollup worker) sleeps this long after the new segment is written and before it replaces the old one (0 = no).",
							NULL, &sift_test_col_refresh_sleep_ms, 0, 0, 60000, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_scan_jitter_us",
							"Testing: a column scan sleeps up to this many microseconds at the points where VACUUM or DML can interleave (0 = no).",
							NULL, &sift_test_col_jitter_us, 0, 0, 1000000, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_dict_shared",
							"Testing: the dictionaries shared between backends (col/dictshm.c): 0 = as built, 1 = not shared, 2 = every dictionary shared, 3 = the table cannot be had.",
							NULL, &sift_test_col_dict_shared, 0, 0, 3, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_verify_inval",
							 "Testing: the heap check of sift.col_verify invalidates the index's relcache entry (what another session's VACUUM or ANALYZE does) at every tuple.",
							 NULL, &sift_test_col_verify_inval, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_rel_inval",
							 "Testing: a sift_col build and the recheck of the rows a leaf cannot store invalidate the index's relcache entry (what a lock taken while they run may do) at every tuple.",
							 NULL, &sift_test_col_rel_inval, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_paranoid",
							 "Testing: a column scan checks the rows on all-visible pages against the heap too and logs the ones the snapshot does not see.",
							 NULL, &sift_test_col_paranoid, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_workers",
							"Testing: parallel workers the last sift_col build of this session launched.",
							NULL, &sift_test_col_workers, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_fail_sb",
							"Testing: the participant of a sift_col build that takes this superblock (1 = the first) fails (0 = none).",
							NULL, &sift_test_col_fail_sb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_segment_kb",
							"Testing: kB of memory a sift_col build's segment writer holds before the segment is cut (0 = by maintenance_work_mem).",
							NULL, &sift_test_col_segment_kb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_max_segments",
							"Testing: segments a participant of a sift_col build with key_cut may hold before it merges runs (0 = what the directory has room for).",
							NULL, &sift_test_col_max_segments, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
#endif
	col_relopt_kind = add_reloption_kind();
	add_int_reloption(col_relopt_kind, "write_buffer_limit",
					  "Write buffer size in kB before it is sealed for promotion",
					  SIFT_DEFAULT_WRITE_BUFFER_LIMIT, 64, 1048576, ShareUpdateExclusiveLock);
	add_int_reloption(col_relopt_kind, "dead_rewrite_pct",
					  "Percent of dead rows above which maintenance rewrites a lone segment to drop them (100 never)",
					  SIFT_COL_DEAD_PCT, 1, 100, ShareUpdateExclusiveLock);
	add_bool_reloption(col_relopt_kind, "vacuum_truncate",
					   "VACUUM gives the free blocks at the end of the index file back to the operating system",
					   true, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "paths",
						 "jsonb paths shredded into columns, by index attribute name",
						 "", NULL, AccessExclusiveLock);
	add_string_reloption(col_relopt_kind, "compression",
						 "chunk compression: auto, none, lz4, zstd, encodings; per column: 'auto; col=zstd:9'",
						 "", sift_col_validate_compression, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "preagg",
						 "pre-aggregates of every unit: default, off, or cube(dim: measure, ...; dim2: ...)",
						 "", sift_col_validate_preagg, ShareUpdateExclusiveLock);
	add_bool_reloption(col_relopt_kind, "rollup_on_demand",
					   "A scan that reads units for lack of their rollups asks the background worker to rebuild them (default on)",
					   true, ShareUpdateExclusiveLock);
	add_bool_reloption(col_relopt_kind, "cluster_advice",
					   "sift.cluster_advice() may recommend a physical order or a key_cut for the table of this index, and CREATE INDEX may suggest key_cut (default on; maintenance only, not part of the format)",
					   true, ShareUpdateExclusiveLock);
	add_enum_reloption(col_relopt_kind, "array_lengths",
					   "The array columns of the index keep their cardinality and array_length(x, 1) as two integer columns of their own, so that cardinality(x), array_length(x, 1) and x IS [NOT] NULL are read without the arrays (about 1 byte a row for each array column): on for every array column, off for none, auto (default) is on for the index that CREATE INDEX or REINDEX builds and off for an index built before the option; after a change the table takes no write until the REINDEX",
					   array_lengths_names, SIFT_COL_ARRLEN_AUTO,
					   "Valid values are \"auto\", \"on\" and \"off\".", ShareUpdateExclusiveLock);
	add_enum_reloption(col_relopt_kind, "dict_lookup",
					   "A dictionary of 1,024 strings or more of the segments written from now on is stored in blocks that are read in place, without loading it whole, where that adds at most 3% to the segment (ids, hashes, uuids and long strings; short strings that repeat words stay in the compact form); the option adds the order of the strings, so that an equality, an IN list or a prefix test reads it in place too: auto (default) for the columns of point_columns, key_columns and return_columns, on for every column, off for none (the segments are column format minor 14; the order is 4 bytes a string)",
					   dict_lookup_names, SIFT_COL_DICT_AUTO,
					   "Valid values are \"auto\", \"on\" and \"off\".", ShareUpdateExclusiveLock);
	add_bool_reloption(col_relopt_kind, "cube_minmax",
					   "The cube of every unit written from now on also holds the minimum and maximum of each integer measure per group; the segments are column format minor 7 (default on)",
					   true, ShareUpdateExclusiveLock);
	add_bool_reloption(col_relopt_kind, "key_table",
					   "The segments written from now on keep, for each key_columns leaf, a table of its distinct keys with the rows, sums, minima and maxima of each, where that adds at most 3% to the segment, so that count, sum, min and max WHERE key = x read one record of a segment, not its units; the segments are column format minor 22, which 3.4 and older libraries cannot read (default on)",
					   true, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "rollup_dims",
						 "the scalar columns that the rollup of the rows of every unit written from now on groups by: 'col, col' or 'col, col: measure, ...', 'auto' (default) or 'off'; the segments are column format minor 8",
						 "", sift_col_validate_rollup_dims, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "calendar_columns",
						 "the date, timestamp and timestamptz columns whose calendar cells (days, else months, else years) every unit written from now on keeps, so that a GROUP BY of date_trunc, extract or a cast of them, and a range, are answered from the cells; 'col, col', 'all' or 'off' (default all, unless the index has JSON paths; the cube holds at most 16 dimensions with the default dims, a list that does not fit is refused and 'all' keeps the columns that fit; the segments are column format minor 8)",
						 "", sift_col_validate_calendar_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "key_columns",
						 "the columns whose keys every unit written from now on keeps, as a key list or a bitmap where that takes a few percent of the unit (a count(DISTINCT column) over whole units reads no row, an equality or IN test skips the units that lack the key) and, for a column named here, as a bloom filter where it does not; 'col, col', 'off' or 'auto' (the int2, int4, int8 and date foreign key columns and the columns named *_id that are not unique, or unique when the index is key cut or the heap is not in their order, and that the cube does not hold; chosen by CREATE INDEX and REINDEX when the option is not set); the segments are column format minor 10, 15 for a key list or bitmap, 23 for a list of runs (default auto)",
						 NULL, sift_col_validate_key_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "key_cut",
						 "the integer, date, timestamp, numeric and float columns, most significant first, whose range cuts the units that promotion, merge and REINDEX write (a key-cut unit lists its TIDs; the segments are column format minor 17); 'col, col', 'off' or 'auto' (CREATE INDEX and REINDEX sample the heap and cut by the first key-shaped column whose equality reads many units and that no ordered leaf competes with; a unit is a superblock otherwise; default auto)",
						 NULL, NULL, ShareUpdateExclusiveLock);
	add_int_reloption(col_relopt_kind, "key_cut_rows",
					  "Rows a key-cut unit aims at (0: as many as an average superblock of the table holds)",
					  0, 0, 32768, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "order_columns",
						 "the integer, date, timestamp, numeric and float columns whose rows every unit written from now on keeps in the order of the column (a sorted run that lets ORDER BY .. LIMIT read the first rows of each unit only); the segments are column format minor 13 (default none)",
						 "", sift_col_validate_order_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "order_array_columns",
						 "the integer, date and timestamp columns whose segments written from now on also keep every row's key and heap address sorted (with the sorted run of the column), so that a range of the column of up to 16,384 rows and ORDER BY column LIMIT n above 32 read the entries between the bounds, whatever the number of units; 4 to 7 bytes a row for each column; the segments are column format minor 21; 'col, col', 'off' or 'auto' (CREATE INDEX and REINDEX choose up to two columns of 100,000 rows or more whose values are scattered over the heap, that is the default)",
						 NULL, sift_col_validate_order_array_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "order_array_group",
						 "a column (integer, date or timestamp) that the arrays of order_array_columns are grouped by: they keep the rows sorted by this column and then the key, so that WHERE group = x ORDER BY column LIMIT n reads n entries of the group x whatever the number of units; a row with no value of it is not in them, and ORDER BY column LIMIT n without the condition reads the units; 3 to 7 bytes a row for each column (default none)",
						 "", sift_col_validate_order_array_group, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "hll_columns",
						 "the int2, int4, int8, date, timestamp, timestamptz, text, varchar and uuid columns whose distinct values every unit written from now on keeps as a HyperLogLog sketch (sift.approx_count_distinct reads one sketch a unit, not the rows; count(DISTINCT) stays exact); the segments are column format minor 18 (default none)",
						 "", sift_col_validate_hll_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "point_columns",
						 "the plain integer, text and uuid columns whose rows every segment written from now on lists by value (a point directory that lets an equality or a short IN list of the access method find its rows with a probe, and the heap check them); the segments are column format minor 14 (default none)",
						 "", sift_col_validate_point_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "count_columns",
						 "the text columns whose dictionary of 1,024 strings or more every segment written from now on stores with the rows of each string in byte order (the columns whose dictionary is read in place do so unless this is off, the ones named here also get that order), so that count(*) of a prefix (LIKE 'abc%'), a range or an equality of the column over segments with no dead row reads two positions of the dictionary and no row; 0.1 to 1 byte for each distinct string, the segments are column format minor 24, which only 3.5 and later read (default empty: automatic; off: none)",
						 "", sift_col_validate_count_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "point_tid_columns",
						 "the plain integer, text and uuid columns whose point directory lists the heap addresses of the rows, not their numbers in the segment (an equality or a short IN list of the access method then returns the rows' addresses from the directory without reading a unit, and the heap checks them; a unit with a dead row is read instead); the segments are column format minor 16, which only 3.3 and later read, and the lists take 1 to 5 bytes per row (default none)",
						 "", sift_col_validate_point_tid_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "return_columns",
						 "the text, varchar and uuid columns an index-only scan returns from the index (their strings are read from the dictionary of the segment; default none: a query that needs one reads the heap)",
						 "", sift_col_validate_return_columns, ShareUpdateExclusiveLock);
	add_int_reloption(col_relopt_kind, "superunit",
					  "Units per superunit summary: the sums and the cube of every group of this many consecutive units of a segment are stored once more, merged, and answer a query that no predicate cuts through from one read; the segments written from now on are column format minor 12 (0: none, else 2 to 256; default 16)",
					  SIFT_SUPER_DEFAULT_FAN, 0, SIFT_SUPER_MAX_FAN, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "rollup",
						 "the leaves of array elements and map values that the rollup of every unit groups by: 'leaf, leaf, ...', or off",
						 "", sift_col_validate_rollup, ShareUpdateExclusiveLock);
}

static const relopt_parse_elt col_relopt_tab[] = {
	{"write_buffer_limit", RELOPT_TYPE_INT, offsetof(SiftColOptions, write_buffer_limit)},
	{"dead_rewrite_pct", RELOPT_TYPE_INT, offsetof(SiftColOptions, dead_rewrite_pct)},
	{"vacuum_truncate", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, vacuum_truncate)},
	{"paths", RELOPT_TYPE_STRING, offsetof(SiftColOptions, paths)},
	{"compression", RELOPT_TYPE_STRING, offsetof(SiftColOptions, compression)},
	{"preagg", RELOPT_TYPE_STRING, offsetof(SiftColOptions, preagg)},
	{"rollup", RELOPT_TYPE_STRING, offsetof(SiftColOptions, rollup)},
	{"rollup_dims", RELOPT_TYPE_STRING, offsetof(SiftColOptions, rollup_dims)},
	{"calendar_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, calendar_columns)},
	{"rollup_on_demand", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, rollup_on_demand)},
	{"cluster_advice", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, cluster_advice)},
	{"cube_minmax", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, cube_minmax)},
	{"key_table", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, key_table)},
	{"array_lengths", RELOPT_TYPE_ENUM, offsetof(SiftColOptions, array_lengths)},
	{"dict_lookup", RELOPT_TYPE_ENUM, offsetof(SiftColOptions, dict_lookup)},
	{"return_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, return_columns)},
	{"superunit", RELOPT_TYPE_INT, offsetof(SiftColOptions, superunit)},
	{"order_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, order_columns)},
	{"order_array_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, order_array_columns)},
	{"order_array_group", RELOPT_TYPE_STRING, offsetof(SiftColOptions, order_array_group)},
	{"hll_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, hll_columns)},
	{"key_cut", RELOPT_TYPE_STRING, offsetof(SiftColOptions, key_cut)},
	{"key_cut_rows", RELOPT_TYPE_INT, offsetof(SiftColOptions, key_cut_rows)},
	{"key_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, key_columns)},
	{"point_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, point_columns)},
	{"count_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, count_columns)},
	{"point_tid_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, point_tid_columns)},
};

static bytea *
col_amoptions(Datum reloptions, bool validate)
{
	SiftColOptions *o = build_reloptions(reloptions, validate, col_relopt_kind, sizeof(SiftColOptions), col_relopt_tab, lengthof(col_relopt_tab));

	if (validate && o && o->superunit == 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("superunit must be 0 or between %d and %d", SIFT_SUPER_MIN_FAN, SIFT_SUPER_MAX_FAN)));
	return (bytea *) o;
}

#ifdef SIFT_TESTING
/* The options of an index with none set and the table that parses them, for sift._test_reloptions() */
extern const void *sift_col_test_reloptions(const relopt_parse_elt **tab, int *n);

const void *
sift_col_test_reloptions(const relopt_parse_elt **tab, int *n)
{
	*tab = col_relopt_tab;
	*n = lengthof(col_relopt_tab);
	return col_amoptions((Datum) 0, false);
}
#endif

const SiftColOptions *
sift_col_options(Relation index)
{
	static const SiftColOptions defaults = {.write_buffer_limit = SIFT_DEFAULT_WRITE_BUFFER_LIMIT, .dead_rewrite_pct = SIFT_COL_DEAD_PCT, .rollup_on_demand = true, .vacuum_truncate = true, .cluster_advice = true, .dict_lookup = SIFT_COL_DICT_AUTO, .cube_minmax = true, .key_table = true, .superunit = SIFT_SUPER_DEFAULT_FAN};

	return index->rd_options ? (const SiftColOptions *) index->rd_options : &defaults;
}

/* a string reloption: stored as an offset from the options struct, 0 when unset */
#define COL_OPT_STR(fn, field, unset) \
const char * \
fn(Relation index) \
{ \
	const SiftColOptions *o = sift_col_options(index); \
\
	return o->field ? (const char *) o + o->field : (unset); \
}

COL_OPT_STR(sift_col_options_paths_raw, paths, "")

/* 'auto' (decided at the build) or 'auto: spec' (decided by an earlier build); a spec starts with a key and a type, so it never reads like this */
/* the option is empty or a bare 'auto' on an index with a jsonb column: its paths are read from the documents at the next build */
bool
sift_col_paths_redecide(Relation index)
{
	const char *opt = sift_col_options_paths_raw(index);

	if (opt[0] != '\0' && !(sift_col_paths_auto(opt) && opt[4] == '\0'))
		return false;
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
		if (TupleDescAttr(RelationGetDescr(index), i)->atttypid == JSONBOID)
			return true;
	return false;
}

bool
sift_col_paths_auto(const char *opt)
{
	return strncmp(opt, "auto", 4) == 0 && (opt[4] == '\0' || opt[4] == ':');
}

/* The spec the shredder parses: the option, or what follows 'auto:' ("" while 'auto' has not been decided) */
const char *
sift_col_options_paths(Relation index)
{
	const char *opt = sift_col_options_paths_raw(index);

	if (!sift_col_paths_auto(opt))
		return opt;
	return opt[4] == ':' ? opt + 5 + strspn(opt + 5, " ") : "";
}

COL_OPT_STR(sift_col_options_compression, compression, "")

COL_OPT_STR(sift_col_options_preagg, preagg, "")

/*
 * An ALTER INDEX is checked by building the state the new options give. paths = 'auto' without a spec (or RESET (paths)) asks for the documents to be read
 * again at the next REINDEX (col/keycols.c), and has no schema until then, as any change of paths leaves the segments on the old one: it is let through.
 */
void
sift_col_alter_check(Relation index)
{
	if (!sift_col_is_index(index))
		return;
	if (sift_col_paths_redecide(index))
		return;
	(void) sift_col_rel(index);
}

COL_OPT_STR(sift_col_options_key_columns, key_columns, NULL)

COL_OPT_STR(sift_col_options_point_columns, point_columns, "")

COL_OPT_STR(sift_col_options_count_columns, count_columns, "")

COL_OPT_STR(sift_col_options_point_tid_columns, point_tid_columns, "")

/* the leaves the option names: 'auto: a, b' is the choice of a build, 'auto' and 'off' name none */
const char *
sift_col_options_key_cut(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);
	const char *v = o->key_cut ? (const char *) o + o->key_cut : "";

	if (strncmp(v, "auto:", 5) == 0)
		return v + 5;
	return strcmp(v, "auto") == 0 || strcmp(v, "off") == 0 ? "" : v;
}

/* whether a build decides key_cut: the option is not set, or holds a choice of an earlier build */
bool
sift_col_options_key_cut_auto(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->key_cut == 0 || strncmp((const char *) o + o->key_cut, "auto", 4) == 0;
}

COL_OPT_STR(sift_col_options_order_columns, order_columns, "")

COL_OPT_STR(sift_col_options_order_array_group, order_array_group, "")

COL_OPT_STR(sift_col_options_order_array_columns, order_array_columns, NULL)

COL_OPT_STR(sift_col_options_hll_columns, hll_columns, "")

COL_OPT_STR(sift_col_options_return_columns, return_columns, "")

COL_OPT_STR(sift_col_options_rollup, rollup, "")

COL_OPT_STR(sift_col_options_rollup_dims, rollup_dims, "")

COL_OPT_STR(sift_col_options_calendar_columns, calendar_columns, "")

/* The cached state of the index, in mcxt (the relcache entry's own in rd_indexcxt, a copy of a caller's in its context) */
static SiftColRel *
build_rel(Relation index, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColRel *r = palloc0(sizeof(SiftColRel));
	TupleDesc	desc = RelationGetDescr(index);
	int			natts = IndexRelationGetNumberOfKeyAttributes(index);
	SiftColDef *defs = palloc0(SIFT_COL_MAX_COLS * sizeof(SiftColDef));
	char	  **names;
	int			n = 0;

	if (index->rd_index->indnatts != natts)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes do not support INCLUDE columns"),
						errhint("List the columns in the index key: a sift_col index stores all of them.")));
	r->natts = natts;
	r->first = palloc((natts + 1) * sizeof(int));
	r->nleaves = palloc((natts + 1) * sizeof(int));
	r->json = palloc0((natts + 1) * sizeof(bool));
	r->name = palloc0((natts + 1) * sizeof(bool));
	r->shred = sift_col_shred_create(mcxt, index);
	for (int a = 1; a <= natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);
		bool		isjson = att->atttypid == JSONBOID;
		int			kshred = r->shred ? sift_col_shred_nleaves(r->shred, a) : 0,
					k0 = 0,
					k;
		SiftColDef	lanes[SIFT_COL_MAX_LANES];

		if (!isjson)
		{
			k0 = sift_col_att_leaves(att, lanes, sift_col_options(index)->array_lengths != SIFT_COL_ARRLEN_ON ? 0 :
									 kshred > 0 && !sift_col_shred_keeps_image(r->shred, a) ? SIFT_COL_ARR_NOIMAGE : SIFT_COL_ARR_LANES);
			if (kshred > 0)
				sift_col_shred_set_base(r->shred, a, k0);
		}
		k = k0 + kshred;
		if (isjson && k == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("the jsonb column \"%s\" of a sift_col index needs paths", NameStr(att->attname)),
					 errhint("Declare them with the paths option: WITH (paths = '%s.* float4'), or REINDEX to read them from the documents.", NameStr(att->attname))));
		if (n + k > sift_limit(col_leaves, SIFT_COL_MAX_COLS))
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("a sift_col index holds at most %d columns", (int) sift_limit(col_leaves, SIFT_COL_MAX_COLS)),
							errdetail("Column \"%s\" brings the index to %d columns and paths.", NameStr(att->attname), n + k),
							errhint("Leave out the columns the queries do not read, or create several indexes: the planner picks among them.")));
		r->first[a] = n;
		r->json[a] = isjson;
		r->name[a] = att->atttypid == NAMEOID;
		r->nleaves[a] = k;
		for (int i = 0; i < k; i++)
		{
			SiftColDef *d = &defs[n + i];

			if (i >= k0)
				sift_col_shred_leaf(r->shred, a, i - k0, d);
			else
				*d = lanes[i];
			d->id = n + i;
			d->attno = a;
			d->parent = d->parent < 0 ? -1 : n + d->parent;
		}
		n += k;
	}
	r->ret = palloc0((natts + 1) * sizeof(bool));
	{
		const char *err = return_walk(sift_col_options_return_columns(index), r, desc);

		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid return_columns option: %s", err)));
		for (int a = 1; a <= natts; a++)
			if (r->ret[a] && index->rd_index->indkey.values[a - 1] == 0)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid return_columns option: \"%s\" is an expression, not a column", NameStr(TupleDescAttr(desc, a - 1)->attname))));
	}
	r->schema = sift_cschema_create(mcxt, defs, n);
	r->policy = palloc(n * sizeof(SiftCcPolicy));
	names = palloc(n * sizeof(char *));
	for (int i = 0; i < n; i++)
	{
		const char *name = defs[i].path[0] ? defs[i].path : NameStr(TupleDescAttr(desc, defs[i].attno - 1)->attname);
		const char *err = sift_cc_policy_for_column(sift_col_options_compression(index), name, &r->policy[i]);

		names[i] = pstrdup(name);
		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid compression option: %s", err)));
	}
	{
		const char *err = sift_cpagg_spec_parse(sift_col_options_preagg(index), r->schema, (const char *const *) names, &r->pagg);

		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid preagg option: %s", err)));
		if ((err = sift_cpagg_rollup_apply(sift_col_options_rollup(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid rollup option: %s", err)));
		if ((err = sift_cpagg_rows_apply(sift_col_options_rollup_dims(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid rollup_dims option: %s", err)));
		if ((err = sift_cpagg_calendar_apply(sift_col_options_calendar_columns(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid calendar_columns option: %s", err)));
		r->pagg.minmax = sift_col_options(index)->cube_minmax;
		r->pagg.key_table = sift_col_options(index)->key_table;
		if ((err = sift_cset_keys_parse(sift_col_options_key_columns(index), r->schema, (const char *const *) names, &r->pagg)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid key_columns option: %s", err)));
		r->pagg.super = sift_col_options(index)->superunit;
		if ((err = sift_crun_spec_parse(sift_col_options_order_columns(index), r->schema, (const char *const *) names, r->pagg.run)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_columns option: %s", err)));
		if ((err = sift_cpagg_leaf_list(sift_cpagg_auto_list(sift_col_options_order_array_columns(index), &r->pagg.gorderauto), r->schema, (const char *const *) names, r->pagg.gorder, sift_cgord_leaf_ok,
										"not an integer, date or timestamp column")) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_columns option: %s", err)));
		const char *glist = sift_cpagg_auto_list(sift_col_options_order_array_group(index), &r->pagg.gorder_group_auto);
		if (glist[0] != '\0' && strcmp(glist, "off") != 0)
		{
			bool		grp[SIFT_COL_MAX_COLS] = {false};
			int			ng = 0;

			if ((err = sift_cpagg_leaf_list(glist, r->schema, (const char *const *) names, grp, sift_cgord_leaf_ok,
											"not an integer, date or timestamp column")) != NULL)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_group option: %s", err)));
			for (int i = 0; i < n; i++)
				if (grp[i])
				{
					if (r->pagg.gorder[i])
						ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_group option: column \"%s\" is in order_array_columns", names[i])));
					r->pagg.gorder_group = i + 1;
					ng++;
				}
			if (ng != 1)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_group option: one column, not %d", ng)));
		}
		for (int i = 0; i < n; i++)
			if (r->pagg.gorder[i] && !r->pagg.run[i] && r->pagg.gorderauto)
				r->pagg.run[i] = true;
			else if (r->pagg.gorder[i] && !r->pagg.run[i])
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid order_array_columns option: column \"%s\" is not in order_columns", names[i]),
								errhint("The global order extends the sorted run of a column: add it to order_columns.")));
		if ((err = sift_hll_spec_parse(sift_col_options_hll_columns(index), r->schema, (const char *const *) names, r->pagg.hll)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid hll_columns option: %s", err)));
		for (int i = 0; i < n; i++)
			if (r->pagg.hll[i])
			{
				Oid			t = TupleDescAttr(desc, defs[i].attno - 1)->atttypid;

				if (t != INT2OID && t != INT4OID && t != INT8OID && t != DATEOID && t != TIMESTAMPOID && t != TIMESTAMPTZOID && t != TEXTOID && t != VARCHAROID && t != UUIDOID)
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid hll_columns option: \"%s\" is a %s column, whose hash a unit cannot compute (char(n) and name hash differently from text)", names[i], format_type_be(t))));
				if ((t == TEXTOID || t == VARCHAROID) && !get_collation_isdeterministic(TupleDescAttr(desc, defs[i].attno - 1)->attcollation))
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid hll_columns option: \"%s\" has a nondeterministic collation, whose hash is not that of its bytes", names[i])));
				/* a column the build chose for its key list (key_columns 'auto') gives it up: a leaf has one blob a unit in the set section, and the user asked for the sketch */
				if (r->pagg.set[i] && r->pagg.keyauto)
					r->pagg.set[i] = r->pagg.pres[i] = false;
				else if (r->pagg.set[i])
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid hll_columns option: \"%s\" is in key_columns, which keeps its distinct keys exactly", names[i])));
			}
		if ((err = sift_cpoint_spec_parse(sift_col_options_point_columns(index), r->schema, (const char *const *) names, r->pagg.point)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid point_columns option: %s", err)));
		if ((err = sift_cpoint_spec_parse(sift_col_options_point_tid_columns(index), r->schema, (const char *const *) names, r->pagg.point_tid)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid point_tid_columns option: %s", err)));
		if ((err = sift_cpoint_spec_parse(sift_col_options_count_columns(index), r->schema, (const char *const *) names, r->pagg.count)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid count_columns option: %s", err)));
		for (int i = 0; i < n; i++)
			if (r->pagg.count[i] && r->schema->cols[i].store != SIFT_COLS_STR)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid count_columns option: \"%s\" is not a text column", names[i])));
		for (int i = 0; i < n; i++)
			r->pagg.lookup[i] = sift_col_options(index)->dict_lookup == SIFT_COL_DICT_ON || r->pagg.count[i] ||
				(sift_col_options(index)->dict_lookup == SIFT_COL_DICT_AUTO && (r->pagg.set[i] || r->pagg.point[i] || r->pagg.point_tid[i]));
		for (int a = 1; a <= natts; a++)
			if (r->ret[a] && sift_col_options(index)->dict_lookup == SIFT_COL_DICT_AUTO)
				r->pagg.lookup[r->first[a]] = true;
		/* a text leaf whose dictionary is read in place (and would have its order, whatever it holds) also counts the rows of each string, unless the option says off */
		if (strcmp(sift_col_options_count_columns(index), "off") != 0)
			for (int i = 0; i < n; i++)
				if (r->schema->cols[i].store == SIFT_COLS_STR && r->schema->cols[i].type == SIFT_COLT_TEXT && r->pagg.lookup[i])
					r->pagg.count[i] = true;
		{
			char	   *list = pstrdup(sift_col_options_key_cut(index)),
					   *save = NULL;

			for (char *tok = strtok_r(list, ", ", &save); tok != NULL; tok = strtok_r(NULL, ", ", &save))
			{
				int			id = -1;

				for (int i = 0; i < n && id < 0; i++)
					if (strcmp(names[i], tok) == 0)
						id = i;
				if (id < 0 || r->ncut == SIFT_COL_CUT_MAX || !(sift_crun_leaf_ok(r->schema, &r->schema->cols[id]) || sift_col_def_shape_start(r->schema->cols, id)))
					ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid key_cut option: \"%s\" is not an integer, date, timestamp, numeric or float column of the index (at most %d)", tok, SIFT_COL_CUT_MAX)));
				r->cut[r->ncut++] = id;
			}
			r->cut_rows = sift_col_options(index)->key_cut_rows;
		}
	}
	r->meta = (SiftColIndexMeta) {.indexoid = RelationGetRelid(index), .heapoid = index->rd_index->indrelid, .natts = natts,
	.ncols = n,.cols = r->schema->cols};
	r->rowcxt = AllocSetContextCreate(mcxt, "sift cache col insert", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(old);
	return r;
}

/* Are the units of the sift_col index cut by the order key? (the planner keeps a ctid range, which selects units by heap superblock, on the normal plan for such an index) */
bool
sift_col_index_keyed(Oid indexoid)
{
	Relation	index = index_open(indexoid, NoLock);
	bool		keyed = sift_col_is_index(index) && sift_col_rel(index)->ncut > 0;

	index_close(index, NoLock);
	return keyed;
}

SiftColRel *
sift_col_rel(Relation index)
{
	if (index->rd_amcache == NULL)
		index->rd_amcache = build_rel(index, index->rd_indexcxt);
	return index->rd_amcache;
}

SiftColRel *
sift_col_rel_private(Relation index, MemoryContext mcxt)
{
	return build_rel(index, mcxt);
}

#ifdef SIFT_TESTING
void
sift_col_test_inval(Relation index)
{
	if (!sift_test_col_rel_inval)
		return;
	/* the reload of the entry that a lock taken during the scan may cause frees the cached state: first overwrite what it holds */
	memset(sift_col_rel(index), 0x7f, sizeof(SiftColRel));
	RelationCacheInvalidateEntry(RelationGetRelid(index));
}
#endif

const SiftColIndexMeta *
sift_col_index_defs(Relation index)
{
	/* check-memory: allow hands out &r->meta at once; the callers copy it before any lock (sift_col_index_meta, col_layout) */
	SiftColRel *r = sift_col_rel(index);

	r->meta.valid = index->rd_index->indisvalid && index->rd_index->indisready && index->rd_index->indislive;
	r->meta.partial = !heap_attisnull(index->rd_indextuple, Anum_pg_index_indpred, NULL);
	return &r->meta;
}

Node *
sift_col_att_expr(Relation index, int attno)
{
	AttrNumber	heapatt = index->rd_index->indkey.values[attno - 1];

#ifdef SIFT_TESTING
	sift_col_test_inval(index);	/* reading the index expressions can take a lock that reloads the entry (and frees its cached state) */
#endif

	if (heapatt != 0)
	{
		Oid			typid = TupleDescAttr(RelationGetDescr(index), attno - 1)->atttypid;
		int32		typmod = TupleDescAttr(RelationGetDescr(index), attno - 1)->atttypmod;

		return (Node *) makeVar(1, heapatt, typid, typmod, TupleDescAttr(RelationGetDescr(index), attno - 1)->attcollation, 0);
	}
	{
		List	   *exprs = RelationGetIndexExpressions(index);
		int			k = 0;

		for (int i = 0; i < attno - 1; i++)
			if (index->rd_index->indkey.values[i] == 0)
				k++;
		return (Node *) copyObject(list_nth(exprs, k));
	}
}

/*
 * The leaf statistics of an index's immutable segments, memoized per backend: the key of an entry is the index and the generations of
 * its immutable segments, so a build, merge or promotion makes the next call read the zones again and nothing else does. A plan reads
 * every partition of a table (4096 at most for the pushdown), so the memo holds as many indexes, and is dropped whole when it grows past them.
 */
#define LEAF_STATS_CACHE	8192

/*
 * The planner needs the share of the units that hold each pre-aggregate section, not the count: reading the sections of every unit costs
 * a few buffers each (2.5 for an index of seven leaves and a cube), on the first plan of every backend. The units of an index are counted
 * up to PRE_CENSUS_UNITS together, shared out over its segments in proportion to their units and at least PRE_CENSUS_MIN of each, spread
 * evenly; an index of fewer units is counted whole.
 */
#define PRE_CENSUS_UNITS	256
#define PRE_CENSUS_MIN		2

typedef struct LeafEntry
{
	Oid			index;			/* hash key */
	uint64		stamp;
	int			ncols;
	SiftColLeafStats *st;
	SiftColZones *zones;
	SiftColPreStats *pre;
}			LeafEntry;

static HTAB *leaf_cache;

uint32
sift_col_pagg_hash(Relation index)
{
	const char *a = sift_col_options_preagg(index),
			   *b = sift_col_options_rollup(index),
			   *c = sift_col_options_rollup_dims(index),
			   *d = sift_col_options_calendar_columns(index);

	return hash_bytes((const unsigned char *) a, strlen(a)) ^ (hash_bytes((const unsigned char *) b, strlen(b)) * 31) ^
		(hash_bytes((const unsigned char *) c, strlen(c)) * 17) ^ (hash_bytes((const unsigned char *) d, strlen(d)) * 13) ^
		(sift_col_options(index)->cube_minmax ? 0x9e3779b9 : 0) ^ (sift_col_options(index)->key_table ? 0x7f4a7c15 : 0) ^ ((uint32) sift_col_format_minor * 0x85ebca6b);
}

static void
zones_free(SiftColZones *zs)
{
	for (int c = 0; c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
			pfree(zs->lo[c]), pfree(zs->hi[c]);
	pfree(zs->lo);
	pfree(zs);
}

/* the sample of the integer scalar leaves, in the memory that stays (TopMemoryContext) */
static SiftColZones *
zones_new(const SiftColIndexMeta *d)
{
	SiftColZones *zs = MemoryContextAllocZero(TopMemoryContext, sizeof(SiftColZones));

	zs->ncols = d->ncols;
	zs->stride = 1;
	zs->lo = MemoryContextAllocZero(TopMemoryContext, 2 * d->ncols * sizeof(int64 *));
	zs->hi = zs->lo + d->ncols;
	for (int c = 0; c < d->ncols; c++)
		if (d->cols[c].store == SIFT_COLS_INT && d->cols[c].kind != SIFT_COLK_GROUP)
		{
			zs->lo[c] = MemoryContextAlloc(TopMemoryContext, SIFT_COL_ZONE_SAMPLE * sizeof(int64));
			zs->hi[c] = MemoryContextAlloc(TopMemoryContext, SIFT_COL_ZONE_SAMPLE * sizeof(int64));
		}
	return zs;
}

/* a sample is filled once: the arrays keep the units sampled, not the room for all of them (an index of a few units is 16 bytes a leaf) */
static void
zones_shrink(SiftColZones *zs)
{
	for (int c = 0; c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
		{
			zs->lo[c] = repalloc(zs->lo[c], Max(zs->n, 1) * sizeof(int64));
			zs->hi[c] = repalloc(zs->hi[c], Max(zs->n, 1) * sizeof(int64));
		}
}

static LeafEntry *
leaf_cache_entry(Relation index, const SiftDirSnapshot *snap)
{
	const SiftColIndexMeta *d = sift_col_index_defs(index);
	SiftDirSnapshot own;
	const SiftDirSnapshot *dir = snap;
	uint64		stamp = index->rd_locator.relNumber;	/* REINDEX starts the generations over */
	Oid			key = RelationGetRelid(index);
	LeafEntry  *e;

	stamp = stamp * 31 + sift_col_pagg_hash(index);	/* the sections of the units are counted against the options */
	if (dir == NULL)
	{
		sift_dir_read(index, &own);
		dir = &own;
	}
	for (int i = 0; i < dir->nsegs; i++)
		stamp = stamp * 31 + dir->segs[i].gen * 8 + dir->segs[i].format + 1;	/* a rewrite of a segment changes its format, and the kinds it keeps */
	if (leaf_cache == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(LeafEntry), .hcxt = TopMemoryContext};

		leaf_cache = hash_create("sift_col leaf statistics", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(leaf_cache, &key, HASH_FIND, NULL);
	if (e != NULL && e->stamp == stamp && e->ncols == d->ncols)
		return e;
	if (e != NULL)
	{
		pfree(e->st);
		pfree(e->pre);
		zones_free(e->zones);
		hash_search(leaf_cache, &key, HASH_REMOVE, NULL);
	}
	else if (hash_get_num_entries(leaf_cache) >= LEAF_STATS_CACHE)
	{
		HASH_SEQ_STATUS seq;

		hash_seq_init(&seq, leaf_cache);
		while ((e = hash_seq_search(&seq)) != NULL)
		{
			pfree(e->st);
			pfree(e->pre);
			zones_free(e->zones);
			hash_search(leaf_cache, &e->index, HASH_REMOVE, NULL);
		}
	}
	{
		MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col leaf statistics", ALLOCSET_DEFAULT_SIZES);
		SiftColLeafStats *st = MemoryContextAlloc(TopMemoryContext, d->ncols * sizeof(SiftColLeafStats));
		SiftColPreStats *pre = MemoryContextAllocZero(TopMemoryContext, sizeof(SiftColPreStats));
		const SiftCPaggSpec *spec = &sift_col_rel(index)->pagg;
		SiftColZones *zs = zones_new(d);
		uint64		nunits = sift_cseg_dir_units(dir),
					census_units = sift_limit(pre_census_units, PRE_CENSUS_UNITS),
					shared_stamp;

		for (int c = 0; c < d->ncols; c++)
			sift_col_leaf_stats_init(&st[c]);
		/* the statistics of the segments are folded once for the server: the first backend that plans keeps them for the others (col/leafshm.c) */
		shared_stamp = stamp * 31 + census_units;
		if (sift_col_leafshm_read(index->rd_locator.relNumber, shared_stamp, d->ncols, st, pre, zs))
			sift_stat_add(SIFT_STAT_LEAF_SHARED, 1);
		else
		{
			for (int i = 0; i < dir->nsegs; i++)
			{
				SiftColSegment *seg;
				SiftCPaggCensus census;
				double		w;

				if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
					continue;
				seg = sift_cseg_open(index, &dir->segs[i], cx);
				for (int c = 0; c < d->ncols; c++)
					sift_cseg_leaf_stats(seg, c, &st[c]);
				sift_cseg_zone_sample(seg, zs);
				sift_cseg_pre_census(seg, spec, sift_col_format_minor, Max(PRE_CENSUS_MIN, (uint64) census_units * seg->nsb / Max(nunits, 1)), &census);
				/* the units counted stand for the units of the segment */
				w = census.units > 0 ? (double) seg->nsb / census.units : 0.0;
				pre->units += census.units * w;
				for (int k = 0; k < spec->ndims; k++)
				{
					pre->dim[spec->dim[k].leaf] += census.dim[k] * w;
					pre->mm[spec->dim[k].leaf] += census.mm[k] * w;
				}
				for (int k = 0; k < spec->nrollups; k++)
					if (spec->rollup[k].list >= 0)
						pre->roll[spec->rollup[k].list] += census.roll[k] * w;
					else
						pre->rows += census.roll[k] * w;
				sift_cseg_close(seg);
			}
			for (int i = 0; i < dir->nsegs; i++)
				if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE && dir->segs[i].format < SIFT_COL_FORMAT_KINDS)
					for (int c = 0; c < d->ncols; c++)
						st[c].nokinds |= sift_col_def_has_kinds(&d->cols[c]);
			sift_stat_add(SIFT_STAT_LEAF_BUILT, 1);
			sift_col_leafshm_write(index->rd_locator.relNumber, shared_stamp, d->ncols, st, pre, zs);
		}
		MemoryContextDelete(cx);
		zones_shrink(zs);
		e = hash_search(leaf_cache, &key, HASH_ENTER, NULL);
		e->stamp = stamp;
		e->ncols = d->ncols;
		e->st = st;
		e->zones = zs;
		e->pre = pre;
		return e;
	}
}

const SiftColPreStats *
sift_col_pre_stats(Relation index)
{
	return leaf_cache_entry(index, NULL)->pre;
}

const SiftColLeafStats *
sift_col_leaf_stats(Relation index)
{
	return leaf_cache_entry(index, NULL)->st;
}

const SiftColZones *
sift_col_zones(Relation index)
{
	return leaf_cache_entry(index, NULL)->zones;
}

SiftColIndexMeta *
sift_col_index_meta(Relation index, SiftColLeafMemo *memo)
{
	const SiftColIndexMeta *d = sift_col_index_defs(index);
	SiftColIndexMeta *m = palloc(sizeof(SiftColIndexMeta));
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	Relation	heap;
	BlockNumber relpages;

	*m = *d;
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];

		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			m->stats.nrows += e->ndocs;
			m->stats.bytes += e->bytes;
			m->stats.nsegments++;
		}
		else
			m->stats.nwbuf++;
	}
	m->stats.nunits = sift_cseg_dir_units(snap);
	m->stats.wbuf_ovf = sift_cwbuf_overflow(snap);
	m->stats.wbuf_jnull = sift_cwbuf_jnull(snap);
	heap = table_open(d->heapoid, AccessShareLock);
	relpages = RelationGetNumberOfBlocks(heap);
	m->stats.heap_pages = relpages;
	m->stats.allvisible_frac = relpages > 0 ? (double) sift_vis_count_allvisible(heap, relpages) / relpages : 0;
	table_close(heap, AccessShareLock);
	if (memo != NULL)
	{
		LeafEntry  *e = leaf_cache_entry(index, snap);

		memo->leaves = e->st;
		memo->zones = e->zones;
		memo->pre = e->pre;
	}
	pfree(snap);
	return m;
}

bool
sift_col_is_index(Relation index)
{
	return index->rd_rel->relam == sift_col_am_oid();
}

Oid
sift_col_am_oid(void)
{
	static Oid	am = InvalidOid;

	if (!OidIsValid(am))
		am = get_index_am_oid("sift_col", true);
	return am;
}

void
sift_col_make_row_rc(SiftColRel *r, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row, MemoryContext mcxt,
					 struct SiftShredRecheck *rc)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	/* detoasting a value locks the toast table and can reload the relcache entry, which frees r when it is the cached state: what the loop reads is taken first
	 * (the schema and the arrays are apart in rd_indexcxt and stay) */
	const SiftColSchema *schema = r->schema;
	const int  *first = r->first,
			   *nleaves = r->nleaves;
	const bool *json = r->json,
			   *name = r->name;
	SiftColShredAdapter *shred = r->shred;
	int			natts = r->natts,
				ncols = schema->ncols;

	row->tid = *tid;
	row->ncols = ncols;
	row->val = palloc0(ncols * sizeof(SiftColValue));
	row->elem = palloc0(ncols * sizeof(SiftColValue *));
	for (int a = 1; a <= natts; a++)
	{
		const SiftColDef *def = &schema->cols[first[a]];

		if (json[a])
			continue;
		if (isnull[a - 1])
			for (int k = 0; k < nleaves[a]; k++)
				row->val[def->id + k].isnull = true;
		else
			sift_col_datum_values(def, values[a - 1], name[a], &row->val[def->id]);
	}
	if (shred)
		sift_col_shred_row_rc(shred, values, isnull, row, first, mcxt, rc);
	MemoryContextSwitchTo(old);
}

void
sift_col_make_row(SiftColRel *r, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row, MemoryContext mcxt)
{
	sift_col_make_row_rc(r, tid, values, isnull, row, mcxt, NULL);
}

/*
 * The rows of a transaction after its first go to the write buffer in runs (wbuf.c, sift_cwbuf_pend). They are written before the commit record and before
 * a savepoint starts, dropped when the transaction or the savepoint aborts: the heap rows of an aborted savepoint are dead at once and VACUUM may free their
 * slots while the transaction goes on, so an index entry written for them later would point at whatever takes the slot. An ON CONFLICT statement kills
 * the heap row of a conflict after the index entries went in, with no abort to drop them: its rows go in at once (batch_stmts).
 */
static bool batch_hooked;
static bool batch_seen;
static int	batch_stmts;

static void
batch_flush_all(void)
{
	Oid			oid;

	while (OidIsValid(oid = sift_cwbuf_pending()))
	{
		Relation	index = try_relation_open(oid, RowExclusiveLock);

		/* dropped in this transaction */
		if (index == NULL)
			sift_cwbuf_discard(oid);
		else
		{
			if (sift_cwbuf_flush(index))
				sift_col_maint_after_seal(index);
			relation_close(index, NoLock);
		}
	}
}

static void
batch_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE)
		batch_flush_all();
	else if (event == XACT_EVENT_ABORT)
		sift_cwbuf_discard(InvalidOid);
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE || event == XACT_EVENT_ABORT)
		batch_seen = false;
}

static void
batch_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_START_SUB)
		batch_flush_all();
	else if (event == SUBXACT_EVENT_ABORT_SUB)
		sift_cwbuf_discard(InvalidOid);
}

static void
batch_stmt_done(void *arg)
{
	batch_stmts--;
}

void
sift_col_stmt_start(QueryDesc *queryDesc)
{
	PlannedStmt *ps = queryDesc->plannedstmt;
	bool		speculative = IsA(ps->planTree, ModifyTable) && ((ModifyTable *) ps->planTree)->onConflictAction != ONCONFLICT_NONE;

	foreach_ptr(Plan, sub, ps->subplans)
		speculative |= sub != NULL && IsA(sub, ModifyTable) && ((ModifyTable *) sub)->onConflictAction != ONCONFLICT_NONE;
	if (speculative)
	{
		MemoryContextCallback *cb = MemoryContextAlloc(queryDesc->estate->es_query_cxt, sizeof(MemoryContextCallback));

		cb->func = batch_stmt_done;
		cb->arg = NULL;
		MemoryContextRegisterResetCallback(queryDesc->estate->es_query_cxt, cb);
		batch_stmts++;
	}
}

bool
sift_col_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
				  IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	/* check-memory: allow copies the hint and the row context, and fetches the entry again to store the hint  */
	SiftColRel *r = sift_col_rel(index);
	SiftCWbufHint hint = r->hint;
	MemoryContext rowcxt = r->rowcxt;
	SiftColRow	row;
	bool		crossed;

	sift_col_make_row(r, heap_tid, values, isnull, &row, rowcxt);
	if (!batch_hooked)
	{
		RegisterXactCallback(batch_xact, NULL);
		RegisterSubXactCallback(batch_subxact, NULL);
		batch_hooked = true;
	}
	/* the first row of a transaction goes in at once: a statement of one row pays nothing for the buffer */
	if (!(batch_seen && batch_stmts == 0 && sift_cwbuf_pend(index, &hint, r->schema, &row, &crossed)))
		crossed = sift_cwbuf_append(index, &hint, r->schema, &row);
	batch_seen = true;
	MemoryContextReset(rowcxt);
	/* the append reads the catalog, which can reload the relcache entry and free r: keep the hint in the entry as it is now */
	sift_col_rel(index)->hint = hint;
	if (crossed)
		sift_col_maint_after_seal(index);
	sift_col_vacuum_tick(RelationGetRelid(index), RelationGetRelid(heap), 0);
	return false;
}

static bool
col_amvalidate(Oid opclassoid)
{
	return true;
}

Datum
sift_col_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = sift_am_routine(25, 1, true);

	am->ambuild = sift_col_ambuild;
	am->ambuildempty = sift_col_ambuildempty;
	am->aminsert = sift_col_aminsert;
	am->ambulkdelete = sift_col_ambulkdelete;
	am->amvacuumcleanup = sift_col_amvacuumcleanup;
	am->amcostestimate = sift_col_amcostestimate;
	am->amoptions = col_amoptions;
	am->ambuildphasename = sift_col_ambuildphasename;
	am->amvalidate = col_amvalidate;
	am->ambeginscan = sift_col_ambeginscan;
	am->amrescan = sift_col_amrescan;
	am->amgetbitmap = sift_col_amgetbitmap;
	am->amgettuple = sift_col_amgettuple;
	am->amcanreturn = sift_col_amcanreturn;
	am->amendscan = sift_col_amendscan;
	am->amsearcharray = am->amsearchnulls = true;

	PG_RETURN_POINTER(am);
}
