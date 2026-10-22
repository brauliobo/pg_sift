/*
 * core/limits.h - the registry of the fixed limits of the extension: one table, SIFT_LIMITS, read by the generator of docs/LIMITS.md
 * (scripts/gen-limits.sh), by scripts/check-limits.sh (a *_MAX_* or *_LIMIT* macro of src/ has a row or a reason to have none) and, in a
 * SIFT_TESTING build, by the setting sift.test_limits = 'id=value,...', which lowers a limit so that a test reaches it on a few rows.
 *
 * A row names a limit; it does not define it: value is the macro of its header (core/limits.c includes the headers), or the literal of a
 * macro that is private to a .c file or of a number that has no macro yet (macro "-"); check-limits.sh compares such a literal with the
 * #define. Columns:
 *   id        the name in sift.test_limits and in sift._test_limits()
 *   macro     the macro it mirrors, "-" if there is none
 *   file      where it is defined or checked, from src/
 *   value     the default, a constant expression
 *   unit      what the value counts
 *   what      the row of docs/LIMITS.md
 *   behaviour what the user sees at the limit: a normal plan (and its slug), an option rejected, an error
 *   setting   the setting or option that changes it, "-" if none
 *   hooked    1 when the code reads the limit through sift_limit(id, default), so sift.test_limits can lower it; 0 until the work that needs it
 *             wires the site (the setting refuses an id that is not hooked: a lowered limit that nothing reads would pass a test for the wrong reason)
 *
 * To lower a limit in a test: add or edit its row, replace the use at the site by sift_limit(id, MACRO) and set hooked to 1. A limit read in a
 * loop is read once before it. Outside a SIFT_TESTING build sift_limit(id, MACRO) is MACRO itself: no array, no setting, and a portable file
 * (portable.list) can use it.
 */
#ifndef SIFT_LIMITS_H
#define SIFT_LIMITS_H

#include <stdint.h>

