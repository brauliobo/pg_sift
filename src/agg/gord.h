/*
 * agg/gord.h - the merge of the global order of a leaf (col/gorder.h) over the sources of a scan: the entries (key, heap address) of the stored segments and the
 * rows of the write buffers, in the order of an ORDER BY on the leaf. The entries are of rows that may have died or changed since: whoever reads them has the
 * heap say which are live and what they hold.
 */
#ifndef SIFT_AGG_GORD_H
#define SIFT_AGG_GORD_H

#include "postgres.h"

#include "storage/itemptr.h"

#include "agg/colsrc.h"

typedef struct SiftGOrd SiftGOrd;

/* The merge for a scan that wants rows rows. NULL when the source has no global order of the leaf, or its write buffers hold too many rows to sort for so few (sift_colsrc_gorder_open) */
extern SiftGOrd *sift_gord_open(SiftSrcSource *src, int leaf, int rows, bool desc, bool nulls_first, MemoryContext cxt);
/* The next entry: its key (isnull: the row has none) and heap address; false at the end. An entry that repeats the last one (a row in two sources) is skipped. */
extern bool sift_gord_next(SiftGOrd *g, int64 *key, bool *isnull, ItemPointerData *tid);
extern void sift_gord_rewind(SiftGOrd *g);

#endif
