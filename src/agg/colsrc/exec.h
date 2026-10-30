/*
 * agg/colsrc/exec.h - the column source: the records of the executor side
 * The dictionary, the vector, the unit, the prune and filter tests, the pre-aggregates and
 * the counters a unit read gives; the second of the four parts of agg/colsrc.h.
 */
#ifndef SIFT_AGG_COLSRC_EXEC_H
#define SIFT_AGG_COLSRC_EXEC_H

#include "agg/colsrc/meta.h"

/* ---- executor side ---- */

typedef struct SiftSrcSource SiftSrcSource;

#define SIFT_SRC_DICT_ENTRY		255	/* the longest entry of a dictionary of a source (SIFT_COL_DICT_MAX_ENTRY) */

/* A dictionary of a text leaf: value c is bytes[off[c] .. off[c + 1]). id is the same pointer for every unit that shares it and is never reused for another dictionary while the source is open */
typedef struct SiftSrcDict
{
	const void *id;
	uint32		n;
	const uint32 *off;			/* n + 1 entries */
	const char *bytes;
	const uint32 *(*order) (const void *octx, uint64 rows);	/* the codes in byte order of their strings (SiftColDict.order), NULL: none */
	const void *octx;
	/* off NULL: the strings stay in the segment and get gives the string of a code (into buf of SIFT_SRC_DICT_ENTRY bytes, valid until the next call); sift_srcdict_whole loads the arrays */
	const char *(*get) (const void *gctx, uint32 code, uint32 *len, char *buf);
	void		(*full) (const void *gctx, struct SiftSrcDict *self);
	const void *gctx;
	bool		numrec;			/* the entries are DNum records (agg/combo_int.h), one for each row, not numerics: a leaf only sums read (SiftSrcProvider.set_numsum) */
	void		(*rec) (const void *gctx, uint32 row);	/* numrec: makes the record of a row whose scale is still WNUM_UNSET */
} SiftSrcDict;

/* The string of entry c: from the arrays, or read in the segment when the dictionary has none */
static inline const char *
sift_srcdict_str(const SiftSrcDict *d, uint32 c, uint32 *len, char *buf)
{
	if (likely(d->off != NULL))
	{
		*len = d->off[c + 1] - d->off[c];
		return d->bytes + d->off[c];
	}
	return d->get(d->gctx, c, len, buf);
}

/* The dictionary with its arrays: d itself, or d completed in place (the whole dictionary is loaded and kept for the backend), for the code that reads off, bytes or the order */
static inline const SiftSrcDict *
sift_srcdict_whole(const SiftSrcDict *d)
{
	if (unlikely(d->off == NULL))
		d->full(d->gctx, (SiftSrcDict *) d);
	return d;
}

/* One decoded leaf of a unit: n values (rows; elements for a child leaf), see the padding rules of agg/kern.h */
typedef struct SiftSrcVec
{
	const void *data;			/* int32[n] / int64[n] / double[n] / uint32[n] by the leaf's vtype; SIFT_SIMD_ALIGN aligned */
	const uint8 *isnull;		/* NULL: none is null; else n flags, 1 = null (the data slot is readable, content undefined) */
	const SiftSrcDict *dict;	/* SCV_CODE leaves */
	int64		n;
	/*
	 * NULL when no value is one the index cannot store, else bit i set = value i is: for a numeric(p <= 18, s) leaf a NaN, which the
	 * provider presents as a value (PG_INT64_MAX, not NULL: it is greater than every number of the leaf, and equal to itself, as
	 * PostgreSQL orders it; the bit tells the code that adds or compares); for the others what the shredder flags as overflow (isnull is
	 * set, the data slot is undefined), which is an error for a row of the snapshot (process_unit). Zeroed by sift_colsrc_read, set by
	 * the provider.
	 */
	const uint64 *ovf;
	/* an array leaf's count vector: the offsets of the elements of its rows (n + 1 entries, the running sum of the counts), when the provider holds them; else NULL, zeroed by sift_colsrc_read */
	const uint32 *eoff;
} SiftSrcVec;

/*
 * Storage the caller owns for one leaf read (sift_colsrc_read_into): the leaf is decoded into it, with no copy between the decoder and the
 * caller's vector. Both have room for sift_colsrc_leaf_n() + SIFT_AGG_PAD values (data: of the leaf's vtype, SIFT_SIMD_ALIGN aligned;
 * isnull: bytes); the SIFT_AGG_PAD values after the last one are zero when the read returns.
 */
