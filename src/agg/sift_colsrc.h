/*
 * sift_colsrc.h - the column source SiftAgg reads (planner metadata and executor read API).
 *
 * This is the narrow interface of the aggregate track (col-plan) to the column storage. The storage is either
 *   - a `sift_col` index over a heap table: the values are copies taken at aminsert/ambuild, a row may be used only
 *     if its heap page is all-visible (visibility map) or the heap says its tuple is visible (docs/COL.md section 3);
 *     the executor asks for that vetting (sift_colsrc_vet) after the value filters, or
 *   - a `sift_table` TABLE (table access method): the table is the columns, its scan is MVCC by itself (sift_colsrc_vet has
 *     nothing to check).
 * Everything above this header is the same for both. The implementations are providers (SiftSrcProvider) registered by their
 * module: the sift_col index (src/agg/sift_aggcol.c, over the column engine of docs/COL.md), the sift_table table, and the test
 * stub (src/agg/sift_aggstub*.c, the `sift_colstub` index AM, SIFT_TESTING builds only), which reads the heap pages raw and builds
 * the same units. The functions below dispatch on the access method of the relation. Keep this header small: it is the seam.
 *
 * Model (docs/COL.md section 1): a source has leaves (ids 0..nleaves-1). A scalar leaf is one value per row; a GROUP
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

#include "sift_host.h"

#include "sift.h"
#include "sift_filter.h"

#define SIFT_COLSRC_MAX_LEAVES		128

/* The element type of the vector a leaf is decoded to (docs/KERNELS.md data model of sift_aggkern.h) */
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
	Oid			wtypid;			/* a wide leaf (docs/COL-LEAFTYPES-32.md): its SQL type, which only a conjunct of the query tests (SAP_WIDE); else InvalidOid */
	bool		wide;			/* numeric of more than 18 digits or any other shape, interval (docs/COL-LEAFTYPES-32.md 8): an SCV_CODE leaf whose dictionary entries are
								 * not text (src/agg/sift_aggwide.h): only an expression PostgreSQL evaluates reads it, no kernel, native aggregate, histogram or text test */
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
	int			nleaves;
	SiftSrcLeaf *leaf;
} SiftSrcLayout;

/* Planner side: statistics the cost model uses (all estimates, never errors) */
typedef struct SiftSrcStats
{
	double		nrows;			/* rows in the source (dead included) */
	double		nunits;			/* units */
	double		bytes;			/* stored (compressed) bytes of the leaves together, 0 when unknown */
	double		allvisible_frac;	/* share of the heap pages that are all-visible (1 for a table) */
	double		exact_frac;		/* share of the units whose pre-aggregates may answer: those on all-visible pages of a heap, the plain superblocks of a table */
	double		ndistinct[SIFT_COLSRC_MAX_LEAVES];	/* distinct values of a scalar leaf (0: unknown) */
	double		min[SIFT_COLSRC_MAX_LEAVES];		/* numeric leaves: smallest and largest value as double (0, 0 unknown) */
	double		max[SIFT_COLSRC_MAX_LEAVES];
	double		elems_per_row[SIFT_COLSRC_MAX_LEAVES];	/* group leaves: average elements per row */
	bool		ovf[SIFT_COLSRC_MAX_LEAVES];	/* some unit holds values the leaf cannot store (overflow): reading it raises an error, a numeric leaf's NaNs apart */
} SiftSrcStats;

/* ---- executor side ---- */

typedef struct SiftSrcSource SiftSrcSource;

/* A dictionary of a text leaf: value c is bytes[off[c] .. off[c + 1]). id is the same pointer for every unit that shares it and is never reused for another dictionary while the source is open */
typedef struct SiftSrcDict
{
	const void *id;
	uint32		n;
	const uint32 *off;			/* n + 1 entries */
	const char *bytes;
} SiftSrcDict;

/* One decoded leaf of a unit: n values (rows; elements for a child leaf), see the padding rules of sift_aggkern.h */
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
	void	   *priv;			/* the source's */
} SiftSrcUnit;

/*
 * A zone test the source may use to skip a unit without reading it: the unit is skipped when no value of the
 * integer-typed (I32/I64) leaf can lie in [lo, hi]. Conservative; the executor filters again. Only leaves of rows (not
 * children of a group).
 */
typedef struct SiftSrcPrune
{
	int			leaf;
	int64		lo;
	int64		hi;
} SiftSrcPrune;

/*
 * A test on a text leaf (SCV_CODE, top level scalar) that a source may run on the encoded form of its units instead of the executor
 * evaluating the predicate over the leaf's dictionary and codes (docs/COL.md 2.9, "Reading text from SiftAgg"). Bytewise, so the planner
 * pushes EQ, NE, IN and NOT IN for any deterministic collation and the range operators and PREFIX only for the C collation. vals are
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
	SSTR_PREFIX					/* values that start with vals[0] */
} SiftSrcStrOp;

