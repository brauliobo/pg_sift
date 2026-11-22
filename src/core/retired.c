/*
 * core/retired.c - the C entry points that the SQL of 5.1 names and the 6.0 library no longer implements.
 *
 * pg_proc keeps naming a C symbol until ALTER EXTENSION pg_sift UPDATE TO '6.0' runs in a database, and so does the frozen script of 5.1 that CREATE EXTENSION ... VERSION '5.1' replays:
 * without a symbol the call fails with 58P01 (undefined_file) and, worse for the script, so does the CREATE FUNCTION. Each symbol below is defined here once, as an error that says what to do.
 *
 * Two kinds. SIFT_RETIRED_FN: the SQL function lives on after the update as a thin alias of its 6.0 successor (sql/6.0/10-compat-aliases.sql rebinds it and this symbol is never called
 * again); the message asks for the update. SIFT_REMOVED_FN: the function is gone, and the message says so.
 *
 * The definitions are weak: a library that still implements one of these in another file (a Format 1 diagnostic, a trigger that Format 1 keeps) links that one, and this file is not edited.
 * scripts/check-retired.sh checks that every symbol here is named by the frozen script of 5.1.
 */
#include "postgres.h"

#include "fmgr.h"

#define SIFT_WEAK __attribute__((weak))

static _Noreturn void
sift_retired_error(const char *sql_name, const char *replacement)
{
	ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					errmsg("%s cannot run before ALTER EXTENSION pg_sift UPDATE TO '6.0'", sql_name),
					errhint("Run ALTER EXTENSION pg_sift UPDATE TO '6.0'; in this database; %s keeps working after it, and %s is its successor.", sql_name, replacement)));
}

static _Noreturn void
sift_removed_error(const char *sql_name)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("%s was removed in pg_sift 6.0", sql_name),
					errhint("See the pg_sift 6.0 upgrade notes for its replacement.")));
}

#define SIFT_DEFINE_ENTRY(c_sym, body)                                                      \
	extern PGDLLEXPORT const Pg_finfo_record *pg_finfo_##c_sym(void);                       \
	SIFT_WEAK const Pg_finfo_record *pg_finfo_##c_sym(void)                                 \
	{                                                                                       \
		static const Pg_finfo_record finfo = {1};                                           \
		return &finfo;                                                                      \
	}                                                                                       \
	extern PGDLLEXPORT Datum c_sym(PG_FUNCTION_ARGS);                                       \
	SIFT_WEAK Datum c_sym(PG_FUNCTION_ARGS)                                                 \
	{                                                                                       \
		body;                                                                               \
	}

#define SIFT_RETIRED_FN(c_sym, sql_name, repl) SIFT_DEFINE_ENTRY(c_sym, sift_retired_error(sql_name, repl))
#define SIFT_REMOVED_FN(c_sym, sql_name) SIFT_DEFINE_ENTRY(c_sym, sift_removed_error(sql_name))

/* Format Major 1 diagnostics and advice (13) */
SIFT_RETIRED_FN(sift_col_index_advice,		"sift.index_advice",		"sift.index_choices()")
SIFT_RETIRED_FN(sift_col_key_cut_decision,	"sift.key_cut_decision",	"sift.index_choices()")
SIFT_RETIRED_FN(sift_col_arena_status,		"sift.col_arena_status",	"sift.status()")
SIFT_RETIRED_FN(sift_col_stamps,			"sift.col_stamps",			"sift.parts()")
SIFT_RETIRED_FN(sift_col_track_convert,		"sift.col_track_convert",	"nothing: it has no work left")
SIFT_RETIRED_FN(sift_col_tracking,			"sift.col_tracking",		"sift.status()")
SIFT_RETIRED_FN(sift_col_unit_aggs_sql,		"sift.col_unit_agg",		"sift.col_granules()")
SIFT_RETIRED_FN(sift_col_unit_hists_sql,	"sift.col_unit_hist",		"sift.col_granules()")
SIFT_RETIRED_FN(sift_col_rollup_counters,	"sift.col_rollup_counters",	"sift.activity()")
SIFT_RETIRED_FN(sift_col_rollup_queue,		"sift.col_rollup_queue",	"sift.activity()")
SIFT_RETIRED_FN(sift_col_rollup_refresh,	"sift.col_rollup_refresh",	"sift.maintain()")
SIFT_RETIRED_FN(sift_col_rollup_misses,		"sift.col_rollup_misses",	"sift.explain_why()")
SIFT_RETIRED_FN(sift_col_shrink_history,	"sift.col_shrink_history",	"sift.space()")

/* Per access method functions that sift.check, promote, merge, maintain, space, parts and stats replace, and the diagnostics of the shared value store (17) */
SIFT_RETIRED_FN(sift_verify,				"sift.verify",				"sift.check()")
SIFT_RETIRED_FN(sift_col_verify,			"sift.col_verify",			"sift.check()")
SIFT_RETIRED_FN(sift_table_verify,			"sift.table_verify",		"sift.check()")
SIFT_RETIRED_FN(sift_col_promote,			"sift.col_promote",			"sift.promote()")
SIFT_RETIRED_FN(sift_col_merge,				"sift.col_merge",			"sift.merge()")
SIFT_RETIRED_FN(sift_col_maintain,			"sift.col_maintain",		"sift.maintain()")
SIFT_RETIRED_FN(sift_table_maintain,		"sift.table_maintain",		"sift.maintain()")
SIFT_RETIRED_FN(sift_index_info,			"sift.index_info",			"sift.space()")
SIFT_RETIRED_FN(sift_col_index_info,		"sift.col_index_info",		"sift.space()")
SIFT_RETIRED_FN(sift_col_space,				"sift.col_space",			"sift.space()")
SIFT_RETIRED_FN(sift_table_space,			"sift.table_space",			"sift.space()")
SIFT_RETIRED_FN(sift_col_segments,			"sift.col_segments",		"sift.parts()")
SIFT_RETIRED_FN(sift_table_segments,		"sift.table_segments",		"sift.parts()")
SIFT_RETIRED_FN(sift_agg_refusal_actions,	"sift.refusal_actions",		"sift.refusal_slugs()")
SIFT_RETIRED_FN(sift_stats,					"sift.stats",				"sift.stats(true)")
SIFT_RETIRED_FN(sift_col_value_store_usage,	"sift.col_value_store_usage",	"sift.status()")
SIFT_RETIRED_FN(sift_col_value_store_members,	"sift.col_value_store_members",	"sift.status()")

/* Gone: the statement trigger of track_changes and the two helpers of the shared value store of a column, which the tree of this release no longer has (3); the two diagnostics of the store live on as empty SQL aliases */
SIFT_REMOVED_FN(sift_col_track_old_rows,		"sift.col_track_old_rows")
SIFT_REMOVED_FN(sift_col_value_store_begin,		"sift._col_value_store_begin")
SIFT_REMOVED_FN(sift_col_value_store_end,		"sift._col_value_store_end")
