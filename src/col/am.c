/*
 * col/am.c - the sift_col index access method: handler, options, the per-relation leaf list, tuple to row conversion and
 * aminsert. The build is in col/build.c, VACUUM and maintenance in col/maint.c, the read side in col/scan.c.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/visibilitymap.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/hsearch.h"
#include "utils/numeric.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/sift.h"
#include "bm25/am.h"
#include "col/am.h"
#include "col/build.h"
#include "col/seg.h"
#include "col/setsum.h"
#include "core/cpu.h"
#include "core/limits.h"
#include "core/ops.h"

bool		sift_enable_col = true;
int			sift_col_format_minor = SIFT_COL_FORMAT_MINOR;
#ifdef SIFT_TESTING
bool		sift_test_col_rel_inval = false;
#endif

/* 5 is the minor of the wide leaves, which a schema takes by itself: the setting is 3, 4, 6, 7, 8, 9 or 10 */
static bool
check_col_format_minor(int *newval, void **extra, GucSource source)
{
	if (*newval == SIFT_COL_FORMAT_WIDE)
	{
		GUC_check_errdetail("Column format minor 5 is written for wide leaves only, whatever the setting.");
		GUC_check_errhint("Use 3 (pg_sift 3.0), 4 (pg_sift 3.1), 6, 7, 8, 9 or 10.");
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
sift_col_validate_set_columns(const char *value)
{
	const char *err = sift_cset_syntax(value);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid set_columns option \"%s\": %s", value, err)));
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
							 "Off by default: the bitmap scan returns rows (the column scans of SiftAgg aggregate them), see docs/COL-SCAN.md.",
							 &sift_enable_col_scan, false, PGC_USERSET, 0, NULL, NULL, NULL);
	sift_col_scan_init();
	DefineCustomBoolVariable("sift.col_rollup_on_demand",
							 "A scan that reads units for lack of their rollups asks the background worker to rebuild them.",
							 "Nothing is written by the query; the next one answers from the rollups. Off: the units stay as they are until a merge or REINDEX.",
							 &sift_col_rollup_on_demand, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.col_rollup_worker",
							 "A request for a rollup refresh starts the background worker that makes it.",
							 "Off: the requests wait in the queue until sift.col_rollup_refresh(index) is called.",
							 &sift_col_rollup_worker, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.col_auto_merge",
							 "After a VACUUM that left a sift_col index scattered by churn, a background worker seals its write buffer and merges all its segments into one.",
							 "The rule is sift.col_auto_merge_dirty_pct, _fragmentation and _buffer_pct; the rewrite sleeps as sift.maintenance_cost_delay says and is skipped "
							 "when the maintenance lock is held. Needs sift.async_maintenance; sift.col_maintain(index) applies the same rule from a job. Set it for the server or the database: the worker reads no session setting.",
							 &sift_col_auto_merge, false, PGC_SUSET, 0, NULL, NULL, NULL);
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
	DefineCustomRealVariable("sift.col_auto_merge_min_churn_pct",
							 "Percent of the rows of a sift_col index that must have changed since its last rewrite (dead, or written after it) for the automatic rewrite to run.",
							 "The rewrite writes the whole index, so the rows it has to fix must be worth it: 5 bounds the write amplification of the automatic rewrite at 20 bytes written per byte changed. "
							 "0 does not wait. sift.col_maintain(index) ignores it.",
							 &sift_col_auto_merge_min_churn_pct, 0.0, 0.0, 100.0, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_auto_merge_max_active",
							"Client backends of the database running a statement above which the automatic rewrite of a sift_col index waits for a quieter moment.",
							"The worker looks again after sift.maintenance_naptime, then twice as late each time, 8 times in all; the next VACUUM asks again. 0: never waits.",
							&sift_col_auto_merge_max_active, 0, 0, 10000, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.col_format_minor",
							"Column format minor of the segments written: 10, 9, 8, 7, 6, 4 to keep them readable by pg_sift 3.1, or 3 for pg_sift 3.0.",
							"Segments already written keep their minor; a 3.2 library reads them all. A segment of a schema with wide leaves is minor 5 at least, and the minor 6 "
							"of the complete kinds is written only for a schema with jsonb paths, from rows whose kinds are known. The minor 7 is written only for an index with the option cube_minmax, the minor 8 only for a sift_table with a timestamp column, the minor 9 only for an index with the option rollup_dims, the minor 10 only for one with the option set_columns.",
							&sift_col_format_minor, SIFT_COL_FORMAT_MINOR, SIFT_COL_FORMAT_MIN_READ, SIFT_COL_FORMAT_MINOR,
							PGC_SUSET, 0, check_col_format_minor, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomIntVariable("sift.test_col_rollup_panic",
							"Testing: the rollup refresh worker PANICs after it wrote the new segment, before it replaces the old one (1 = yes). Set it in postgresql.conf: the worker reads no session setting.",
							NULL, &sift_test_col_rollup_panic, 0, 0, 1, PGC_SIGHUP, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_merge_panic",
							 "Testing: a column merge (promotion excepted) PANICs after it wrote the new segment, before it replaces the old ones.",
							 NULL, &sift_test_col_merge_panic, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_refresh_sleep_ms",
							"Testing: the refresh of a segment (sift.col_refresh, the rollup worker) sleeps this long after the new segment is written and before it replaces the old one (0 = no).",
							NULL, &sift_test_col_refresh_sleep_ms, 0, 0, 60000, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_scan_jitter_us",
							"Testing: a column scan sleeps up to this many microseconds at the points where VACUUM or DML can interleave (0 = no).",
							NULL, &sift_test_col_jitter_us, 0, 0, 1000000, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_dict_cache",
							"Testing: kB of parsed segment dictionaries the backend keeps for the next query.",
							NULL, &sift_cdict_cache_kb, 65536, 0, INT_MAX, PGC_USERSET, GUC_UNIT_KB | GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
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
	add_bool_reloption(col_relopt_kind, "cube_minmax",
					   "The cube of every unit written from now on also holds the minimum and maximum of each integer measure per group; the segments are column format minor 7 (default off)",
					   false, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "rollup_dims",
						 "the scalar columns that the rollup of the rows of every unit written from now on groups by: 'col, col' or 'col, col: measure, ...' (default off; the segments are column format minor 8)",
						 "", sift_col_validate_rollup_dims, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "set_columns",
						 "the plain integer and text columns whose distinct values every unit written from now on keeps (a small-set summary that lets an equality or IN test skip units); the segments are column format minor 10 (default none)",
						 "", sift_col_validate_set_columns, ShareUpdateExclusiveLock);
	add_int_reloption(col_relopt_kind, "set_limit",
					  "Up to this many distinct values a unit keeps a column's small-set summary exactly, beyond it as a bloom filter",
					  SIFT_CSET_LIMIT, 2, SIFT_CSET_LIMIT_MAX, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "rollup",
						 "the leaves of array elements and map values that the rollup of every unit groups by: 'leaf, leaf, ...', or off",
						 "", sift_col_validate_rollup, ShareUpdateExclusiveLock);
}

static bytea *
col_amoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"write_buffer_limit", RELOPT_TYPE_INT, offsetof(SiftColOptions, write_buffer_limit)},
		{"dead_rewrite_pct", RELOPT_TYPE_INT, offsetof(SiftColOptions, dead_rewrite_pct)},
		{"vacuum_truncate", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, vacuum_truncate)},
		{"paths", RELOPT_TYPE_STRING, offsetof(SiftColOptions, paths)},
		{"compression", RELOPT_TYPE_STRING, offsetof(SiftColOptions, compression)},
		{"preagg", RELOPT_TYPE_STRING, offsetof(SiftColOptions, preagg)},
		{"rollup", RELOPT_TYPE_STRING, offsetof(SiftColOptions, rollup)},
		{"rollup_dims", RELOPT_TYPE_STRING, offsetof(SiftColOptions, rollup_dims)},
		{"rollup_on_demand", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, rollup_on_demand)},
		{"cube_minmax", RELOPT_TYPE_BOOL, offsetof(SiftColOptions, cube_minmax)},
		{"set_columns", RELOPT_TYPE_STRING, offsetof(SiftColOptions, set_columns)},
		{"set_limit", RELOPT_TYPE_INT, offsetof(SiftColOptions, set_limit)},
	};

	return (bytea *) build_reloptions(reloptions, validate, col_relopt_kind, sizeof(SiftColOptions), tab, lengthof(tab));
}

