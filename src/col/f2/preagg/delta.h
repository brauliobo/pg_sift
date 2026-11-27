/*
 * col/preagg_delta.h - the pre-aggregate of a granule and its correction for the rows a query leaves out.
 *
 * A granule with dead rows is not given up: when few of its rows are dead (at most 15%, 1228 at most) their values are read, and subtracted from the groups they fall in, which costs the dead rows
 * and not the granule. A granule is given up past the share of dead rows at which a compaction rewrites its part (15%), so that every granule a compaction leaves alone is still corrected. A dead row that is the least or the greatest of its group cannot be subtracted from the extremes (nor one that is NaN or infinite from a float), and the sum of a float
 * is not subtracted from at all: the caller then scans the granule, as it does when more rows are dead.
 */
#ifndef SIFT_PREAGG_DELTA_H
#define SIFT_PREAGG_DELTA_H

#include "core/host.h"

#include "col/f2/del/delvec.h"
#include "col/f2/sink/sink.h"
#include "col/f2/preagg/v2.h"

#define SIFT_PREAGG_DEAD_MAX	1228
#define SIFT_PREAGG_DEAD_PCT	15
#define SIFT_PREAGG_SPARSE_PRED_MAX 128

/*
 * Whether a granule of nrows rows may be corrected by taking nexcl rows out of its groups instead of being scanned: at most 15% of its rows and 1228 rows, or 128 while the only rows to take out
 * are those that one WHERE col IS NULL predicate excludes (ndead == 0, n_is_null_preds == 1): a sparse column is not null on a few rows of most granules and the cube stays the answer there.
 */
static inline bool
sift_preagg_excl_ok(int nrows, int nexcl, int ndead, int n_is_null_preds)
{
	int			cap = ndead == 0 && n_is_null_preds == 1 ? SIFT_PREAGG_SPARSE_PRED_MAX : SIFT_PREAGG_DEAD_MAX;

	return nexcl <= Min(cap, nrows * SIFT_PREAGG_DEAD_PCT / 100);
}

/* Whether a granule of nrows rows with ndead dead may be corrected instead of scanned */
static inline bool
sift_preagg_delta_ok(int nrows, int ndead)
{
	return sift_preagg_excl_ok(nrows, ndead, ndead, 0);
}

/*
 * The union of two ascending lists of row ordinals without a row twice (a row that is dead and also not null for a WHERE col IS NULL predicate is taken out of the cube once): its length
 * when it is at most cap, else cap + 1 (out is not complete then).
 */
extern uint16 sift_excl_union_u16(const uint16 *a, uint16 na, const uint16 *b, uint16 nb, uint16 *out, uint16 cap);

/*
 * The rows of the granule the snapshot does not see (o->gd is the entry of granule o->g in the directory of the part): those the deletion vector shows deleted to the snapshot and those the pending deletes
 * (o->px) name, ascending, in dead (SIFT_PREAGG_DEAD_MAX of them). Their number, or -1 when there are more than sift_preagg_delta_ok allows.
 */
extern int	sift_preagg_dead_rows(const SiftF2GranOffer *o, uint16 *dead);

/* What a query reads of the measures of a cube, a bit for each measure: the measures it reads, those whose minimum or maximum it reads, and those whose float sum it reads */
typedef struct SiftPreaggReads
{
	uint32		meas;
	uint32		minmax;
	uint32		float_sum;
} SiftPreaggReads;

#define SIFT_PREAGG_READS_ALL	((SiftPreaggReads) {~UINT32_C(0), ~UINT32_C(0), 0})

/*
 * Subtracts the rows of an ascending list (the dead rows, the rows a predicate excludes, or their union: the caller checks sift_preagg_excl_ok) from the groups of a cube of a granule; the group of a
 * row is found by its dimensions, which are read at the rows only. A group left with no row keeps its cell with row_count 0. False, with the cells untouched, when a row is the least or the
 * greatest of its group and the query reads the minimum or the maximum of that measure, is NaN or infinite, has a measure whose float sum the query reads, or its group is not in the cube. Only the
 * measures the query reads (bit m: measure m of the cube) are corrected: the others are not read by the caller. The sets of distinct values are not
 * corrected: a query that counts distinct values scans the granule.
 */
/* Sets want[leaf] for the columns sift_preagg_v2_subtract reads at the rows: the dimensions and the measures the query reads */
extern void sift_preagg_v2_subtract_cols(const SiftPreaggCubeDesc *cd, const SiftPreaggReads *reads, bool *want);

extern bool sift_preagg_v2_subtract(const SiftPreaggCubeDesc *cd, SiftPreaggGran *gran, const SiftGranuleView *v, const uint16 *rows, uint16 nrows, const SiftPreaggReads *reads);

/*
 * The groups of a cube of a granule after a column delta gave the rows of an ascending list new values of the integer measure m: each row leaves the sum and the count of the values that are
 * not NULL of its group with the value the granule stores (read at the rows, with the dimensions that find the group) and enters them with values[i], or none when isnull[i]. The caller
 * reads neither the minimum nor the maximum of m. False, with the cells untouched, when m is not an integer measure or the group of a row is not in the cube.
 */
extern bool sift_preagg_v2_repatch(const SiftPreaggCubeDesc *cd, SiftPreaggGran *gran, const SiftGranuleView *v, int m, const uint16 *rows, const int64 *values, const bool *isnull, uint16 nrows);

#endif							/* SIFT_PREAGG_DELTA_H */
