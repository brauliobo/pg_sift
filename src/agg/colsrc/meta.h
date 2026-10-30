/*
 * agg/colsrc/meta.h - the column source: the layout and the statistics of its leaves
 * The leaf, layout, zone, statistics and stamp records the planner reads, with no executor
 * call; the first of the four parts of agg/colsrc.h.
 */
#ifndef SIFT_AGG_COLSRC_META_H
#define SIFT_AGG_COLSRC_META_H

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
	uint32		gord_coll[SIFT_COLSRC_MAX_LEAVES];	/* the collation of the text order arrays of a leaf (UINT32_MAX: they differ), 0: the leaf's are not text */
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

#endif							/* SIFT_AGG_COLSRC_META_H */
