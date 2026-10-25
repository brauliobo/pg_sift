/*
 * col/pagg.h - the pre-aggregates of a unit through the column source (docs/COL-PREAGG.md): what a consumer may ask of the unit it
 * holds from sift_col_next, with the visibility rule built in. PostgreSQL side of src/col/preagg.h (the formats, portable).
 */
#ifndef SIFT_COL_PAGG_H
#define SIFT_COL_PAGG_H

#include "col/col.h"
#include "col/preagg.h"
#include "col/seg.h"

/* The stored unit behind a source unit; false for an in-memory one (write buffer, delta batch), which has no sidecar (col/scan_unit.c) */
extern bool sift_col_unit_stored(const SiftColUnit *unit, SiftColSegment **seg, uint32 *u);
/* A superunit (sift_col_super): its segment and the index of its group there; false for any other unit */
extern bool sift_col_unit_super(const SiftColUnit *unit, SiftColSegment **seg, uint32 *k);

/*
 * May the pre-aggregates of this unit stand for the rows a query without filters sees: the unit is stored, has no dead row and every page it
 * has rows on is all-visible to the snapshot (sift_col_unit_exact). When false the unit is scanned. Counts and sums are then exact over
 * exactly the rows of the unit; a predicate on top is the consumer's (zone cover test, docs/COL-PREAGG.md section 4).
 */
extern bool sift_col_unit_preagg_ok(const SiftColUnit *unit);

/*
 * The key presence of leaf col in a stored unit that is exact (col/setsum.h, kind PRES): the key of bit 0 divided by 64, the words and their number, valid
 * until the next call on the segment. false for a superunit, a unit that is not exact or has no presence for the leaf.
 */
extern bool sift_col_unit_presence(const SiftColUnit *unit, int col, SiftCPresence *p);
/* The HyperLogLog sketch of col in an exact unit (col/hll.h), valid until the next call; false when the unit has none */
extern bool sift_col_unit_hll(const SiftColUnit *unit, int col, const uint8 **blob);

/*
 * Exceptions (docs/COL-PREAGG.md 11): a stored exact unit may hold the cube and rollups of its rows where one leaf is not NULL. With leaf >= 0 and the unit
 * holding them for that leaf, this returns true and the next sift_col_unit_hist / sift_col_unit_rollup on the unit give the groups and tuples of the rows where
 * the leaf IS NULL: counts and sums net of the exceptions, no minimum or maximum, false when a float sum would lose more than 6 bits to the subtraction or the
 * unit lacks the dim; sift_col_unit_agg and sift_col_unit_presence refuse the unit. Leaf -1 (and true) returns it to its own rows. unit->except holds the leaf.
 */
extern bool sift_col_unit_except(SiftColUnit *unit, int leaf);

/*
 * A dirty unit net of its dead rows (col/delta.c, docs/COL-PREAGG.md 12): when every page the unit has rows on is all-visible and no more than max_pct percent of its rows are dead, the
 * unit answers from its pre-aggregates minus the dead rows' for counts and sums, until it is given back with a negative max_pct. Returns the number of its dead rows, or -1 when
 * it cannot (and nothing changes). While on, sift_col_unit_preagg_ok holds, sift_col_unit_agg gives the live counts and sums with min and max that only bound (SIFT_CAGG_LOOSE),
 * sift_col_unit_hist and sift_col_unit_rollup the groups and tuples of the live rows without min and max, and sift_col_unit_presence, _hll and _except refuse the unit.
 * With bounds, only the cheap tests are made (no liveness read): sift_col_unit_agg then gives the unit's zone with its dead rows (min, max, has_minmax: bounds of the live ones) and
 * counts and sums that are not net; a call without bounds builds the rest.
 */
extern int	sift_col_unit_dead(SiftColUnit *unit, int max_pct, bool bounds);
extern bool sift_col_unit_dead_on(const SiftColUnit *unit);
extern bool sift_col_unit_dead_bounds(const SiftColUnit *unit);
/* ... and the histogram and rollup of the unit minus its dead rows, row by row: false when a leaf cannot be read at them */
extern bool sift_col_unit_dead_hist(const SiftColUnit *unit, SiftCHistDim *h);
extern bool sift_col_unit_dead_rollup(const SiftColUnit *unit, SiftCRollup *r);
extern bool sift_col_unit_dead_leaf(const SiftColUnit *unit, int col, uint32 *ndead, uint32 *nnull, SiftCLeafAgg *la);

/* The aggregate of a leaf (zone + exact sums). false when the unit may not answer or the leaf overflowed in it (SIFT_COLZ_OVERFLOW) */
extern bool sift_col_unit_agg(const SiftColUnit *unit, int col, SiftCAgg *a);

/*
 * The histogram of dim in the unit: GROUP BY dim -> count and, for the measures the unit stored (SiftCHistDim.meas), (non-NULL count, sum).
 * Arrays palloc'd in mcxt. A STR dim's keys are codes of dict (set; NULL for the other dims), the segment's dictionary, valid for the life of the
 * source. false when the unit may not answer or has no histogram of dim (more groups than the cap, the byte budget, an overflow value, inline text).
 */
extern bool sift_col_unit_hist(const SiftColUnit *unit, int dim, MemoryContext mcxt, SiftCHistDim *out, SiftColDict *dict);

/*
 * The rollup of a list in the unit (SiftCRollup, docs/COL-PREAGG.md): its tuples of the dims it holds. Arrays palloc'd in mcxt; dict[j] is the dictionary of a
 * STR dim j (set; zeroed for the other dims). false when the unit may not answer or has no rollup of the list.
 */
/* the tuples of the unit's rollup of the list without decoding it (0: the unit has none) */
extern uint32 sift_col_unit_rollup_tuples(const SiftColUnit *unit, int list);
extern bool sift_col_unit_rollup(const SiftColUnit *unit, int list, MemoryContext mcxt, const SiftCRollupNeed *need, SiftCRollup *r, SiftColDict *dict);

/* A scan reads the unit for lack of its rollup (missing, other dims, a measure it lacks): asks for the segment to be rewritten (sift_col_rollup_request) */
extern bool sift_col_unit_rollup_miss(const SiftColUnit *unit);

/* Per leaf of the index definition: the dims and measures a default or configured spec asks for (planner side, docs/COL-PREAGG.md section 3) */
struct RelationData;
extern void sift_col_pagg_spec(struct RelationData *index, SiftCPaggSpec *spec);

#endif							/* SIFT_COL_PAGG_H */
