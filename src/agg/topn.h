/*
 * agg/topn.h - ORDER BY <columns> LIMIT n answered from the column units (docs/COL-SEARCH.md, "Top-N").
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
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "nodes/pathnodes.h"

#include "agg/colsrc.h"

#define SIFT_TOPN_MAX_KEYS	4
#define SIFT_TOPN_MAX_ROWS	100000		/* LIMIT + OFFSET of a plan above this keeps the normal plan; the heap holds no more: past it at run time the rows go to a sort */

/* ---- planner ---- */

/* Can the leaf be an ORDER BY key: an integer-like, date, timestamp, numeric (scaled integer) or float leaf, whose order is the type's */
extern bool sift_topn_key_ok(const SiftSrcLeaf *lf);

/*
 * The spec the plan carries (SiftAggSpec.topn): a List of the keys (an integer list: keys: leaf, descending, nulls first; out: the heap
 * attribute of each column of the scan tuple), the LIMIT and the OFFSET (NULL: none) as the expressions of the query, int8 with no
 * Var, volatile function or sub-plan: constants, or parameters of the statement that the scan evaluates when it starts.
 */
extern Node *sift_topn_spec(int nkey, const int *leaf, const bool *desc, const bool *nulls_first, int nout, const int *attno, Node *count, Node *offset);

/* The rows of the heap for EXPLAIN: LIMIT + OFFSET when they are constants */
extern char *sift_topn_limit_text(Node *spec);

/* ---- executor ---- */

typedef struct SiftTopN SiftTopN;

/*
 * LIMIT + OFFSET are evaluated here (econtext, parent: as for the search operands), and whatever they are the scan answers as the
 * normal plan does, the Limit node above deciding what it takes: up to SIFT_TOPN_MAX_ROWS the best rows are kept in a heap; NULL
 * (all rows), more rows or an overflow keep every row that passes in a sort of keys and TIDs (work_mem, then disk), which still
 * fetches from the heap only the rows the Limit takes; nothing wanted (zero or negative) reads nothing.
 */
extern SiftTopN *sift_topn_open(Node *spec, const SiftSrcLayout *lay, Relation heap, Snapshot snapshot, ExprContext *econtext, PlanState *parent, MemoryContext cxt);
extern void sift_topn_close(SiftTopN *t);
extern bool sift_topn_empty(const SiftTopN *t);

extern int	sift_topn_nkey(const SiftTopN *t);
extern int	sift_topn_leaf(const SiftTopN *t, int j);

/*
 * The order the superblocks are visited in. Before the scan the zones of the sort column's first key in a few superblocks, spread over the
 * table (sift_topn_probe_sb), tell whether it follows the table (sift_topn_probe, sift_topn_start). The scan then takes windows of
 * superblocks from the end its best values are at (sift_topn_next_window). The probes only choose the order: a window is read when a zone of
 * its units can beat the n-th best so far (sift_topn_zone_viable), whatever the windows before it were, because a moved row (the new version
 * of an update) has a value that is not that of its neighbours; within a window each unit is tested by its zone before it is read.
 */
extern bool sift_topn_wants_zones(const SiftTopN *t);
extern int	sift_topn_nprobes(void);
extern uint32 sift_topn_probe_sb(const SiftTopN *t, int i, uint32 nsb);
extern void sift_topn_probe(SiftTopN *t, uint32 sb, const SiftSrcZone *z);
extern void sift_topn_start(SiftTopN *t, uint32 nsb);
extern bool sift_topn_reverse(const SiftTopN *t);	/* the superblocks are visited from the last */
extern bool sift_topn_next_window(SiftTopN *t, uint32 *lo, uint32 *hi, uint32 *best);
extern bool sift_topn_zone_viable(const SiftTopN *t, const SiftSrcZone *z);
extern void sift_topn_skip(SiftTopN *t);

/* The rows of the unit in rmask (nrow bits); key0: the decoded first sort leaf, in the numbering of the rows; the others are read for the few rows that need them */
extern void sift_topn_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow);

/* After the scan: the rows in order; sift_topn_next stores the next one (all its attributes, copied) in slot, a virtual slot of the scan tuple, false at the end */
extern void sift_topn_finish(SiftTopN *t);
extern bool sift_topn_next(SiftTopN *t, TupleTableSlot *slot);
extern void sift_topn_rewind(SiftTopN *t);

extern int64 sift_topn_skipped(const SiftTopN *t);
extern int64 sift_topn_fetched(const SiftTopN *t);
extern int64 sift_topn_sorted(const SiftTopN *t);	/* rows that went to the sort; -1: the heap holds the rows */

#endif							/* SIFT_TOPN_H */