typedef struct SiftSrcStrTest
{
	int			leaf;
	SiftSrcStrOp op;
	int			nvals;
	const char *const *vals;
	const uint32 *lens;
} SiftSrcStrTest;

/*
 * A test of a conjunct on a wide leaf: SWT_* ops are the comparison operators of SiftCmpOp (0..5) and the four below. vals are the operands in
 * the type of the leaf (a NULL operand is isnull[i]; an IN or NOT IN list has nvals of them, a comparison one, a NULL test none).
 */
#define SWT_IN			6
#define SWT_NOTIN		7
#define SWT_ISNULL		8
#define SWT_NOTNULL		9

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
} SiftSrcUnitAgg;

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

/*
 * Planner: the histograms and rollups the index writes (a unit may still lack one, or hold fewer dims of a rollup); ndims 0 when none.
 * aggs: units have unit_agg sums (always, even with ndims 0)
 */
typedef struct SiftSrcPreagg
{
	bool		aggs;
	int			ndims;
	struct
	{
		int			leaf;
		int			nmeas;
		int			meas[SIFT_SRC_MAX_MEAS];
	}			dim[SIFT_SRC_MAX_DIMS];
	int			nrollups;
	struct
	{
		int			list;			/* the SCL_ARRAY / SCL_OBJECT leaf */
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
	SSF_NOTNULL					/* a value is present */
} SiftSrcFilterOp;

typedef struct SiftSrcFilter
{
	int			leaf;
	SiftSrcFilterOp op;
	int64		val;			/* a comparison */
	int			nset;			/* IN, NOTIN: the members */
	const int64 *set;
} SiftSrcFilter;

#define SIFT_COLSRC_MAX_FILTERS		64

/*
 * The Vulkan path (docs/GPU.md, optional provider callbacks gpu_*): the identity of a unit whose stored chunks stand for the rows a query
 * sees, and a leaf's codec blob (docs/COL-CODEC.md). The chunk cache of the device is keyed by (store, gen, unit, leaf): a segment is
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
	int64		units_pruned;	/* skipped by the zone tests */
	int64		rows;			/* rows of the units returned */
	int64		bytes;			/* bytes decoded */
	int64		rows_vetted;	/* rows checked against the heap */
	int64		heap_pages;		/* heap pages read for them */
} SiftSrcCounters;

/*
 * A provider: the column source of one kind of index or table, registered at load time by the module that implements it.
 * `impl` is the provider's source object. The functions of the seam below dispatch to the provider that owns the relation's
 * access method.
 */
