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
 * (portable.list) can use it. A limit that work_mem or another setting gives (the row shows its default) is read through sift_limit_or(id, derived):
 * the setting's value, unless a test lowered the limit.
 */
#ifndef SIFT_LIMITS_H
#define SIFT_LIMITS_H

#include <stdint.h>

#define SIFT_LIMITS(X) \
	X(partitions, "SIFT_AGG_MAX_PARTS", "agg/part.h", SIFT_AGG_MAX_PARTS, "partitions", \
	  "leaf partitions per SiftAgg scan, after PostgreSQL's pruning; no more than hash memory (partition_memory at least) holds at 8 kB a partition (2,048 at the default work_mem)", "normal plan (slug partition_count, the detail has the count)", "-", 1) \
	X(partition_memory, "SIFT_AGG_PART_FLOOR", "agg/part.h", SIFT_AGG_PART_FLOOR, "bytes", \
	  "what the partitions of a scan may keep together when hash memory is smaller (SIFT_AGG_PART_STATE, 8 kB, a partition)", "normal plan (the partition limit follows it)", "-", 1) \
	X(hash_part_combos, "HASH_MAX_COMBOS", "agg/part/prune.c", 1048576, "combinations", \
	  "value combinations the remainders of a hash level are computed for (once per level)", "the partitions of the level are kept (not pruned)", "-", 1) \
	X(part_key_tests, "KEYS_MAX_TESTS", "agg/part/prune.c", 20000, "tests", \
	  "keys times range or list partitions a list of keys is tested against exactly", "only the range of the keys prunes", "-", 1) \
	X(group_spill_parts, "SPILL_MAX_PARTS", "agg/spill.c", 128, "partitions", \
	  "partitions a group table that outgrows hash memory is written to (the planner's groups over the groups the first table held, a power of two from 8)", "a partition that does not fit is partitioned again", "work_mem, hash_mem_multiplier", 1) \
	X(group_spill_depth, "SPILL_MAX_DEPTH", "agg/spill_int.h", 8, "levels", \
	  "levels of partitions a partition of a spilled group table is partitioned again (a hash of the same key values, another level each time)", "ERROR 54000 naming work_mem and hash_mem_multiplier", "work_mem, hash_mem_multiplier", 1) \
	X(joins, "SIFT_AGG_MAX_JOINS", "agg/spec/expr.h", SIFT_AGG_MAX_JOINS, "tables", \
	  "lookup-joined tables per scan", "normal plan (slug shape_width)", "-", 1) \
	X(combos, "SIFT_AGG_MAX_COMBOS", "agg/spec/expr.h", SIFT_AGG_MAX_COMBOS, "combinations", \
	  "value combinations per scan", "normal plan (slug shape_width)", "-", 1) \
	X(combo_entries, "COMBO_MAX_ENTRIES", "agg/combo/int.h", 16777216, "entries", \
	  "distinct input tuples of one value-combination cache", "the caches start again (re-evaluates the values they held)", "-", 1) \
	X(combo_cache_bytes, "-", "agg/combo/ents.c", 4194304, "bytes", \
	  "memory the cache of one value combination holds, and the values of its dictionary leaves: half of hash memory, shared by the participants of the scan and its combos (4 MB by default for one)", \
	  "the cache starts again (re-evaluates the values it held)", "work_mem, hash_mem_multiplier", 1) \
	X(join_keys, "SIFT_AGG_MAX_JKEYS", "agg/spec/expr.h", SIFT_AGG_MAX_JKEYS, "columns", \
	  "equality columns of a lookup join", "normal plan", "-", 0) \
	X(join_cols, "SIFT_AGG_MAX_JCOLS", "agg/spec/expr.h", SIFT_AGG_MAX_JCOLS, "columns", \
	  "columns of the joined table that expressions read", "normal plan (slug shape_width)", "-", 1) \
	X(join_index_eqs, "SIFT_AGG_MAX_JIDX", "agg/spec/expr.h", SIFT_AGG_MAX_JIDX, "equalities", \
	  "equalities of a joined-table column with a value, for the index scan that loads it", "normal plan", "-", 0) \
	X(merge_fanin, "MERGE_MAX_FANIN", "col/v1/maint/maint_int.h", 64, "segments", \
	  "segments a step of a merge of sift_col segments opens at once (fewer when maintenance_work_mem holds fewer inputs of 192 kB)", "the step merges the cheapest window of them; a forced merge goes on in passes", "maintenance_work_mem", 0) \
	X(lookup_kb, "-", "agg/plan/finish.c", 2048, "kB", \
	  "estimated memory of the rows of a lookup table (a joined table, sub-select, VALUES list or function, a first-row child) or of a spine: half of work_mem (4 MB by default)", "normal plan (slug join_child, spine_rows)", "work_mem", 1) \
	X(semi_lut_bytes, "-", "agg/init.c", 4194304, "bytes", \
	  "the byte table of the keys of a semi, anti or inner join by one integer key (its range of values) or the set of its keys, for each combination of the scan: its share of work_mem", \
	  "the scan's key filter tests the keys, else the join is evaluated row by row", "work_mem", 1) \
	X(param_set_elems, "AGG_PARAM_SET_ELEMS", "agg/plan/base.c", 1000, "elements", \
	  "elements a generic plan is priced for when its set filter is one array parameter (the planner assumes 10)", "the paths that fetch the rows of the set one by one are priced for the elements of the planner's guess", "work_mem", 1) \
	X(lookup_shared_bytes, "SHARE_MAX", "agg/par/int.h", 1073741824, "bytes", \
	  "a lookup table shared with the parallel workers through DSM", "each backend loads its own", "sift.agg_lookup_shared_kb", 0) \
	X(lookup_shared_slots, "LOOKSHM_SLOTS", "agg/lookshm.c", 64, "tables", \
	  "lookup tables shared through DSM at one time", "each backend loads its own", "sift.agg_lookup_shared_kb", 0) \
	X(topn_rows, "SIFT_TOPN_MAX_ROWS", "agg/topn.h", SIFT_TOPN_MAX_ROWS, "rows", \
	  "LIMIT + OFFSET a top-N scan keeps in a heap", "the scan sorts every row that passes (in memory up to hash_mem, then a tuplesort at work_mem) and the plan is priced with it", "-", 1) \
	X(topn_keys, "SIFT_TOPN_MAX_KEYS", "agg/topn.h", SIFT_TOPN_MAX_KEYS, "columns", \
	  "ORDER BY columns a top-N scan keeps in a heap", "the scan sorts every row that passes (in memory up to hash_mem, then a tuplesort at work_mem) and the plan is priced with it", "-", 1) \
	X(topn_code_entries, "ORD_MAX_ENTRIES", "agg/topn/int.h", 1048576, "entries", \
	  "entries of the dictionary of a segment a top-N scan makes an order code for (8 bytes an entry, up to 8 dictionaries at a time)", "the rows of its units are compared with the bar as bytes, as before the order code", "-", 1) \
	X(skip_pending, "GORD_PEND_MAX", "agg/gord/int.h", 1048576, "entries", \
	  "the entries an OFFSET over a global order leaves to the heap to confirm, and the entries of one group of an order read group after group that it checks at once (12 bytes each); a longer one is walked", \
	  "the entries the skip leaves are read one by one, as without the skip (the rows are the same)", "-", 1) \
	X(gord_seek_pct, "SIFT_CGORD_SEEK_MAX_PCT", "col/v1/gorder.h", 115, "percent", \
	  "the bytes a seekable chunk of an order array (a fence key every 32 entries) may take, against the plain chunk of the same entries: the writer keeps the seekable form of a chunk up to this; 0 writes none", \
	  "the chunk is written plain and read by the old walk (the rows are the same)", "-", 1) \
	X(gord_pos_pct, "SIFT_CGORD_POS_MAX_PCT", "col/v1/gorder.h", 75, "percent", \
	  "the bytes a positional chunk of a grouped order array (its entries as positions in the plain order of the leaf, read through it) may take, against the other form of the same entries: the writer keeps the positional form of a chunk up to this; 0 writes none", \
	  "the chunk keeps the keys and addresses of its entries (the rows are the same)", "-", 1) \
	X(gord_pos_gap, "SIFT_CGORD_POS_MAX_GAP", "col/v1/gorder.h", 1024, "entries", \
	  "the mean distance between the positions of a group in a chunk of a grouped order array that the writer keeps positional (as many entries as two chunks of the plain order): farther apart, each entry a scan reads is in a chunk of the plain order of its own", \
	  "the chunk keeps the keys and addresses of its entries (the rows are the same)", "-", 1) \
	X(gord_mem_rows, "SIFT_CGORD_MEM_MAX", "col/v1/gorder.h", SIFT_CGORD_MEM_MAX, "rows", \
	  "rows a top-N, range or count by an order array sorts in memory for one source: the write buffers' and those of the segments the overlays left without the array (a text key under a locale: at least 16,384 or 8 for each row wanted)", \
	  "the scan reads the units, and the planner prices no walk of the array (the rows are the same)", "-", 1) \
	X(gord_free_rows, "SIFT_CGORD_FREE_MAX", "col/v1/gorder.h", SIFT_CGORD_FREE_MAX, "rows", \
	  "rows of the segments that keep no order array of a leaf (the overlays written lean) that a top-N, range or count by the array of the leaf decodes and sorts in memory once for the backend (the first scan; the next find the order kept)", \
	  "the scan reads the units, and the planner prices no walk of the array (the rows are the same)", "-", 1) \
	X(blob_bytes, "BLOB_CHUNK_BYTES", "agg/blob/dump.c", 268435456, "bytes", \
	  "the partial result a worker of a parallel aggregate sends in one message: the groups of its table go in messages of about this many bytes (a pair of a count(DISTINCT) takes 8); a group that alone takes more than four times this (1 GB at most) ends the scan", \
	  "more messages (the leader adds them one after the other); ERROR 54000 for a single group that takes more than four times it, naming max_parallel_workers_per_gather", "-", 1) \
	X(first_keys, "FIRST_MAX_KEYS", "agg/order/int.h", 4, "columns", \
	  "sort columns after the DISTINCT ON columns", "normal plan", "-", 0) \
	X(first_order_cols, "SIFT_AGG_MAX_FORD", "agg/spec/join.h", SIFT_AGG_MAX_FORD, "columns", \
	  "ORDER BY columns of a first-row lookup child", "the sub-select runs once per key, as a lookup subquery", "-", 1) \
	X(first_out_cols, "SIFT_AGG_MAX_FOUT", "agg/spec/join.h", SIFT_AGG_MAX_FOUT, "columns", \
	  "output columns of a first-row lookup child that map to the table", "normal plan", "-", 0) \
	X(group_keys, "SIFT_AGG_MAX_KEYS", "agg/spec/expr.h", SIFT_AGG_MAX_KEYS, "keys", \
	  "group keys (grouping sets count with their keys)", "normal plan (slug too_many_keys)", "-", 1) \
	X(num_in_list, "AGG_MAX_NUM_IN", "agg/plan_pred.c", 64, "constants", \
	  "constants of a numeric IN list compared one by one; a longer list is tested as a hash set", "a hash set of the scaled values", "-", 1) \
	X(bits_set_values, "MIN_BITS_VALUES", "agg/pred_in.c", 1024, "values", \
	  "members of an IN list from which a range wider than a byte table (4 million values) is tested in a bit table of at most half of hash memory (up to 2^28 values); a shorter list or a wider range is a hash table", "a hash table of the values", "-", 1) \
	X(dist_narrow_words, "DIST_NARROW_WORDS", "agg/dist_int.h", 8, "words", \
	  "words of a group in a narrow count(DISTINCT) set (64 values a word, one window of values for all the groups); 0 turns the form off", "the set is a bitmap, a directory and chunks of 4096 bits a group", "-", 1) \
	X(arms, "SIFT_AGG_MAX_ARMS", "agg/spec/expr.h", SIFT_AGG_MAX_ARMS, "arms", \
	  "UNION ALL arms of a fused or two-level scan", "normal plan (slug shape_width)", "-", 1) \
	X(virtual_leaves, "SIFT_AGG_MAX_VLEAVES", "agg/spec/expr.h", SIFT_AGG_MAX_VLEAVES, "leaves", \
	  "virtual leaves (expressions) per scan", "normal plan (slug shape_width)", "-", 1) \
	X(layout_leaves, "SIFT_COLSRC_MAX_LEAVES", "col/v1/src_meta.h", SIFT_COLSRC_MAX_LEAVES, "leaves", \
	  "leaves of the layout of one scan", "normal plan", "-", 0) \
	X(scan_filters, "SIFT_COLSRC_MAX_FILTERS", "col/v1/src_exec.h", SIFT_COLSRC_MAX_FILTERS, "filters", \
	  "filters of a scan that skip units", "the others do not skip units", "-", 0) \
	X(wide_conjuncts, "SIFT_AGG_MAX_WIDE", "agg/spec/expr.h", SIFT_AGG_MAX_WIDE, "conjuncts", \
	  "conjuncts on wide leaves per scan", "normal plan", "-", 0) \
	X(generic_leaves, "SIFT_AGG_MAX_GLEAVES", "agg/spec/expr.h", SIFT_AGG_MAX_GLEAVES, "leaves", \
	  "leaves one argument of a generic aggregate reads", "normal plan", "-", 0) \
	X(generic_args, "SIFT_AGG_MAX_GARGS", "agg/spec/expr.h", SIFT_AGG_MAX_GARGS, "arguments", \
	  "arguments of a generic aggregate (PostgreSQL FUNC_MAX_ARGS)", "normal plan", "-", 0) \
	X(float_ops, "SIFT_AGG_MAX_VX", "agg/spec/expr.h", SIFT_AGG_MAX_VX, "operations", \
	  "operations of one float8 aggregate argument", "normal plan", "-", 0) \
	X(combo_leaves, "SIFT_AGG_MAX_CLEAVES", "agg/spec/expr.h", SIFT_AGG_MAX_CLEAVES, "leaves", \
	  "leaves one value combination reads", "normal plan", "-", 0) \
	X(rows_shared, "SIFT_ROWS_MAX_SHARED", "agg/rows.h", SIFT_ROWS_MAX_SHARED, "aggregates", \
	  "aggregates sharing one rows-set (count(DISTINCT) grouping)", "normal plan (slug rows_shape)", "-", 1) \
	X(text_prune, "COL_MAX_STR_PRUNE", "agg/chunkfilter.h", COL_MAX_STR_PRUNE, "conditions", \
	  "text conditions of a scan that prune units by prefix", "the others do not prune units", "-", 0) \
	X(filter_set, "FILTER_MAX_SET", "agg/init_filter.c", 8, "values", \
	  "values of an IN set sent as a zone filter", "a larger set is not used as a zone filter", "-", 0) \
	X(merge_disk_peak, "-", "col/v1/maint/maint.c", 2, "times the live index", \
	  "disk the index needs while a merge or a rewrite runs: the output is written before the inputs are freed (measured 2.02, 1.86 and 2.07 times the live index), and the next VACUUM gives the end of the file back", "the step fails with PostgreSQL's disk-full error before it replaces a segment", "-", 0) \
	X(col_leaves, "SIFT_COL_MAX_COLS", "col/api/format.h", SIFT_COL_MAX_COLS, "leaves", \
	  "leaf slots of a sift_col index of format major 1 and columns of a sift_table of format major 1 (127 are usable)", "CREATE INDEX, CREATE TABLE or ALTER TABLE rejected", "-", 1) \
	X(f2_leaves, "SIFT_F2_MAX_COLS", "col/api/format.h", SIFT_F2_MAX_COLS, "leaves", \
	  "leaf slots (columns and shredded jsonb paths) of a sift_col index of format major 2", "CREATE INDEX rejected", "-", 1) \
	X(tbl_f2_columns, "SIFT_TBL_F2_MAX_COLS", "col/api/format.h", SIFT_TBL_F2_MAX_COLS, "columns", \
	  "columns of a sift_table of format major 2 (the 1,600 of PostgreSQL)", "CREATE TABLE or ALTER TABLE rejected", "-", 0) \
	X(string_bytes, "SIFT_COL_MAX_VALUE", "col/api/format.h", SIFT_COL_MAX_VALUE, "bytes", \
	  "one string value of a column leaf", "ERROR 54000 at build or merge", "-", 1) \
	X(unit_bytes, "SIFT_COL_UNIT_MAX_BYTES", "col/api/format.h", SIFT_COL_UNIT_MAX_BYTES, "bytes", \
	  "string bytes of one leaf in one column unit", "the values after it are stored as overflow and read from the heap (NOTICE)", "-", 1) \
	X(unit_rows, "SIFT_COL_UNIT_MAX_ROWS", "col/api/format.h", SIFT_COL_UNIT_MAX_ROWS, "rows", \
	  "rows of one column unit (256 heap pages)", "a new unit starts", "-", 0) \
	X(dict_entry, "SIFT_COL_DICT_MAX_ENTRY", "col/api/format.h", SIFT_COL_DICT_MAX_ENTRY, "bytes", \
	  "longest string that is a dictionary entry", "the unit stores its strings inline", "-", 0) \
	X(dict_bytes, "SIFT_COL_DICT_MAX_BYTES", "col/api/format.h", SIFT_COL_DICT_MAX_BYTES, "bytes", \
	  "one segment dictionary", "the unit stores its strings inline", "-", 0) \
	X(par_build_rows, "PAR_BUILD_ROWS", "col/v1/build_par.c", 150000, "rows", \
	  "rows a participant of a parallel CREATE INDEX of a sift_col index takes at the least (a worker is launched for each further 150,000 rows of the table)", \
	  "fewer workers are launched, down to the build of the leader alone: one segment", "max_parallel_maintenance_workers, the parallel_workers option of the table", 1) \
	X(pre_census_units, "-", "col/am/cache.c", 16, "units", \
	  "units of each segment of an index the planner counts the pre-aggregate sections of (spread over the segment)", \
	  "the share of the units that hold a section is that of the units counted; a segment of fewer units is counted whole", "-", 1) \
	X(dict_lookup_min, "CDICT_LOOKUP_MIN", "col/v1/seg_write/seg_write_int.h", 1024, "strings", \
	  "strings a segment dictionary needs before it is written in blocks (and with the codes in byte order for a lookup structure), so that a fetch or a lookup reads it in place (column format minor 14)", \
	  "the dictionary is written in the compact form and a fetch or a lookup loads it whole", "dict_lookup option", 1) \
	X(dict_tsig_min, "CDICT_TSIG_MIN_KB", "col/v1/seg_write/dict.c", 1024, "kB", \
	  "bytes of the strings a segment dictionary in blocks needs before it is written with a trigram signature of each block, once the dictionary of its leaf, scaled to the index, no longer fits the parsed ones a backend keeps (dict_cache_kb), so that a substring test reads the blocks that may hold the part and not all of them (column format minor 29)", \
	  "no signature: a substring test reads every block of the dictionary", "-", 1) \
	X(lst_min_units, "LST_MIN_UNITS", "col/v1/seg_write/seg_write_int.h", 64, "units", \
	  "units a segment needs before it keeps its leaf statistics after its sections, so that a plan reads a few pages of it, not the zone arrays of every leaf", \
	  "no statistics block: the plan reads the zone arrays of the segment (a few pages)", "-", 1) \
	X(mirror_bytes, "SEG_MIRROR_BYTES", "col/v1/seg_write/seg_write_int.h", 6144, "bytes", \
	  "bytes of the header page of a segment that keep a copy of the small structures a plan and a first read of the segment open (the schema and the unit directory, the point directories, the order headers, the headers of the pre-aggregates), so that they cost no pages of their own", \
	  "no copy: each of them is read from its own page of the segment", "-", 1) \
	X(lst_share_div, "LST_SHARE_DIV", "col/v1/seg_write/seg_write.c", 33, "1/n of a segment", \
	  "the share of its segment a leaf statistics block may take: a segment whose block would take more than 1/n of its bytes (3% at 33; units of a few rows and many leaves) keeps none", \
	  "no statistics block: the plan reads the zone arrays of the segment", "-", 1) \
	X(int_digits, "SIFT_COL_MAX_SCALE", "col/api/leaf.h", SIFT_COL_MAX_SCALE, "digits", \
	  "precision of a numeric the integer leaf class holds exactly", "the wide leaf class holds it", "-", 0) \
	X(wide_digits, "SIFT_COL_MAX_WIDE_SCALE", "core/wide_datum.h", SIFT_COL_MAX_WIDE_SCALE, "digits", \
	  "precision of a numeric in a wide leaf", "ERROR 54000 at build or merge", "-", 0) \
	X(cube_dims, "SIFT_PAGG_MAX_DIMS", "col/v1/preagg.h", SIFT_PAGG_MAX_DIMS, "dimensions", \
	  "dimensions of the cube of an index", "option rejected at CREATE or ALTER INDEX", "cube option", 0) \
	X(rollup_dims, "SIFT_PAGG_MAX_RDIMS", "col/v1/preagg.h", SIFT_PAGG_MAX_RDIMS, "dimensions", \
	  "dimensions of one rollup", "option rejected at CREATE or ALTER INDEX", "rollup option", 0) \
	X(rollups, "SIFT_PAGG_MAX_ROLLUPS", "col/v1/preagg.h", SIFT_PAGG_MAX_ROLLUPS, "rollups", \
	  "rollups of one index", "option rejected at CREATE or ALTER INDEX", "rollup option", 0) \
	X(cube_measures, "SIFT_PAGG_MAX_MEAS", "col/v1/preagg.h", SIFT_PAGG_MAX_MEAS, "measures", \
	  "measures of a cube or rollup", "option rejected at CREATE or ALTER INDEX", "cube option", 0) \
	X(cube_groups, "SIFT_PAGG_DEF_GROUPS", "col/v1/preagg.h", SIFT_PAGG_DEF_GROUPS, "groups", \
	  "groups of a default cube dimension in one unit", "the unit has no histogram for the dimension (it is scanned)", "-", 0) \
	X(calendar_groups, "SIFT_PAGG_CAL_GROUPS", "col/v1/preagg.h", SIFT_PAGG_CAL_GROUPS, "groups", \
	  "groups of a calendar dimension (days, else months, else years) in one unit", "the unit tries the next grain, then has no histogram for the dimension (it is scanned)", "-", 0) \
	X(explicit_groups, "SIFT_PAGG_CUBE_GROUPS", "col/v1/preagg.h", SIFT_PAGG_CUBE_GROUPS, "groups", \
	  "groups of a dimension of an explicit cube(...) in one unit", "the unit has no histogram for the dimension (it is scanned)", "cube option", 0) \
	X(rollup_tuples, "SIFT_PAGG_ROLLUP_TUPLES", "col/v1/preagg.h", SIFT_PAGG_ROLLUP_TUPLES, "tuples", \
	  "tuples of an explicit rollup in one unit", "the unit has no rollup (it is scanned)", "rollup option", 0) \
	X(set_keys, "SIFT_CSET_LIMIT", "col/v1/setsum.h", SIFT_CSET_LIMIT, "keys", \
	  "distinct keys of a column of key_columns a unit keeps exactly in a summary, a bloom filter beyond", "a filter instead of the keys", "-", 0) \
	X(set_keys_max, "SIFT_CSET_LIMIT_MAX", "col/v1/setsum.h", SIFT_CSET_LIMIT_MAX, "keys", \
	  "distinct keys of a stored exact summary", "the summary is a bloom filter", "-", 0) \
	X(presence_pct, "SIFT_CSET_PRES_PCT", "col/v1/setsum.h", SIFT_CSET_PRES_PCT, "percent", \
	  "share of a unit's bytes the key lists and bitmaps of all its key_columns may take together", "a column past the share has no key list or bitmap in the unit (it is scanned)", "key_columns option", 0) \
	X(super_fan_max, "SIFT_SUPER_MAX_FAN", "col/v1/preagg.h", SIFT_SUPER_MAX_FAN, "units", \
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
	X(shred_elems, "SIFT_SHRED_DEFAULT_ELEMS", "col/shred.h", SIFT_SHRED_DEFAULT_ELEMS, "elements", \
	  "list and map elements of one document a unit stores", "the value is OVERFLOW (read from the heap: the scans that need it read the whole document again, which holds it in memory as SQL does)", "-", 1) \
	X(shred_keys, "SIFT_SHRED_DEFAULT_KEYS", "col/shred.h", SIFT_SHRED_DEFAULT_KEYS, "keys", \
	  "distinct keys of a jsonb wildcard object per segment", "the value is OVERFLOW (read from the heap); CREATE INDEX says how many units hold one (NOTICE)", "-", 1) \
	X(eset_rows, "SIFT_ESET_ROWS", "col/v1/spool.h", SIFT_ESET_ROWS, "entries", \
	  "the entries of one order array that a segment writer keeps in memory (16 bytes each, a leaf holds the entries of its values and of its NULL rows apart) before it writes them to a temp file as a sorted run", \
	  "none: past it the entries go to temp files (an error only from temp_file_limit or a full disk)", "-", 1) \
	X(wide_dict_bytes, "-", "table/v1/wide.c", 1073741823, "bytes", \
	  "the distinct values of a wide column (a numeric past 18 digits, an unconstrained numeric, money, an interval) in the rows of one unit of a sift_table, as the scan keeps them to compare and group", \
	  "ERROR 54000 naming the column", "-", 1) \
	X(max_segments, "SIFT_DIR_POLICY_MAX", "core/page.h", SIFT_DIR_POLICY_MAX, "segments", \
	  "directory entries a writer of a sift or sift_col index keeps it within (a sift_table has sift.table_max_segments for a test); a directory of more is a chain of pages, which holds 32,768", \
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
	  "default write buffer; a sift_table row that does not fit it is accepted with sift.table_wide_rows (on)", "ERROR 54000 for a larger sift_table row only when sift.table_wide_rows is off (on, the default: up to 1 GB)", "sift.table_wide_rows", 0) \
	X(table_vblocks, "-", "table/v1/wbuf.c", 4294967294, "blocks", \
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
	X(maint_backoff_ms, "BACKOFF_MAX_MS", "core/maint.c", 3600000, "ms", \
	  "longest wait before a failing background maintenance retries", "retried once an hour", "-", 0) \
	X(semi_set, "SIFT_SEMI_MAX_SET", "bm25/planner.h", 65536, "rows", \
	  "rows of a table whose joined keys a ranked scan tests the INCLUDE column against (the planner offers a table of at most this many rows, the scan stops at this many keys)", "the join keeps every row the scan returns (the rows of the table are checked after it, as without INCLUDE columns)", "-", 0) \
	X(key_store_codes, "SIFT_KEYSTORE_MAX_CODES", "col/keystore/keystore.h", SIFT_KEYSTORE_MAX_CODES, "codes", \
	  "keys of a scope key store (the distinct text, varchar or uuid values of a scope key column)", \
	  "ERROR 54000 before the strings are appended", "-", 1) \
	X(dict_block_bytes, "SIFT_DICT_BLOCK_MAX_BYTES", "bm25/dict.h", SIFT_DICT_BLOCK_MAX_BYTES, "bytes", \
	  "one block of 32 terms of the dictionary of a BM25 segment, header included (a block takes about 22 kB at most: terms of 255 bytes with their inline postings and positions)", \
	  "ERROR 54000 at build or merge", "-", 1) \
	X(scope_batch_entries, "SIFT_SCOPE_BATCH_MAX_ENTRIES", "col/scope.h", SIFT_SCOPE_BATCH_MAX_ENTRIES, "entries", \
	  "entries (a row of the batch and a scope key) that the rows of one batch expand to when a scope index is built, promoted or refilled: each row has an entry for every key of its member, 12 bytes each", \
	  "ERROR 54000 naming max_multiplicity", "-", 1) \
	X(rows_groups, "SIFT_ROWS_MAX_GROUPS", "agg/rows.h", SIFT_ROWS_MAX_GROUPS, "groups", \
	  "groups the node that groups rows (SiftAggRows) holds: their count and the arrays of their keys and aggregates are 32 bits", \
	  "normal plan (NOTICE), ERROR 54000 once rows were sent", "-", 1) \
	X(quarantine_entries, "QUAR_ENTRIES", "core/quarantine_int.h", 256, "index files", \
	  "index files the registry of the server holds out of service (sift.quarantine); the metapage of an index holds it out as well once a worker has flagged it (persisted)", \
	  "the entry of the oldest index that is persisted is taken, else the oldest: an index whose entry was taken without a flag is in service again until its damage is read again", "-", 1) \
	X(shared_tables, "DIR_ENTRIES", "col/dsmreg.c", 1024, "tables", \
	  "named shared tables (caches and registries in dynamic shared memory) of the libraries loaded in the server at one time; a table lasts as long as a backend maps it", \
	  "the table is not made and the backend goes without that cache (sift.shared_memory() says why)", "-", 0) \
	X(arena_slots, "ARENA_SLOTS", "col/v1/wbuf/arena.c", 256, "write buffers", \
	  "write buffers of sift_col indexes that keep the values of their rows in the shared arena at one time (the test hook is sift._test_arena_slots)", \
	  "a row that finds no slot is written as a record that holds its values; the buffer written longest ago is sealed to give its slot back", "-", 0) \
	X(progress_slots, "PROGRESS_SLOTS", "core/progress.c", 64, "jobs", \
	  "maintenance jobs that report their progress at one time (sift.progress)", \
	  "the job runs and is not listed", "-", 0) \
	X(rollup_queue, "ROLLUP_QUEUE", "col/v1/rollup.c", 64, "segments", \
	  "segments waiting for the rollup refresh worker, for all databases (sift.col_rollup_queue)", \
	  "the request is dropped and counted (sift.col_rollup_counters); the scan reads the units and a later scan asks again", "-", 0) \
	X(rollup_workers, "ROLLUP_WORKERS", "col/v1/rollup.c", 8, "databases", \
	  "databases with a rollup refresh worker running", \
	  "no worker is started for the database; its requests wait in the queue, and sift.col_rollup_refresh(index) refreshes them in a session", "-", 0) \
	X(par_build_workers, "MAX_WORKERS", "col/v1/build_par.c", 15, "workers", \
	  "workers of a parallel CREATE INDEX of a sift_col index besides the leader (the directory is shared by the participants and leaves 7 segments to each of 16)", \
	  "fewer workers are launched than max_parallel_maintenance_workers allows", "max_parallel_maintenance_workers", 0) \
	X(rank_batch_leaves, "RANK_BATCH_MAX_LEAVES", "bm25/bounds.c", 4, "terms", \
	  "scoring leaves of a top-k query up to which the batched block-max ranking is used", \
	  "normal plan: the query is ranked by max-score over the superblocks instead", "-", 0) \
	X(rank_batch_units, "RANK_BATCH_MAX_UNITS", "bm25/bounds.c", 2048, "units", \
	  "ranking units of a top-k query up to which the batched block-max ranking is used", \
	  "normal plan: the query is ranked by max-score over the superblocks instead", "-", 0) \
	X(prefetch_blocks, "SIFT_PREFETCH_MAX_BLKS", "core/store.h", 512, "blocks", \
	  "blocks of segment pages one wave of the read-ahead of a ranked query asks for", \
	  "the next wave starts after the blocks of this one", "-", 0) \
	X(shm_epoch_slots, "SIFT_SHM_EPOCH_SLOTS", "core/shm_manifest.h", 1024, "backends", \
	  "backends whose epoch the registry of the manifests in shared memory tracks: a snapshot is pinned without a lock by the backends numbered below it", \
	  "a backend numbered above it pins a snapshot under the lock of its slot", "-", 0) \
	X(star_join_parts, "STAR_MAX_PARTS", "agg/v6/v6_star_run.c", 65536, "parts", \
	  "parts one small relation of a star join is read in when it does not fit hash memory: each part is one more pass over the facts", \
	  "ERROR 54000 naming work_mem and hash_mem_multiplier", "work_mem, hash_mem_multiplier", 0) \
	X(proj_cols, "SIFT_PROJ_MAX_COLS", "col/f2/f2_proj.h", SIFT_PROJ_MAX_COLS, "columns", \
	  "key columns, INCLUDE columns and measures of one projection (each list)", \
	  "sift.create_projection refuses the spec (too many columns)", "-", 0) \
	X(kll_cols, "SIFT_F2_KLL_MAX_COLS", "col/f2/f2_kll.h", SIFT_F2_KLL_MAX_COLS, "columns", \
	  "columns a part keeps a quantile sketch of; the sketches option names at most twice this many columns", \
	  "ERROR 54000 naming the option", "-", 0) \
	X(point_cols, "SIFT_POINT_MAX_COLS", "col/f2/f2_point_dir.h", SIFT_POINT_MAX_COLS, "columns", \
	  "columns of a part that get a point directory: the ones with the most distinct values", \
	  "the other columns are read as before (the rows are the same)", "-", 0) \
	X(point_comp, "SIFT_POINT_MAX_COMP", "col/f2/f2_point_dir.h", SIFT_POINT_MAX_COMP, "columns", \
	  "columns of one key of a point directory (a key of several columns is nearly all distinct only together)", \
	  "a key of more columns is not made (the rows are the same)", "-", 0) \
	X(point_values, "SIFT_POINT_MAX_VALUES", "col/f2/f2_point_dir.h", SIFT_POINT_MAX_VALUES, "values", \
	  "constants of an equality or IN list the point directory answers", \
	  "the granules are read (the rows are the same)", "-", 0) \
	X(defaults_attnum, "SIFT_DEFAULTS_MAX_ATTNUM", "col/f2/f2_defaults.h", SIFT_DEFAULTS_MAX_ATTNUM, "attributes", \
	  "highest attribute number a part records the default of an added column for (the PostgreSQL limit of the columns of a table)", \
	  "a record that names more is damage (the part is not read)", "-", 0) \
	X(defaults_bytes, "SIFT_DEFAULTS_MAX_VALUE", "col/f2/f2_defaults.h", SIFT_DEFAULTS_MAX_VALUE, "bytes", \
	  "the default of a text column added to a Format 2.0 table, which the parts record", \
	  "the default is refused: a default value is larger than a part keeps", "-", 0) \
	X(win_keys, "SIFT_WIN_MAX_KEYS", "agg/window.h", 4, "keys", \
	  "PARTITION BY keys, and ORDER BY keys, of a window the window operator runs", \
	  "normal plan", "-", 0) \
	X(win_funcs, "SIFT_WIN_MAX_FUNCS", "agg/window.h", 8, "functions", \
	  "window functions of one window the window operator runs", \
	  "normal plan", "-", 0) \
	X(win_offset, "SIFT_WIN_MAX_OFFSET", "agg/window.h", 4096, "rows", \
	  "LAG and LEAD offset the window operator reads", \
	  "normal plan", "-", 0) \
	X(win_regroup_rows, "RG_MAX_ROWS", "agg/v6/v6_win_par.c", 67108863, "rows", \
	  "rows a window of a SiftAgg scan holds while it puts them together by PARTITION BY (no array of them passes 1 GB; work_mem bounds them sooner at its default)", \
	  "ERROR 54000 naming work_mem", "work_mem", 0) \
	X(win_regroup_groups, "RG_MAX_GROUPS", "agg/v6/v6_win_par.c", 33554431, "partitions", \
	  "partitions a window of a SiftAgg scan holds while it puts its rows together by PARTITION BY (no array of their keys passes 1 GB; work_mem bounds them sooner at its default)", \
	  "ERROR 54000 naming work_mem", "work_mem", 0) \
	X(num_expr_bits, "-", "agg/v6/v6_expr_num.c", 95, "bits", \
	  "magnitude of the scaled integer a row of an exact numeric product takes in a SiftAgg scan (below 2^95, so that 2^32 rows add up in the 128 bits of a sum)", \
	  "ERROR 22003, the hint names sift.enable_agg = off", "-", 0) \
	X(num_factors, "SIFT_VN_MAX_FACTORS", "agg/v6/v6_expr_num.h", 3, "factors", \
	  "factors of an exact numeric product (a * b * c) of a SiftAgg scan, each a numeric column with a constant added and a constant multiplied", \
	  "normal plan", "-", 0) \
	X(v6_derived, "SIFT_V6_MAX_DERIVE", "agg/v6/v6.h", 8, "expressions", \
	  "different computed keys and arguments (length, extract, CASE, arithmetic, exact numeric products) of one SiftAgg scan", \
	  "normal plan", "-", 0) \
	X(v6_case_conds, "SIFT_V6_MAX_CASE", "agg/v6/v6.h", 4, "conditions", \
	  "equalities ANDed in the WHEN of the CASE of a computed text key or argument of a SiftAgg scan (the columns of an arithmetic expression of an unnest are one more)", \
	  "normal plan", "-", 0) \
	X(arith_tokens, "SIFT_ARITH_MAX_TOKENS", "agg/v6/v6_plan_rewrite.h", 32, "tokens", \
	  "columns, constant and operators of an arithmetic expression of float8 columns in the scan of an unnest (postfix)", \
	  "normal plan", "-", 0) \
	X(arith_stack, "SIFT_ARITH_MAX_STACK", "agg/v6/v6_plan_rewrite.h", 8, "values", \
	  "operands an arithmetic expression of the scan of an unnest holds at once while it is computed", \
	  "normal plan", "-", 0) \
	X(v6_topk_items, "SIFT_V6_MAX_TOPK_ITEMS", "agg/v6/v6.h", 8, "columns", \
	  "ORDER BY columns of the Top-K of the groups or rows of a SiftAgg scan", \
	  "normal plan", "-", 0) \
	X(v6_topk_k, "SIFT_TOPK_MAX_K", "agg/v6/v6_group_topk.h", 65536, "rows", \
	  "LIMIT + OFFSET of the Top-K of the groups of a SiftAgg scan (ORDER BY .. LIMIT)", \
	  "normal plan (a Sort of the groups)", "-", 0) \
	X(v6_lock_k, "SIFT_LOCK_MAX_K", "agg/v6/v6_plan_rewrite.h", 4096, "rows", \
	  "LIMIT + OFFSET of a GROUP BY .. LIMIT with no ORDER BY, HAVING or DISTINCT ON, for which the group table stops taking new groups once it holds them", \
	  "the table takes every group, as without a LIMIT", "-", 0) \
	X(search_arms, "SEARCH_MAX_ARMS", "agg/search.c", 16, "arms", \
	  "arms of the ORs of searches a scan takes: the product of the branches of its ORs (the plain matches and one branch of each OR, ANDed)", \
	  "the OR stays a filter of the scan", "-", 0) \
	X(winx_keys, "SIFT_WINX_MAX_KEYS", "agg/v6/v6_datebin_win.h", SIFT_WINX_MAX_KEYS, "keys", \
	  "PARTITION BY keys, and ORDER BY keys, of a window of a SiftAgg scan", \
	  "normal plan", "-", 0) \
	X(winx_funcs, "SIFT_WINX_MAX_FUNCS", "agg/v6/v6_datebin_win.h", SIFT_WINX_MAX_FUNCS, "functions", \
	  "window functions of a window of a SiftAgg scan", \
	  "normal plan", "-", 0) \
	X(winx_filters, "SIFT_WINX_MAX_FILTERS", "agg/v6/v6_datebin_win.h", SIFT_WINX_MAX_FILTERS, "filters", \
	  "FILTER clauses of the window functions of a SiftAgg scan", \
	  "normal plan", "-", 0) \
	X(winx_offset, "SIFT_WINX_MAX_OFFSET", "agg/v6/v6_datebin_win.h", SIFT_WINX_MAX_OFFSET, "rows", \
	  "LAG and LEAD offset of a window of a SiftAgg scan", \
	  "normal plan", "-", 0) \
	X(vexpr_regs, "SIFT_VEXPR_MAX_REGS", "agg/vexpr.h", 16, "registers", \
	  "value registers of a compiled expression", \
	  "the expression is not compiled: normal plan", "-", 0) \
	X(vexpr_masks, "SIFT_VEXPR_MAX_MASKS", "agg/vexpr.h", 16, "masks", \
	  "boolean slots of a compiled expression", \
	  "the expression is not compiled: normal plan", "-", 0) \
	X(vexpr_ops, "SIFT_VEXPR_MAX_OPS", "agg/vexpr.h", 64, "operations", \
	  "operations of a compiled expression", \
	  "the expression is not compiled: normal plan", "-", 0) \
	X(vexpr_args, "SIFT_VEXPR_MAX_ARGS", "agg/vexpr.h", 17, "arguments", \
	  "arguments of a call, and arms of a CASE, in a compiled expression", \
	  "the expression is not compiled: normal plan", "-", 0) \
	X(stream_aggs, "SIFT_V6_STREAM_MAX_AGGS", "agg/v6/v6_stream.h", 16, "aggregates", \
	  "aggregates one stream node folds", \
	  "normal plan", "-", 0) \
	X(stream_filters, "SIFT_V6_STREAM_MAX_FILTERS", "agg/v6/v6_stream.h", 4, "filters", \
	  "distinct FILTER clauses one stream node folds", \
	  "normal plan", "-", 0) \
	X(pivot_filters, "SIFT_PIVOT_MAX_FILTERS", "agg/v6/v6_filter_pivot.h", SIFT_PIVOT_MAX_FILTERS, "filters", \
	  "distinct FILTER clauses of the aggregates of one scan, evaluated once per batch", \
	  "normal plan", "-", 0) \
	X(v6_join_keys, "JOIN_MAX_KEYS", "agg/v6/join.c", 8, "columns", \
	  "equality column pairs of a join of two SiftAgg scans", \
	  "normal plan", "-", 0) \
	X(maint_workers, "SIFT_MAINT_MAX_WORKERS", "core/maint_sched.h", SIFT_MAINT_MAX_WORKERS, "workers", \
	  "background maintenance workers the scheduler runs at one time", \
	  "a larger sift.max_maint_workers runs this many", "sift.max_maint_workers", 0) \
	X(gcache_frames, "SIFT_F2_GCACHE_MAX_FRAMES", "table/f2/f2_gcache.h", SIFT_F2_GCACHE_MAX_FRAMES, "frames", \
	  "frames of 256 values of the strings and doubles of sift_table the cache of a backend keeps", \
	  "the oldest frame is dropped and inflated again when a row asks for it", "-", 0) \
	X(gcache_granules, "SIFT_F2_GCACHE_MAX_GRANULES", "table/f2/f2_gcache.h", SIFT_F2_GCACHE_MAX_GRANULES, "granules", \
	  "decoded granules of sift_table the cache of a backend keeps", \
	  "the oldest granule is dropped and read again when needed", "-", 0) \
	X(gcache_blocks, "SIFT_F2_GCACHE_MAX_BLOCKS", "table/f2/f2_gcache.h", SIFT_F2_GCACHE_MAX_BLOCKS, "blocks", \
	  "decoded row pack blocks of 64 rows of sift_table the cache of a backend keeps", \
	  "the oldest block is dropped and read again when a row asks for it", "-", 0) \
	X(zstd_dict_bytes, "SIFT_ZSTD_DICT_MAX_BYTES", "col/compress.h", SIFT_ZSTD_DICT_MAX_BYTES, "bytes", \
	  "the dictionary of the zstd compression of a part", \
	  "a larger dictionary is not written; the part is compressed without it", "-", 0) \
	X(par_dist, "SIFT_PAR_MAX_DIST", "agg/v6/v6_par.h", 32, "aggregates", \
	  "count(DISTINCT), sum(DISTINCT) and avg(DISTINCT) aggregates of one scan a parallel plan runs", \
	  "the scan runs without parallel workers", "-", 0) \
	X(first_cols, "SIFT_V6_FIRST_MAX_COLS", "agg/v6/v6_first.h", SIFT_V6_FIRST_MAX_COLS, "columns", \
	  "ORDER BY columns of a DISTINCT ON that SiftAggV6First keeps the first row of each group by (the DISTINCT ON columns are among them)", \
	  "the normal plan sorts the rows and keeps the first of each group", "-", 0) \
	X(float_sum_rows, "V6FSUM_MAX_ROWS", "agg/v6/v6_fsum.h", 137438953472, "rows", \
	  "rows one group adds in the float8 sum or average of a Format Major 2 scan (its bins are 64-bit integer sums of 26-bit digits)", \
	  "an error (the float8 sum is over more rows than the bins hold)", "-", 0)

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
#define sift_limit_or(id, derived) (sift_limit_eff[SIFT_LIM_##id] != sift_limit_default[SIFT_LIM_##id] ? sift_limit_eff[SIFT_LIM_##id] : (int64_t) (derived))
#else
#define sift_limit(id, dflt) ((int64_t) (dflt))
#define sift_limit_or(id, derived) ((int64_t) (derived))
#endif

#endif
