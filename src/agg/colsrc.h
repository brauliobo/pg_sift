/*
 * agg/colsrc.h - the column source SiftAgg reads (planner metadata and executor read API).
 *
 * This is the narrow interface of the aggregate track (col-plan) to the column storage. The storage is either
 *   - a `sift_col` index over a heap table: the values are copies taken at aminsert/ambuild, a row may be used only
 *     if its heap page is all-visible (visibility map) or the heap says its tuple is visible;
 *     the executor asks for that vetting (sift_colsrc_vet) after the value filters, or
 *   - a `sift_table` TABLE (table access method): the table is the columns, its scan is MVCC by itself (sift_colsrc_vet has
 *     nothing to check).
 * Everything above this header is the same for both. The implementations are providers (SiftSrcProvider) registered by their
 * module: the sift_col index, the sift_table table, and the test
 * stub (src/agg/stub*.c, the `sift_colstub` index AM, SIFT_TESTING builds only), which reads the heap pages raw and builds
 * the same units. The functions below dispatch on the access method of the relation. Keep this header small: it is the seam.
 *
 * Model: a source has leaves (ids 0..nleaves-1). A scalar leaf is one value per row; a GROUP
 * leaf is an array (or object) of elements: per row it has an element count (NULL: absent), and its child leaves hold
 * one value per element, all elements of the unit concatenated in row order (child element j of every child of a group
 * is the same array element). Rows of a unit are numbered 0..nrow-1 in TID order (the row ordinal); a child element's
 * parent ordinal is recovered from the counts by sift_colsrc_parents().
 *
 * A unit is the rows of one segment inside one superblock (SIFT_SB_PAGES heap pages); an SB can be held by several
 * units (disjoint rows). Dictionaries of text leaves are per unit source (per segment): codes are NOT comparable
 * between units whose SiftSrcDict::id differs; the executor translates by value.
 */
#ifndef SIFT_COLSRC_H
#define SIFT_COLSRC_H

#include "core/host.h"

#include "core/sift.h"
#include "core/filter.h"

struct SiftCGOrdBuf;

#define SIFT_COLSRC_MAX_LEAVES		128

/* The element type of the vector a leaf is decoded to */
/* how unit_dead reads a unit with dead rows */
typedef enum SiftSrcDead
{
	SSD_OFF,
	SSD_BOUNDS,
	SSD_ON
} SiftSrcDead;

typedef enum SiftSrcVecType
{
	SCV_I32,					/* int32: int2, int4, date, bool (0/1) */
	SCV_I64,					/* int64: int8, timestamp, timestamptz */
	SCV_F64,					/* double: float4 (widened exactly) and float8 */
	SCV_CODE					/* uint32 code into the unit's dictionary of the leaf: text, varchar, char(n), name */
} SiftSrcVecType;

typedef enum SiftSrcLeafKind
{
	SCL_SCALAR,					/* a value per row (or per element, for a child leaf) */
	SCL_ARRAY,					/* a jsonb array of objects: element count per row; children are its fields */
	SCL_OBJECT					/* a jsonb object with free keys: element count per row; two children, "key" (CODE) and "value" */
} SiftSrcLeafKind;

typedef struct SiftSrcLeaf
{
	int			id;
	int			attno;			/* 1-based index column (table: heap column) the leaf is read from */
	int			parent;			/* -1, or the id of the SCL_ARRAY / SCL_OBJECT leaf this leaf is a child of */
	SiftSrcLeafKind kind;
	SiftSrcVecType vtype;		/* of the values; of the counts (SCV_I32) for a group leaf */
	Oid			typid;			/* SQL type of the values: what the query's expression has to produce for a match. InvalidOid for a wide leaf: no
								 * expression reads its values (wtypid) */
	int			shape;			/* the head of a range (1) or a network (2) that also keeps the lanes its containment operators test (SWT_R_*, SWT_N_*; col/shape.c); else 0 */
	Oid			wtypid;			/* a wide leaf: its SQL type, which only a conjunct of the query tests (SAP_WIDE); else InvalidOid */
	bool		wide;			/* numeric of more than 18 digits or any other shape, interval: an SCV_CODE leaf whose dictionary entries are
								 * not text (src/agg/wide.h): only an expression PostgreSQL evaluates reads it, no kernel, native aggregate, histogram or text test */
	bool		arrhead;		/* the image of an array that keeps its lengths: the leaves id + 1 and id + 2 are its arrlen lanes */
	bool		noimage;		/* ... and the head is NULL for every array a list of one dimension from 1 holds (SIFT_COLD_NOIMAGE) */
	int			arrlen;			/* 1: this leaf is cardinality(x) of the array whose image is leaf id - 1, 2: array_length(x, 1) of the one at id - 2; else 0 */
	int			scale;			/* numeric leaf (typid NUMERICOID, SCV_I64): digits after the decimal point, the value is the number times 10^scale */
	bool		lossy;			/* the shredder may have left values out of the leaf (overflow): no predicate may use it */
	bool		astext;			/* a text leaf of the `->>` text of any scalar: not the strings a jsonpath, `@>` or `?` compares */
	List	   *path;			/* jsonb path below the attribute, a list of String nodes; NIL for a plain attribute.
								 * Array/object leaf: the path of the array; child: NIL and name set */
	char		name[NAMEDATALEN];	/* child leaf of a group: the object field name ("key"/"value" for SCL_OBJECT) */
} SiftSrcLeaf;

typedef struct SiftSrcLayout
{
	Oid			heapoid;		/* the table the rows belong to */
	bool		is_table;		/* a sift_table table (rows already visible) rather than an index over a heap */
	int			natts;			/* attributes of the index (table: columns of the table) */
	List	   *attexprs;		/* natts expressions over the heap row (Var nodes with varno 1, or the index expressions
								 * with varno 1); a jsonb attribute's leaves are described by path. Copies, palloc'd */
	List	   *attalias;		/* NIL or natts entries: a second expression that denotes the same attribute (NULL: none). A sift_table keeps the
								 * generation expression of a stored generated column, so that the column is read where the query writes the expression */
	int			nleaves;
	SiftSrcLeaf *leaf;
} SiftSrcLayout;

