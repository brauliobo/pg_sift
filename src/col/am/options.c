/*
 * col/am/options.c - the reloptions of a column index: the validators of each option,
 * the registration of the options, how an index reads them and the
 * checks of an ALTER of them
 */
#include "postgres.h"
#include "access/reloptions.h"
#include "utils/varlena.h"
#include "col/dictshm.h"
#include "col/leafshm.h"
#include "col/point.h"
#include "col/hll.h"
#include "col/gorder.h"
#include "col/setsum.h"
#include "col/seg.h"
#include "col/am/am_int.h"
#include "core/ops.h"

/*
 * col/am/am.c - the sift_col index access method: handler, options, the per-relation leaf list, tuple to row conversion and
 * aminsert. The build is in col/build/build.c, VACUUM and maintenance in col/maint/maint.c, the read side in col/scan/scan.c.
 */

bool		sift_enable_col = true;

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

/* 'a, b': the names of the text, varchar and uuid columns a tuple scan returns; r NULL: just the syntax. Empty or 'off': none */
const char *
am_return_walk(const char *opt, SiftColRel *r, TupleDesc desc)
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

/* An option whose value is a list with its own syntax checker: the error names the option and what is wrong */
#define SYNTAX_VALIDATOR(option, check) \
void \
sift_col_validate_##option(const char *value) \
{ \
	const char *err = check; \
\
	if (err) \
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid " #option " option \"%s\": %s", value, err))); \
}

/* 'auto' or 'auto: a, b' stands for the columns the build chooses: the rest is a list of columns */
static const char *
order_array_syntax(const char *value)
{
	bool		autolist;

	return sift_crun_syntax(sift_cpagg_auto_list(value, &autolist));
}

SYNTAX_VALIDATOR(preagg, sift_cpagg_syntax(value))
SYNTAX_VALIDATOR(rollup, sift_cpagg_rollup_syntax(value))
SYNTAX_VALIDATOR(rollup_dims, sift_cpagg_rows_syntax(value))
SYNTAX_VALIDATOR(calendar_columns, sift_cpagg_calendar_syntax(value))
SYNTAX_VALIDATOR(return_columns, am_return_walk(value, NULL, NULL))
SYNTAX_VALIDATOR(order_columns, sift_crun_syntax(value))
SYNTAX_VALIDATOR(order_array_group, order_array_syntax(value))
SYNTAX_VALIDATOR(order_layout, sift_cgord_layout_parse(value, NULL, NULL, NULL))
SYNTAX_VALIDATOR(order_array_columns, order_array_syntax(value))
SYNTAX_VALIDATOR(hll_columns, sift_hll_syntax(value))
SYNTAX_VALIDATOR(point_columns, sift_cpoint_syntax(value))
SYNTAX_VALIDATOR(count_columns, sift_cpoint_syntax(value))
SYNTAX_VALIDATOR(point_tid_columns, sift_cpoint_syntax(value))

/* one column name, or off or auto */
static const char *
row_key_syntax(const char *value)
{
	if (strpbrk(value, ", ;") != NULL)
		return "a single column: a composite key is not supported";
	return NULL;
}

SYNTAX_VALIDATOR(track_updates, row_key_syntax(value))

/* one column, or 'group, key': the names themselves are checked where the options they stand for are */
static const char *
key_order_syntax(const char *value)
{
	const char *comma;

	if (value[0] == '\0' || strcmp(value, "off") == 0)
		return NULL;
	if (strncmp(value, "auto:", 5) == 0)
		value += 5 + strspn(value + 5, " ");
	comma = strchr(value, ',');
	if (value[0] == ' ' || value[0] == ',')
		return "a column name, or 'group, key'";
	if (comma && (strchr(comma + 1, ',') != NULL || comma[1 + strspn(comma + 1, " ")] == '\0'))
		return "at most two columns, 'group, key'";
	return NULL;
}

SYNTAX_VALIDATOR(key_order, key_order_syntax(value))

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

