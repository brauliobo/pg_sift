/*
 * sift_table_seg.c - the column groups of a sift_table: the glue to the column engine (docs/COL-TAM.md sections 7 and 12).
 *
 * STUB(col-core): the engine's segment writer and reader (sift_col_seg.h) have not landed, so a table has no column groups: rows
 * stay in delta segments (row format), every other part of the access method works on them. The functions below are the
 * seams the engine will fill: sealing a delta segment into a frozen column segment, reading a row of a group by TID, the
 * visibility hook of the aggregate pushdown.
 */
#include "postgres.h"

#include "sift_table.h"

bool
sift_tbl_group_fetch(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv)
{
	return false;
}
