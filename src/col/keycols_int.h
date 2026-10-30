/*
 * col/keycols_int.h - what the files of the build-time defaults (keycols*.c) share.
 */
#ifndef SIFT_COL_KEYCOLS_INT_H
#define SIFT_COL_KEYCOLS_INT_H

#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/rel.h"

#include "col/col.h"

/*
 * Before a build: the columns that get the global order array (order_array_columns, col/gorder.h) when the option is unset or 'auto' (not 'off', not a list). A
 * range of a column of up to SIFT_CORDER_MAX_ROWS rows reads the entries between its bounds, where the units are all read when the values are scattered over the
 * heap (their min and max cover the range): 18.4M instructions against 2.6M for 790 of 800,000 rows. It costs the array (4.4 bytes a row for an int4, 5.6 for a
 * timestamp) and the sorted run it extends (2.2 and 3.5), so it is built where it pays only, from the statistics of the table: a column of GORD_AUTO_DISTINCT or
 * more distinct values (fewer are answered by a key list, a point directory or the units) whose correlation with the heap order is under GORD_AUTO_CORR (a
 * column in heap order has narrow zones), on a table of GORD_AUTO_ROWS or more rows (below it a scan of the units is within 100k instructions), not a column
 * that cuts the units, and an array of no more than GORD_AUTO_GROWTH_PCT% of the index: the bytes of the
 * index a row are estimated from the statistics of its columns (the bits of their distinct values, the values of a dictionary (strings, uuids), at most their width), so a narrow index
 * of a few small columns gets none. At most GORD_AUTO_MAX columns: the dates and timestamps first (a range and an ORDER BY .. LIMIT of a time column is the common
 * use of an order), then the text columns (ORDER BY .. LIMIT of strings up to GORD_TEXT_WIDTH bytes, by the default formula like a time column), then the integers, each the
 * least correlated first. A table of GORD_AUTO_ROWS rows or more with no statistics is analyzed first (ANALYZE, once, with a
 * NOTICE), because the choice reads them. A REINDEX decides again.
 */
#define GORD_AUTO_ROWS		100000
#define GORD_AUTO_DISTINCT	1000
#define GORD_AUTO_CORR		0.99
#define GORD_AUTO_GROWTH_PCT 25
#define GORD_AUTO_MAX		2
#define GORD_TIME_SPEEDUP	25.0	/* S of the shape a time column's array is for: ts >= x ORDER BY ts LIMIT 10 on scattered values, 9.39 M to 0.38 M instructions at 500,000 rows (ORDER BY ts DESC LIMIT 10 10x, OFFSET 1000 11x) */
#define GORD_TIME_WRITE_PCT	35.0	/* W: CREATE INDEX of a one-column timestamp index 1.031 G to 1.387 G instructions (500,000 rows) */
#define GORD_TEXT_SPEEDUP	300.0	/* S of ORDER BY txt LIMIT 10 on a table at the 100,000-row floor: 82.8 M to 0.27 M instructions (3,000,000 rows 2,429 M to 0.86 M, so the floor is the least) */
#define GORD_TEXT_WRITE_C	62.0	/* W: CREATE INDEX of (id, text, timestamp, int4) with the text array under the C collation 2.84 G to 4.62 G instructions (500,000 rows) */
#define GORD_TEXT_WRITE_LOC	420.0	/* ... under a locale (pt_BR.UTF-8), 2.83 G to 14.73 G: the sort of the strings by strcoll. Both for 4 columns; the share of a wider index is smaller */
#define GORD_TEXT_WIDTH		64		/* the array holds the strings again (8.8 bytes a row for 9-byte strings, 38 for 61, 100 for 101: the index grows 77%, 158%, 330%): a column of longer strings is left to order_array_columns by name */
#define GORD_ROW_OVERHEAD	1.3		/* the bytes a row of an index beyond the bits of its distinct values (units, zones, presence): 3.73 measured against 2.4 estimated for one timestamp column */
#define GORD_GROUP_DISTINCT	100		/* the group of a time column's arrays: an integer of this many distinct values or more (an entity), at most one in GORD_GROUP_DEPTH rows of the table apiece (the 24 bytes of a group stay under a tenth of the 5 of an entry) */
#define GORD_GROUP_DEPTH	64
#define GORD_GROUP_BYTES	6.0		/* the second order of a timestamp: its 5.6 bytes a row and the groups (the structure's share of the room, as an array's) */

extern AttrNumber	keycols_heap_attno(Relation index, const SiftColDef *d);
extern bool	keycols_col_stats(Relation heap, AttrNumber hatt, double *ndistinct, double *corr, int32 *width);
extern bool	keycols_ends_in_id(const char *name);
extern bool	keycols_is_fk_column(Relation heap, AttrNumber attno);
extern const char *	keycols_leaf_name(Relation index, const SiftColDef *d);
extern double	keycols_est_index_bytes(Relation heap, Relation index);
extern void	keycols_analyze_if_unknown(Relation heap, Relation index);
extern void	keycols_append_name(StringInfo s, const char *name);

extern void	keycols_auto_array_specs(Relation heap, Relation index, StringInfo spec, StringInfo only, StringInfo both);
extern double	keycols_est_col_bytes(Relation heap, const FormData_pg_attribute *att, double d, int32 width);
#endif	/* SIFT_COL_KEYCOLS_INT_H */
