/*
 * col/keycols/keycols_int.h - what the files of the build-time defaults (keycols/) share.
 */
#ifndef SIFT_COL_KEYCOLS_INT_H
#define SIFT_COL_KEYCOLS_INT_H

#include "postgres.h"

#include "lib/stringinfo.h"

#include "col/col.h"
#include "col/enc.h"

/*
 * Before a build: the columns that get the global order array (order_array_columns, col/gorder.h) when the option is unset or 'auto' (not 'off', not a list). A
 * range of a column of up to SIFT_CORDER_MAX_ROWS rows reads the entries between its bounds, where the units are all read when the values are scattered over the
 * heap (their min and max cover the range): 18.4M instructions against 2.6M for 790 of 800,000 rows. It costs the array (4.4 bytes a row for an int4, 5.6 for a
 * timestamp) and the sorted run it extends (2.2 and 3.5), so it is built where it pays only, from the statistics of the table: a column of GORD_AUTO_DISTINCT or
 * more distinct values (fewer are answered by a key list, a point directory or the units) whose correlation with the heap order is under GORD_AUTO_CORR (a
 * column in heap order has narrow zones), on a table of GORD_AUTO_ROWS or more rows (below it a scan of the units is within 100k instructions), not a column
 * that cuts the units. The bytes of the
 * index a row are estimated from the statistics of its columns (the bits of their distinct values, the values of a dictionary (strings, uuids), at most their width), so a narrow index
 * of a few small columns gets none. At most GORD_AUTO_MAX columns: the dates and timestamps first (a range and an ORDER BY .. LIMIT of a time column is the common
 * use of an order), then the text columns (ORDER BY .. LIMIT of strings up to GORD_TEXT_WIDTH bytes, by the default formula like a time column), then the integers, each the
 * least correlated first. A table of GORD_AUTO_ROWS rows or more with no statistics is analyzed first (ANALYZE, once, with a
 * NOTICE), because the choice reads them. A REINDEX decides again.
 */
#define GORD_AUTO_ROWS		100000
#define GORD_AUTO_DISTINCT	1000
#define GORD_AUTO_CORR		0.99
#define POINT_AUTO_UNIQUE	0.5		/* the distinct values of a column over the rows of the table: nearly unique, a key of the point directory or of an order array of its own (keycols_point_key) */
#define GORD_KEYORDER_ENTITIES	10	/* the fewest entities the key_order layout serves: ORDER BY entity, ts OFFSET 10,000 costs 2.3 to 2.8 times the wall time of the plain choice with 5, 1.96 with 10 */
#define GORD_AUTO_MAX		2
#define GORD_TEXT_SPEEDUP	300.0	/* S of ORDER BY txt LIMIT 10 on a table at the 100,000-row floor: 82.8 M to 0.27 M instructions (3,000,000 rows 2,429 M to 0.86 M, so the floor is the least) */
#define GORD_TEXT_WRITE_C	62.0	/* W: CREATE INDEX of (id, text, timestamp, int4) with the text array under the C collation 2.84 G to 4.62 G instructions (500,000 rows) */
#define GORD_TEXT_WRITE_LOC	420.0	/* ... under a locale (pt_BR.UTF-8), 2.83 G to 14.73 G: the sort of the strings by strcoll. Both for 4 columns; the share of a wider index is smaller */
#define GORD_TIES_OFFSET	10000	/* a date or timestamp column of fewer than GORD_AUTO_DISTINCT values is chosen by the default formula too, for ORDER BY col, key LIMIT n OFFSET GORD_TIES_OFFSET: the units read every row of the table, the array the rows of the groups the offset ends in */
#define GORD_TIES_SPEEDUP	0.37	/* S = this * rows / (GORD_TIES_OFFSET + the rows of the largest group, the NULL ones or a value's): 300,000 rows of a date, 365 values 113.4 M to 10.6 M instructions (S 10.7), 100 values 113.6 to 14.9 (7.6), 30 values 114.7 to 27.9 (4.1), 3 values 207 to 215 (0.96) */
#define GORD_TIES_BYTES		3.0		/* the bytes a row of that array (2.3 to 3.3 measured for a date) */
#define GORD_TIES_WRITE		11.0	/* W: CREATE INDEX of 7 columns with a date array 475 ms to 530 ms; the share of a wider index is smaller */
#define GORD_TEXT_WIDTH		64		/* the array holds the strings again (8.8 bytes a row for 9-byte strings, 38 for 61, 100 for 101: the index grows 77%, 158%, 330%): a column of longer strings is left to order_array_columns by name */
#define GORD_ROW_OVERHEAD	1.3		/* the bytes a row of an index beyond the bits of its distinct values (units, zones, presence): 3.73 measured against 2.4 estimated for one timestamp column */
#define GORD_GROUP_DISTINCT	100		/* the group of the date, timestamp and integer arrays: an integer of this many distinct values or more (an entity); the rows an entity has only enter the weighing of the second order, by the directory of the groups */
#define GORD_GROUP_DEPTH	3		/* the rows an entity has, at the least: an equality of fewer returns nothing to order (a group of one row costs the 24 bytes of its directory entry and the 5.6 of its row for no query) */
#define GORD_GROUP_BYTES	5.6		/* the bytes a row of the second order of a date, timestamp or integer array, without the directory of its groups */
#define GORD_GROUP_SPEEDUP	3.5		/* S of the latest row of each entity (a LATERAL .. ORDER BY ts DESC LIMIT 1), the least of the shapes the second order serves: 300,000 rows 48.5 M to 12.9 M instructions (3.8x), 48.8 M to 16.9 M on a narrow index (2.9x) */
#define GORD_ENDS_BYTES		2.4		/* the bytes a row of the array of an integer, date or timestamp column (the ends of its order and the addresses) */
#define GORD_GROUP_WRITE	6.0		/* W: CREATE INDEX +3.6% instructions (300,000 rows, 7 columns), +5.7% (500,000 rows, 9 columns) */

extern AttrNumber	keycols_heap_attno(Relation index, const SiftColDef *d);
extern double	keycols_heap_rows(Relation heap);
extern double	keycols_col_nullfrac(Relation heap, AttrNumber hatt);
extern bool	keycols_point_key(Relation heap, Relation index, const SiftColSchema *schema, const SiftColDef *d, double *corr);
extern bool	keycols_col_stats(Relation heap, AttrNumber hatt, double *ndistinct, double *corr, int32 *width);
extern bool	keycols_is_entity(Relation heap, AttrNumber hatt, const char *name);
extern const char *	keycols_leaf_name(Relation index, const SiftColDef *d);
extern double	keycols_est_index_bytes(Relation heap, Relation index);
extern void	keycols_analyze_if_unknown(Relation heap, Relation index);
extern void	keycols_append_name(StringInfo s, const char *name);

extern void	keycols_auto_array_specs(Relation heap, Relation index, StringInfo spec, StringInfo only, StringInfo both);
extern double	keycols_est_col_bytes(Relation heap, const FormData_pg_attribute *att, double d, int32 width);
#endif	/* SIFT_COL_KEYCOLS_INT_H */
