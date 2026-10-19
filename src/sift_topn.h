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
 * The order the superblocks are visited in. Before the scan the zones of the sort column's first key in a few superblocks, spread over the
 * table (sift_topn_probe_sb), tell whether it follows the table (sift_topn_probe, sift_topn_start). The scan then takes windows of
 * superblocks from the end its best values are at (sift_topn_next_window); when the column follows the table and the best superblock of
 * a window cannot beat the n-th best so far, none of the rest can (sift_topn_zone_viable, sift_topn_stop); within a window each unit is
 * tested by its zone before it is read.
 */
extern bool sift_topn_wants_zones(const SiftTopN *t);
extern int	sift_topn_nprobes(void);
extern uint32 sift_topn_probe_sb(const SiftTopN *t, int i, uint32 nsb);
extern void sift_topn_probe(SiftTopN *t, uint32 sb, const SiftSrcZone *z);
extern void sift_topn_start(SiftTopN *t, uint32 nsb);
extern bool sift_topn_follows(const SiftTopN *t);
extern bool sift_topn_reverse(const SiftTopN *t);	/* the superblocks are visited from the last */
extern bool sift_topn_next_window(SiftTopN *t, uint32 *lo, uint32 *hi, uint32 *best);
extern bool sift_topn_zone_viable(const SiftTopN *t, const SiftSrcZone *z);
extern void sift_topn_stop(SiftTopN *t);
extern void sift_topn_skip(SiftTopN *t);

/* The rows of the unit in rmask (nrow bits); key0: the decoded first sort leaf, in the numbering of the rows; the others are read for the few rows that need them */
extern void sift_topn_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow);

/* After the scan: the rows in order; sift_topn_fetch stores row i (all its attributes, copied) in slot, a virtual slot of the scan tuple */
extern int	sift_topn_finish(SiftTopN *t);
extern bool sift_topn_fetch(SiftTopN *t, int i, TupleTableSlot *slot);

extern int64 sift_topn_skipped(const SiftTopN *t);
extern int64 sift_topn_fetched(const SiftTopN *t);

#endif							/* SIFT_TOPN_H */