/* the order of the strings of a text block: the collation of the column, as the b-tree of the column compares (varstr_cmp ends a tie by the bytes of a deterministic one) */
static int
gord_strcmp(const char *a, uint32 la, const char *b, uint32 lb, uint32 collid)
{
	return varstr_cmp(a, (int) la, b, (int) lb, (Oid) collid);
}

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
	sift_cgord_strcmp = gord_strcmp;
	sift_cseg_probe_init();
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
	DefineCustomIntVariable("sift.col_dict_shared_cache_kb",
							"Shared memory, for all backends together, that keeps the parsed dictionaries of sift_col segments (128 kB or more each), so that a new backend or a pooled connection reads a dictionary another backend parsed in place instead of parsing it (0: off, each backend parses and keeps its own, up to the dict_cache_kb of the index).",
							"The ring of records is allocated, at the size of this setting, by the first backend that keeps a dictionary after the server starts, and its size is recorded for the other backends: a later change turns the use of it on and off (0), never its size, which changes at the next start of the server. "
							"The memory is in addition to shared_buffers, is taken whole when the ring is made, and with dynamic_shared_memory_type = posix comes from /dev/shm (at most a quarter of it is used, with a message in the log) and counts in the shared memory of the container. "
							"A record is about 21 bytes for each distinct value, and 16 to 32 more with its hash table; a record over half of the ring is not kept.",
							&sift_col_dict_shared_cache_kb, 65536, 0, 4194304,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_leaf_cache_kb",
							"Shared memory, for all backends together, that keeps what the zones of the units of a sift_col index say to the planner (a record for each leaf and a sample of the unit zones), so that a new backend does not read them all (0: off).",
							"The table is allocated, at the size of this setting, by the first backend that plans a statement over a sift_col index after the server starts; a later change turns the use of it on and off (0), not its size. A record is about 33 kB for each integer leaf and 0.4 kB for the others; a record that does not fit is not kept, and the oldest goes when the table is full.",
							&sift_col_leaf_cache_kb, 16384, 0, 262144,
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
	DefineCustomIntVariable("sift.test_col_open_gate",
							"Testing: a column scan waits, right after it read the directory, for the advisory lock of this number (0 = no gate).",
							NULL, &sift_test_col_open_gate, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_gkept_fail",
							 "Testing: extending the kept order of the write buffer's rows fails after its new arrays are made and before the order takes them.",
							 NULL, &sift_test_col_gkept_fail, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_dict_shared",
							"Testing: the dictionaries shared between backends (col/dictshm.c): 0 = as built, 1 = every dictionary shared, 2 = the table cannot be had.",
							NULL, &sift_test_col_dict_shared, 0, 0, 2, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_verify_inval",
							 "Testing: the heap check of sift.col_verify invalidates the index's relcache entry (what another session's VACUUM or ANALYZE does) at every tuple.",
							 NULL, &sift_test_col_verify_inval, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_rel_inval",
							 "Testing: a sift_col build and the recheck of the rows a leaf cannot store invalidate the index's relcache entry (what a lock taken while they run may do) at every tuple.",
							 NULL, &sift_test_col_rel_inval, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_net",
							 "Testing: VACUUM writes the pre-aggregates net of their dead rows for the dirty units of a sift_col index (off: the units are read net of the dead rows by sift.agg_dead_delta).",
							 NULL, &sift_test_col_net, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_paranoid",
							 "Testing: a column scan checks the rows on all-visible pages against the heap too and logs the ones the snapshot does not see.",
							 NULL, &sift_test_col_paranoid, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_point_unpacked",
							 "Testing: the point directories are written as tables of slots (column format minor 33), not as packed tables (minor 35).",
							 NULL, &sift_test_col_point_unpacked, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_gorder_strings",
							 "Testing: the global order of a text leaf keeps a copy of its strings (as before column format minor 42), not the dictionary codes of them.",
							 NULL, &sift_test_col_gorder_strings, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_gorder_unpacked",
							 "Testing: the chunks of a global order code their addresses with 9 bits of offset (as before column format minor 38), not the bits of the largest offset of the chunk.",
							 NULL, &sift_test_col_gorder_unpacked, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_gorder_ends",
							"Testing: the entries a block of the ends only keeps at each end (0: 4096).",
							NULL, &sift_test_col_gorder_ends, 0, 0, SIFT_CGORD_ENDS, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_gorder_choice",
							"Testing: the global orders of the build's own choice of leaves keep the ends of the segment (1) or all of it (2), whatever the zones say (0: sift_cgord_pay decides).",
							NULL, &sift_test_col_gorder_choice, 0, 0, 2, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_gorder_noskip", "Testing: an OFFSET over a global order walks the entries it throws away.",
							 NULL, &sift_test_col_gorder_noskip, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_dict_unpacked",
							 "Testing: the order of a dictionary searched in place takes 32 bits a code (as before column format minor 39), not the bits of a code.",
							 NULL, &sift_test_col_dict_unpacked, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_point_unrooted",
							 "Testing: the header and records of the point directories precede the tables (as before column format minor 37), not follow them.",
							 NULL, &sift_test_col_point_unrooted, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_workers",
							"Testing: parallel workers the last sift_col build of this session launched.",
							NULL, &sift_test_col_workers, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_fail_sb",
							"Testing: the participant of a sift_col build that takes this superblock (1 = the first) fails (0 = none).",
							NULL, &sift_test_col_fail_sb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_parallel_always",
							 "Testing: a parallel sift_col build with key_cut launches its workers whatever its merge costs (off: only when the merge is made in slices).",
							 NULL, &sift_test_col_parallel_always, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_merge_par_kb",
							"Testing: kB of segments from which a merge outside a build is made by the workers of the backend.",
							NULL, &sift_test_col_merge_par_kb, 12288, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_merge_workers",
							"Testing: workers that merge the segments of a parallel sift_col build (-1 = those that built them, 0 = the leader alone).",
							NULL, &sift_test_col_merge_workers, -1, -1, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_merge_slices",
							"Testing: slices the participants of the last parallel sift_col build merged in (0 = one process merged).",
							NULL, &sift_test_col_merge_slices, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_gap_blocks",
							"Testing: blocks a parallel sift_col build reserves in front of its segments for the merge (-1 = by the statistics, 0 = none).",
							NULL, &sift_test_col_gap_blocks, -1, -1, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
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
					  "Percent of dead rows above which maintenance rewrites a lone segment to drop them (0 when the scans of its dirty units cost more than the rewrite, 100 never)",
					  SIFT_COL_DEAD_AUTO, 0, 100, ShareUpdateExclusiveLock);
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
	add_bool_reloption(col_relopt_kind, "track_deletes",
					   "Needs track_updates. Two internal statement triggers on the table (AFTER DELETE, AFTER UPDATE; transition table of the old rows) write a tombstone for the entries of deleted rows and of rows whose key changed, so that the log is complete (default off; the triggers are the object this option creates, the index drops them)",
					   false, ShareUpdateExclusiveLock);
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
	add_string_reloption(col_relopt_kind, "key_order",
						 "the integer, date, timestamp, numeric and float column, or 'group, key', whose order the rows are kept in: for one column it stands for key_cut, order_columns, order_array_columns and point_tid_columns on it, for 'group, key' for order_columns and order_array_columns on the key and order_array_group on the group (key_cut is left to the build), each while the option itself is not set; for a table that is stored in the order of the key (default none)",
						 "", sift_col_validate_key_order, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "order_layout",
						 "how the order arrays of order_array_columns are laid out: 'whole' keeps the key of every row, 'compact' keeps the heap address of every row and its key rounded to a bucket (the exact key is read from the table row where a scan needs it); 'auto' (default) keeps each chunk of 512 entries compact where that is at least a fifth smaller and whole elsewhere; a list gives a layout to columns: 'compact: ts, created; whole: id'. It never adds an array: order_array_columns says which columns have one",
						 "", sift_col_validate_order_layout, ShareUpdateExclusiveLock);
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
						 "the plain integer, text and uuid columns whose point directory lists the heap addresses of the rows, not their numbers in the segment (an equality or a short IN list of the access method then returns the rows' addresses from the directory without reading a unit, and the heap checks them; a unit with a dead row is read instead); the segments are column format minor 33 or more (37 from 3.7), which only 3.7 and later read, and the lists take 1 to 5 bytes per row (default none)",
						 "", sift_col_validate_point_tid_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "track_updates",
						 "the column that identifies a row (off, auto = the primary key, or a column with a unique index of its own, NOT NULL): when a row is updated the index finds the older entry of the same key and records that it stopped being live (a tombstone log; column format minor 43, which only 4.0 and later read); it adds the column to point_tid_columns (default auto: the primary key when the index holds it, else off with a NOTICE)",
						 "auto", sift_col_validate_track_updates, ShareUpdateExclusiveLock);
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
	{"track_deletes", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, track_deletes)},
	{"cluster_advice", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, cluster_advice)},
	{"cube_minmax", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, cube_minmax)},
	{"key_table", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, key_table)},
	{"array_lengths", RELOPT_TYPE_ENUM, offsetof(SiftColOptions, array_lengths)},
	{"dict_lookup", RELOPT_TYPE_ENUM, offsetof(SiftColOptions, dict_lookup)},
	{"return_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, return_columns)},
	{"superunit", RELOPT_TYPE_INT, offsetof(SiftColOptions, superunit)},
	{"order_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, order_columns)},
	{"order_array_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, order_array_columns)},
	{"key_order", RELOPT_TYPE_STRING, offsetof(SiftColOptions, key_order)},
	{"order_layout", RELOPT_TYPE_STRING, offsetof(SiftColOptions, order_layout)},
	{"order_array_group", RELOPT_TYPE_STRING, offsetof(SiftColOptions, order_array_group)},
	{"hll_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, hll_columns)},
	{"key_cut", RELOPT_TYPE_STRING, offsetof(SiftColOptions, key_cut)},
	{"key_cut_rows", RELOPT_TYPE_INT, offsetof(SiftColOptions, key_cut_rows)},
	{"key_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, key_columns)},
	{"point_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, point_columns)},
	{"count_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, count_columns)},
	{"point_tid_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, point_tid_columns)},
	{"track_updates", RELOPT_TYPE_STRING, offsetof(SiftColOptions, track_updates)},
};

/*
 * The columns key_order stands for are appended to the options, where the accessors of the options it fills read them while those are not set. 'a, b' is a
 * group and a key (the group of the order arrays; key_cut and the point directory are left to the build, which cuts by the group only where its rule says), 'a' is
 * a key (key_cut, order_columns, order_array_columns and point_tid_columns). The columns are trimmed: 'a,b' and 'a , b' are the same option.
 */
static void
key_order_expand(SiftColOptions **op)
{
	SiftColOptions *o = *op;
	const char *spec = (const char *) o + o->key_order;
	bool		autochosen = strncmp(spec, "auto:", 5) == 0;
	char	   *first,
			   *last,
			   *comma;
	size_t		base = VARSIZE(o);
	char	   *dst;

	if (strcmp(spec, "off") == 0)
		return;
	first = pstrdup(autochosen ? spec + 5 + strspn(spec + 5, " ") : spec);
	last = first;
	comma = strchr(first, ',');

	if (comma)
	{
		*comma = '\0';
		last = comma + 1 + strspn(comma + 1, " ");
		while (comma > first && comma[-1] == ' ')
			*--comma = '\0';
	}
	o = repalloc(o, base + strlen(first) + strlen(last) + 4);
	dst = (char *) o + base;
	o->ko_lead = base;
	dst += sprintf(dst, "%s", first) + 1;
	if (comma)
	{
		o->ko_last = dst - (char *) o;
		dst += sprintf(dst, "%s", last) + 1;
	}
	else
		o->ko_last = base;
	o->ko_group = comma ? o->ko_lead : 0;
	o->ko_key = comma || autochosen ? 0 : o->ko_lead;
	o->ko_auto = autochosen;
	SET_VARSIZE(o, dst - (char *) o);
	*op = o;
}

bytea *
am_col_amoptions(Datum reloptions, bool validate)
{
	SiftColOptions *o = build_reloptions(reloptions, validate, col_relopt_kind, sizeof(SiftColOptions), col_relopt_tab, lengthof(col_relopt_tab));

	if (validate && o && o->superunit == 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("superunit must be 0 or between %d and %d", SIFT_SUPER_MIN_FAN, SIFT_SUPER_MAX_FAN)));
	if (o && o->key_order && *((const char *) o + o->key_order))
		key_order_expand(&o);
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
	return am_col_amoptions((Datum) 0, false);
}
#endif

const SiftColOptions *
sift_col_options(Relation index)
{
	static const SiftColOptions defaults = {.write_buffer_limit = SIFT_DEFAULT_WRITE_BUFFER_LIMIT, .dead_rewrite_pct = SIFT_COL_DEAD_AUTO, .rollup_on_demand = true, .vacuum_truncate = true, .cluster_advice = true, .dict_lookup = SIFT_COL_DICT_AUTO, .cube_minmax = true, .key_table = true, .superunit = SIFT_SUPER_DEFAULT_FAN};

	return index->rd_options ? (const SiftColOptions *) index->rd_options : &defaults;
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
 * again at the next REINDEX (col/keycols/keycols.c), and has no schema until then, as any change of paths leaves the segments on the old one: it is let through.
 */
void
sift_col_alter_check(Relation index)
{
	if (!sift_col_is_index(index))
		return;
	if (sift_col_paths_redecide(index))
		return;
	(void) sift_col_rel(index);
	sift_col_row_key_check(index);
	sift_col_track_reconcile(index);
}

COL_OPT_STR(sift_col_options_key_columns, key_columns, NULL)

COL_OPT_STR(sift_col_options_point_columns, point_columns, "")

COL_OPT_STR(sift_col_options_count_columns, count_columns, "")

COL_OPT_STR_KO(sift_col_options_point_tid_columns, point_tid_columns, ko_key)

COL_OPT_STR(sift_col_options_key_order, key_order, "")

/* whether the key_order is what a build chose ('auto: ...'), decided again by the next one (keycols/gorder.c) */
bool
sift_col_options_key_order_auto(Relation index)
{
	return strncmp(sift_col_options_key_order(index), "auto:", 5) == 0;
}

COL_OPT_STR(sift_col_options_row_key, track_updates, "auto")

/* whether the user set order_columns, by the option or by a key_order of their own: what the build chose with key_cut ('auto: a, b' in key_order) is decided again with it */
bool
sift_col_options_order_columns_set(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return (o->order_columns && *((const char *) o + o->order_columns)) || (o->ko_last && !o->ko_auto);
}

/* the leaves the option names: 'auto: a, b' is the choice of a build, 'auto' and 'off' name none */
const char *
sift_col_options_key_cut(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);
	const char *v = o->key_cut ? (const char *) o + o->key_cut : o->ko_key ? (const char *) o + o->ko_key : "";

	if (strncmp(v, "auto:", 5) == 0)
		return v + 5;
	return strcmp(v, "auto") == 0 || strcmp(v, "off") == 0 ? "" : v;
}

/* whether a build decides key_cut: the option is not set, or holds a choice of an earlier build */
bool
sift_col_options_key_cut_auto(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return (o->key_cut == 0 && o->ko_key == 0) || (o->key_cut != 0 && strncmp((const char *) o + o->key_cut, "auto", 4) == 0);
}