typedef struct SiftSrcBuf
{
	void	   *data;
	uint8	   *isnull;
	uint64	   *ovf;			/* optional, bits for the values of the leaf the index cannot hold (SiftSrcVec.ovf): NULL, such a value is an error */
} SiftSrcBuf;

typedef struct SiftSrcUnit
{
	uint32		sb;
	int			nrow;
	uint32		nsu;			/* a superunit (sift_colsrc_super): the units it stands for; it has pre-aggregates only. 0 for a unit */
	void	   *priv;			/* the source's */
} SiftSrcUnit;

/*
 * A zone test the source may use to skip a unit without reading it: the unit is skipped when no value of the
 * integer-typed (I32/I64) leaf can lie in [lo, hi], nor in vals when there are nvals (sorted values, within [lo, hi]: a set too
 * sparse for the interval to tell). Conservative; the executor filters again. Only leaves of rows (not children of a group).
 * With nkids > 0 the entry holds no test of its own: a unit is skipped when one of the kids skips it (kids ANDed), or when all of them
 * do (any: an OR, where a disjunct that tests nothing is no kid at all, so the whole OR is none).
 */
typedef struct SiftSrcPrune
{
	int			leaf;
	int64		lo;
	int64		hi;
	int			nvals;
	const int64 *vals;
	bool		any;
	int			nkids;
	const struct SiftSrcPrune *kids;
	int			nulltest;		/* SSN_ISNULL / SSN_NOTNULL: the entry tests the NULLs of the unit's zone (a unit with none, or with only NULLs, is skipped); lo and hi are unused */
} SiftSrcPrune;
#define SSN_ISNULL		1
#define SSN_NOTNULL		2

/*
 * A test on a text leaf (SCV_CODE, top level scalar) that a source may run on the encoded form of its units instead of the executor
 * evaluating the predicate over the leaf's dictionary and codes. Bytewise, so the planner
 * pushes EQ, NE, IN, NOT IN, PREFIX and CONTAINS for any deterministic collation (CONTAINS only where a byte match is a character match) and the range operators only for the C collation. vals are
 * not copied: they outlive the call (prune_str: the scan).
 */
typedef enum SiftSrcStrOp
{
	SSTR_EQ = 1,
	SSTR_NE,					/* non-NULL values different from vals[0] */
	SSTR_LT,
	SSTR_LE,
	SSTR_GT,
	SSTR_GE,
	SSTR_IN,
	SSTR_NOTIN,					/* non-NULL values none of vals */
	SSTR_PREFIX,				/* values that start with vals[0] */
	SSTR_CONTAINS,				/* values that hold vals[0] (not empty), bytewise */
	SSTR_NOTCONTAINS,			/* non-NULL values that do not hold vals[0] */
	SSTR_SUFFIX,				/* values that end with vals[0] */
	SSTR_ICONTAINS				/* values whose lower() holds vals[0] (ASCII, lower case; nvals 3: and vals[1], vals[2] are lower() of U+0130 and U+212A) */
} SiftSrcStrOp;

typedef struct SiftSrcStrTest
{
	int			leaf;
	SiftSrcStrOp op;
	Oid			collid;			/* the collation the test compares under (set by the planner's recognizer; the source ignores it) */
	int			nvals;
	const char *const *vals;
	const uint32 *lens;
	const int64 *ints;			/* the operands of a test of an integer leaf (an element of a list of integers): vals and lens are unused */
} SiftSrcStrTest;

/*
 * A test of a conjunct on a wide leaf: SWT_* ops are the comparison operators of SiftCmpOp (0..5) and the four below. vals are the operands in
 * the type of the leaf (a NULL operand is isnull[i]; an IN or NOT IN list has nvals of them, a comparison one, a NULL test none).
 */
#define SWT_IN			6
#define SWT_NOTIN		7
#define SWT_ISNULL		8
#define SWT_NOTNULL		9
/* the operators of a leaf with shape != 0, the leaf on the left (SIFT_COLW_R_*, SIFT_COLW_N_* of col/col.h are the same numbers); the operand is a range, an element of its subtype, or a network */
#define SWT_SHAPE_FIRST	16
#define SWT_R_CONTAINS	16
#define SWT_R_CONTAINED	17
#define SWT_R_OVERLAPS	18
#define SWT_R_BEFORE	19
#define SWT_R_AFTER		20
#define SWT_R_OVERLEFT	21
#define SWT_R_OVERRIGHT	22
#define SWT_R_ADJACENT	23
#define SWT_R_ELEM		24
#define SWT_N_SUB		25
#define SWT_N_SUBEQ		26
#define SWT_N_SUP		27
#define SWT_N_SUPEQ		28
#define SWT_N_OVERLAP	29
#define SWT_R_EQ		30
#define SWT_N_EQ		31