/*
 * The zones of a sample of the source's units, so that the planner tests the constants of a query against the real min and max of each unit
 * instead of assuming how the values spread: lo[leaf] and hi[leaf] are n values each for an integer leaf of rows, NULL for the others. A unit
 * the leaf has no value in has lo > hi. covered: the share of the source's units the sample describes (a write buffer's are not in it).
 */
typedef struct SiftSrcZones
{
	int			n;
	double		covered;
	const int64 *lo[SIFT_COLSRC_MAX_LEAVES];
	const int64 *hi[SIFT_COLSRC_MAX_LEAVES];
} SiftSrcZones;

/* Planner side: statistics the cost model uses (all estimates, never errors) */
typedef struct SiftSrcStats
{
	double		nrows;			/* rows in the source (dead included) */
	double		nunits;			/* units */
	double		bytes;			/* stored (compressed) bytes of the leaves together, 0 when unknown */
	double		allvisible_frac;	/* share of the heap pages that are all-visible (1 for a table) */
	double		exact_frac;		/* share of the units whose pre-aggregates may answer: those on all-visible pages of a heap, the plain superblocks of a table */
	double		dim_miss[SIFT_COLSRC_MAX_LEAVES];	/* share of the units without the histogram of a leaf, without its min and max, and without the rollup of a list: units written before the options asked for them (0: the source does not tell) */
	double		mm_miss[SIFT_COLSRC_MAX_LEAVES];
	double		roll_miss[SIFT_COLSRC_MAX_LEAVES];
	double		rows_miss;		/* the same for the rollup of the rows */
	double		ndistinct[SIFT_COLSRC_MAX_LEAVES];	/* distinct values of a scalar leaf (0: unknown) */
	double		min[SIFT_COLSRC_MAX_LEAVES];		/* numeric leaves: smallest and largest value as double (0, 0 unknown) */
	double		max[SIFT_COLSRC_MAX_LEAVES];
	double		point_cover[SIFT_COLSRC_MAX_LEAVES];	/* the share of the units whose segment keeps a point directory of the leaf, which lists the rows of a short list of keys (0: none does) */
	double		set_cover[SIFT_COLSRC_MAX_LEAVES];	/* the share of the units that hold a small-set summary of the leaf (0: none do) */
	double		set_keys[SIFT_COLSRC_MAX_LEAVES];	/* ... and the distinct keys such a unit holds on average */
	double		head_cover[SIFT_COLSRC_MAX_LEAVES];	/* the share of the units whose sorted run of the leaf holds a head, the best 32 keys at each end with their heap addresses (order_columns): ORDER BY .. LIMIT 32 or less reads the head of a few units (0: none does) */
	double		gord_cover[SIFT_COLSRC_MAX_LEAVES];	/* the share of the values of a leaf that the segments keep in a plain global order (order_array_columns): ORDER BY .. LIMIT reads n entries of it, not the units (0: none does) */
	int			gord_group[SIFT_COLSRC_MAX_LEAVES];	/* the leaf by which most of the values are also (or only) kept in an order sorted by group (order_array_group): a scan that pins that leaf reads n entries of its group, -1: none */
	double		zone_span[SIFT_COLSRC_MAX_LEAVES];	/* integer leaves: the mean width of the zone of a unit as a share of the leaf's range (0: the source has no zones to tell) */
	const SiftSrcZones *zones;	/* the zones of a sample of the units (NULL: the source has none to show, zone_span is the whole of it) */
	double		elems_per_row[SIFT_COLSRC_MAX_LEAVES];	/* group leaves: average elements per row */
	double		ovf_units[SIFT_COLSRC_MAX_LEAVES];	/* share of the units flagged with an overflow value of the leaf (at least one unit when ovf: a write buffer may hold one) */
	double		ovf_frac[SIFT_COLSRC_MAX_LEAVES];	/* of the rows of a unit flagged with an overflow value of the leaf, the share a scan reads again from the heap, from a sample of the flagged units (negative: none could be sampled); of the rows, for an element leaf: its share times the elements of a row */
	bool		ovf[SIFT_COLSRC_MAX_LEAVES];	/* some unit holds values the leaf cannot store (overflow): reading it raises an error, a numeric leaf's NaNs apart */
	bool		jnull[SIFT_COLSRC_MAX_LEAVES];	/* some unit may hold a JSON null in the leaf (a write buffer, or a segment that cannot tell it from a missing key, may): its cast to a number raises before PostgreSQL 18 */
	bool		nokinds[SIFT_COLSRC_MAX_LEAVES];	/* some segment may not tell a JSON null from a missing key in the leaf, or the type of an astext value: a test that needs it raises the REINDEX error */
} SiftSrcStats;

/*
 * The state of an index the facts of the planner (its layout, statistics and pre-aggregates) are read at: the same stamp, and no change of the
 * catalog entry of the index or of its table, means the same facts. A stamp is cheap to take (a few pages), the facts are not.
 */
typedef struct SiftSrcStamp
{
	uint32		file;			/* the index's file: a REINDEX starts the directory's versions over */
	uint32		options;		/* the options and format the units are counted against */
	uint32		version;		/* the directory's: bumped by every change of the segments */
	uint32		pages;			/* the pages of the table and those the visibility map says are all-visible */
	uint32		visible;
} SiftSrcStamp;

/* What the zones of the stored units of an index source depend on (SiftSrcProvider.walk_stamp) */
typedef struct SiftSrcWalkStamp
{
	Oid			index;
	uint32		file;
	uint32		version;
} SiftSrcWalkStamp;

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
	SSTR_ICONTAINS				/* values whose lower() holds vals[0] (ASCII, lower case; nvals 3: and vals[1], vals[2] are lower() of U+0130 and U+212A) */
} SiftSrcStrOp;

