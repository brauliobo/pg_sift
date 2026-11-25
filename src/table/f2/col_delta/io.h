/*
 * table/f2/col_delta/io.h - the column delta chains a modify reads without keeping them whole (f2_col_delta_foreign.c)
 */
#ifndef SIFT_TBL_F2_COL_DELTA_IO_H
#define SIFT_TBL_F2_COL_DELTA_IO_H

#include "table/f2/col_delta/col_delta.h"

/* The newest record of the row that another transaction wrote and its writer did not abort: the log of the chain of the partition of the entry follows it from one statement to the next */
extern SiftColDeltaLast sift_col_delta_foreign_latest(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *anchor, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops);

#endif
