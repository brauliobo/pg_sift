/*
 * col/preagg_delta.h - the pre-aggregate of a granule and its correction for deleted rows.
 *
 * SEC_PREAGG keeps, for each granule of a compacted part, the cube of its rows by one integer dimension: for each value the count, the sum, the sum of squares, the least and
 * the greatest of an integer measure. A granule with deleted rows is not given up: when few of its rows are dead (at most 5%, 64 at most) their values are read, and subtracted
 * from the cells they fall in, which costs the dead rows and not the granule. A dead row that is the least or the greatest of its cell cannot be subtracted from the extremes: the
 * caller then scans the granule with the deletion vector, as it does when more rows are dead.
 */
#ifndef SIFT_PREAGG_DELTA_H
#define SIFT_PREAGG_DELTA_H

#include "core/host.h"

#include "col/f2/delvec.h"
#include "col/f2/preagg_v2.h"

typedef struct SiftPreaggCell
{
	int64		key;
	uint64		count;			/* rows with a measure */
	int64		sum;
	int64		min;
	int64		max;
	double		sumsq;
} SiftPreaggCell;

StaticAssertDecl(sizeof(SiftPreaggCell) == 48, "SiftPreaggCell must be 48 bytes");

#define SIFT_PREAGG_DEAD_MAX	64
#define SIFT_PREAGG_DEAD_PCT	5
#define SIFT_PREAGG_SPARSE_PRED_MAX 128

/* The cells of a granule (ascending key); rows with a null dimension are left out */
extern SiftPreaggCell *sift_preagg_build(const SiftGranuleView *v, int dim_col, int measure_col, int *ncells);

/*
 * Whether a granule of nrows rows may be corrected by taking nexcl rows out of its cells instead of being scanned: at most 5% of its rows and 64 rows, or 128 while the only rows to take out
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
 * Subtracts the rows of an ascending list from the cells (cells with no row left are removed); false, with the cells untouched, when a row is the least or the greatest of its cell. The list is
 * the dead rows, the rows a predicate excludes, or their union: the caller checks sift_preagg_excl_ok.
 */
extern bool sift_preagg_subtract_rows(const SiftGranuleView *v, int dim_col, int measure_col, const uint16 *excl_rows, uint16 nexcl, SiftPreaggCell *cells, int *ncells);

/* Subtracts the dead rows from the cells; false, with the cells untouched, when the correction is not possible */
extern bool sift_preagg_subtract(const SiftGranuleView *v, int dim_col, int measure_col, const SiftDvGran *dead, SiftPreaggCell *cells, int *ncells);

/* The cube of every granule of an opened part: the cells of granule g, or NULL when the part has none */
struct SiftPart;
extern SiftPreaggCell *sift_part_read_preagg(Relation rel, const struct SiftPart *part, uint32 g, int *ncells);

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

#endif							/* SIFT_PREAGG_DELTA_H */