typedef struct SiftSrcStrTest
{
	int			leaf;
	SiftSrcStrOp op;
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

typedef struct SiftSrcProvider
{
	bool		(*owns_index) (Oid indexrelam);
	bool		(*owns_table) (Oid tablerelam);
	SiftSrcLayout *(*layout) (Relation rel);
	void		(*stats) (Relation rel, SiftSrcStats *st);
	/* Optional, required with open_part: what changes the layout, statistics and pre-aggregates of an index besides its catalog entries and its table's (sift_colsrc_stamp) */
	void		(*stamp) (Relation index, SiftSrcStamp *stamp);
	void	   *(*open) (Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
	/* Optional (NULL: the provider has no partitioned tables, sift_colsrc_can_part): open for a partition read as part of its partitioned table, whose privileges and row security were checked */
	void	   *(*open_part) (Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
	void		(*close) (void *impl);
	/* Optional: release the buffers the source keeps pinned between units; a partitioned table's scan calls it when it leaves a partition */
	void		(*unpin) (void *impl);
	const SiftSrcLayout *(*source_layout) (const void *impl);
	uint32		(*nsb) (void *impl);
	void		(*begin) (void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune);
	/*
	 * Optional: sets bit sb of kept for every superblock (below nspace) in which a begin with these prune entries would find a unit, and counts the units it would
	 * prune everywhere in the source's counters (a begin then counts none); false when the source cannot tell.
	 */
	bool		(*kept) (void *impl, const SiftSrcPrune *prune, int nprune, uint32 nspace, uint64 *kept, uint64 *rows);
	bool		(*next) (void *impl, SiftSrcUnit *unit);
	void		(*read) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);
	void		(*live) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	void		(*vet) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	/* Optional (NULL: always): false when vet would clear no row of the unit (every page is all-visible, or the source is MVCC by itself) */
	bool		(*vet_needed) (void *impl, const SiftSrcUnit *unit);
	void		(*counters) (const void *impl, SiftSrcCounters *c);
	/*
	 * Optional (NULL: the executor tests the values itself). prune_str, called before `begin`, adds text tests to the zone pruning of the
	 * scan: units whose zones prove that no value can satisfy one are skipped (and counted in units_pruned); for a unit that stores its
	 * text as codes this reads the segment dictionary once, for an inline unit the prefix zone. filter_str ANDs `rows` (the unit's nrow bits,
	 * positions as in `read` and `live`) with the rows whose value satisfies the test, NULL never; it returns false, leaving rows as
	 * they were, when it cannot (the executor then reads the leaf and tests it). The tests are exact, not conservative.
	 */
	void		(*prune_str) (void *impl, const SiftSrcStrTest *t, int nt);
	bool		(*filter_str) (void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows);
	/*
	 * Optional, both or neither (sift_colsrc_can_wide): the tests of a conjunct on a wide leaf. set_wide, called before `begin`, takes the tests of the scan (operands as Datums, in
	 * the type of the leaf), joins them to the zone pruning and returns how many it will apply: the executor does not evaluate them. filter_wide
	 * ANDs `rows` with the rows that pass test i and says whether any is left; NULL never passes, a NaN as PostgreSQL orders it.
	 */
	int			(*set_wide) (void *impl, const SiftSrcWideTest *t, int nt);
	bool		(*filter_wide) (void *impl, const SiftSrcUnit *unit, int i, uint64 *rows);
	/*
	 * Optional (NULL: sift_colsrc_read_into reads with `read` and copies). leaf_n: the values a read of the leaf yields (rows, or for a child
	 * the elements of the whole unit; an upper bound for a unit restricted to a match set), without reading it. read_into: `read` into the caller's storage (SiftSrcBuf), the form `read` itself
	 * is a wrapper of in a provider that has it.
	 */
	int64		(*leaf_n) (void *impl, const SiftSrcUnit *unit, int leaf);
	void		(*read_into) (void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec);
	/*
	 * Optional (NULL: the source cannot be restricted to a match set, sift_colsrc_can_select says so to the planner). select, called before
	 * `begin`, restricts the units `begin` starts to the rows whose key (blk << 16 | off) the filter has (NULL: all rows again): a unit
	 * without such a row is not returned (it counts in units_pruned), and in the others `nrow`, `read`, `live` and `vet` are about the kept
	 * rows only, which the source may renumber 0..nrow-1 in key order (docs/COL-SEARCH.md, src/agg/sel.h).
	 */
	void		(*select) (void *impl, const SiftFilter *keep);
	/* Optional (NULL: no unit has pre-aggregates), see SiftSrcUnitAgg; `preagg` is the planner's, over the index relation */
	void		(*preagg) (Relation rel, SiftSrcPreagg *pa);
	bool		(*unit_exact) (void *impl, const SiftSrcUnit *unit);
	/*
	 * Optional: a unit that is not exact because rows of it are dead answers net of them (counts and integer sums, min and max loose) while SSD_ON; returns the number of dead rows, -1
	 * when it cannot (then nothing is on). SSD_BOUNDS only checks that it could, for the cost of its pages' visibility: unit_agg then gives the zone (min, max, has_minmax) of the unit
	 * with its dead rows, a bound of the live ones, and nothing else is valid. SSD_OFF reads the unit as its own rows again.
	 */
	int			(*unit_dead) (void *impl, const SiftSrcUnit *unit, int mode);
	/*
	 * Optional (NULL: no unit has exceptions). Leaf >= 0: when the exact unit holds the pre-aggregates of its rows where `leaf` is not NULL (docs/COL-PREAGG.md 11), the
	 * next unit_hist and unit_rollup calls on it return what the rows where `leaf` IS NULL add up to (counts and sums net of those rows, no minimum or maximum; false
	 * from them when they cannot tell) and this returns true. Leaf -1: the unit's own rows again. unit_agg and unit_presence refuse a unit set to a leaf.
	 */
	bool		(*unit_except) (void *impl, const SiftSrcUnit *unit, int leaf);
	bool		(*unit_agg) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg);
	bool		(*unit_hist) (void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h);
	/* Optional (NULL: none): the presence of an exact unit's leaf of SiftSrcPreagg.pres, valid until the next call; false when the unit has none */
	bool		(*unit_presence) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p);
	/* Optional (NULL: none): the HyperLogLog sketch of an exact unit's leaf of SiftSrcPreagg.hll (col/hll.h), valid until the next call; false when the unit has none */
	bool		(*unit_hll) (void *impl, const SiftSrcUnit *unit, int leaf, const uint8 **blob);
	uint32		(*unit_rollup_tuples) (void *impl, const SiftSrcUnit *unit, int list);
	bool		(*unit_rollup) (void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *r);
	/* Optional: the consumer scanned an exact unit because its rollup lacks what the query reads; the source may arrange for the unit to get it. True: a refresh is queued for it */
	bool		(*unit_rollup_miss) (void *impl, const SiftSrcUnit *unit);
	/* Optional (NULL: none): `next` may hand over a group of exact units as one unit that has pre-aggregates only (nsu > 0); split gives the units of the group back, one by one, to the next calls */
	void		(*super) (void *impl, bool on);
	bool		(*whole) (void *impl, int leaf, const int *meas, int nmeas);	/* a unit may be a whole segment whose key table of leaf, with these measures, answers (a superunit that has that histogram only); true: an item of the scan has to span a segment for that, which a source with no superunits does not do alone */
	/* Optional (NULL: none), after super: the count(*) of the rows of text leaf that pass one test (op a SiftSrcStrOp of EQ, LT, LE, GT, GE or PREFIX against s) and nothing else; a segment whose dictionary holds the rows of each string may be handed over as one unit (nsu > 0) whose unit_count is the rows that pass: the consumer folds them and reads nothing */
	void		(*count_range) (void *impl, int leaf, SiftSrcStrOp op, const char *s, uint32 len);
	/* Optional: the same for the comparisons of an integer leaf, all the conditions of the scan: a segment whose global order of the leaf holds the keys counts them */
	void		(*count_cmp) (void *impl, int leaf);
	bool		(*unit_count) (void *impl, const SiftSrcUnit *unit, int64 *rows);
	/* Optional (NULL: none): the element tests (prune_str's tests that stand for an EXISTS over a list, bit i: the i-th test given) whose rows the unit's point directory lists exactly: the unit comes narrowed to them, and the executor need not test them again */
	uint64		(*point_exact) (void *impl, const SiftSrcUnit *unit);
	void		(*split) (void *impl, const SiftSrcUnit *unit);
	uint32		(*super_span) (void *impl);	/* the superblocks a range of a parallel scan should span for a group of units to fit in it */
	/*
	 * Optional (NULL: no ordered top-N scan on this source, sift_colsrc_can_tids says so to the planner). tids: the heap TIDs of the rows
	 * `rows` (ascending, in the numbering `read` and `live` use) of the unit the source is at. zone: the zone of an integer-typed leaf
	 * of the unit the source is at, over the unit as stored.
	 */
	void		(*tids) (void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
	/* Optional (NULL: yes): is the number of every unit the heap superblock its rows are in? Not when a segment cuts its units by key or TID range: a TID range then selects no units by number */
	bool		(*sb_heap) (void *impl);
	void		(*zone) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z);
	/* zone_range: the zone of a leaf over all the units of the superblocks [lo, hi) together, without opening them; returns how many units it counts (required with tids) */
	uint32		(*zone_range) (void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
	/*
	 * Optional (both or neither): the zone of the stored units alone, and what those zones depend on, so that a top-N may keep the order of its walk between queries:
	 * walk_stamp fills the identity of the stored units (a REINDEX or any change of the segments gives another) and lists, ascending, the superblocks that also hold
	 * rows of a write buffer (at most cap); false: the zones of this source are not kept
	 */
	uint32		(*zone_range_segs) (void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
	bool		(*walk_stamp) (void *impl, SiftSrcWalkStamp *st, uint32 *mem, int cap, int *nmem);
	/* optional (NULL: none does): can the unit the source is at hold, in `leaf`, a value the source cannot store (SiftSrcVec.ovf)? */
	bool		(*overflow) (void *impl, const SiftSrcUnit *unit, int leaf);
	/*
	 * Optional (NULL: no sorted runs): the sorted run of a leaf of the unit the source is at, in the numbering `read` uses: false when the
	 * unit has none or its rows are not numbered as stored (a unit of a match set, rows read again from the heap, a unit narrowed to a few rows). The rows
	 * of positions [0, nvals) of the run have a value, ascending by (value, row), the next nnull are the NULL rows ascending, the rest are the rows whose
	 * value the leaf cannot store, ascending. run_read gives the rows at positions [pos, pos + cnt) of that run. id names the unit within the scan. walk: the
	 * caller will read the rows of the run (not only its head); a source may refuse a run that is the rows in their own order, which reads no faster than the unit.
	 */
	bool		(*run_info) (void *impl, const SiftSrcUnit *unit, int leaf, bool walk, uint32 *nvals, uint32 *nnull, uint64 *id);
	void		(*run_read) (void *impl, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows);
	/*
	 * The global order of a leaf (optional, col/gorder.h): every row's key and heap address sorted, in sources (stored segments, the rows of the write
	 * buffers). gorder_open: their number, or -1 when the order cannot answer. gorder_info: the entries of source i and how many have a value (the first ones;
	 * the others are NULL rows by address). gorder_head: the first max(w, up to p) entries of the chunk that holds entry p, as keys and heap addresses into buf (SiftCGOrdBuf: the chunk's first entry, its entries, the count decoded). The entries are
	 * of rows that may have died since: the caller has the heap say which are live and what they hold.
	 */
	int			(*gorder_open) (void *impl, int leaf, int rows, int npin, const int *pinleaf, const int64 *pinval, int *grouped);	/* grouped: the leaf the blocks are grouped by, -1; pins: conditions leaf = value of the query, which a block grouped by one of the leaves needs */
	bool		(*gorder_info) (void *impl, int i, int leaf, uint32 *entries, uint32 *values);
	void		(*gorder_head) (void *impl, int i, int leaf, uint32 p, uint32 w, struct SiftCGOrdBuf *buf);
	void		(*gorder_range) (void *impl, int i, int leaf, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne);	/* the value entries [vs, ve) and the NULL ones [ns, ne) of the pinned group (all of a source that is not grouped) */
	uint32		(*gorder_seek) (void *impl, int i, int leaf, uint32 vs, uint32 ve, int64 key, bool after, struct SiftCGOrdBuf *keep);	/* the first entry of [vs, ve) with key >= key (after: > key); ve when none; a segment leaves the chunk it ends in in keep (when not NULL) */
	/*
	 * Optional (needs run_info found a run): the head of that run, the best SiftSrcRunHeads.n entries of the run read ascending (lo) and descending (hi)
	 * with the heap address of each row; 0 when the run has none. Stored when the unit was written: sound only if every row of the unit still passes.
	 */
	/* Optional: run_heads for the unit of superblock sb without reading it, only when every row of the unit is live and visible (the unit of a table, of a key-cut segment or of a write buffer has none) */
	bool		(*sb_heads) (void *impl, uint32 sb, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
	int			(*run_heads) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
	/* read for the rows `rows` (ascending) only, valid until the next read of the leaf: vec->n == n, element k is row rows[k] */
	void		(*read_rows) (void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);
	/* optional: the int filters above. set_filters (before begin) returns the bits of those it will apply, filter narrows the rows of a unit by them */
	uint64		(*set_filters) (void *impl, const SiftSrcFilter *f, int nf);
	bool		(*filter) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	/* optional: read only the rows of bits (the other rows of the vector are zero; a source restricted to a match set may read the whole leaf) */
	void		(*read_sel) (void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);
	/*
	 * optional: the rows of bits, which are few, are all the executor needs of the unit. When true, the unit goes on as those rows alone,
	 * renumbered 0..unit->nrow-1 as in a unit of a match set: `nrow`, `read`, `live`, `vet` and the tests are about them, and what the executor
	 * read of the unit before is stale. False: nothing changed.
	 */
	bool		(*narrow) (void *impl, SiftSrcUnit *unit, const uint64 *bits);
	/*
	 * Optional, both or neither (sift_colsrc_can_types): the JSON types of the values of a leaf (SIFT_SRC_JT_*), for a leaf that holds the
	 * values of one type and sets aside the others (overflow). filter_types clears the rows of bits whose type is not one of the types
	 * (SIFT_SRC_JT_NONE: jsonb_typeof is NULL, a missing key; SIFT_SRC_JT_NULL: a JSON null) and says whether any row is left. It raises the
	 * value-overflow error when SIFT_SRC_JT_READ is set (the query reads the leaf elsewhere) and a row of one of the types has a value the
	 * leaf cannot hold, unless no snapshot sees the row, and when the type of a row is not known (an index written before the types were kept,
	 * and for a NULL or an astext value before the kinds). tolerate_ovf, called before `begin`, makes the reads of the leaf give a value it
	 * cannot hold as NULL instead of raising that error: the executor has filter_types drop or refuse every row that has one.
	 */
	bool		(*filter_types) (void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
	void		(*tolerate_ovf) (void *impl, int leaf);
	/*
	 * Optional (sift_colsrc_can_raw). set_raw, called before `begin`, makes the reads of an image leaf (SiftSrcLeaf.wide: uuid, bytea) give the codes
	 * of the dictionary the unit stores, whose entries are the stored images, instead of the per-unit dictionary of aligned datums: for a leaf the
	 * executor only compares and copies, never hands to PostgreSQL's functions.
	 */
	void		(*set_raw) (void *impl, int leaf);
	/*
	 * Optional. set_numsum, called before `begin`, says that the executor only adds the values of a numeric leaf (a sum or average that needs no numeric): the
	 * source may give its dictionary as one DNum record for each row (SiftSrcDict.numrec) instead of the datums of its distinct values. A source that does not
	 * keeps the dictionary; the executor tells by the flag of each read.
	 */
	void		(*set_numsum) (void *impl, int leaf);
	/*
	 * Optional, both or neither: the rows of a unit that hold, in a leaf the query reads, a value the
	 * leaf cannot store are read again from the heap as the query's SQL reads them. set_recheck, called before `begin`, says how the query reads
	 * the leaf (a SiftSrcRecheck); the rows of a unit with such a value leave it (`live` does not have them) and the source returns them as the
	 * unit that follows it, from the heap (the snapshot's version of each), their leaf now holding the value SQL gives, or still an
	 * overflow value where SQL raises. key (SRK_CAST of a leaf that is the value column of an object with free keys): only the member of this key is
	 * read as the cast to the type cast reads it, a value of another key is not an error. overflow_raise raises the error SQL raises for the first of `rows` (rows of the unit just returned, which
	 * has such values) when the source recorded one, and returns when it did not.
	 */
	void		(*set_recheck) (void *impl, int leaf, int mode, const char *key, Oid cast);
	void		(*overflow_raise) (void *impl, const SiftSrcUnit *unit, const uint64 *rows);
	/*
	 * The rows `live` took out of the unit the source is at, to be read again from the heap (NULL: none, or the source reads them all). They are
	 * the source's to read, but the executor may clear those that a conjunct on a column the stored unit holds rejects: they are not read.
	 */
	uint64	   *(*recheck_rows) (void *impl, const SiftSrcUnit *unit);
	/*
	 * Also with them: read_types gives the JSON type of the value of the leaf of each row of the unit in tags[0..nrow): the index of the
	 * name jsonb_typeof gives (0 string, 1 number, 2 boolean, 3 object, 4 array, 5 null) or SIFT_SRC_TYPE_NONE (its NULL). It raises when
	 * the index did not store the type of some row (as filter_types does).
	 */
	void		(*read_types) (void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags);
	/*
	 * Optional (sift_colsrc_can_jnull): the JSON nulls among the NULL slots of a leaf, bit k of bits for the k-th value of the leaf as `read`
	 * gives it (an element leaf: its elements); the other NULL slots are missing keys. Raises when the index did not store the kinds of the
	 * slots (as filter_types does).
	 */
	void		(*read_jnull) (void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits);
	/*
	 * Optional (NULL: the Vulkan path does not run on this source). gpu_unit: the unit just returned by `next` is stored, has no dead row and
	 * every page it has rows on is all-visible, so its chunks are the rows a query sees. gpu_dense: the leaf has no NULL and no overflowed
	 * value in the unit (from its zone). gpu_chunk: the leaf's codec blob; false when the leaf has none (a NULL, an inline text value).
	 * gpu_dict: the dictionary of a text leaf (the one its codes are of).
	 */
	bool		(*gpu_unit) (void *impl, const SiftSrcUnit *unit, SiftSrcGpuUnit *g);
	bool		(*gpu_dense) (void *impl, const SiftSrcUnit *unit, int leaf);
	bool		(*gpu_chunk) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcGpuChunk *c);
	void		(*gpu_dict) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcDict *dict);
} SiftSrcProvider;

struct SiftSrcSource
{
	const SiftSrcProvider *prov;
	void	   *impl;
};

extern void sift_colsrc_register(const SiftSrcProvider *prov);

/* ---- planner side ---- */

/* Is the access method of an index (table) one a provider owns? Cheap; used by the planner on every rel */
extern bool sift_colsrc_is_index(Oid indexrelam);
extern bool sift_colsrc_is_table(Oid tablerelam);

/* The layout of an open index (table) relation; palloc'd in CurrentMemoryContext */
extern SiftSrcLayout *sift_colsrc_layout(Relation rel);
extern void sift_colsrc_stats(Relation rel, SiftSrcStats *st);
extern void sift_colsrc_stamp(Relation index, SiftSrcStamp *stamp);

/*
 * Opens the source of an index relation (heap == its table) or of a sift_table table (index == NULL). The snapshot is the
 * scan's. mcxt holds everything the source allocates until sift_colsrc_close.
 */
extern SiftSrcSource *sift_colsrc_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);

/* Can the sources of an index (or, with index false, a table) of this access method be read as the partitions of one table (src/agg/part.h)? sift_colsrc_open_part is sift_colsrc_open for one of them */
extern bool sift_colsrc_can_part(Oid relam, bool index);
extern SiftSrcSource *sift_colsrc_open_part(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
extern void sift_colsrc_close(SiftSrcSource *src);
extern void sift_colsrc_unpin(SiftSrcSource *src);
extern const SiftSrcLayout *sift_colsrc_source_layout(const SiftSrcSource *src);

/* Superblocks of the heap now: the morsels of a parallel scan are ranges of [0, nsb) */
extern uint32 sift_colsrc_nsb(SiftSrcSource *src);

/* Bit i of a row (or element) bitmap */
static inline bool
sift_colsrc_bit(const uint64 *bits, size_t i)
{
	return (bits[i >> 6] >> (i & 63)) & 1;
}

/* the only value a numeric(p <= 18, s) leaf cannot hold is NaN: the executor evaluates those rows as the numeric NaN they are */
static inline bool
sift_colsrc_nan_leaf(const SiftSrcLeaf *l)
{
	return l->typid == NUMERICOID && l->kind == SCL_SCALAR;
}

/*
 * A NaN of a numeric leaf is a value to the executor, the greatest of the leaf: PG_INT64_MAX (no scaled number reaches it), not NULL, and
 * its bit in vec->ovf for the code that evaluates it as PostgreSQL does. The engine stores it as NULL with the flag; a source calls this
 * after it filled data and isnull.
 */
/* The bytes of one value of each vector type, by SiftSrcVecType */
extern const size_t sift_src_vtype_size[];
extern void sift_colsrc_nan_values(const SiftSrcLeaf *l, void *data, uint8 *isnull, const SiftSrcVec *vec);

/* The leaf of leaf def i of a shredded jsonb attribute (a sift_col index, or the declared paths of a sift_table); desc names the attribute and gives its type */
struct SiftColDef;
extern void sift_colsrc_def_leaf(const struct SiftColDef *cols, int i, TupleDesc desc, SiftSrcLeaf *l);
/* The numbers of SiftSrcStats of the leaves of cols from the zones and dictionaries; wovf[i], wjnull[i]: a write buffer holds an overflow value, a JSON null, of leaf i */
struct SiftColLeafStats;
extern void sift_colsrc_leaf_stats(const struct SiftColDef *cols, int ncols, const struct SiftColLeafStats *ls, const bool *wovf, const bool *wjnull, SiftSrcStats *st);

/* Optional text tests of the provider (see SiftSrcProvider.prune_str / filter_str); no-ops that say "not done" without them */
static inline void
sift_colsrc_prune_str(SiftSrcSource *src, const SiftSrcStrTest *t, int nt)
{
	if (src->prov->prune_str)
		src->prov->prune_str(src->impl, t, nt);
}

static inline bool
sift_colsrc_filter_str(SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	return src->prov->filter_str != NULL && src->prov->filter_str(src->impl, unit, t, rows);
}

/* Can the source of this access method test the wide leaves (SiftSrcProvider.set_wide / filter_wide)? */
extern bool sift_colsrc_can_wide(Oid relam, bool index);
extern int sift_colsrc_set_wide(SiftSrcSource *src, const SiftSrcWideTest *t, int nt);
extern bool sift_colsrc_filter_wide(SiftSrcSource *src, const SiftSrcUnit *unit, int i, uint64 *rows);

/* Can the source of this access method be restricted to a match set (SiftSrcProvider.select)? */
extern bool sift_colsrc_can_select(Oid relam, bool index);

/* Can the source of this access method give the TIDs of its rows and the zones of its units (SiftSrcProvider.tids, zone)? */
extern bool sift_colsrc_can_tids(Oid relam, bool index);
extern void sift_colsrc_tids(SiftSrcSource *src, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
extern bool sift_colsrc_sb_heap(SiftSrcSource *src);
extern void sift_colsrc_zone(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z);
extern uint32 sift_colsrc_zone_range(SiftSrcSource *src, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
extern uint32 sift_colsrc_zone_range_segs(SiftSrcSource *src, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
extern bool sift_colsrc_walk_stamp(SiftSrcSource *src, SiftSrcWalkStamp *st, uint32 *mem, int cap, int *nmem);
/* Does the source give the zone of a range of superblocks (zone_range is optional: a source without a top-N has none)? */
extern bool sift_colsrc_has_zone_range(SiftSrcSource *src);
extern bool sift_colsrc_overflow(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf);
extern void sift_colsrc_read_rows(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);
/* The sorted run of a leaf of the unit (SiftSrcProvider.run_info): false when there is none to use, then the rows of the run [pos, pos + cnt) */
extern bool sift_colsrc_run_info(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, bool walk, uint32 *nvals, uint32 *nnull, uint64 *id);
/* The head of the run (SiftSrcProvider.run_heads): its H, 0 when there is none */
extern bool sift_colsrc_sb_heads(SiftSrcSource *src, uint32 sb, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
extern int	sift_colsrc_run_heads(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi);
extern int sift_colsrc_gorder_open(SiftSrcSource *src, int leaf, int rows, int npin, const int *pinleaf, const int64 *pinval, int *grouped);
extern bool sift_colsrc_gorder_info(SiftSrcSource *src, int i, int leaf, uint32 *entries, uint32 *values);
extern void sift_colsrc_gorder_head(SiftSrcSource *src, int i, int leaf, uint32 p, uint32 w, struct SiftCGOrdBuf *buf);
extern void sift_colsrc_gorder_range(SiftSrcSource *src, int i, int leaf, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne);
extern uint32 sift_colsrc_gorder_seek(SiftSrcSource *src, int i, int leaf, uint32 vs, uint32 ve, int64 key, bool after, struct SiftCGOrdBuf *keep);
extern void sift_colsrc_run_read(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows);

/* Restricts the rows of the units the next sift_colsrc_begin starts to the keys of the filter, which must outlive them (NULL: no restriction) */
extern void sift_colsrc_select(SiftSrcSource *src, const SiftFilter *keep);

/* Pre-aggregates of the unit just returned by sift_colsrc_next: false without a provider callback or when the unit cannot answer */
extern void sift_colsrc_preagg(Relation rel, SiftSrcPreagg *pa);

static inline bool
sift_colsrc_unit_exact(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->unit_exact != NULL && src->prov->unit_exact(src->impl, unit);
}

static inline int
sift_colsrc_unit_dead(SiftSrcSource *src, const SiftSrcUnit *unit, int mode)
{
	return src->prov->unit_dead != NULL ? src->prov->unit_dead(src->impl, unit, mode) : -1;
}

static inline bool
sift_colsrc_unit_except(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf)
{
	return src->prov->unit_except != NULL && src->prov->unit_except(src->impl, unit, leaf);
}

static inline bool
sift_colsrc_unit_agg(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	return src->prov->unit_agg != NULL && src->prov->unit_agg(src->impl, unit, leaf, agg);
}

static inline bool
sift_colsrc_unit_hll(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint8 **blob)
{
	return src->prov->unit_hll != NULL && src->prov->unit_hll(src->impl, unit, leaf, blob);
}

static inline bool
sift_colsrc_unit_presence(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p)
{
	return src->prov->unit_presence != NULL && src->prov->unit_presence(src->impl, unit, leaf, p);
}

static inline bool
sift_colsrc_unit_hist(SiftSrcSource *src, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	return src->prov->unit_hist != NULL && src->prov->unit_hist(src->impl, unit, dim, h);
}

static inline bool
sift_colsrc_gpu_unit(SiftSrcSource *src, const SiftSrcUnit *unit, SiftSrcGpuUnit *g)
{
	return src->prov->gpu_unit != NULL && src->prov->gpu_unit(src->impl, unit, g);
}

static inline bool
sift_colsrc_gpu_dense(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf)
{
	return src->prov->gpu_dense != NULL && src->prov->gpu_dense(src->impl, unit, leaf);
}

static inline bool
sift_colsrc_gpu_chunk(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcGpuChunk *c)
{
	return src->prov->gpu_chunk != NULL && src->prov->gpu_chunk(src->impl, unit, leaf, c);
}

static inline void
sift_colsrc_gpu_dict(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcDict *dict)
{
	src->prov->gpu_dict(src->impl, unit, leaf, dict);
}

/* the tuples of the unit's rollup of the list, 0 when the unit has none or the source cannot tell */
static inline uint32
sift_colsrc_unit_rollup_tuples(SiftSrcSource *src, const SiftSrcUnit *unit, int list)
{
	return src->prov->unit_rollup_tuples == NULL ? 0 : src->prov->unit_rollup_tuples(src->impl, unit, list);
}

static inline bool
sift_colsrc_unit_rollup(SiftSrcSource *src, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *r)
{
	return src->prov->unit_rollup != NULL && src->prov->unit_rollup(src->impl, unit, list, need, r);
}

static inline bool
sift_colsrc_unit_rollup_miss(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->unit_rollup_miss != NULL && src->prov->unit_rollup_miss(src->impl, unit);
}

/* Asks the source for superunits (docs/COL-PREAGG.md section 10): true when it has them */
static inline bool
sift_colsrc_super(SiftSrcSource *src, bool on)
{
	if (src->prov->super == NULL)
		return false;
	src->prov->super(src->impl, on);
	return true;
}

static inline bool
sift_colsrc_whole(SiftSrcSource *src, int leaf, const int *meas, int nmeas)
{
	return src->prov->whole != NULL && src->prov->whole(src->impl, leaf, meas, nmeas);
}

static inline void
sift_colsrc_count_range(SiftSrcSource *src, int leaf, SiftSrcStrOp op, const char *s, uint32 len)
{
	if (src->prov->count_range != NULL)
		src->prov->count_range(src->impl, leaf, op, s, len);
}

static inline void
sift_colsrc_count_cmp(SiftSrcSource *src, int leaf)
{
	if (src->prov->count_cmp != NULL)
		src->prov->count_cmp(src->impl, leaf);
}

static inline uint64
sift_colsrc_point_exact(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->point_exact != NULL ? src->prov->point_exact(src->impl, unit) : 0;
}

static inline bool
sift_colsrc_unit_count(SiftSrcSource *src, const SiftSrcUnit *unit, int64 *rows)
{
	return src->prov->unit_count != NULL && src->prov->unit_count(src->impl, unit, rows);
}

static inline uint32
sift_colsrc_super_span(SiftSrcSource *src)
{
	return src->prov->super_span ? src->prov->super_span(src->impl) : 0;
}

static inline void
sift_colsrc_split(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	src->prov->split(src->impl, unit);
}

/* Units of the superblocks [sb_lo, sb_hi), ascending sb, in the source's order within an sb */
extern void sift_colsrc_begin(SiftSrcSource *src, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune);
extern bool sift_colsrc_kept(SiftSrcSource *src, const SiftSrcPrune *prune, int nprune, uint32 nspace, uint64 *kept, uint64 *rows);
extern bool sift_colsrc_next(SiftSrcSource *src, SiftSrcUnit *unit);

/*
 * Leaf `leaf` of the unit decoded to its vtype. Valid until the next sift_colsrc_read of the same leaf or the next
 * sift_colsrc_next. A group leaf yields the SCV_I32 element counts per row (isnull: array absent); a child yields all the
 * elements of the unit.
 */
extern void sift_colsrc_read(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);

/*
 * The same into storage of the caller (sift_colsrc_leaf_n values and SIFT_AGG_PAD more, see SiftSrcBuf): vec->data and vec->isnull point into
 * buf, valid as long as the caller keeps it. For a caller that keeps its vectors across units or wants them next to each other, and the one
 * way to read without a copy by the source: sift_colsrc_read is this over the source's own buffers.
 */
extern int64 sift_colsrc_leaf_n(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf);
extern void sift_colsrc_read_into(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec);

/*
 * Row bitmaps of the unit (nrow bits in ceil(nrow / 64) words, zero past nrow). live: the rows that exist (an index entry of a
 * deleted row is not). vet: clears the rows of bits that the snapshot does not see; call it after the value filters, it reads the heap
 * for the rows of pages that are not all-visible (a sift_table table has nothing to check).
 */
extern void sift_colsrc_live(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits);
extern void sift_colsrc_vet(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits);
extern bool sift_colsrc_vet_needed(SiftSrcSource *src, const SiftSrcUnit *unit);

extern void sift_colsrc_counters(const SiftSrcSource *src, SiftSrcCounters *c);

/*
 * Offers the conjuncts (at most SIFT_COLSRC_MAX_FILTERS, the constants outlive the scan) to the source before sift_colsrc_begin; returns the
 * bits of those it applies (0: none, or the source has no filters). sift_colsrc_filter then clears the rows of `bits` that fail any of them
 * (all of them: the order is the source's) and says whether any row is left. A raise of the value-overflow error is the same as a read of the leaf.
 */
extern uint64 sift_colsrc_set_filters(SiftSrcSource *src, const SiftSrcFilter *f, int nf);
extern bool sift_colsrc_filter(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits);

/* The JSON types of a value (jsonb_typeof), as the bits of the types a filter keeps; the bit t - 1 is the chunk tag t (SIFT_COLTAG_*) */
#define SIFT_SRC_JT_STRING		0x01
#define SIFT_SRC_JT_NUMBER		0x02
#define SIFT_SRC_JT_BOOLEAN		0x04
#define SIFT_SRC_JT_OBJECT		0x08
#define SIFT_SRC_JT_ARRAY		0x10
#define SIFT_SRC_JT_NONE		0x20	/* the path has no value: jsonb_typeof is NULL */
#define SIFT_SRC_JT_NULL		0x40	/* a JSON null: jsonb_typeof is 'null' */
#define SIFT_SRC_JT_ALL			0x7F
#define SIFT_SRC_TYPE_NONE		0xFF	/* SiftSrcProvider.read_types: the path has no value */
#define SIFT_SRC_JT_READ		0x80	/* not a type: the query reads the leaf's values besides, so a row of the types whose value the leaf cannot hold is an error */

/* How the query reads a leaf whose value the leaf cannot store (SiftSrcProvider.set_recheck) */
typedef enum SiftSrcRecheck
{
	SRK_NONE,					/* not at all: such a value is an error for a row of the snapshot */
	SRK_TEXT,					/* the text of the scalar (a record column of jsonb_to_recordset, `->>` and a cast of it) through the input function of the leaf's type; a key of an array is NULL */
	SRK_RECORDSET,				/* an array leaf unnested by jsonb_to_recordset: the whole list of a row raises when one of its values does, and a list that is not an array of objects raises */
	SRK_CAST,					/* a member of jsonb_each read only through the cast of its jsonb value to the leaf's number type: a value of another JSON type raises where the row reaches the aggregate */
	SRK_RECORDSET_LAZY,			/* a spec's SRK_RECORDSET whose error the EXISTS test of the list raises when it reaches the row, not the rows' conjuncts (the source is given SRK_RECORDSET) */
	SRK_CAST_TESTED,			/* a spec's SRK_CAST that a conjunct of the members casts too: the cast of the conjunct raises first, for every row that reaches the members, so the error is raised before they are tested (the source is given SRK_CAST) */
	SRK_IOCAST					/* a path of the document read only as `(doc ->> 'k')::T`, T the leaf's type: the text of the scalar through the input function of T, which raises where the row reaches the aggregate (the source is given SRK_TEXT) */
} SiftSrcRecheck;

/* Can the source of this access method recheck its overflow rows (SiftSrcProvider.set_recheck)? */
extern bool sift_colsrc_can_recheck(Oid relam, bool index);
extern void sift_colsrc_set_recheck(SiftSrcSource *src, int leaf, SiftSrcRecheck mode, const char *key, Oid cast);
extern void sift_colsrc_overflow_raise(SiftSrcSource *src, const SiftSrcUnit *unit, const uint64 *rows);
extern uint64 *sift_colsrc_recheck_rows(SiftSrcSource *src, const SiftSrcUnit *unit);

/* Can the source of this access method filter by the JSON type of a leaf (SiftSrcProvider.filter_types)? */
extern bool sift_colsrc_can_types(Oid relam, bool index);
extern bool sift_colsrc_filter_types(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
extern void sift_colsrc_read_types(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint8 *tags);
extern void sift_colsrc_tolerate_ovf(SiftSrcSource *src, int leaf);

/* Can the source of this access method read an image leaf as the codes of its stored dictionary (SiftSrcProvider.set_raw)? */
extern bool sift_colsrc_can_raw(Oid relam, bool index);
extern void sift_colsrc_set_raw(SiftSrcSource *src, int leaf);

/* Tell the source (if it can) that a numeric leaf is only summed (SiftSrcProvider.set_numsum) */
extern void sift_colsrc_set_numsum(SiftSrcSource *src, int leaf);

/* Does the source of this access method tell a JSON null from a missing key per value of a leaf (SiftSrcProvider.read_jnull)? */
extern bool sift_colsrc_can_jnull(Oid relam, bool index);
extern void sift_colsrc_read_jnull(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint64 *bits);

/* Leaf `leaf` decoded for the rows of bits only (the other rows read as zero, not NULL); the whole leaf when the source cannot */
extern bool sift_colsrc_can_read_sel(const SiftSrcSource *src);
extern bool sift_colsrc_can_narrow(const SiftSrcSource *src);
extern bool sift_colsrc_narrow(SiftSrcSource *src, SiftSrcUnit *unit, const uint64 *bits);
extern void sift_colsrc_read_sel(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);

#endif							/* SIFT_COLSRC_H */
