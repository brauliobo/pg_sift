/*
 * table/f2/col_delta/io.h - the column delta chains a modify reads without keeping them whole (f2_col_delta_foreign.c)
 */
#ifndef SIFT_TBL_F2_COL_DELTA_IO_H
#define SIFT_TBL_F2_COL_DELTA_IO_H

#include "table/f2/col_delta/col_delta.h"

/* The writers of the records of the row (sift_col_delta_writers_add): the log of the chain of the partition of the entry follows it from one statement to the next */
extern SiftColDeltaWriters sift_col_delta_foreign_writers(Relation rel, const SiftPartitionEntry *entry, const SiftColDeltaAnchor *anchor, const SiftColDeltaTarget *t, const SiftColDeltaOps *ops);

#endif