typedef struct SiftSrcWideTest
{
	int			leaf;
	int			op;
	int			nvals;
	const Datum *vals;
	const bool *isnull;
} SiftSrcWideTest;

/* The zone of a scalar leaf in a unit: min and max of the integer-typed (SCV_I32/SCV_I64) values, dead rows included (docs/COL-SEARCH.md, top-N) */
typedef struct SiftSrcZone
{
	int64		min;			/* meaningless when nnull == nvals */
	int64		max;
	uint32		nvals;
	uint32		nnull;
	bool		inexact;		/* some value is outside what min/max describe (overflow): no bound */
} SiftSrcZone;
/*
 * Pre-aggregates (docs/COL-PREAGG.md), optional provider callbacks: what a unit knows about its rows without reading a chunk. A unit may answer
 * only when it is fully visible and clean (unit_exact: no dead row, every page it has rows on all-visible to the snapshot; a unit of the write
 * buffer never is); the consumer applies its predicate on top (a unit whose zone lies inside the predicate answers whole, any other is scanned).
 * unit_agg / unit_hist / unit_rollup return false when the unit cannot answer (not exact, an overflow value, no histogram for that dim in that unit).
 * Memory of a result is valid until the next unit_hist or unit_rollup call of the source.
 */
typedef __int128 SiftSrcI128 __attribute__((aligned(8)));

#define SIFT_SRC_MAX_DIMS		32
#define SIFT_SRC_MAX_MEAS		8
#define SIFT_SRC_MAX_RDIMS		6
#define SIFT_SRC_MAX_ROLLUPS	8

typedef struct SiftSrcUnitAgg
{
	int64		nvals;			/* rows of the unit (elements, for a child leaf) */
	int64		nnull;
	bool		has_minmax;		/* some value is not NULL and the leaf is not text: min and max are set */
	int64		min;			/* SCV_I32 / SCV_I64 leaves: the values as stored (numeric: times 10^scale) */
	int64		max;
	double		min_f;			/* SCV_F64 leaves (NaN is the largest value, as in PostgreSQL's order) */
	double		max_f;
	bool		has_sums;		/* integer leaf: sum and sumsq are exact */
	bool		has_sumsq;		/* sumsq did not overflow int128 */
	SiftSrcI128 sum;
	SiftSrcI128 sumsq;
	double		sum_f;			/* SCV_F64: the sum in the unit's row order (the order caveat of a parallel sum) */
	bool		has_nan;
	bool		is_const;		/* every non-NULL value equal */
	bool		loose;			/* a unit net of its dead rows: min and max only bound the values left (the counts and the integer sums are exact) */
} SiftSrcUnitAgg;

/*
 * The distinct non-NULL values of a leaf in one unit: a bitmap (bit i of word w, little endian and not aligned, is the value 64 * (wbase + w) + i) or,
 * with nkeys > 0, the list of them in ascending order
 */
typedef struct SiftSrcPresence
{
	int64		wbase;
	uint64		nwords;
	const uint8 *words;
	uint32		nkeys;
	const int64 *keys;
} SiftSrcPresence;

/* GROUP BY dim over one unit: count of rows per group and, per stored measure, the non-NULL count and the sum. The NULL group, when there is one, is last */
typedef struct SiftSrcHist
{
	uint32		ngroups;
	bool		has_null;
	const int64 *key;			/* group value as stored; SCV_CODE dims: a code of dict (the executor translates by value) */
	const SiftSrcDict *dict;	/* SCV_CODE dims */
	const uint32 *count;
	int			nmeas;
	int			meas[SIFT_SRC_MAX_MEAS];	/* leaf ids */
	const uint32 *nn[SIFT_SRC_MAX_MEAS];
	const SiftSrcI128 *sum[SIFT_SRC_MAX_MEAS];	/* integer measure (numeric: times 10^scale), else NULL */
	const double *sumf[SIFT_SRC_MAX_MEAS];		/* float measure, else NULL */
	const int64 *min[SIFT_SRC_MAX_MEAS];		/* integer measure: the smallest and largest value of each group, NULL when the unit does not store them; meaningless where nn is 0 */
	const int64 *max[SIFT_SRC_MAX_MEAS];
	int			grain;			/* a calendar dim: the grain of the cells (SIFT_CAL_*, in col/preagg.h: 1 day, 2 month, 3 year), else 0 */
} SiftSrcHist;

