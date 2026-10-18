/*
 * sift_topn.h - ORDER BY <columns> LIMIT n answered from the column units (docs/COL-SEARCH.md, "Top-N").
 *
 * The scan of SiftAgg evaluates the filters and the match set of a text search as it does for an aggregate; for each unit it hands the rows
 * that pass to a bounded heap of the n best by the sort columns, which read the leaves of the rows that can still enter the heap only.
 * The zones of the units bound the sort column, so that units that cannot hold a row better than the n-th are not read, and the
 * superblocks are visited from the end the sort column grows toward when the table is clustered on it. At the end the heap rows are
 * fetched from the heap by TID, in order: the heap is read n times, whatever the number of matches.
 */
#ifndef SIFT_TOPN_H
#define SIFT_TOPN_H

#include "access/tableam.h"
#include "executor/tuptable.h"
#include "nodes/pg_list.h"
#include "nodes/pathnodes.h"

#include "sift_colsrc.h"

#define SIFT_TOPN_MAX_KEYS	4
#define SIFT_TOPN_MAX_ROWS	100000		/* LIMIT + OFFSET above this keeps the normal plan */

/* ---- planner ---- */

/* Can the leaf be an ORDER BY key: an integer-like, date, timestamp, numeric (scaled integer) or float leaf, whose order is the type's */
extern bool sift_topn_key_ok(const SiftSrcLeaf *lf);

/* The spec the plan carries (an integer list, SiftAggSpec.topn): limit rows; keys: leaf, descending, nulls first; out: the heap attribute of each column of the scan tuple */
extern Node *sift_topn_spec(int limit, int nkey, const int *leaf, const bool *desc, const bool *nulls_first, int nout, const int *attno);

extern int	sift_topn_limit(Node *spec);

/* ---- executor ---- */

typedef struct SiftTopN SiftTopN;

extern SiftTopN *sift_topn_open(Node *spec, const SiftSrcLayout *lay, Relation heap, Snapshot snapshot, MemoryContext cxt);
extern void sift_topn_close(SiftTopN *t);

extern int	sift_topn_nkey(const SiftTopN *t);
extern int	sift_topn_leaf(const SiftTopN *t, int j);

/*
 * Before the scan: the zones of the sort column's first key in every unit (sift_topn_zone, over a pass of the units), then the order
 * the superblocks are visited in. The scan asks sift_topn_next_range for the next run of superblocks that can still hold a row of the
 * result, [*lo, *hi), until it returns false.
 */
extern bool sift_topn_wants_zones(const SiftTopN *t);
extern void sift_topn_zones_begin(SiftTopN *t, uint32 nsb);
extern void sift_topn_zone(SiftTopN *t, uint32 sb, const SiftSrcZone *z);
extern void sift_topn_start(SiftTopN *t, uint32 nsb);
extern bool sift_topn_next_range(SiftTopN *t, uint32 *lo, uint32 *hi);

/* The rows of the unit in rmask (nrow bits); keys[j]: the decoded sort leaf j, in the numbering of the rows */
extern void sift_topn_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *const *keys, const uint64 *rmask, int nrow);

/* After the scan: the rows in order; sift_topn_fetch stores row i (all its attributes, copied) in slot, a virtual slot of the scan tuple */
extern int	sift_topn_finish(SiftTopN *t);
extern bool sift_topn_fetch(SiftTopN *t, int i, TupleTableSlot *slot);

extern int64 sift_topn_skipped(const SiftTopN *t);
extern int64 sift_topn_fetched(const SiftTopN *t);

#endif							/* SIFT_TOPN_H */
