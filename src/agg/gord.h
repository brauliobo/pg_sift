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

/* A range of the key, ends included, for the conjuncts of the query on it: the rows with no value are not in it */
typedef struct SiftGBound
{
	bool		has_lo;
	bool		has_hi;
	int64		lo;
	int64		hi;
	int			npin;			/* conditions leaf = value of the query: a global order grouped by one of the leaves reads the entries of that group only, and cannot answer without it */
	int			pinleaf[4];
	int64		pinval[4];
} SiftGBound;

/* The merge for a scan that wants rows rows. NULL when the source has no global order of the leaf, or its write buffers hold too many rows to sort for so few (sift_colsrc_gorder_open) */
extern SiftGOrd *sift_gord_open(SiftSrcSource *src, int leaf, int rows, bool desc, bool nulls_first, uint32 collid, const SiftGBound *bound, MemoryContext cxt);
/* The string of the entry the last sift_gord_next gave, of a text key (collid not 0 at the open): the key is then a rank in its source, not comparable with another's */
extern const char *sift_gord_str(const SiftGOrd *g, uint32 *len);
/* The next entry: its key (isnull: the row has none) and heap address; false at the end. An entry that repeats the last one (a row in two sources) is skipped. */
extern bool sift_gord_next(SiftGOrd *g, int64 *key, bool *isnull, ItemPointerData *tid);
/* The entry sift_gord_next just gave is of a row that is live and visible to every snapshot: the heap need not confirm it (the first rows an OFFSET throws away) */
extern bool sift_gord_exact(const SiftGOrd *g, const ItemPointerData *tid);
extern void sift_gord_rewind(SiftGOrd *g);
/* The leaf the arrays are grouped by, whose pinned value the scan reads the entries of; -1 when they are not grouped */
extern int	sift_gord_group(const SiftGOrd *g);

#endif
