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
 * module: the sift_col index (src/sift_aggcol.c, over the column engine of docs/COL.md), the sift_table table, and the test
 * stub (src/sift_aggstub*.c, the `sift_colstub` index AM, SIFT_TESTING builds only), which reads the heap pages raw and builds
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
	Oid			typid;			/* SQL type of the values: what the query's expression has to produce for a match */
	int			scale;			/* numeric leaf (typid NUMERICOID, SCV_I64): digits after the decimal point, the value is the number times 10^scale */
	bool		lossy;			/* the shredder may have left values out of the leaf (overflow): no predicate may use it */
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
	double		allvisible_frac;	/* share of the heap pages that are all-visible (1 for a table) */
	double		ndistinct[SIFT_COLSRC_MAX_LEAVES];	/* distinct values of a scalar leaf (0: unknown) */
	double		min[SIFT_COLSRC_MAX_LEAVES];		/* numeric leaves: smallest and largest value as double (0, 0 unknown) */
	double		max[SIFT_COLSRC_MAX_LEAVES];
	double		elems_per_row[SIFT_COLSRC_MAX_LEAVES];	/* group leaves: average elements per row */
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
	 * numeric leaves (the only values a numeric(p <= 18, s) column cannot hold are its NaNs): NULL when none of the values is one,
	 * else bit i set = value i is a NaN of the heap (isnull is set too, the data slot is undefined). Zeroed by sift_colsrc_read,
	 * set by the provider. The executor refuses a vector that has it unless the reader evaluates the value with PostgreSQL.
	 */
	const uint64 *ovf;
} SiftSrcVec;

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
	void		(*close) (void *impl);
	const SiftSrcLayout *(*source_layout) (const void *impl);
	uint32		(*nsb) (void *impl);
	void		(*begin) (void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune);
	bool		(*next) (void *impl, SiftSrcUnit *unit);
	void		(*read) (void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);
	void		(*live) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
	void		(*vet) (void *impl, const SiftSrcUnit *unit, uint64 *bits);
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
 * Row bitmaps of the unit (nrow bits in ceil(nrow / 64) words, zero past nrow). live: the rows that exist (an index entry of a
 * deleted row is not). vet: clears the rows of bits that the snapshot does not see; call it after the value filters, it reads the heap
 * for the rows of pages that are not all-visible (a sift_table table has nothing to check).
 */
extern void sift_colsrc_live(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits);
extern void sift_colsrc_vet(SiftSrcSource *src, const SiftSrcUnit *unit, uint64 *bits);

extern void sift_colsrc_counters(const SiftSrcSource *src, SiftSrcCounters *c);

#endif							/* SIFT_COLSRC_H */