typedef struct SiftSrcProvider
{
	bool		(*owns_index) (Oid indexrelam);
	bool		(*owns_table) (Oid tablerelam);
	SiftSrcLayout *(*layout) (Relation rel);
	void		(*stats) (Relation rel, SiftSrcStats *st);
	void	   *(*open) (Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
	/* Optional (NULL: the provider has no partitioned tables, sift_colsrc_can_part): open for a partition read as part of its partitioned table, whose privileges and row security were checked */
	void	   *(*open_part) (Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
	void		(*close) (void *impl);
	const SiftSrcLayout *(*source_layout) (const void *impl);
	uint32		(*nsb) (void *impl);
	void		(*begin) (void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune);
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
	 * Optional, both or neither (sift_colsrc_can_wide): the tests of a conjunct on a wide leaf (SiftSrcLeaf.wtypid: numeric of more than 18
	 * digits, money, interval; docs/COL-LEAFTYPES-32.md). set_wide, called before `begin`, takes the tests of the scan (operands as Datums, in
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
	 * rows only, which the source may renumber 0..nrow-1 in key order (docs/COL-SEARCH.md, src/agg/sift_aggsel.h).
	 */
	void		(*select) (void *impl, const SiftFilter *keep);
	/* Optional (NULL: no unit has pre-aggregates), see SiftSrcUnitAgg; `preagg` is the planner's, over the index relation */
	void		(*preagg) (Relation rel, SiftSrcPreagg *pa);
	bool		(*unit_exact) (void *impl, const SiftSrcUnit *unit);
	bool		(*unit_agg) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg);
	bool		(*unit_hist) (void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h);
	bool		(*unit_rollup) (void *impl, const SiftSrcUnit *unit, int list, SiftSrcRollup *r);
	/* Optional: the consumer scanned an exact unit because its rollup lacks what the query reads; the source may arrange for the unit to get it. True: a refresh is queued for it */
	bool		(*unit_rollup_miss) (void *impl, const SiftSrcUnit *unit);
	/*
	 * Optional (NULL: no ordered top-N scan on this source, sift_colsrc_can_tids says so to the planner). tids: the heap TIDs of the rows
	 * `rows` (ascending, in the numbering `read` and `live` use) of the unit the source is at. zone: the zone of an integer-typed leaf
	 * of the unit the source is at, over the unit as stored.
	 */
	void		(*tids) (void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
	void		(*zone) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z);
	/* zone_range: the zone of a leaf over all the units of the superblocks [lo, hi) together, without opening them; returns how many units it counts (required with tids) */
	uint32		(*zone_range) (void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
	/* optional (NULL: none does): can the unit the source is at hold, in `leaf`, a value the source cannot store (SiftSrcVec.ovf)? */
	bool		(*overflow) (void *impl, const SiftSrcUnit *unit, int leaf);
	/* read for the rows `rows` (ascending) only, valid until the next read of the leaf: vec->n == n, element k is row rows[k] */
	void		(*read_rows) (void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);
	/* optional: the int filters above. set_filters (before begin) returns the bits of those it will apply, filter narrows the rows of a unit by them */
	uint64		(*set_filters) (void *impl, const SiftSrcFilter *f, int nf);
	bool		(*filter) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	/* optional: read only the rows of bits (the other rows of the vector are zero; a source restricted to a match set may read the whole leaf) */
	void		(*read_sel) (void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);
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

/*
 * Opens the source of an index relation (heap == its table) or of a sift_table table (index == NULL). The snapshot is the
 * scan's. mcxt holds everything the source allocates until sift_colsrc_close.
 */
extern SiftSrcSource *sift_colsrc_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);

/* Can the sources of an index of this access method be read as the partitions of one table (src/agg/sift_aggpart.h)? sift_colsrc_open_part is sift_colsrc_open for one of them */
extern bool sift_colsrc_can_part(Oid indexrelam);
extern SiftSrcSource *sift_colsrc_open_part(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt);
extern void sift_colsrc_close(SiftSrcSource *src);
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
extern void sift_colsrc_nan_values(const SiftSrcLeaf *l, void *data, uint8 *isnull, const SiftSrcVec *vec);

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
extern void sift_colsrc_zone(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z);
extern uint32 sift_colsrc_zone_range(SiftSrcSource *src, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z);
extern bool sift_colsrc_overflow(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf);
extern void sift_colsrc_read_rows(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);

/* Restricts the rows of the units the next sift_colsrc_begin starts to the keys of the filter, which must outlive them (NULL: no restriction) */
extern void sift_colsrc_select(SiftSrcSource *src, const SiftFilter *keep);

/* Pre-aggregates of the unit just returned by sift_colsrc_next: false without a provider callback or when the unit cannot answer */
extern void sift_colsrc_preagg(Relation rel, SiftSrcPreagg *pa);

static inline bool
sift_colsrc_unit_exact(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->unit_exact != NULL && src->prov->unit_exact(src->impl, unit);
}

static inline bool
sift_colsrc_unit_agg(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	return src->prov->unit_agg != NULL && src->prov->unit_agg(src->impl, unit, leaf, agg);
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

static inline bool
sift_colsrc_unit_rollup(SiftSrcSource *src, const SiftSrcUnit *unit, int list, SiftSrcRollup *r)
{
	return src->prov->unit_rollup != NULL && src->prov->unit_rollup(src->impl, unit, list, r);
}

static inline bool
sift_colsrc_unit_rollup_miss(SiftSrcSource *src, const SiftSrcUnit *unit)
{
	return src->prov->unit_rollup_miss != NULL && src->prov->unit_rollup_miss(src->impl, unit);
}

/* Units of the superblocks [sb_lo, sb_hi), ascending sb, in the source's order within an sb */
extern void sift_colsrc_begin(SiftSrcSource *src, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune);
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

/* Can the source of this access method filter by the JSON type of a leaf (SiftSrcProvider.filter_types)? */
extern bool sift_colsrc_can_types(Oid relam, bool index);
extern bool sift_colsrc_filter_types(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits);
extern void sift_colsrc_read_types(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint8 *tags);
extern void sift_colsrc_tolerate_ovf(SiftSrcSource *src, int leaf);

/* Does the source of this access method tell a JSON null from a missing key per value of a leaf (SiftSrcProvider.read_jnull)? */
extern bool sift_colsrc_can_jnull(Oid relam, bool index);
extern void sift_colsrc_read_jnull(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, uint64 *bits);

/* Leaf `leaf` decoded for the rows of bits only (the other rows read as zero, not NULL); the whole leaf when the source cannot */
extern bool sift_colsrc_can_read_sel(const SiftSrcSource *src);
extern void sift_colsrc_read_sel(SiftSrcSource *src, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);

#endif							/* SIFT_COLSRC_H */
