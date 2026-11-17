/*
 * col/order_units.c - the rows a key-cut unit aims at, and the statistics of the heap that say them
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/build.h"
#include "col/enc.h"

#define ORDER_DEFAULT_ROWS	1024
#define ORDER_MAX_ROWS		32768		/* the most rows of a unit; the directory of a unit is read with the head of its record (seg.c sift_cseg_unit_head) */

uint64
sift_colk_row_bytes(const SiftColSchema *schema)
{
	return 32 + (uint64) schema->ncols * 16;
}

/*
 * Rows a key-cut unit aims at: the rows of 256 average heap pages (pg_class: ANALYZE or VACUUM keeps reltuples and relpages; sift.col_unit_rows sets it), so that the number of
 * units stays what the superblock cut gave and the planner's per-unit costs are as before; 0 when unknown. A unit has at most ORDER_MAX_ROWS of them.
 */
uint32
sift_colk_aim_rows(Relation index)
{
	uint32		n = sift_col_unit_rows;
	HeapTuple	tp;

	if (n == 0 && (tp = SearchSysCache1(RELOID, ObjectIdGetDatum(index->rd_index->indrelid))) != NULL)
	{
		Form_pg_class c = (Form_pg_class) GETSTRUCT(tp);

		if (c->relpages > 0 && c->reltuples > 0)
			n = (uint32) Min((double) SIFT_COL_UNIT_MAX_ROWS, (double) SIFT_SB_PAGES * c->reltuples / c->relpages);
		ReleaseSysCache(tp);
	}
	return n;
}

/* The superblocks of the heap as of its last VACUUM or ANALYZE, which a merge compares its rows with to tell whether they are sparse (0 without statistics) */
uint64
sift_colk_heap_sbs(Relation index)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(index->rd_index->indrelid));
	uint64		sbs = 0;

	if (tp != NULL)
	{
		sbs = Max(((Form_pg_class) GETSTRUCT(tp))->relpages, 0) / SIFT_SB_PAGES;
		ReleaseSysCache(tp);
	}
	return sbs;
}

/* The rows of a unit for an aim (0: unknown) */
uint32
sift_colk_unit_rows(uint32 aim)
{
	return aim == 0 ? ORDER_DEFAULT_ROWS : Min(Max(aim, 64), ORDER_MAX_ROWS);
}

/* The aim for the superblock batches of a build run: the statistics of the heap, or without them (a table never analyzed) the rows a superblock holds in the run */
uint32
sift_colk_run_rows(Relation index, SiftColBatch *const *in, int n)
{
	uint32		aim = sift_colk_aim_rows(index);
	uint64		rows = 0;

	if (aim > 0 || n == 0)
		return aim;
	for (int i = 0; i < n; i++)
		rows += in[i]->nrows;
	return (uint32) Min(rows / n, (uint64) SIFT_COL_UNIT_MAX_ROWS);
}