#define SIFT_LIMITS(X) \
	X(partitions, "SIFT_AGG_MAX_PARTS", "agg/part.h", SIFT_AGG_MAX_PARTS, "partitions", \
	  "leaf partitions per SiftAgg scan, after PostgreSQL's pruning", "normal plan (slug partition_count, the detail has the count)", "-", 1) \
	X(hash_part_combos, "HASH_MAX_COMBOS", "agg/part.c", 1048576, "combinations", \
	  "value combinations the remainders of a hash level are computed for (once per level)", "the partitions of the level are kept (not pruned)", "-", 1) \
	X(part_key_tests, "KEYS_MAX_TESTS", "agg/part.c", 20000, "tests", \
	  "keys times range or list partitions a list of keys is tested against exactly", "only the range of the keys prunes", "-", 1) \
	X(joins, "SIFT_AGG_MAX_JOINS", "agg/spec.h", SIFT_AGG_MAX_JOINS, "tables", \
	  "lookup-joined tables per scan", "normal plan (slug shape_width)", "-", 1) \
	X(combos, "SIFT_AGG_MAX_COMBOS", "agg/spec.h", SIFT_AGG_MAX_COMBOS, "combinations", \
	  "value combinations per scan", "normal plan (slug shape_width)", "-", 1) \
	X(combo_entries, "COMBO_MAX_ENTRIES", "agg/combo.c", 16777216, "entries", \
	  "distinct input tuples of one value-combination cache", "the caches start again (re-evaluates the values they held)", "-", 1) \
	X(join_keys, "SIFT_AGG_MAX_JKEYS", "agg/spec.h", SIFT_AGG_MAX_JKEYS, "columns", \
	  "equality columns of a lookup join", "normal plan", "-", 0) \
	X(join_cols, "SIFT_AGG_MAX_JCOLS", "agg/spec.h", SIFT_AGG_MAX_JCOLS, "columns", \
	  "columns of the joined table that expressions read", "normal plan (slug shape_width)", "-", 1) \
	X(join_index_eqs, "SIFT_AGG_MAX_JIDX", "agg/spec.h", SIFT_AGG_MAX_JIDX, "equalities", \
	  "equalities of a joined-table column with a value, for the index scan that loads it", "normal plan", "-", 0) \
	X(lookup_kb, "-", "agg/plan.c", 262144, "kB", \
	  "estimated memory of the rows of a lookup table (a sub-select, VALUES list or function, a first-row child) or of a spine", "normal plan (slug join_child, spine_rows)", "sift.agg_lookup_max_kb", 0) \
	X(lookup_shared_bytes, "SHARE_MAX", "agg/par.c", 1073741824, "bytes", \
	  "a lookup table shared with the parallel workers through DSM", "each backend loads its own", "sift.agg_lookup_shared_kb", 0) \
	X(lookup_shared_slots, "LOOKSHM_SLOTS", "agg/lookshm.c", 64, "tables", \
	  "lookup tables shared through DSM at one time", "each backend loads its own", "sift.agg_lookup_shared_kb", 0) \
	X(topn_rows, "SIFT_TOPN_MAX_ROWS", "agg/topn.h", SIFT_TOPN_MAX_ROWS, "rows", \
	  "LIMIT + OFFSET a top-N scan keeps in a heap", "the scan sorts every row that passes (in memory up to hash_mem, then a tuplesort at work_mem) and the plan is priced with it", "-", 1) \
	X(topn_keys, "SIFT_TOPN_MAX_KEYS", "agg/topn.h", SIFT_TOPN_MAX_KEYS, "columns", \
	  "ORDER BY columns a top-N scan keeps in a heap", "the scan sorts every row that passes (in memory up to hash_mem, then a tuplesort at work_mem) and the plan is priced with it", "-", 1) \
	X(first_keys, "FIRST_MAX_KEYS", "agg/plan_order.c", 4, "columns", \
	  "sort columns after the DISTINCT ON columns", "normal plan", "-", 0) \
	X(first_order_cols, "SIFT_AGG_MAX_FORD", "agg/spec.h", SIFT_AGG_MAX_FORD, "columns", \
	  "ORDER BY columns of a first-row lookup child", "the sub-select runs once per key, as a lookup subquery", "-", 1) \
	X(first_out_cols, "SIFT_AGG_MAX_FOUT", "agg/spec.h", SIFT_AGG_MAX_FOUT, "columns", \
	  "output columns of a first-row lookup child that map to the table", "normal plan", "-", 0) \
	X(group_keys, "SIFT_AGG_MAX_KEYS", "agg/spec.h", SIFT_AGG_MAX_KEYS, "keys", \
	  "group keys (grouping sets count with their keys)", "normal plan (slug too_many_keys)", "-", 1) \
	X(num_in_list, "AGG_MAX_NUM_IN", "agg/plan_pred.c", 64, "constants", \
	  "constants of a numeric IN list compared one by one; a longer list is tested as a hash set", "a hash set of the scaled values", "-", 1) \
	X(arms, "SIFT_AGG_MAX_ARMS", "agg/spec.h", SIFT_AGG_MAX_ARMS, "arms", \
	  "UNION ALL arms of a fused or two-level scan", "normal plan (slug shape_width)", "-", 1) \
	X(virtual_leaves, "SIFT_AGG_MAX_VLEAVES", "agg/spec.h", SIFT_AGG_MAX_VLEAVES, "leaves", \
	  "virtual leaves (expressions) per scan", "normal plan (slug shape_width)", "-", 1) \
	X(layout_leaves, "SIFT_COLSRC_MAX_LEAVES", "agg/colsrc.h", SIFT_COLSRC_MAX_LEAVES, "leaves", \
	  "leaves of the layout of one scan", "normal plan", "-", 0) \
	X(scan_filters, "SIFT_COLSRC_MAX_FILTERS", "agg/colsrc.h", SIFT_COLSRC_MAX_FILTERS, "filters", \
	  "filters of a scan that skip units", "the others do not skip units", "-", 0) \
	X(wide_conjuncts, "SIFT_AGG_MAX_WIDE", "agg/spec.h", SIFT_AGG_MAX_WIDE, "conjuncts", \
	  "conjuncts on wide leaves per scan", "normal plan", "-", 0) \
	X(generic_leaves, "SIFT_AGG_MAX_GLEAVES", "agg/spec.h", SIFT_AGG_MAX_GLEAVES, "leaves", \
	  "leaves one argument of a generic aggregate reads", "normal plan", "-", 0) \
	X(generic_args, "SIFT_AGG_MAX_GARGS", "agg/spec.h", SIFT_AGG_MAX_GARGS, "arguments", \
	  "arguments of a generic aggregate (PostgreSQL FUNC_MAX_ARGS)", "normal plan", "-", 0) \
	X(float_ops, "SIFT_AGG_MAX_VX", "agg/spec.h", SIFT_AGG_MAX_VX, "operations", \
	  "operations of one float8 aggregate argument", "normal plan", "-", 0) \
	X(combo_leaves, "SIFT_AGG_MAX_CLEAVES", "agg/spec.h", SIFT_AGG_MAX_CLEAVES, "leaves", \
	  "leaves one value combination reads", "normal plan", "-", 0) \
	X(rows_shared, "SIFT_ROWS_MAX_SHARED", "agg/rows.h", SIFT_ROWS_MAX_SHARED, "aggregates", \
	  "aggregates sharing one rows-set (count(DISTINCT) grouping)", "normal plan (slug rows_shape)", "-", 1) \
	X(text_prune, "COL_MAX_STR_PRUNE", "agg/col_int.h", COL_MAX_STR_PRUNE, "conditions", \
	  "text conditions of a scan that prune units by prefix", "the others do not prune units", "-", 0) \
	X(filter_set, "FILTER_MAX_SET", "agg/init_filter.c", 8, "values", \
	  "values of an IN set sent as a zone filter", "a larger set is not used as a zone filter", "-", 0) \
	X(col_leaves, "SIFT_COL_MAX_COLS", "col/col.h", SIFT_COL_MAX_COLS, "leaves", \
	  "leaf slots of a sift_col index and columns of a sift_table (127 are usable)", "CREATE INDEX, CREATE TABLE or ALTER TABLE rejected", "-", 1) \
	X(string_bytes, "SIFT_COL_MAX_VALUE", "col/col.h", SIFT_COL_MAX_VALUE, "bytes", \
	  "one string value of a column leaf", "ERROR 54000 at build or merge", "-", 1) \
	X(unit_bytes, "SIFT_COL_UNIT_MAX_BYTES", "col/col.h", SIFT_COL_UNIT_MAX_BYTES, "bytes", \
	  "string bytes of one leaf in one column unit", "the values after it are stored as overflow and read from the heap (NOTICE)", "-", 1) \
	X(unit_rows, "SIFT_COL_UNIT_MAX_ROWS", "col/col.h", SIFT_COL_UNIT_MAX_ROWS, "rows", \
	  "rows of one column unit (256 heap pages)", "a new unit starts", "-", 0) \
	X(dict_entry, "SIFT_COL_DICT_MAX_ENTRY", "col/col.h", SIFT_COL_DICT_MAX_ENTRY, "bytes", \
	  "longest string that is a dictionary entry", "the unit stores its strings inline", "-", 0) \
	X(dict_bytes, "SIFT_COL_DICT_MAX_BYTES", "col/col.h", SIFT_COL_DICT_MAX_BYTES, "bytes", \
	  "one segment dictionary", "the unit stores its strings inline", "-", 0) \
	X(dict_cache_kb, "CDICT_CACHE_MAX_KB", "col/dictcache.c", 65536, "kB", \
	  "parsed segment dictionaries a backend keeps for its next query, with the lookup table and the sorted order built over them", \
	  "the least recently used unpinned dictionary is dropped; one over a quarter of it is kept for its scan only and searched entry by entry", "-", 1) \
	X(int_digits, "SIFT_COL_MAX_SCALE", "col/col.h", SIFT_COL_MAX_SCALE, "digits", \
	  "precision of a numeric the integer leaf class holds exactly", "the wide leaf class holds it", "-", 0) \
	X(wide_digits, "SIFT_COL_MAX_WIDE_SCALE", "col/wide.h", SIFT_COL_MAX_WIDE_SCALE, "digits", \
	  "precision of a numeric in a wide leaf", "ERROR 54000 at build or merge", "-", 0) \
	X(cube_dims, "SIFT_PAGG_MAX_DIMS", "col/preagg.h", SIFT_PAGG_MAX_DIMS, "dimensions", \
	  "dimensions of the cube of an index", "option rejected at CREATE or ALTER INDEX", "cube option", 0) \
	X(rollup_dims, "SIFT_PAGG_MAX_RDIMS", "col/preagg.h", SIFT_PAGG_MAX_RDIMS, "dimensions", \
	  "dimensions of one rollup", "option rejected at CREATE or ALTER INDEX", "rollup option", 0) \
	X(rollups, "SIFT_PAGG_MAX_ROLLUPS", "col/preagg.h", SIFT_PAGG_MAX_ROLLUPS, "rollups", \
	  "rollups of one index", "option rejected at CREATE or ALTER INDEX", "rollup option", 0) \
	X(cube_measures, "SIFT_PAGG_MAX_MEAS", "col/preagg.h", SIFT_PAGG_MAX_MEAS, "measures", \
	  "measures of a cube or rollup", "option rejected at CREATE or ALTER INDEX", "cube option", 0) \
	X(cube_groups, "SIFT_PAGG_DEF_GROUPS", "col/preagg.h", SIFT_PAGG_DEF_GROUPS, "groups", \
	  "groups of a default cube dimension in one unit", "the unit has no histogram for the dimension (it is scanned)", "-", 0) \
	X(calendar_groups, "SIFT_PAGG_CAL_GROUPS", "col/preagg.h", SIFT_PAGG_CAL_GROUPS, "groups", \
	  "groups of a calendar dimension (days, else months, else years) in one unit", "the unit tries the next grain, then has no histogram for the dimension (it is scanned)", "-", 0) \
	X(explicit_groups, "SIFT_PAGG_CUBE_GROUPS", "col/preagg.h", SIFT_PAGG_CUBE_GROUPS, "groups", \
	  "groups of a dimension of an explicit cube(...) in one unit", "the unit has no histogram for the dimension (it is scanned)", "cube option", 0) \
	X(rollup_tuples, "SIFT_PAGG_ROLLUP_TUPLES", "col/preagg.h", SIFT_PAGG_ROLLUP_TUPLES, "tuples", \
	  "tuples of an explicit rollup in one unit", "the unit has no rollup (it is scanned)", "rollup option", 0) \
	X(set_keys, "SIFT_CSET_LIMIT", "col/setsum.h", SIFT_CSET_LIMIT, "keys", \
	  "distinct keys of a set column a unit keeps exactly (default)", "no pruning by the set", "set_limit option", 0) \
	X(set_keys_max, "SIFT_CSET_LIMIT_MAX", "col/setsum.h", SIFT_CSET_LIMIT_MAX, "keys", \
	  "largest value of the set_limit option", "option rejected at CREATE or ALTER INDEX", "set_limit option", 0) \
	X(super_fan_max, "SIFT_SUPER_MAX_FAN", "col/preagg.h", SIFT_SUPER_MAX_FAN, "units", \
	  "largest value of the superunit option: units in one group summary", "option rejected at CREATE or ALTER INDEX", "superunit option", 0) \
	X(shred_spec_bytes, "SIFT_SHRED_MAX_SPEC_BYTES", "col/shred.h", SIFT_SHRED_MAX_SPEC_BYTES, "bytes", \
	  "size of a jsonb shredding spec", "ERROR at CREATE INDEX", "-", 0) \
	X(shred_leaves, "SIFT_SHRED_MAX_COLS", "col/shred.h", SIFT_SHRED_MAX_COLS, "leaves", \
	  "leaves of one jsonb shredding spec", "ERROR at CREATE INDEX", "-", 0) \
	X(shred_groups, "SIFT_SHRED_MAX_GROUPS", "col/shred.h", SIFT_SHRED_MAX_GROUPS, "groups", \
	  "arrays and wildcards of one jsonb shredding spec", "ERROR at CREATE INDEX", "-", 0) \
	X(shred_steps, "SIFT_SHRED_MAX_STEPS", "col/shred.h", SIFT_SHRED_MAX_STEPS, "steps", \
	  "steps of one jsonb path", "ERROR at CREATE INDEX", "-", 0) \
	X(shred_text, "SIFT_SHRED_MAX_TEXT", "col/shred.h", SIFT_SHRED_MAX_TEXT, "bytes", \
	  "one text value of a jsonb leaf", "the value is OVERFLOW (read from the heap)", "-", 0) \
	X(shred_key_bytes, "SIFT_SHRED_MAX_KEY_BYTES", "col/shred.h", SIFT_SHRED_MAX_KEY_BYTES, "bytes", \
	  "a key of a jsonb wildcard object", "the value is OVERFLOW (read from the heap)", "-", 0) \
	X(shred_keys, "SIFT_SHRED_DEFAULT_KEYS", "col/shred.h", SIFT_SHRED_DEFAULT_KEYS, "keys", \
	  "distinct keys of a jsonb wildcard object per segment", "the value is OVERFLOW (read from the heap); CREATE INDEX says how many units hold one (NOTICE)", "-", 1) \
	X(max_segments, "SIFT_MAX_SEGMENTS", "core/sift.h", SIFT_MAX_SEGMENTS, "segments", \
	  "directory entries of a sift or sift_col index (a sift_table has sift.table_max_segments for a test)", \
	  "writers merge the smallest closed segments from 16 entries below it; a forced seal leaves the write buffer open (NOTICE)", "-", 1) \
	X(term_bytes, "SIFT_MAX_TERM_BYTES", "core/sift.h", SIFT_MAX_TERM_BYTES, "bytes", \
	  "a BM25 token", "the token is its first 236 bytes and a hash of all of it", "-", 0) \
	X(term_tf, "SIFT_MAX_TF", "core/sift.h", SIFT_MAX_TF, "occurrences", \
	  "term frequency in one document", "saturated", "-", 0) \
	X(doc_length, "SIFT_MAX_DL", "core/sift.h", SIFT_MAX_DL, "tokens", \
	  "length of one document", "saturated", "-", 0) \
	X(fields, "SIFT_MAX_FIELDS", "core/sift.h", SIFT_MAX_FIELDS, "fields", \
	  "fields of a BM25 index", "CREATE INDEX rejected", "fields option", 0) \
	X(prefix_index, "SIFT_MAX_PREFIX_INDEX", "core/sift.h", SIFT_MAX_PREFIX_INDEX, "characters", \
	  "prefix_index option", "option rejected at CREATE INDEX", "prefix_index option", 0) \
	X(write_buffer_kb, "SIFT_DEFAULT_WRITE_BUFFER_LIMIT", "core/sift.h", SIFT_DEFAULT_WRITE_BUFFER_LIMIT, "kB", \
	  "default write buffer; a sift_table row that does not fit it needs sift.table_wide_rows", "ERROR 54000 for a larger sift_table row unless sift.table_wide_rows is on (then up to 1 GB)", "sift.table_wide_rows", 0) \
	X(table_vblocks, "-", "table/wbuf.c", 4294967294, "blocks", \
	  "virtual blocks of TIDs of a sift_table, never reused (sift.table_space: vblocks_left)", "ERROR 54000, VACUUM FULL renumbers them", "-", 0) \
	X(query_bytes, "SIFT_QUERY_MAX_BYTES", "bm25/query.h", SIFT_QUERY_MAX_BYTES, "bytes", \
	  "length of a search query", "ERROR 54000", "-", 1) \
	X(query_leaves, "SIFT_MAX_LEAVES", "core/sift.h", SIFT_MAX_LEAVES, "terms", \
	  "terms of a search query", "ERROR 54000", "-", 0) \
	X(query_depth, "SIFT_MAX_DEPTH", "core/sift.h", SIFT_MAX_DEPTH, "levels", \
	  "nesting of a search query", "ERROR 54000", "-", 0) \
	X(phrase_terms, "SIFT_MAX_PHRASE", "core/sift.h", SIFT_MAX_PHRASE, "terms", \
	  "terms of one phrase", "ERROR 54000", "-", 1) \
	X(boost, "SIFT_MAX_BOOST", "bm25/query.h", SIFT_MAX_BOOST, "factor", \
	  "combined boost of a query term", "ERROR 54000", "-", 0) \
	X(regex_bytes, "SIFT_MAX_REGEX_BYTES", "bm25/query.h", SIFT_MAX_REGEX_BYTES, "bytes", \
	  "a regular expression of a query", "ERROR 54000", "-", 0) \
	X(any_list_rows, "ANY_LIST_MAX_ROWS", "bm25/planner_path.c", 100000, "rows", \
	  "rows a query list (= ANY(array)) may match in one scan", "normal plan", "-", 0) \
	X(maint_backoff_ms, "BACKOFF_MAX_MS", "bm25/maint.c", 3600000, "ms", \
	  "longest wait before a failing background maintenance retries", "retried once an hour", "-", 0)

typedef enum SiftLimitId
{
#define X(id, macro, file, value, unit, what, behaviour, setting, hooked) SIFT_LIM_##id,
	SIFT_LIMITS(X)
#undef X
	SIFT_LIM_COUNT
} SiftLimitId;

#ifdef SIFT_TESTING
extern const int64_t sift_limit_default[SIFT_LIM_COUNT];
extern int64_t sift_limit_eff[SIFT_LIM_COUNT];
extern void sift_limits_init(void);
#define sift_limit(id, dflt) (sift_limit_eff[SIFT_LIM_##id])
#else
#define sift_limit(id, dflt) ((int64_t) (dflt))
#endif

#endif
