/*
 * col/v1/pagg.h - the pre-aggregates of a unit through the column source (docs/COL-PREAGG.md): what a consumer may ask of the unit it
 * holds from sift_col_next, with the visibility rule built in. PostgreSQL side of src/col/v1/preagg/preagg.h (the formats, portable).
 */
#ifndef SIFT_COL_PAGG_H
#define SIFT_COL_PAGG_H

#include "col/col.h"
#include "col/v1/preagg/preagg.h"
#include "col/v1/seg/seg.h"

/* The stored unit behind a source unit; false for an in-memory one (write buffer, delta batch), which has no sidecar (col/v1/scan/unit.c) */
extern bool sift_col_unit_stored(const SiftColUnit *unit, SiftColSegment **seg, uint32 *u);

/*
 * A dirty unit net of its dead rows (col/v1/delta.c, docs/COL-PREAGG.md 12): when every page the unit has rows on is all-visible and no more than max_pct percent of its rows are dead, the
 * unit answers from its pre-aggregates minus the dead rows' for counts and sums, until it is given back with a negative max_pct. Returns the number of its dead rows, or -1 when
 * it cannot (and nothing changes). While on, sift_col_unit_preagg_ok holds, sift_col_unit_agg gives the live counts and sums with min and max that only bound (SIFT_CAGG_LOOSE),
 * sift_col_unit_hist and sift_col_unit_rollup the groups and tuples of the live rows without min and max, and sift_col_unit_presence, _hll and _except refuse the unit.
 * With bounds, only the cheap tests are made (no liveness read): sift_col_unit_agg then gives the unit's zone with its dead rows (min, max, has_minmax: bounds of the live ones) and
 * counts and sums that are not net; a call without bounds builds the rest.
 */
extern int	sift_col_unit_dead(SiftColUnit *unit, int max_pct, bool bounds);
extern bool sift_col_unit_dead_on(const SiftColUnit *unit);
extern bool sift_col_unit_net(const SiftColUnit *unit);
extern uint32 sift_col_unit_net_dead(const SiftColUnit *unit);
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
extern bool sift_col_unit_hist(const SiftColUnit *unit, int dim, MemoryContext mcxt, const SiftCRollupNeed *need, SiftCHistDim *out, SiftColDict *dict);

/* Per leaf of the index definition: the dims and measures a default or configured spec asks for (planner side, docs/COL-PREAGG.md section 3) */
struct RelationData;
extern void sift_col_pagg_spec(struct RelationData *index, SiftCPaggSpec *spec);

#endif							/* SIFT_COL_PAGG_H */
