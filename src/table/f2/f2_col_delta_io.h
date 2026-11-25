/*
 * table/f2/f2_col_delta_io.h - the column delta chains a modify reads without keeping them whole (f2_col_delta_foreign.c)
 */
#ifndef SIFT_TBL_F2_COL_DELTA_IO_H
#define SIFT_TBL_F2_COL_DELTA_IO_H

#include "table/f2/f2_col_delta.h"

/* The newest record of the row that another transaction wrote and its writer did not abort: the chain of the partition of the entry is read page by page, for the transaction */
extern SiftColDeltaLast sift_col_delta_foreign_latest(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops);

#endif
