/*
 * sift_col_preagg.h - pre-aggregates of a unit (docs/COL-PREAGG.md): what the segment writer computes once, when a unit is built, merged
 * or rewritten, and what a reader may answer from it without reading the unit's chunks. Portable (sift_host.h only).
 *
 *   - per INT leaf: the exact sum and the sum of squares as 128-bit integers (the zone's int64 sum overflows, this does not);
 *   - per unit: a mini cube, GROUP BY dim -> count and, for the configured measures, (non-NULL count, sum). A dim is a scalar leaf of
 *     a few distinct values in the unit: a dictionary-coded text leaf (the group key is the segment code) or an integer, date or
 *     numeric leaf (the key is the stored integer). So "count per country", "revenue per category", "orders per month" (a leaf that
 *     holds date_trunc('month', ts)) are one add per group and unit instead of one per row;
 *   - per unit, from column format minor 4: the same for the leaves of a list (an array of objects, `items[].kind` with `items[].score`), whose
 *     rows are the elements of the list: the element cubes, one cube per list (docs/COL-JSONB-31.md 5.1). Depth 1: a list inside a list has none.
 *
 * Immutable like the unit it describes: never updated in place; whether a unit's pre-aggregate may answer a query is the visibility rule of
 * sift_col_unit_exact (clean unit, every page all-visible) and the predicate rule of the consumer (docs/COL-PREAGG.md section 4).
 */
#ifndef SIFT_COL_PREAGG_H
#define SIFT_COL_PREAGG_H

#include "sift_host.h"

#include "sift_col.h"
#include "sift_col_enc.h"

typedef __int128 SiftCI128 __attribute__((aligned(8)));

#define SIFT_PAGG_MAX_DIMS		16			/* dimensions of one cube: the top level's, or those of one list */
#define SIFT_PAGG_SPEC_DIMS		32			/* dimensions of an option: the top level's, then those of the lists */
#define SIFT_PAGG_MAX_MEAS		8
#define SIFT_PAGG_DEF_MEAS		3			/* measures of a default dim */
#define SIFT_PAGG_DEF_GROUPS	256			/* default: a dim with more groups in a unit has no histogram in that unit */
#define SIFT_PAGG_CUBE_GROUPS	4096		/* explicit cube(...): the same bound */
#define SIFT_PAGG_DEF_PERMILLE	20			/* default: the cube of a unit takes at most 2% of the unit's bytes ... */
#define SIFT_PAGG_MIN_BUDGET	2048		/* ... but always may take this many bytes */

/* flags of SiftCLeafAgg / SiftCAgg */
#define SIFT_CAGG_SUMSQ_OVF		0x1			/* the sum of squares left int128 (never: the sum cannot) */

typedef struct SiftCLeafAgg
{
	SiftCI128	sum;					/* exact sum of the non-NULL values (INT leaves) */
	SiftCI128	sumsq;					/* exact sum of their squares unless SIFT_CAGG_SUMSQ_OVF */
	uint32		flags;
} SiftCLeafAgg;

/* Everything a unit knows about a leaf without reading it: the zone (docs/COL.md 2.1) and the exact sums. INT leaves only: sum and sumsq valid then */
typedef struct SiftCAgg
{
	uint32		nvals;
	uint32		nnull;
	int64		min;					/* key domain; meaningless when nnull == nvals */
	int64		max;
	double		sum_f;					/* F4, F8: in row order of the unit */
	SiftCI128	sum;
	SiftCI128	sumsq;
	uint32		zflags;					/* SIFT_COLZ_* of the zone */
	uint32		flags;					/* SIFT_CAGG_* */
	bool		has_sums;				/* INT leaf: sum and sumsq are set */
} SiftCAgg;

typedef struct SiftCPaggDim
{
	int16		leaf;
	uint8		nmeas;
	int16		meas[SIFT_PAGG_MAX_MEAS];
} SiftCPaggDim;

/* The reloption `preagg`, resolved against a schema */
typedef struct SiftCPaggSpec
{
	bool		off;					/* no cube (the exact leaf sums are still written: they cost 40 bytes per integer leaf and unit) */
	bool		fixed;					/* cube(...): exactly these dims and measures, no byte budget */
	int			ndims;
	SiftCPaggDim dim[SIFT_PAGG_SPEC_DIMS];	/* the leaves of a list (parent >= 0) are its element cubes */
	uint32		max_groups;
	uint32		permille;				/* default: of the unit's bytes; 0 = no budget */
} SiftCPaggSpec;

/*
 * Leaves that can be a dim (a scalar INT or STR leaf, not a float, of the top level or of a list of the top level) and a measure of it (a scalar INT
 * or float leaf, not a date/time/bool, of the same list or the top level)
 */
extern bool sift_cpagg_dim_ok(const SiftColSchema *s, const SiftColDef *d);
extern bool sift_cpagg_meas_ok(const SiftColSchema *s, const SiftColDef *m, const SiftColDef *dim);