/*
 * The rollup of the elements of one list (an array of objects, an array of scalars, a map) in one unit: the distinct tuples of the values of its dims
 * (low-cardinality leaves of the elements) that the unit holds, each with the elements it stands for and, per measure, the non-NULL count, the sum, the
 * smallest and the largest value. Tuples are in ascending order of the dims (the first dim first, a dim's NULL after its values). A GROUP BY over
 * any subset of the dims, a filter on them and these aggregates are answered by adding the tuples up. Memory valid until the next unit_rollup call.
 */
typedef struct SiftSrcRollup
{
	uint32		ntuples;
	int			ndims;
	int			dim[SIFT_SRC_MAX_RDIMS];	/* leaf ids */
	const int64 *key[SIFT_SRC_MAX_RDIMS];	/* [ntuples] as stored; SCV_CODE dims: a code of dict[j] (the executor translates by value) */
	const uint8 *isnull[SIFT_SRC_MAX_RDIMS];	/* [ntuples], NULL: no tuple has a NULL there */
	const SiftSrcDict *dict[SIFT_SRC_MAX_RDIMS];	/* SCV_CODE dims, else NULL */
	const uint32 *count;
	int			nmeas;
	int			meas[SIFT_SRC_MAX_MEAS];	/* leaf ids */
	const uint32 *nn[SIFT_SRC_MAX_MEAS];
	const SiftSrcI128 *sum[SIFT_SRC_MAX_MEAS];	/* integer measure (numeric: times 10^scale), else NULL */
	const double *sumf[SIFT_SRC_MAX_MEAS];		/* float measure, else NULL */
	const int64 *min[SIFT_SRC_MAX_MEAS];		/* integer measure; meaningless where nn is 0 */
	const int64 *max[SIFT_SRC_MAX_MEAS];
	const double *minf[SIFT_SRC_MAX_MEAS];		/* float measure */
	const double *maxf[SIFT_SRC_MAX_MEAS];
} SiftSrcRollup;

/* The dims and measures of a rollup a unit_rollup caller reads; the arrays of the others may be NULL in the result (the source skips them) */
typedef struct SiftSrcRollupNeed
{
	bool		all;			/* more leaves than the arrays hold: every column */
	int			ndims;
	int			dim[SIFT_SRC_MAX_RDIMS + SIFT_SRC_MAX_MEAS];	/* leaf ids: the dims of the query and the leaves of count(leaf) */
	int			nmeas;
	struct
	{
		int			leaf;
		bool		sums;			/* sum / sumf */
		bool		minmax;			/* min and max, minf and maxf */
	}			meas[SIFT_SRC_MAX_MEAS];	/* nn is always given */
} SiftSrcRollupNeed;

/*
 * Planner: the histograms and rollups the index writes (a unit may still lack one, or hold fewer dims of a rollup); ndims 0 when none.
 * aggs: units have unit_agg sums (always, even with ndims 0)
 */
#define SIFT_SRC_CAL	128			/* unit_hist(dim): the calendar cells of a date or timestamp leaf, at the grain the unit has, are SIFT_SRC_CAL + the leaf */

typedef struct SiftSrcPreagg
{
	bool		aggs;
	int			npres;			/* the integer leaves whose units keep every distinct value as a key list or bitmap (key_columns), answered by unit_presence */
	int			pres[SIFT_SRC_MAX_DIMS];
	bool		hll[SIFT_SRC_CAL];	/* by leaf: the leaves whose units keep a HyperLogLog sketch (hll_columns), answered by unit_hll */
	int			ndims;
	struct
	{
		int			leaf;
		int			nmeas;
		int			meas[SIFT_SRC_MAX_MEAS];
		bool		minmax;			/* the units written from now on store min and max of the integer measures per group */
		bool		ktab;			/* the dim is only the key table of the segments (a key leaf of the index with no cube dim): a whole segment answers, no unit */
		bool		cal;			/* leaf is a date or timestamp leaf and the histogram groups it by calendar cells (keys: the first day of a cell, days since 2000-01-01; SiftSrcHist.grain), asked for as unit_hist(SIFT_SRC_CAL + leaf) */
	}			dim[SIFT_SRC_MAX_DIMS];
	int			nrollups;
	struct
	{
		int			list;			/* the SCL_ARRAY / SCL_OBJECT leaf; -1: the rollup of the rows of a unit */
		int			ndims;
		int			dim[SIFT_SRC_MAX_RDIMS];
		int			nmeas;
		int			meas[SIFT_SRC_MAX_MEAS];
	}			rollup[SIFT_SRC_MAX_ROLLUPS];
} SiftSrcPreagg;

