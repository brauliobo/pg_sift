/*
 * col/am/options.c - the reloptions of a column index: the validators of each option,
 * the registration of the options, how an index reads them and the
 * checks of an ALTER of them
 */
#include "postgres.h"
#include "access/reloptions.h"
#include "utils/varlena.h"
#include "col/vstore/vstore.h"
#include "col/dictshm.h"
#include "col/dsmreg.h"
#include "col/leafshm.h"
#include "col/point.h"
#include "col/hll.h"
#include "col/colspool.h"
#include "col/gorder.h"
#include "col/setsum.h"
#include "col/seg.h"
#include "col/am/am_int.h"
#include "col/conn.h"
#include "col/scope.h"
#include "core/lane.h"
#include "core/ops.h"

int			sift_col_unit_rows = 0;

/*
 * col/am/am.c - the sift_col index access method: handler, options, the per-relation leaf list, tuple to row conversion and
 * aminsert. The build is in col/build/build.c, VACUUM and maintenance in col/maint/maint.c, the read side in col/scan/scan.c.
 */

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

	while (isspace((unsigned char) *p))
		p++;
	if (*p == '\0' || strcmp(p, "off") == 0)
		return NULL;
	for (;;)
	{
		char	   *name = sift_cpagg_name_take(&p, ",");
		int			a = 0;

		if (*name == '\0')
			return "empty column name";
		if (r != NULL)
		{
			for (int i = 1; i <= r->natts && a == 0; i++)
				if (strcmp(NameStr(TupleDescAttr(desc, i - 1)->attname), name) == 0)
					a = i;
			if (a == 0)
				return psprintf("\"%s\" is not a column of the index", name);
			if (!sift_col_returns_str(desc, a, r->nleaves[a], r->json[a]))
				return "not a text, varchar or uuid column";
			r->ret[a] = true;
		}
		if (*p == '\0')
			return NULL;
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

/* The option that is not valid, with its value and what is wrong: one text for the check of the value and for the use of it */
void
sift_col_option_error(const char *option, const char *value, const char *err)
{
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid %s option \"%s\": %s", option, value, err)));
}

