/*
 * table/f2/cost_hook.h - what the planner pays for the rows an index of a sift_table of Format 2.0 gives it. The rows are read from granules of columns, not from heap pages: an index scan or a
 * bitmap heap scan pays for each row it fetches the decode of the cells of the row (a point read of the chunk of each column the row returns), which the heap costs of PostgreSQL leave out. A one-row
 * lookup is still cheapest through its B-tree, and the ranges of hundreds of rows and more go to the scan of the table (the zones and the marks of the key order leave the granules that can hold them).
 * An index-only scan fetches no row and is not repriced.
 */
#ifndef SIFT_TBL_F2_COST_HOOK_H
#define SIFT_TBL_F2_COST_HOOK_H

#include "postgres.h"

#define SIFT_TBL_F2_FETCH_ROW_US	40.0	/* microseconds to fetch a row of a sift_table by its address, measured (table_f2_fast_fetch) */
#define SIFT_TBL_COST_UNIT_US		10.0	/* microseconds one unit of the planner's cost stands for: a page read in sequence */

/* Called from sift_tbl_v2_init: the index paths of a sift_table, and the cost of those of Format 2.0 */
extern void sift_tbl_f2_cost_hook_init(void);

#endif							/* SIFT_TBL_F2_COST_HOOK_H */
