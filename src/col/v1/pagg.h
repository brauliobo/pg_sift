/*
 * col/v1/pagg.h - the pre-aggregates of a unit through the column source (docs/COL-PREAGG.md): what a consumer may ask of the unit it
 * holds from sift_col_next, with the visibility rule built in. PostgreSQL side of src/col/v1/preagg/preagg.h (the formats, portable).
 */
#ifndef SIFT_COL_PAGG_H
#define SIFT_COL_PAGG_H

#include "nodes/bitmapset.h"

#include "col/col.h"
#include "col/v1/preagg/preagg.h"
#include "col/v1/seg/seg.h"

/* The stored unit behind a source unit; false for an in-memory one (write buffer, delta batch), which has no sidecar (col/v1/scan/unit.c) */
extern bool sift_col_unit_stored(const SiftColUnit *unit, SiftColSegment **seg, uint32 *u);
/*
 * Exceptions (docs/COL-PREAGG.md 11): with leaf, a unit whose exceptions are of that leaf answers its histograms for the rows where the leaf IS NULL, net of the exceptions: counts and sums,
 * no minimum or maximum, false when a float sum would lose more than 6 bits to the subtraction or the unit lacks the dim; sift_col_unit_agg refuses the unit. false: the unit keeps no exceptions
 * of the leaf. Leaf -1 (and true) returns it to its own rows.
 */
extern bool sift_col_unit_except(SiftColUnit *unit, int leaf);
/* A group of units (sift_col_super): its segment and the index of its group there; false for any other unit */
extern bool sift_col_unit_super(const SiftColUnit *unit, SiftColSegment **seg, uint32 *k);

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

/* count(*) of a test of a string leaf over the stored segment of the unit, from the row counts of its dictionary, when every unit of the segment is exact (col/v1/scan/unit.c) */
extern bool sift_col_unit_seg_count(const SiftColUnit *unit, int col, SiftColOp op, const SiftColValue *c, uint64 *rows);
/* The key table of leaf col (col/v1/ktab.h) of the stored segment of the unit, every unit of which is exact (col/v1/scan/unit.c); NULL: none */
extern const SiftCHistDim *sift_col_unit_seg_ktab(const SiftColUnit *unit, int col);

/* A range of the keys of a leaf: [lo, hi] of an integer leaf, or for a text leaf the strings from slo to shi (NULL: no bound; excl: the bound itself is out) under the collation collid */
typedef struct SiftColRange
{
	bool		str;
	int64		lo;
	int64		hi;
	const char *slo;
	const char *shi;
	uint32		slolen;
	uint32		shilen;
	bool		slo_excl;
	bool		shi_excl;
	Oid			collid;
} SiftColRange;

/*
 * count(*) of the rows of the stored segment of the unit whose key of leaf col lies in the range, from the global order of the leaf (order_array_columns): the entries of the units of
 * the segment that are exact (every page all-visible, the directory current, no dead row). *read (in cx) is the superblocks of the units that are not, which the scan reads instead; with
 * any, the segment must not be cut by key and at most a quarter of its units are read. false: the whole segment is read (col/v1/scan/unit.c).
 */
extern bool sift_col_unit_seg_range(const SiftColUnit *unit, int col, const SiftColRange *r, MemoryContext cx, uint64 *rows, Bitmapset **read);

/* The aggregate of a leaf (zone + exact sums). false when the unit may not answer or the leaf overflowed in it (SIFT_COLZ_OVERFLOW) */
extern bool sift_col_unit_agg(const SiftColUnit *unit, int col, SiftCAgg *a);

/* The key presence of leaf col (col/v1/setsum.h: a list or a bitmap of its keys) of a unit that is exact, not net of dead rows, not a group of units nor answering for its exceptions, and has no overflow value of the leaf; false when it has none */
extern bool sift_col_unit_presence(const SiftColUnit *unit, int col, SiftCPresence *p);

/*
 * The histogram of dim in the unit: GROUP BY dim -> count and, for the measures the unit stored (SiftCHistDim.meas), (non-NULL count, sum).
 * Arrays palloc'd in mcxt. A STR dim's keys are codes of dict (set; NULL for the other dims), the segment's dictionary, valid for the life of the
 * source. false when the unit may not answer or has no histogram of dim (more groups than the cap, the byte budget, an overflow value, inline text).
 */
extern bool sift_col_unit_hist(const SiftColUnit *unit, int dim, MemoryContext mcxt, const SiftCRollupNeed *need, SiftCHistDim *out, SiftColDict *dict);

/*
 * The rollup of list in the unit (rollup_columns and the lists of the elements of an array, docs/COL-PREAGG.md; -1: of its rows): its tuples of the dims it holds, the dims
 * and measures need names (NULL: all). Arrays palloc'd in mcxt; dict[j] is the dictionary of a STR dim j (zeroed for the other dims). false when the unit may not answer, is a group of units,
 * answers for its exceptions or has no rollup of list.
 */
extern bool sift_col_unit_rollup(const SiftColUnit *unit, int list, MemoryContext mcxt, const SiftCRollupNeed *need, SiftCRollup *r, SiftColDict *dict);

/* Per leaf of the index definition: the dims and measures a default or configured spec asks for (planner side, docs/COL-PREAGG.md section 3) */
struct RelationData;
extern void sift_col_pagg_spec(struct RelationData *index, SiftCPaggSpec *spec);

#endif							/* SIFT_COL_PAGG_H */