const SiftColOptions *
sift_col_options(Relation index)
{
	static const SiftColOptions defaults = {.write_buffer_limit = SIFT_DEFAULT_WRITE_BUFFER_LIMIT, .dead_rewrite_pct = SIFT_COL_DEAD_PCT, .rollup_on_demand = true, .vacuum_truncate = true};

	return index->rd_options ? (const SiftColOptions *) index->rd_options : &defaults;
}

const char *
sift_col_options_paths(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->paths ? (const char *) o + o->paths : "";
}

const char *
sift_col_options_compression(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->compression ? (const char *) o + o->compression : "";
}

const char *
sift_col_options_preagg(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->preagg ? (const char *) o + o->preagg : "";
}

void
sift_col_alter_check(Relation index)
{
	if (sift_col_is_index(index))
		(void) sift_col_rel(index);
}

const char *
sift_col_options_set_columns(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->set_columns ? (const char *) o + o->set_columns : "";
}

const char *
sift_col_options_rollup(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->rollup ? (const char *) o + o->rollup : "";
}

const char *
sift_col_options_rollup_dims(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->rollup_dims ? (const char *) o + o->rollup_dims : "";
}

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
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes do not support INCLUDE columns")));
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
		int			k = isjson ? (r->shred ? sift_col_shred_nleaves(r->shred, a) : 0) : 0;
		SiftColDef	lanes[SIFT_COL_MAX_LANES];

		if (!isjson)
			k = sift_col_att_leaves(att, lanes);
		if (isjson && k == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("the jsonb column \"%s\" of a sift_col index needs paths", NameStr(att->attname)),
					 errhint("Declare them with the paths option: WITH (paths = '%s.* float4')", NameStr(att->attname))));
		if (n + k > sift_limit(col_leaves, SIFT_COL_MAX_COLS))
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a sift_col index holds at most %d columns", (int) sift_limit(col_leaves, SIFT_COL_MAX_COLS))));
		r->first[a] = n;
		r->json[a] = isjson;
		r->name[a] = att->atttypid == NAMEOID;
		r->nleaves[a] = k;
		for (int i = 0; i < k; i++)
		{
			SiftColDef *d = &defs[n + i];

			if (isjson)
				sift_col_shred_leaf(r->shred, a, i, d);
			else
				*d = lanes[i];
			d->id = n + i;
			d->attno = a;
			d->parent = d->parent < 0 ? -1 : n + d->parent;
		}
		n += k;
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
		r->pagg.minmax = sift_col_options(index)->cube_minmax;
		if ((err = sift_cset_spec_parse(sift_col_options_set_columns(index), r->schema, (const char *const *) names, r->pagg.set)) != NULL)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid set_columns option: %s", err)));
		r->pagg.set_limit = sift_col_options(index)->set_limit;
	}
	r->meta = (SiftColIndexMeta) {.indexoid = RelationGetRelid(index), .heapoid = index->rd_index->indrelid, .natts = natts,
	.ncols = n,.cols = r->schema->cols};
	r->rowcxt = AllocSetContextCreate(mcxt, "sift cache col insert", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(old);
	return r;
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
			   *c = sift_col_options_rollup_dims(index);

	return hash_bytes((const unsigned char *) a, strlen(a)) ^ (hash_bytes((const unsigned char *) b, strlen(b)) * 31) ^
		(hash_bytes((const unsigned char *) c, strlen(c)) * 17) ^
		(sift_col_options(index)->cube_minmax ? 0x9e3779b9 : 0) ^ ((uint32) sift_col_format_minor * 0x85ebca6b);
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

		for (int c = 0; c < d->ncols; c++)
			sift_col_leaf_stats_init(&st[c]);
		for (int i = 0; i < dir->nsegs; i++)
		{
			SiftColSegment *seg;
			SiftCPaggCensus census;

			if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
				continue;
			seg = sift_cseg_open(index, &dir->segs[i], cx);
			for (int c = 0; c < d->ncols; c++)
				sift_cseg_leaf_stats(seg, c, &st[c]);
			sift_cseg_zone_sample(seg, zs);
			sift_cseg_pre_census(seg, spec, sift_col_format_minor, &census);
			pre->units += census.units;
			for (int k = 0; k < spec->ndims; k++)
			{
				pre->dim[spec->dim[k].leaf] += census.dim[k];
				pre->mm[spec->dim[k].leaf] += census.mm[k];
			}
			for (int k = 0; k < spec->nrollups; k++)
				if (spec->rollup[k].list >= 0)
					pre->roll[spec->rollup[k].list] += census.roll[k];
				else
					pre->rows += census.roll[k];
			sift_cseg_close(seg);
		}
		for (int i = 0; i < dir->nsegs; i++)
			if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE && dir->segs[i].format < SIFT_COL_FORMAT_KINDS)
				for (int c = 0; c < d->ncols; c++)
					st[c].nokinds |= sift_col_def_has_kinds(&d->cols[c]);
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
	BlockNumber allvis;
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
	visibilitymap_count(heap, &allvis, NULL);
	m->stats.heap_pages = relpages;
	m->stats.allvisible_frac = relpages > 0 ? (double) allvis / relpages : 0;
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
	int			ncols = r->schema->ncols;

	row->tid = *tid;
	row->ncols = ncols;
	row->val = palloc0(ncols * sizeof(SiftColValue));
	row->elem = palloc0(ncols * sizeof(SiftColValue *));
	for (int a = 1; a <= r->natts; a++)
	{
		const SiftColDef *def = &r->schema->cols[r->first[a]];

		if (r->json[a])
			continue;
		if (isnull[a - 1])
			for (int k = 0; k < r->nleaves[a]; k++)
				row->val[def->id + k].isnull = true;
		else
			sift_col_datum_values(def, values[a - 1], r->name[a], &row->val[def->id]);
	}
	if (r->shred)
	{
		int		   *first = r->first;

		sift_col_shred_row_rc(r->shred, values, isnull, row, first, mcxt, rc);
	}
	MemoryContextSwitchTo(old);
}

void
sift_col_make_row(SiftColRel *r, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row, MemoryContext mcxt)
{
	sift_col_make_row_rc(r, tid, values, isnull, row, mcxt, NULL);
}

bool
sift_col_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
				  IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	SiftColRel *r = sift_col_rel(index);
	SiftColRow	row;
	bool		crossed;

	sift_col_make_row(r, heap_tid, values, isnull, &row, r->rowcxt);
	crossed = sift_cwbuf_append(index, &r->hint, r->schema, &row);
	MemoryContextReset(r->rowcxt);
	if (crossed)
		sift_col_maint_after_seal(index);
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
	IndexAmRoutine *am = sift_am_routine(5, 1, true);

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
	am->amendscan = sift_col_amendscan;
	am->amsearcharray = am->amsearchnulls = true;

	PG_RETURN_POINTER(am);
}
