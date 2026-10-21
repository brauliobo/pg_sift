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

/*
 * May the pre-aggregates of this unit stand for the rows a query without filters sees: the unit is stored, has no dead row and every page it
 * has rows on is all-visible to the snapshot (sift_col_unit_exact). When false the unit is scanned. Counts and sums are then exact over
 * exactly the rows of the unit; a predicate on top is the consumer's (zone cover test, docs/COL-PREAGG.md section 4).
 */
extern bool sift_col_unit_preagg_ok(const SiftColUnit *unit);

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
extern bool sift_col_unit_rollup(const SiftColUnit *unit, int list, MemoryContext mcxt, const SiftCRollupNeed *need, SiftCRollup *r, SiftColDict *dict);

/* A scan reads the unit for lack of its rollup (missing, other dims, a measure it lacks): asks for the segment to be rewritten (sift_col_rollup_request) */
extern bool sift_col_unit_rollup_miss(const SiftColUnit *unit);

/* Per leaf of the index definition: the dims and measures a default or configured spec asks for (planner side, docs/COL-PREAGG.md section 3) */
struct RelationData;
extern void sift_col_pagg_spec(struct RelationData *index, SiftCPaggSpec *spec);

#endif							/* SIFT_COL_PAGG_H */