/*
 * A conjunct the source may evaluate on its stored (encoded) values instead of the executor on decoded ones: `leaf op constant` on an
 * integer-valued scalar leaf of rows (int, date, timestamp, numeric as its scaled integer), with the SQL meaning (a NULL value never
 * passes). A source that accepts it (set_filters) applies it exactly: the executor does not test the rows again, and the source may use
 * it to skip units (docs/COL-PLAN.md, "Filters in the source"). The text tests are SiftSrcStrTest above.
 */
typedef enum SiftSrcFilterOp
{
	SSF_EQ,
	SSF_NE,
	SSF_LT,
	SSF_LE,
	SSF_GT,
	SSF_GE,
	SSF_IN,
	SSF_NOTIN,
	SSF_NOTNULL,				/* a value is present */
	SSF_RANGES,					/* the union of ranges: set holds nset / 2 pairs (lo, hi), inclusive, ascending and apart, PG_INT64_MIN / PG_INT64_MAX where a side is open */
	SSF_RANGES_HINT				/* the same, for rows the executor tests again: a unit that holds many of the ranges is left as it is */
} SiftSrcFilterOp;

typedef struct SiftSrcFilter
{
	int			leaf;
	SiftSrcFilterOp op;
	int64		val;			/* a comparison */
	int			nset;			/* IN, NOTIN: the members; RANGES: twice the number of ranges */
	const int64 *set;
} SiftSrcFilter;

#define SIFT_COLSRC_MAX_FILTERS		64

/*
 * The Vulkan path (docs/GPU.md, optional provider callbacks gpu_*): the identity of a unit whose stored chunks stand for the rows a query
 * sees, and a leaf's codec blob. The chunk cache of the device is keyed by (store, gen, unit, leaf): a segment is
 * immutable, a new generation is new storage.
 */
typedef struct SiftSrcGpuUnit
{
	uint64		store;			/* database and relation file of the index */
	uint64		gen;			/* the segment generation */
	uint32		unit;			/* the unit's number in the segment */
	uint32		nrows;
} SiftSrcGpuUnit;

typedef struct SiftSrcGpuChunk
{
	const uint8 *blob;			/* valid until the next gpu_chunk of the source; NULL for a constant chunk */
	uint32		len;
	uint32		n;
	bool		is_const;		/* no bytes: every row has cval */
	int64		cval;
} SiftSrcGpuChunk;

/* Counters for EXPLAIN ANALYZE */
typedef struct SiftSrcCounters
{
	int64		units;			/* units returned */
	int64		units_pruned;	/* skipped before they were read: by their zones, a small-set summary or the search selection */
	int64		units_set_pruned;	/* of those, the ones only a small-set summary skipped */
	int64		units_search_pruned;	/* of those, the ones without a row of the search selection */
	int64		rows;			/* rows of the units returned */
	int64		bytes;			/* bytes decoded */
	int64		rows_vetted;	/* rows checked against the heap */
	int64		heap_pages;		/* heap pages read for them */
	int64		rechecked;		/* rows of units read again from the heap (SiftSrcProvider.set_recheck) */
	int64		recheck_units;	/* the units that had such rows */
	int64		point_units;	/* units narrowed to the rows a point directory lists */
	int64		point_rows;		/* and the rows they were narrowed to */
	int64		run_windows;	/* units whose range on a leaf with a sorted run was answered by the run */
} SiftSrcCounters;

/*
 * A provider: the column source of one kind of index or table, registered at load time by the module that implements it.
 * `impl` is the provider's source object. The functions of the seam below dispatch to the provider that owns the relation's
 * access method.
 */
#define SIFT_SRC_HEAD_MAX	32		/* the most entries of a run head at each end (col/runs.h SIFT_CRUN_HEAD_MAX) */

typedef struct SiftSrcHead
{
	int64		key;
	uint64		addr;			/* block << 16 | offset */
} SiftSrcHead;

#endif							/* SIFT_AGG_COLSRC_EXEC_H */