/* 'default': every dim candidate (the top level's first, then the lists') with up to SIFT_PAGG_DEF_MEAS measures (numeric and floats first, then integers). Never fails */
extern void sift_cpagg_spec_default(const SiftColSchema *s, SiftCPaggSpec *spec);
/*
 * The option: "" / "default", "off", or "cube(dim: m1, m2; dim2: m3; dim3)" with leaf names (an attribute name or a shredder path; names[leaf]).
 * Returns NULL, or an error message (palloc'd) for an unknown or unsuitable name.
 */
extern const char *sift_cpagg_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec);
/* Cheap syntax check for the reloption validator (no schema yet) */
extern const char *sift_cpagg_syntax(const char *opt);

/* One leaf of a unit as the writer sees it, in the key domain; key is the segment's code for a dictionary leaf. n: the rows of the unit, the elements of the unit's list for a leaf of one */
typedef struct SiftCPaggCol
{
	const int64 *key;
	const uint64 *nulls;
	const uint64 *ovf;
	uint32		n;
	bool		ready;					/* key is usable: false for a STR leaf stored inline and for a leaf that was not decoded */
} SiftCPaggCol;

/* The exact sums of one INT leaf's values */
extern void sift_cpagg_leaf(const SiftCPaggCol *c, SiftCLeafAgg *a);
/*
 * The cube of a unit of nrows rows, appended to out as one blob (nothing appended when no dim fits). unit_bytes is the encoded size of the unit,
 * the base of the default byte budget. Deterministic: the same columns give the same bytes.
 *
 * The element cubes, the dims of the spec that are leaves of a list, are appended to eout as one blob, in a unit's element blob (below): the
 * rows of such a cube are the elements of the list, cols[leaf].n of them. eout NULL: the spec's element dims are not built (a segment of column
 * format minor 3 has none). The byte budget is shared by the cube and the element cubes: the smallest histograms of both are kept first.
 *
 * Element blob: u8 nlists, nlists x (list leaf, bytes of its cube) as varints, then the cubes (the blob format of the cube), in ascending list order.
 */
extern void sift_cpagg_cube(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, Size unit_bytes,
							StringInfo out, StringInfo eout);

/* A decoded cube. Group arrays are in ascending key order, the NULL group (when has_null) last with no key */
typedef struct SiftCHistDim
{
	int			leaf;
	uint32		ngroups;
	bool		has_null;
	int			nmeas;
	int16		meas[SIFT_PAGG_MAX_MEAS];
	int64	   *key;					/* [ngroups] (the NULL group's is 0) */
	uint32	   *count;					/* rows of each group */
	uint32	   *nn[SIFT_PAGG_MAX_MEAS];	/* non-NULL values of the measure in each group */
	SiftCI128  *sum[SIFT_PAGG_MAX_MEAS];	/* INT measure: exact sum; NULL for a float measure */
	double	   *sumf[SIFT_PAGG_MAX_MEAS];	/* float measure: sum in row order; NULL for an INT one */
} SiftCHistDim;

typedef struct SiftCHist
{
	int			ndims;
	SiftCHistDim dim[SIFT_PAGG_MAX_DIMS];
} SiftCHist;

/* false when the bytes are not a cube of this schema; the arrays are palloc'd in mcxt */
extern bool sift_cpagg_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCHist *h);
/* Decodes only the dim of this leaf: 1 found (out set), 0 the unit has none (the cap, the byte budget, an overflow), -1 the bytes are not a cube of this schema */
extern int	sift_cpagg_decode_dim(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, int leaf, SiftCHistDim *out);
/* The cube of the list group in an element blob (NULL: none, or the bytes are not an element blob of this schema: *len is then UINT32_MAX) */
extern const uint8 *sift_cpagg_elem_cube(const SiftColSchema *s, const uint8 *eblob, uint32 elen, int group, uint32 *len);
/* The dim with this leaf, NULL if the unit has none (the cap, the budget, an overflow) */
extern const SiftCHistDim *sift_cpagg_find(const SiftCHist *h, int leaf);
/* The measure slot of leaf m in the dim, -1 if it is not stored */
extern int	sift_cpagg_meas_slot(const SiftCHistDim *d, int m);

/*
 * C4 for the sidecar: recomputes the cube and the element cubes of the stored dims and measures from the unit's columns and compares the bytes;
 * false on any difference. A blob of length 0 is none.
 */
extern bool sift_cpagg_check(const SiftColSchema *s, const SiftCPaggCol *cols, uint32 nrows, const uint8 *blob, uint32 len,
							 const uint8 *eblob, uint32 elen);
/* The leaves a stored cube and element blob read (dims and measures), marked true in need[ncols]; false when they do not decode */
extern bool sift_cpagg_leaves(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, bool *need);

#endif							/* SIFT_COL_PREAGG_H */
