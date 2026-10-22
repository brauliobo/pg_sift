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
	  "leaf partitions per SiftAgg scan", "normal plan (slug partition_source)", "-", 0) \
	X(joins, "SIFT_AGG_MAX_JOINS", "agg/spec.h", SIFT_AGG_MAX_JOINS, "tables", \
	  "lookup-joined tables per scan", "normal plan", "-", 0) \
	X(combos, "SIFT_AGG_MAX_COMBOS", "agg/spec.h", SIFT_AGG_MAX_COMBOS, "combinations", \
	  "value combinations per scan", "normal plan", "-", 0) \
	X(combo_entries, "-", "agg/combo.c", 16777216, "entries", \
	  "distinct input tuples of one value-combination cache", "ERROR 54000 mid-query", "-", 0) \
	X(topn_rows, "SIFT_TOPN_MAX_ROWS", "agg/topn.h", SIFT_TOPN_MAX_ROWS, "rows", \
	  "LIMIT + OFFSET of a top-N scan", "normal plan (slug topn_shape)", "-", 0) \
	X(topn_keys, "SIFT_TOPN_MAX_KEYS", "agg/topn.h", SIFT_TOPN_MAX_KEYS, "columns", \
	  "ORDER BY columns of a top-N scan", "normal plan (slug topn_shape)", "-", 0) \
	X(spine_rows, "SPINE_MAX_ROWS", "agg/plan_two.c", 10000, "rows", \
	  "rows of a spine joined to the arms of a two-level scan", "normal plan (slug spine_rows)", "-", 0) \
	X(unit_bytes, "-", "col/batch.c", 2147483647, "bytes", \
	  "variable-length bytes of the rows of one column unit", "ERROR 54000 at build or merge", "-", 0) \
	X(string_bytes, "SIFT_COL_MAX_VALUE", "col/col.h", SIFT_COL_MAX_VALUE, "bytes", \
	  "one string value of a column leaf", "ERROR 54000 at build or merge", "-", 0) \
	X(max_segments, "SIFT_MAX_SEGMENTS", "core/sift.h", SIFT_MAX_SEGMENTS, "segments", \
	  "directory entries of an index or sift_table", "writers merge, then ERROR 54000", "-", 0) \
	X(query_bytes, "SIFT_QUERY_MAX_BYTES", "bm25/query.h", SIFT_QUERY_MAX_BYTES, "bytes", \
	  "length of a search query", "ERROR 54000", "-", 1) \
	X(phrase_terms, "SIFT_MAX_PHRASE", "core/sift.h", SIFT_MAX_PHRASE, "terms", \
	  "terms of one phrase", "ERROR 54000", "-", 1)

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