/* An option whose value is a list with its own syntax checker: the error names the option and what is wrong */
#define SYNTAX_VALIDATOR(option, check) \
void \
sift_col_validate_##option(const char *value) \
{ \
	const char *err = check; \
\
	if (err) \
		sift_col_option_error(#option, value, err); \
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
SYNTAX_VALIDATOR(rollup_leaves, sift_cpagg_rollup_syntax(value))
SYNTAX_VALIDATOR(rollup_columns, sift_cpagg_rows_syntax(value))
SYNTAX_VALIDATOR(calendar_columns, sift_cpagg_calendar_syntax(value))
SYNTAX_VALIDATOR(return_columns, am_return_walk(value, NULL, NULL))
SYNTAX_VALIDATOR(order_array_group, order_array_syntax(value))
SYNTAX_VALIDATOR(order_layout, sift_cgord_layout_parse(value, NULL, NULL, NULL))
SYNTAX_VALIDATOR(order_array_columns, order_array_syntax(value))
SYNTAX_VALIDATOR(hll_columns, sift_hll_syntax(value))
SYNTAX_VALIDATOR(count_columns, sift_cpoint_syntax(value))
SYNTAX_VALIDATOR(point_tid_columns, sift_cpoint_syntax(value))

/* one column name, or off or auto */
static const char *
row_key_syntax(const char *value)
{
	const char *p = value;

	pfree(sift_cpagg_name_take(&p, ",;"));
	return *p == '\0' ? NULL : "a single column: a composite key is not supported";
}

SYNTAX_VALIDATOR(track_changes, row_key_syntax(value))

/* 'fk -> parent(key)', or empty or off: the names are resolved where the connection is made (col/conn_decl.c) */
static const char *
connect_syntax(const char *value)
{
	const char *err = NULL;

	if (value[0] == '\0' || strcmp(value, "off") == 0 || strcmp(value, "auto") == 0 || strcmp(value, "auto:") == 0)
		return NULL;
	return sift_conn_parse(value, &err) ? NULL : err;
}

SYNTAX_VALIDATOR(connect, connect_syntax(value))

/* 'key_name: fk -> relation(member, scope_key)', or empty or off: the names are resolved where the declaration is made (col/scope_decl.c) */
static const char *
scope_syntax(const char *value)
{
	const char *err = NULL;

	if (value[0] == '\0' || strcmp(value, "off") == 0)
		return NULL;
	return sift_scope_parse(value, &err) ? NULL : err;
}

SYNTAX_VALIDATOR(scope, scope_syntax(value))
/* 'name, name': the projections of sift.projection_index; the names are resolved by sift.projection_status, not here: a restore creates the index before the data of the definitions is there (pg_upgrade never has it) */
static const char *
projection_syntax(const char *value)
{
	for (const char *p = value; *p; p += strspn(p, " "))
	{
		size_t		n = strspn(p, "abcdefghijklmnopqrstuvwxyz0123456789_$");

		if (n == 0 || isdigit((unsigned char) *p) || (p[n] != '\0' && p[n] != ','))
			return "a list of projection names (lower case letters, digits, _ and $) separated by commas";
		p += n + (p[n] == ',');
	}
	return NULL;
}

SYNTAX_VALIDATOR(projection, projection_syntax(value))

/* one column, or 'group, key': the names themselves are checked where the options they stand for are */
static const char *
key_order_syntax(const char *value)
{
	const char *p = value;
	char	   *name;

	if (value[0] == '\0' || strcmp(value, "off") == 0)
		return NULL;
	if (strncmp(value, "auto:", 5) == 0)
		p += 5;
	name = sift_cpagg_name_take(&p, ",");
	if (*name == '\0')
		return "a column name, or 'group, key'";
	pfree(name);
	if (*p == ',')
	{
		p++;
		name = sift_cpagg_name_take(&p, ",");
		if (*name == '\0' || *p != '\0')
			return "at most two columns, 'group, key'";
		pfree(name);
	}
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

#ifdef SIFT_TESTING
static char *open_stats_unused;

static const char *
show_open_stats(void)
{
	return sift_test_col_open_stats;
}
#endif

void
sift_col_init(void)
{
	sift_col_stmt_loaded();
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
	DefineCustomBoolVariable("sift.col_auto_connect",
							 "CREATE INDEX and REINDEX of a sift_col index connect it to the sift_col indexes of the tables its validated foreign keys reference.",
							 "On: an index without the option connect that holds the column of a validated single column foreign key to an int2, int4 or int8 key of a heap table that has a sift_col index tracking that key "
							 "is connected to it (the choice is written into the option as 'auto: ..', and a NOTICE names it). "
							 "Off: only an index with connect = 'auto' or an explicit connect option is connected. The first child connected to a parent index holds the writers of the parent table out from the end of its scan to the end of the statement.",
							 &sift_col_auto_connect, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_connect_refresh_pct",
							"Percent of the rows of a parent table whose keys changed since the stamp of a segment of a connected sift_col index from which sift.col_maintenance_due says 'stale connection' and the status asks for a REINDEX.",
							"A read patches the rows of the changed keys from the join, so a few changed keys cost less than the rewrite of the segment (the found flags it holds are as of its stamp). "
							"A segment that was never filled (the first child of a parent, whose journal starts after its scan) or whose parent index was rebuilt is stale whatever the share. "
							"Nothing rewrites the index by itself: reads keep to the join, and REINDEX INDEX CONCURRENTLY fills the flags again.",
							&sift_col_connect_refresh_pct, 10, 1, 100, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_scope_refresh_pct",
							"Percent of the pairs of the scope relations that changed since the stamp of the scope segments of a sift_col index from which sift.col_maintenance_due says 'stale scope' and the status asks for a REINDEX.",
							"A read patches the members whose pairs changed from the join, so a few changed members cost less than the rewrite of the scope segments. "
							"Scope segments never filled, or stamped under a journal a relation has since lost, are stale whatever the share. "
							"Reads patch the changed members from the join until a REINDEX INDEX CONCURRENTLY; sift.col_connect_refresh_pct is the same rule for the connected indexes.",
							&sift_col_scope_refresh_pct, 10, 1, 100, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_scope",
							 "A scan of an index with the option scope answers the rows of a scope key (a join to its scope relation, or IN, EXISTS over it, by that key) from the scope segments of the index.",
							 "Off: the planner does not recognise the scope forms, and a scan that carries one reads the entity path (the join as written).",
							 &sift_agg_scope, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.scope_max_multiplicity",
							 "The most scope entries for each fact row (entries of a scope over the rows of the table) that CREATE INDEX of a scope index accepts: the limit of a declaration that states no max_multiplicity(n), which the index records.",
							 "The scope segments hold a row once for each scope key of its member, so the index grows by about 17 percent of the table and its indexes for each entry a row has (its writes cost about 16 percent more for each) "
							 "while a read gains 2 to 9 times (a total up to 22 times) over the join: past about 3 entries a row the growth costs more than the reads gain. A scope above the limit is refused with the figure, "
							 "and the join answers it. CREATE INDEX writes the limit it accepted the scope under into the option scope (max_multiplicity(n)), so REINDEX, a restore and pg_upgrade check that one, not the setting of their session.",
							 &sift_scope_max_multiplicity, 3.0, 1.0, 1000000.0, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.col_auto_merge",
							 "Keeps the sift_col indexes answerable after churn: a background worker vacuums the heap table, then seals the write buffer and merges the small segments the policy picks. It never rewrites an index: that is REINDEX INDEX CONCURRENTLY.",
							 "Rows on a page the visibility map does not call all-visible (any page with an UPDATE or DELETE since the last VACUUM) are checked one by one in the heap, "
							 "and a unit holding a dead row answers from its rows; the VACUUM sets the map, and the merges after it make the units answer from their pre-aggregates again. "
							 "A backend that inserts a row version, or reads many rows in the heap, looks at the statistics of the table once per sift.maintenance_naptime and starts the worker "
							 "when the rows changed since the last VACUUM are a thousandth of the table (50 rows at least, 100,000 at most). It vacuums as autovacuum does (its reloptions, cost delay, skipped when the table is locked), "
							 "at most once per five sift.maintenance_naptime (ten times the average VACUUM of the table on PostgreSQL 18), and not a table with autovacuum_enabled = false. "
							 "Its merges sleep as sift.maintenance_cost_delay says and are skipped when the maintenance lock is held. Needs sift.async_maintenance. "
							 "sift.col_maintenance_status says, for the reasons only a rewrite of the whole index serves (dirty units, fragmented units, a stale connection or scope), that a REINDEX is recommended; "
							 "sift.col_maintain(index) is that rewrite, in the session that calls it. "
							 "A DELETE alone calls nothing and waits for a scan or an insert; a scan of a partitioned table does not ask, an insert into a partition does. "
							 "Set it for the server or the database: the worker reads no session setting.",
							 &sift_col_auto_merge, true, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.col_auto_merge_fragmentation",
							 "Units listed per superblock they cover from which sift.col_maintenance_due says 'fragmented units' for a sift_col index.",
							 "Each promotion of the write buffer adds a segment over the superblocks the churn touched.",
							 &sift_col_auto_merge_fragmentation, 2.0, 1.1, 1000.0, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.col_auto_merge_buffer_pct",
							 "Percent of its rows the write buffer of a sift_col index holds above which sift.col_maintenance_due says 'write buffer rows'.",
							 "The rows of the buffer are read as one in-memory unit per superblock they touch, on every query, until a promotion.",
							 &sift_col_auto_merge_buffer_pct, 1.0, 0.01, 100.0, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.col_auto_merge_wal_pct",
							 "Percent of max_wal_size that one pass of the merge of the overlays of a sift_col index may write.",
							 "A pass of sift.col_maintain(index) that merges the small segments left over the partitions of a key_cut index takes an eighth of max_wal_size at most, and this percent of it, "
							 "so that it forces no checkpoint of its own; the next pass takes the rest. 0 does not limit the pass to a share.",
							 &sift_col_auto_merge_wal_pct, 5.0, 0.0, 100.0, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_auto_merge_pass_mb",
							"Megabytes of overlapping runs one pass of sift.col_maintain(index) rewrites in a sift_col index.",
							"A build leaves runs of rows sorted by the key_cut key, each as large as a partition and overlapping its neighbours in the key; the rewrite cuts them by key, "
							"a window of the runs that overlap at a time, and stops when a pass has written this much. The next sift.col_maintain(index) goes on, "
							"so the WAL and the I/O of the rewrite are spread over passes (the index stays valid and equal between them). 0: one pass does all of it.",
							&sift_col_auto_merge_pass_mb, 1024, 0, MAX_KILOBYTES / 1024, PGC_SUSET, GUC_UNIT_MB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_auto_merge_partition_kb",
							"Size of the segments the merges of a sift_col index write, so that sift.col_maintain(index) rewrites only the ones the churn touched; 0 rewrites the whole index into one.",
							"A step rewrites the partition with most dirty units, or the overlays the promotions left over the partitions, "
							"and holds the maintenance lock and needs the free space of one partition, not of the index. A segment under half of this size is an overlay; "
							"the merge tiers leave the partitions alone. An index under half of it is rewritten whole. 0: partitions of sift.vacuum_rewrite_limit for an index over it, or of a third of maintenance_work_mem where that is less, which is what the writer of a segment holds in memory.",
							&sift_col_auto_merge_partition_kb, 0, 0, MAX_KILOBYTES, PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_unit_floor",
							"Rows per unit below which the rows of a sift_col promotion or overlay merge are cut by TID range, not one unit per superblock; 0 keeps a unit per superblock.",
							"Churn puts a few dozen rows in each of the superblocks it touches, and a unit costs a scan about as much as a thousand rows however few it holds, so a promotion that reaches "
							"16 superblocks or more with under half this many rows each is written as units of this many rows in TID order, and a merge that stays that sparse does the same. "
							"A merge that reaches this many rows per superblock cuts by superblock again. Needs a table of 16 superblocks (4,096 pages) or more.",
							&sift_col_unit_floor, 2048, 0, 32768, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.col_wbuf_ref",
							 "A row of a heap table with a sift_col index that tracks changes goes to the write buffer as a 16 byte reference to its heap tuple, not as its values.",
							 "The backend that inserts the row keeps its values in shared memory (1 MB segments made on demand, 128 MB for the server at most, 88 bytes a row of a 13 column table), and the reader of the buffer "
							 "takes them from there. A server that starts again, a standby and a promoted standby have none: the first reader builds them from the heap, once for the buffer. A row that finds no room "
							 "is written with its values (sift.col_arena_status shows how many). Records written while it was on stay references after it is turned off; a buffer that holds one is column format minor 44. "
							 "An index without track_changes, a speculative insertion and a table that is not a heap keep the values.",
							 &sift_col_wbuf_ref, true, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.col_wbuf_key_range",
							 "The pages of the write buffers of a key_cut index with an integer key hold the range of the keys of their rows, so that a read of some keys skips the pages that hold none of them.",
							 "A buffer opened with it on is column format minor 55: a release before 5.1 refuses the index, so upgrade every server that reads it (a standby too) before the primary opens one, or set it off first: off writes buffers of minor 51, which 5.0 reads. "
							 "The buffers opened while it is off, and one that was open when the release was installed, keep the pages they have. A read of keys decodes the rows of the pages that meet them until it has decoded "
							 "as many rows as the buffer holds, then keeps the whole buffer as the reads of a buffer without ranges do.",
							 &sift_col_wbuf_key_range, true, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_unit_rows",
							"The rows a key-cut unit aims at, 0 for the rows of 256 average heap pages.",
							"A smaller unit prunes finer and costs more directory per row; it applies to the units written from now on. It also gets several units out of a small table.",
							&sift_col_unit_rows, 0, 0, 32768, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.col_value_store",
							 "CREATE INDEX and REINDEX of a sift_col index share the strings of a column with the other indexes of the table that hold it, in a shared value store.",
							 "On: a text, varchar or uuid leaf that another sift_col index of the table holds with the same key order, and whose sharing spares at least 5 bytes a row and a quarter of the leaf, keeps its strings in the store "
							 "of its column (column format minor 46, which 4.3 and older refuse). Off: the build keeps the strings in its segments and makes no store, and a REINDEX of a member takes its strings back; "
							 "change tracking and every other structure are written as usual (column format minor 44 at the most), and the setting changes no existing index.",
							 &sift_col_value_store, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_dict_shared_cache_kb",
							"Shared memory, for all backends together, that keeps the parsed dictionaries of sift_col segments (128 kB or more each), so that a new backend or a pooled connection reads a dictionary another backend parsed in place instead of parsing it (0: off, each backend parses and keeps its own, up to the dict_cache_kb of the index).",
							"The ring of records is allocated, at the size of this setting, by the first backend that keeps a dictionary after the server starts, and its size is recorded for the other backends: a later change turns the use of it on and off (0), never its size, which changes at the next start of the server. "
							"The memory is in addition to shared_buffers, is taken whole when the ring is made, and with dynamic_shared_memory_type = posix comes from /dev/shm (at most a quarter of it is used, with a message in the log) and counts in the shared memory of the container. "
							"A record is about 21 bytes for each distinct value, and 16 to 32 more with its hash table; a record over half of the ring is not kept.",
							&sift_col_dict_shared_cache_kb, 65536, 0, 4194304,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_leaf_cache_kb",
							"Shared memory, for all backends together, that keeps what the zones of the units of a sift_col index say to the planner (a record for each leaf and a sample of the unit zones), so that a new backend does not read them all (0: off).",
							"The table is allocated, at the size of this setting, by the first backend that plans a statement over a sift_col index after the server starts; a later change turns the use of it on and off (0), and a size no table has yet is a table of its own (the tables of other sizes stay until the server restarts). A record is about 33 kB for each integer leaf and 0.4 kB for the others; a record that does not fit is not kept, and the oldest goes when the table is full.",
							&sift_col_leaf_cache_kb, 16384, 0, 262144,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	sift_col_build_define_gucs();
	DefineCustomIntVariable("sift.col_format_minor",
							"Column format minor of the segments written (15 by default; an option with a newer layout, such as key_cut or point_tid_columns, stamps its own higher minor): 6 keeps them readable by pg_sift 3.2, 4 by 3.1, 3 by 3.0.",
							"Segments already written keep their minor; a 3.2 library refuses the ones above 6. A segment of a schema with wide leaves is minor 5 at least, and the minor 6 "
							"of the complete kinds is written only for a schema with jsonb paths, from rows whose kinds are known. The minor 7 is written only for an index with the option cube_minmax, the minor 8 only for a sift_table with a date or timestamp column, the minor 9 only for an index with the option rollup_dims, the minor 10 only for one with the option key_columns, the minor 12 only for one with the option superunit, the minor 13 only for one with the option order_columns, the minor 14 only for one with a dictionary of 1,024 strings or more (the option dict_lookup), the minor 15 only for one whose key_columns keep a key list or a bitmap. Those above 15 are written by the options that need them, and a library older than the release that added the layout refuses the segment.",
							&sift_col_format_minor, SIFT_COL_FORMAT_MINOR, SIFT_COL_FORMAT_MIN_READ, SIFT_COL_FORMAT_MINOR,
							PGC_SUSET, 0, check_col_format_minor, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomIntVariable("sift.test_col_rollup_panic",
							"Testing: the rollup refresh worker PANICs after it wrote the new segment, before it replaces the old one (1 = yes). Set it in postgresql.conf: the worker reads no session setting.",
							NULL, &sift_test_col_rollup_panic, 0, 0, 1, PGC_SIGHUP, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_open_all",
							 "Testing: a scan opens every segment of the index, not only those of the partitions that may hold the keys its conditions confine it to (the answer is the same).",
							 NULL, &sift_test_col_open_all, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_merge_panic",
							 "Testing: a column merge (promotion excepted) PANICs after it wrote the new segment, before it replaces the old ones.",
							 NULL, &sift_test_col_merge_panic, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_record_partitions",
							 "Testing: a merge of a key-cut index records no partitions in the directory (an index as 5.0 wrote it); sift_col_adopt_partitions records them.",
							 NULL, &sift_test_col_record_partitions, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_merge_error",
							"Testing: a column merge (promotion excepted) fails after it wrote the new segment, before it replaces the old ones: 1 = with an error, 2 = as a worker that the operator shut down.",
							NULL, &sift_test_col_merge_error, 0, 0, 2, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_gorder_seek",
							 "Testing: every chunk of an order array is written seekable (a fence key every 32 entries), whatever its bytes.",
							 NULL, &sift_test_col_gorder_seek, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_gorder_rows",
							"Testing: 1024 writes the global order with the chunks of column format minor 21 (1,024 entries), 0 the current ones (512).",
							NULL, &sift_test_col_gorder_rows, 0, 0, 1024, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_conn_fill",
							"Testing: how the fill of the found flags of a connected index knows the keys of the parent: 1 reads the parent, 2 probes its btree, 0 takes the cheaper.",
							NULL, &sift_test_conn_fill, 0, 0, 2, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_conn_no_horizon",
							 "Testing: the fill of the found flags of a connected index writes stamps without the insert horizon of the parent, as the segments written before it have.",
							 NULL, &sift_test_conn_no_horizon, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_scope_patch",
							 "Testing: a read of a scope key patches the members that changed, however many and whatever it costs, where it reads the entity path when that costs less.",
							 NULL, &sift_test_scope_patch, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_scope_sample_blocks",
							"Testing: the blocks a table needs for the build of a scope index to estimate the multiplicity of its scopes from a sample first (0 = 2,048).",
							NULL, &sift_test_scope_sample_blocks, 0, 0, 1048576, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_member_hash_bits",
							"Testing: the low bits kept of the word the journal of a scope relation records for a text, varchar or uuid member (0 = all 64), to make members collide.",
							NULL, &sift_test_member_hash_bits, 0, 0, 64, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
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
	DefineCustomIntVariable("sift.test_col_track_gate",
							"Testing: the install of the change-tracking triggers of a log waits for the advisory lock of this number before it asks for anything of the table (0 = no gate).",
							NULL, &sift_test_col_track_gate, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_track_decided_gate",
							"Testing: the install of the change-tracking triggers of a log waits for the advisory lock of this number after it decided whether the table may be asked for the stronger lock, and before it acts on that (0 = no gate).",
							NULL, &sift_test_col_track_decided_gate, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_gkept_fail",
							 "Testing: extending the kept order of the write buffer's rows fails after its new arrays are made and before the order takes them.",
							 NULL, &sift_test_col_gkept_fail, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomStringVariable("sift.test_dsm_fail",
							   "Testing: the shared tables whose name has one of these parts (comma separated; bgworker: before a part: background workers only) cannot be made, as when /dev/shm is full.",
							   NULL, &sift_test_dsm_fail, "", PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
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
	DefineCustomIntVariable("sift.test_col_cut_record",
							"Testing: a segment cut by a key is written without the record of its cut (1: with the flag of the cut, as before SIFT_SEG_CUTREC; 2: without it, as 4.3 and older).",
							NULL, &sift_test_col_cut_record, 0, 0, 2, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_loose_inputs",
							 "Testing: a merge reads every keyed input as cut another way: its rows are loaded and sorted, or sorted by the sort that spills, and none is a stream.",
							 NULL, &sift_test_col_loose_inputs, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_merge_eager",
							 "Testing: a merge of streams reads the first unit of every input before its first row, as before an input waited for the key range of its rows.",
							 NULL, &sift_test_col_merge_eager, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
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
	DefineCustomStringVariable("sift.test_vstore_columns", "testing: columns whose values the indexes built next share through a value store",
							NULL, &sift_test_vstore_columns, "", PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_vstore_gc_kb", "Testing: the size (kB) of a shared value store from which VACUUM looks for codes that no index names.",
							NULL, &sift_vstore_gc_kb, 8192, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_workers",
							"Testing: parallel workers the last sift_col build of this session launched.",
							NULL, &sift_test_col_workers, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_from_index_off",
							 "Testing: REINDEX reads the heap even when the index they replace could give the rows.",
							 NULL, &sift_test_col_from_index_off, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_from_index_mutant",
							"Testing: a defect in the build from the replaced index (1: the write buffers are left out).",
							NULL, &sift_test_col_from_index_mutant, 0, 0, 1, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_from_index_used",
							"Testing: 1 when the last sift_col build of this session took its rows from the index it replaces, 0 when it read the heap.",
							NULL, &sift_test_col_from_index_used, 0, 0, 1, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_build_table_buffers",
							"Testing: the buffers of the table (any fork) the last sift_col build of this session accessed, as the statistics count them, from its start to its end.",
							NULL, &sift_test_col_build_table_buffers, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_build_peak_kb",
							"Testing: kB of memory the last sift_col build of this session held above what the backend started it with, at its peak.",
							NULL, &sift_test_col_build_peak_kb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_build_peak_log",
							 "Testing: a sift_col build logs its largest memory contexts each time its peak grows by a MB.",
							 NULL, &sift_test_col_build_peak_log, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
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
	DefineCustomIntVariable("sift.test_col_scope_merges",
							"Testing: merges of scope segments this session has written.",
							NULL, &sift_test_col_scope_merges, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_scope_merge_limit",
							"Testing: the merge of scope segments after this many of them (sift.test_col_scope_merges) stops with an error (0 = no limit).",
							NULL, &sift_test_col_scope_merge_limit, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_temp_free_kb",
							"Testing: kB of disk the temporary files of a sift_col build by key ranges may use, instead of what the temporary tablespaces have free (-1 = the disk).",
							NULL, &sift_test_col_temp_free_kb, -1, -1, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_gap_blocks",
							"Testing: blocks a parallel sift_col build reserves in front of its segments for the merge (-1 = by the statistics, 0 = none).",
							NULL, &sift_test_col_gap_blocks, -1, -1, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_segment_kb",
							"Testing: kB of memory a sift_col build's segment writer holds before the segment is cut (0 = by maintenance_work_mem; a merge honors it too, but the one that ends a parallel build).",
							NULL, &sift_test_col_segment_kb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_spill_run_kb",
							"Testing: kB of memory a run of a sift_col build by key ranges may take (0 = 768 MB).",
							NULL, &sift_test_col_spill_run_kb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_build_runs_overlap",
							 "Testing: a sift_col build with key_cut writes the runs of the table's order, which overlap in the key, to leave a rewrite to do.",
							 NULL, &sift_test_col_build_runs_overlap, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_spill_bytes",
							"Testing: bytes the temporary files of the last sift_col build by key ranges of this session took.",
							NULL, &sift_test_col_spill_bytes, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_open",
							"Testing: open units of a sift_col build (0 none, 1 for the values of the key that fill units, 2 for every range of the key, with no temporary files).",
							NULL, &sift_test_col_open, 0, -1, 2, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_open_mutant",
							"Testing: a defect in the open units of a sift_col build (1 a value joins the unit of another, 2 the tail units are lost, 3 a unit is written twice, 4 the cap of the open units is not looked at, 5 the units that wait are not counted out when they are written, 6 the units that wait are never written before the end).",
							NULL, &sift_test_col_open_mutant, 0, 0, 6, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_open_kb",
							"Testing: kB the open units of a sift_col build may hold (0 = the memory of the first run).",
							NULL, &sift_test_col_open_kb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_open_single",
							"Testing: the most values of the key that have an open unit of their own (-1 = half the ranges).",
							NULL, &sift_test_col_open_single, -1, -1, 1000000, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomStringVariable("sift.test_col_open_stats",
							   "Testing: what the last sift_col build by open units did.",
							   NULL, &open_stats_unused, "", PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, show_open_stats);
	DefineCustomIntVariable("sift.test_col_spill_rows",
							"Testing: rows the temporary files of the last sift_col build by key ranges of this session hold.",
							NULL, &sift_test_col_spill_rows, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_spool_layer",
							"Testing: the layer over the cascade codecs of the chunks of a sift_col build by key ranges (0 = chosen from a sample, 1 cascade alone, 2 lz4, 3 zstd, 4 no compression).",
							NULL, &sift_test_col_spool_layer, 0, 0, 4, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_spool_chunk_rows",
							"Testing: rows of a chunk of the temporary files of a sift_col build by key ranges (0 = from the memory the build gives them).",
							NULL, &sift_test_col_spool_chunk_rows, 0, 0, 1 << 20, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_spool_share_kb",
							"Testing: kB of memory the chunks of all the key ranges of a sift_col build may hold, instead of the build's budget (0 = the budget).",
							NULL, &sift_test_col_spool_share_kb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_spool_unbounded",
							"Testing: the chunks of a sift_col build by key ranges are as large as they can be and no memory limit writes them (a build that ignores its share).",
							NULL, &sift_test_col_spool_unbounded, 0, 0, 1, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_spool_used_rows",
							"Testing: rows of a chunk of the last sift_col build by key ranges of this session (0 = its rows went to packed files).",
							NULL, &sift_test_col_spool_used_rows, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_spool_ranges",
							"Testing: key ranges of the last sift_col build by key ranges of this session (each has a spool of chunks).",
							NULL, &sift_test_col_spool_ranges, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_spool_est_bytes",
							"Testing: bytes the last sift_col build by key ranges of this session priced its chunks at.",
							NULL, &sift_test_col_spool_est_bytes, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_no_present",
							 "Testing: a sift_col build writes no presence leaf for the jsonb documents (the layout of an index made before 4.2).",
							 NULL, &sift_test_col_no_present, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_log_max_read",
							"Testing: the newest column format whose tombstone log a sift_col index opens (0 = the build's own); a log of a newer format is refused.",
							NULL, &sift_test_col_log_max_read, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	sift_col_btree_test_gucs();
	DefineCustomIntVariable("sift.test_col_build_runs",
							"Testing: runs a sift_col build with key_cut keeps before it merges some (0 = the default).",
							NULL, &sift_test_col_build_runs, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
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
	add_bool_reloption(col_relopt_kind, "cluster_advice",
					   "sift.cluster_advice() may recommend a physical order or a key_cut for the table of this index, and CREATE INDEX may suggest key_cut (default on; maintenance only, not part of the format)",
					   true, ShareUpdateExclusiveLock);
	add_enum_reloption(col_relopt_kind, "array_lengths",
					   "The array columns of the index keep their cardinality and array_length(x, 1) as two integer columns of their own, so that cardinality(x), array_length(x, 1) and x IS [NOT] NULL are read without the arrays (about 1 byte a row for each array column): on for every array column, off for none, auto (default) is on for the index that CREATE INDEX or REINDEX builds and off for an index built before the option; a change is refused while the index holds segments",
					   array_lengths_names, SIFT_COL_ARRLEN_AUTO,
					   "Valid values are \"auto\", \"on\" and \"off\".", ShareUpdateExclusiveLock);
	add_enum_reloption(col_relopt_kind, "dict_lookup",
					   "A dictionary of 1,024 strings or more of the segments written from now on is stored in blocks that are read in place, without loading it whole, where that adds at most 3% to the segment (ids, hashes, uuids and long strings; short strings that repeat words stay in the compact form); the option adds the order of the strings, so that an equality, an IN list or a prefix test reads it in place too: auto (default) for the columns of point_tid_columns, key_columns and return_columns, on for every column, off for none (the segments are column format minor 14; the order is 4 bytes a string)",
					   dict_lookup_names, SIFT_COL_DICT_AUTO,
					   "Valid values are \"auto\", \"on\" and \"off\".", ShareUpdateExclusiveLock);
	add_bool_reloption(col_relopt_kind, "cube_minmax",
					   "The cube of every unit written from now on also holds the minimum and maximum of each integer measure per group; the segments are column format minor 7 (default on)",
					   true, ShareUpdateExclusiveLock);
	add_bool_reloption(col_relopt_kind, "key_table",
					   "The segments written from now on keep, for each key_columns leaf, a table of its distinct keys with the rows, sums, minima and maxima of each, where that adds at most 3% to the segment, so that count, sum, min and max WHERE key = x read one record of a segment, not its units; the segments are column format minor 22, which 3.4 and older libraries cannot read (default on)",
					   true, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "rollup_dims",
						 "Deprecated alias of rollup_columns, which wins when both are set",
						 "", sift_col_validate_rollup_dims, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "rollup_columns",
						 "the scalar columns that the rollup of the rows of every unit written from now on groups by: 'col, col' or 'col, col: measure, ...', 'auto' (default) or 'off'; the segments are column format minor 8",
						 "", sift_col_validate_rollup_columns, ShareUpdateExclusiveLock);
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
					  "Deprecated and ignored; accepted so that an index that stores it, a dump and ALTER INDEX .. RESET keep working",
					  0, 0, INT_MAX, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "order_columns",
						 "Deprecated and ignored; accepted so that an index that stores it, a dump and ALTER INDEX .. RESET keep working (the order arrays of order_array_columns answer what its sorted runs did)",
						 "", NULL, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "order_array_columns",
						 "the integer, date and timestamp columns whose segments written from now on also keep every row's key and heap address sorted, so that a range of the column of up to 16,384 rows and ORDER BY column LIMIT n above 32 read the entries between the bounds, whatever the number of units; 4 to 7 bytes a row for each column; the segments are column format minor 21; 'col, col', 'off' or 'auto' (CREATE INDEX and REINDEX choose up to two columns of 100,000 rows or more whose values are scattered over the heap, that is the default)",
						 NULL, sift_col_validate_order_array_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "key_order",
						 "the integer, date, timestamp, numeric and float column, or 'group, key', whose order the rows are kept in: for one column it stands for key_cut and order_array_columns on it, for 'group, key' for order_array_columns on the key and order_array_group on the group (key_cut is left to the build), each while the option itself is not set; for a table that is stored in the order of the key (default none)",
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
						 "Deprecated and ignored; accepted so that an index that stores it, a dump and ALTER INDEX .. RESET keep working (point_tid_columns lists the heap addresses of the rows of a column)",
						 "", NULL, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "count_columns",
						 "the text columns whose dictionary of 1,024 strings or more every segment written from now on stores with the rows of each string in byte order (the columns whose dictionary is read in place do so unless this is off, the ones named here also get that order), so that count(*) of a prefix (LIKE 'abc%'), a range or an equality of the column over segments with no dead row reads two positions of the dictionary and no row; 0.1 to 1 byte for each distinct string, the segments are column format minor 24, which only 3.5 and later read (default empty: automatic; off: none)",
						 "", sift_col_validate_count_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "point_tid_columns",
						 "the plain integer, text and uuid columns whose point directory lists the heap addresses of the rows, not their numbers in the segment (an equality or a short IN list of the access method then returns the rows' addresses from the directory without reading a unit, and the heap checks them; a unit with a dead row is read instead); the segments are column format minor 33 or more (37 from 3.7), which only 3.7 and later read, and the lists take 1 to 5 bytes per row (default none)",
						 "", sift_col_validate_point_tid_columns, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "track_changes",
						 "the column that identifies a row (off, auto = the primary key when the index holds it, else off with a NOTICE, or a column with a unique index of its own, NOT NULL): the index records the entries of the row versions that stopped being live (an UPDATE, a key change, a DELETE) in a tombstone log, so that a read of rows changed and not yet vacuumed can leave the heap out. It installs two internal statement triggers on the table (AFTER UPDATE and AFTER DELETE), dropped with the index, and writes a baseline of the dead versions in a pass over the heap (column format minor 43, which only 4.0 and later read); it adds the column to point_tid_columns",
						 "auto", sift_col_validate_track_changes, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "connect",
						 "connects this index to the sift_col index of the table it joins: 'fk -> [schema.]parent(key)' names the column of this index that holds the parent key, the parent table (in the schema of this index's table when not qualified) and its key column; the parent columns that queries read come from the parent's own index. The parent index (the one that tracks the key) keeps a journal of the keys its updates and deletes end, and this index depends on it, so DROP INDEX of the parent index needs CASCADE (column format minor 44, which only 4.3 and later read); the key is an int2, int4 or int8 column (default none)",
						 "", sift_col_validate_connect, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "scope",
						 "Declares that the rows of this index are also cut by a key they reach through a table of (member, scope) pairs: 'key_name: fk -> [schema.]relation(member, scope_key) [cols(..)] [max_multiplicity(n)]' names the derived key, the column of this index that holds the member key, the relation (in the schema of this index's table when not qualified) and its two integer columns, which a btree covers (unique, or not: a pair may then be several rows); several scopes are separated by ';' (4 at most). The index depends on that btree, so DROP INDEX of it needs CASCADE; 'off' or RESET removes the declaration (column format minor 45, which only 5.0 and later read; default none)",
						 "", sift_col_validate_scope, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "projection",
						 "the named projections (sift.projection_create) this index holds or serves: 'name, name'. sift.projection_index writes it; sift.projection_status says whether the index still holds the columns, the filter and the rollups of each. Only a record: it changes nothing in the segments, ALTER INDEX .. SET (projection = ..) takes effect at once and a definition that is replaced is compared with the index again (default none)",
						 "", sift_col_validate_projection, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "return_columns",
						 "the text, varchar and uuid columns an index-only scan returns from the index (their strings are read from the dictionary of the segment; default none: a query that needs one reads the heap)",
						 "", sift_col_validate_return_columns, ShareUpdateExclusiveLock);
	add_int_reloption(col_relopt_kind, "superunit",
					  "Units per superunit summary: the sums and the cube of every group of this many consecutive units of a segment are stored once more, merged, and answer a query that no predicate cuts through from one read; the segments written from now on are column format minor 12 (0: none, else 2 to 256; default 16)",
					  SIFT_SUPER_DEFAULT_FAN, 0, SIFT_SUPER_MAX_FAN, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "rollup",
						 "Deprecated alias of rollup_leaves, which wins when both are set",
						 "", sift_col_validate_rollup, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "rollup_leaves",
						 "the leaves of array elements and map values that the rollup of every unit groups by: 'leaf, leaf, ...', 'auto' (default) or off",
						 "", sift_col_validate_rollup_leaves, ShareUpdateExclusiveLock);
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
	{"rollup_leaves", RELOPT_TYPE_STRING, offsetof(SiftColOptions, rollup_leaves)},
	{"rollup_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, rollup_columns)},
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
	{"track_changes", RELOPT_TYPE_STRING, offsetof(SiftColOptions, track_changes)},
	{"connect", RELOPT_TYPE_STRING, offsetof(SiftColOptions, connect)},
	{"scope", RELOPT_TYPE_STRING, offsetof(SiftColOptions, scope)},
	{"projection", RELOPT_TYPE_STRING, offsetof(SiftColOptions, projection)},
};

/*
 * The columns key_order stands for are appended to the options, where the accessors of the options it fills read them while those are not set. 'a, b' is a
 * group and a key (the group of the order arrays; key_cut is left to the build, which cuts by the group where the units read by an equality on it are many), 'a' is
 * a key (key_cut and order_array_columns; the point directory is left out, it makes the index 71 to 95% larger for lists of the key alone). The columns are trimmed: 'a,b' and 'a , b' are the same option.
 */
static void
key_order_expand(SiftColOptions **op)
{
	SiftColOptions *o = *op;
	const char *spec = (const char *) o + o->key_order;
	bool		autochosen = strncmp(spec, "auto:", 5) == 0;
	const char *p = autochosen ? spec + 5 : spec;
	char	   *first,
			   *last;
	bool		two;
	size_t		base = VARSIZE(o);
	char	   *dst;

	if (strcmp(spec, "off") == 0)
		return;
	first = last = sift_cpagg_name_take(&p, ",");
	two = *p == ',';
	if (two)
	{
		p++;
		last = sift_cpagg_name_take(&p, ",");
	}
	o = repalloc(o, base + strlen(first) + strlen(last) + 4);
	dst = (char *) o + base;
	o->ko_lead = base;
	dst += sprintf(dst, "%s", first) + 1;
	if (two)
	{
		o->ko_last = dst - (char *) o;
		dst += sprintf(dst, "%s", last) + 1;
	}
	else
		o->ko_last = base;
	o->ko_group = two ? o->ko_lead : 0;
	o->ko_key = two || autochosen ? 0 : o->ko_lead;
	o->ko_auto = autochosen;
	SET_VARSIZE(o, dst - (char *) o);
	*op = o;
	if (two)
		pfree(last);
	pfree(first);
}

/* The older name counts when the new one is empty (the strings live in the same bytea, so the offset is shared) */
static void
alias_string(const SiftColOptions *o, int old, int *neu)
{
	if (old && *((const char *) o + old) && !(*neu && *((const char *) o + *neu)))
		*neu = old;
}

/* the column track_changes names, without the quotes it was written in ("Order Id"): taken out once where the options are built, so that every reader has the name as it is */
static void
row_key_unquote(SiftColOptions *o)
{
	char	   *text = (char *) o + o->track_changes;
	const char *p = text;
	char	   *name = sift_cpagg_name_take(&p, "");

	strcpy(text, name);
	pfree(name);
}

bytea *
am_col_amoptions(Datum reloptions, bool validate)
{
	SiftColOptions *o = build_reloptions(reloptions, validate, col_relopt_kind, sizeof(SiftColOptions), col_relopt_tab, lengthof(col_relopt_tab));

	SIFT_TEST_GATE(validate && sift_test_alter_gate, 14);
	if (o)
	{
		alias_string(o, o->rollup, &o->rollup_leaves);
		alias_string(o, o->rollup_dims, &o->rollup_columns);
	}
	if (validate && o && reloptions != (Datum) 0)
		foreach_ptr(DefElem, def, untransformRelOptions(reloptions))
		{
			if (strcmp(def->defname, "rollup") == 0)
				sift_relopt_alias_notice("rollup", "rollup_leaves");
			else if (strcmp(def->defname, "rollup_dims") == 0)
				sift_relopt_alias_notice("rollup_dims", "rollup_columns");
			else if (strcmp(def->defname, "key_cut_rows") == 0 || strcmp(def->defname, "point_columns") == 0 || strcmp(def->defname, "order_columns") == 0)
				sift_relopt_removed_notice(def->defname);
		}

	if (validate && o && o->superunit == 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("superunit must be 0 or between %d and %d", SIFT_SUPER_MIN_FAN, SIFT_SUPER_MAX_FAN)));
	if (o && o->key_order && *((const char *) o + o->key_order))
		key_order_expand(&o);
	if (o && o->track_changes && *((const char *) o + o->track_changes) == '"')
		row_key_unquote(o);
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

