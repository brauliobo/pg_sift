/*
 * col/preagg.h - pre-aggregates of a unit (docs/COL-PREAGG.md): what the segment writer computes once, when a unit is built, merged
 * or rewritten, and what a reader may answer from it without reading the unit's chunks. Portable (core/host.h only).
 *
 *   - per INT leaf: the exact sum and the sum of squares as 128-bit integers (the zone's int64 sum overflows, this does not);
 *   - per unit: a mini cube, GROUP BY dim -> count and, for the configured measures, (non-NULL count, sum and, from column format minor 7 when
 *     the index asks for it, the minimum and maximum of an integer measure). A dim is a scalar leaf of
 *     a few distinct values in the unit: a dictionary-coded text leaf (the group key is the segment code) or an integer, date or
 *     numeric leaf (the key is the stored integer). So "count per country", "revenue per category", "orders per month" (a leaf that
 *     holds date_trunc('month', ts)) are one add per group and unit instead of one per row;
 *   - per unit, from column format minor 4: for the leaves of a list (an array of objects, an array of scalars, a map; its rows are the
 *     elements) one rollup: the distinct tuples of the values of a few low-cardinality leaves (the dims) that the unit holds, each with its element
 *     count and, per measure, (non-NULL count, sum, min, max). A GROUP BY over any subset of the dims, a filter on the dims and these aggregates
 *     are answered by adding up the tuples (docs/COL-PREAGG.md section 2). Depth 1: a list inside a list has none.
 *
 * Immutable like the unit it describes: never updated in place; whether a unit's pre-aggregate may answer a query is the visibility rule of
 * sift_col_unit_exact (clean unit, every page all-visible) and the predicate rule of the consumer (docs/COL-PREAGG.md section 4).
 */
#ifndef SIFT_COL_PREAGG_H
#define SIFT_COL_PREAGG_H

#include "core/host.h"

#include "col/col.h"
#include "col/enc.h"

typedef __int128 SiftCI128 __attribute__((aligned(8)));

#define SIFT_PAGG_MAX_DIMS		16			/* dimensions of the top level's cube */
#define SIFT_PAGG_SPEC_DIMS		32			/* planner side: dimensions of the cube and of all rollups together (SIFT_SRC_MAX_DIMS) */
#define SIFT_PAGG_MAX_RDIMS		6			/* dimensions of one rollup */
#define SIFT_PAGG_MAX_ROLLUPS	8			/* rollups of an option: one per list */
#define SIFT_PAGG_ROLLUP_TUPLES	4096		/* the most tuples of a rollup in a unit (explicit rollup(...)); the default takes at most a 32nd of the unit's elements, at least 64 */
#define SIFT_PAGG_ROLLUP_MIN	64
#define SIFT_PAGG_ROWS_CELLS	4096		/* the most cells (tuples) of the rollup of a unit's rows ... */
#define SIFT_PAGG_ROWS_RATIO	10			/* ... and at most a 10th of the unit's rows (at least SIFT_PAGG_ROLLUP_MIN): a tuple costs about 15 rows to read, so past a 16th the unit scans anyway; a unit with more has none and scans */
#define SIFT_PAGG_MAX_MEAS		8
#define SIFT_KTAB_MEAS			2			/* measures a key table keeps besides the rows (col/ktab.h) */
#define SIFT_PAGG_CAL			128			/* a calendar dim (the cells of a date or timestamp leaf) has the directory leaf grain * this + the leaf (SIFT_COL_MAX_COLS leaves precede it); asked of a reader, this + the leaf is the dim at the grain the unit has */
#define SIFT_CAL_DAY			1			/* the grains of a cell, finest first: a day, a month, a year; the key of a cell is its first day, as days since 2000-01-01 (a date) */
#define SIFT_CAL_MONTH			2
#define SIFT_CAL_YEAR			3
#define SIFT_PAGG_DEF_MEAS		8			/* measures of a default dim */
#define SIFT_PAGG_DEF_GROUPS	256			/* default: a dim with more groups in a unit has no histogram in that unit */
#define SIFT_PAGG_CAL_GROUPS	1024		/* the cells of a calendar dim in a unit: 2.8 years of days, 85 of months */
#define SIFT_PAGG_CAL_ROWS		8			/* a cell stands for this many rows or more on average, else the unit goes on to the next grain: answering from cells has to read far fewer than the rows */
#define SIFT_PAGG_CUBE_GROUPS	4096		/* explicit cube(...): the same bound */
#define SIFT_PAGG_DEF_PERMILLE	400			/* default: the dims of a unit's cube take at most this share of its encoded bytes, counted as built (on disk the index holds about a quarter of that for dims of few groups, more for many) */
#define SIFT_PAGG_ROLLUP_PERMILLE	20			/* the rollups of one list of elements take at most this share of the unit's bytes */
#define SIFT_PAGG_EXC_DIV		100			/* a leaf with at most 1/100 of a unit's rows not NULL has exceptions (docs/COL-PREAGG.md 11) */
#define SIFT_PAGG_EXC_SHARE		2			/* ... kept when they take at most 1/2 of the bytes of the unit's own cube and element blob */
#define SIFT_PAGG_MIN_BUDGET	2048		/* a unit's rollups of a list always may take this many bytes */
#define SIFT_PAGG_CUBE_MIN_BUDGET	8192		/* ... and its cube's dims (and calendar cells) this many, or the tail of a table would keep none under the share */
#define SIFT_PAGG_CAL_PERMILLE	100		/* the calendar dims of a unit together may take this much of its bytes, counted as built (at least the cube's minimum budget) */

/* flags of SiftCLeafAgg / SiftCAgg */
#define SIFT_CAGG_SUMSQ_OVF		0x1			/* the sum of squares left int128 (never: the sum cannot) */
#define SIFT_CAGG_LOOSE			0x4			/* a unit net of its dead rows (col/delta.c): min and max bound its live rows, they are not their minimum and maximum */

typedef struct SiftCLeafAgg
{
	SiftCI128	sum;					/* exact sum of the non-NULL values (INT leaves) */
	SiftCI128	sumsq;					/* exact sum of their squares unless SIFT_CAGG_SUMSQ_OVF */
	uint32		flags;
} SiftCLeafAgg;

/* Everything a unit knows about a leaf without reading it: the zone and the exact sums. INT leaves only: sum and sumsq valid then */
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
	bool		cal;					/* leaf is a date or timestamp leaf and the dim groups it by calendar cells: each unit stores the finest grain (SIFT_CAL_*) that fits, the key of a cell being its first day */
	uint8		grain;					/* cal: 0, the writer chooses the grain; else exactly this grain (a stored dim, which the verify recomputes) */
	bool		no_minmax;				/* the spec asks for min and max, this dim holds none: a table's calendar cells, which the byte budget of the calendar dims fits only without them */
	int16		meas[SIFT_PAGG_MAX_MEAS];
} SiftCPaggDim;

/* The rollup of one list: the dims it may hold (a unit keeps the ones that fit) and its measures */
typedef struct SiftCPaggRollup
{
	int16		list;					/* the GROUP leaf; -1: the rows of the unit (column format minor 8), dims and measures are top-level leaves */
	uint8		ndims;
	int16		dim[SIFT_PAGG_MAX_RDIMS];
	uint8		nmeas;
	int16		meas[SIFT_PAGG_MAX_MEAS];
} SiftCPaggRollup;

/* The reloption `preagg`, resolved against a schema */
typedef struct SiftCPaggSpec
{
	bool		off;					/* no cube (the exact leaf sums are still written: they cost 40 bytes per integer leaf and unit) */
	bool		fixed;					/* cube(...): exactly these dims and measures, no byte budget */
	bool		rfixed;					/* rollup(...): exactly these dims (all or none per unit), no byte budget */
	bool		cal_all;				/* every date and timestamp leaf was asked for (calendar_columns = 'all', or its default): the ones past the dimension cap have no calendar dim (sift_cpagg_cal_missing) */
	bool		table;					/* the default of a sift_table (table/seg.c): no exception cubes, which its source does not read, and runs only where they fit the share of the unit that key lists take */
	bool		key_table;				/* the segments keep a key table of each leaf of set (option key_table, column format minor 22; col/ktab.h) */
	bool		minmax;					/* every group of the cube also holds the minimum and maximum of each integer measure (column format minor 7) */
	bool		set[SIFT_COL_MAX_COLS];	/* the leaves that keep their keys in every unit (option key_columns, column format minors 10 and 15; col/setsum.h) */
	uint8		ktn[SIFT_COL_MAX_COLS];		/* the key leaves that get a key table and their measures: how many, which (sift_ktab_spec, for the planner; the writer chooses the same) */
	int16		ktm[SIFT_COL_MAX_COLS][SIFT_KTAB_MEAS];
	bool		keyauto;				/* key_columns is the choice of the build ('auto: a, b'): a leaf keeps only a key list or a bitmap, within the share of the unit, never a filter */
	uint16		super;					/* units per superunit summary (column format minor 12); 0: none */
	bool		lookup[SIFT_COL_MAX_COLS];	/* the leaves whose dictionary of many strings is written to be searched in place (option dict_lookup, column format minor 14); the others have the compact form */
	uint8		lookup_pct;				/* a sift_table: the orders of the dictionaries together take at most this percent of the segment's chunks (the leaves of lookup); 0: no cap */
	bool		pres[SIFT_COL_MAX_COLS];	/* the leaves of set that can keep every distinct key of a unit as a bitmap or a list (column format minor 15; col/setsum.h) */
	bool		run[SIFT_COL_MAX_COLS];	/* the leaves that get a sorted run in every unit (option order_columns, column format minor 13; col/runs.h) */
	bool		hll[SIFT_COL_MAX_COLS];	/* the leaves that keep a HyperLogLog sketch of their values in every unit (option hll_columns, column format minor 20; col/hll.h) */
	bool		gorder[SIFT_COL_MAX_COLS];	/* the leaves that get a global order block in the segment (column format minor 21; col/gorder.h) */
	bool		gorderauto;				/* order_array_columns is the choice of the build ('auto: a, b'): the leaves of it get their sorted run too */
	int			gorder_group;	/* ... sorted by the leaf (this - 1) first (option order_array_group): the rows with no value of it are not in the block; 0: none */
	bool		gorder_group_auto;	/* order_array_group is the choice of the build ('auto: g'): only the blocks of dates and timestamps are grouped, a range of another leaf keeps its plain order */
	bool		count[SIFT_COL_MAX_COLS];	/* the text leaves whose dictionary of many strings also stores the rows of each string (option count_columns, column format minor 24; col/seg_write.c) */
	bool		point[SIFT_COL_MAX_COLS];	/* the leaves that get a point directory in the segment (option point_columns, column format minor 14; col/point.h) */
	bool		point_tid[SIFT_COL_MAX_COLS];	/* ... and, of these or others, those whose directory lists heap addresses (option point_tid_columns, column format minor 16) */
	int			ndims;
	SiftCPaggDim dim[SIFT_PAGG_MAX_DIMS];	/* top level only */
	int			nrollups;
	SiftCPaggRollup rollup[SIFT_PAGG_MAX_ROLLUPS];
	uint32		max_groups;
	uint32		permille;				/* default: of the unit's bytes; 0 = no budget */
} SiftCPaggSpec;

/*
 * Leaves that can be a dim (a scalar INT or STR leaf, not a float, of the top level or of a list of the top level) and a measure of it (a scalar INT
 * or float leaf, not a date/time/bool, of the same list or the top level)
 */
extern bool sift_cpagg_dim_ok(const SiftColSchema *s, const SiftColDef *d);
/* The leaf names of an option that lists columns ('a, b', empty or off for none): sets leaf[c] for each, checked by ok (false: the message bad); s NULL checks the syntax only */
/* The list of an option that the build may choose ('auto: a, b'): the text after the word, and whether the word is there */
extern const char *sift_cpagg_auto_list(const char *opt, bool *autolist);
extern const char *sift_cpagg_leaf_list(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf,
										bool (*ok) (const SiftColSchema *, const SiftColDef *), const char *bad);
extern bool sift_cpagg_meas_ok(const SiftColSchema *s, const SiftColDef *m, const SiftColDef *dim);

/*
 * Adds to a default spec the calendar dim of every date, timestamp and timestamptz leaf of the top level, with the measures of a default dim: a unit then
 * stores, for the leaf, the groups of its days, else of its months, else of its years (the finest that fits the cell limit and the byte budget), and every
 * key that is a function of the cell (date_trunc, extract, ::date) is a histogram of the unit (docs/COL-PREAGG.md 2). A timestamptz is stored as UTC days.
 * A sift_table has no index option to ask for it, so its writer adds it to the default; column format minor 8.
 */
extern void sift_cpagg_spec_calendar(const SiftColSchema *s, SiftCPaggSpec *spec);
/* The first day (days since 2000-01-01) of the cell of a grain that holds a day, and of the cell after the one that starts at day */
extern int64 sift_cpagg_cal_cell(int grain, int64 day);
extern int64 sift_cpagg_cal_next(int grain, int64 day);
/* Can a stored cell key of a leaf of this type (SIFT_COLT_*) be one: the first day of a cell of the grain, a day the type holds */
extern bool sift_cpagg_cal_key_ok(int type, int grain, int64 key);
/* Is leaf c a date, timestamp or timestamptz leaf of the top level that has calendar cells? */
extern bool sift_cpagg_cal_type(const SiftColSchema *s, int c);
/* the date, timestamp and timestamptz columns that get calendar cells (a plain dimension gives way for each of them in a default cube of more than 16 columns) */
extern int	sift_cpagg_cal_count(const SiftColSchema *s);
/* The date, timestamp and timestamptz leaves of the top level with no calendar dim in spec, in leaf order, at most max of them in out; returns how many there are */
extern int	sift_cpagg_cal_missing(const SiftColSchema *s, const SiftCPaggSpec *spec, int *out, int max);

/* 'default': every top level dim candidate with up to SIFT_PAGG_DEF_MEAS measures (numeric and floats first, then integers), and the rollup of every list. Never fails */
extern void sift_cpagg_spec_default(const SiftColSchema *s, SiftCPaggSpec *spec);
/*
 * The option: "" / "default", "off", or "cube(dim: m1, m2; dim2: m3; dim3)" for the top level and "rollup(d1, d2: m1, m2; ...)" for lists (one entry per list,
 * its dims then its measures), either or both, with leaf names (an attribute name or a shredder path; names[leaf]). A clause replaces the default of its kind and writes exactly
 * what it names (no byte budget); the kind that is not named keeps the default.
 * Returns NULL, or an error message (palloc'd) for an unknown or unsuitable name.
 */
extern const char *sift_cpagg_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec);
/* Cheap syntax check for the reloption validator (no schema yet) */
extern const char *sift_cpagg_syntax(const char *opt);
/*
 * The `rollup` option: "off", or "leaf, leaf, ..." the dims of the rollups (leaves of array elements or map values, as the names of paths; the
 * rollup of each list is these exactly, its measures are the numeric leaves of the list). Replaces the rollups of spec, which preagg built.
 * Same returns as sift_cpagg_spec_parse; apply with an empty option changes nothing.
 */
extern const char *sift_cpagg_rollup_apply(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec);
extern const char *sift_cpagg_rollup_syntax(const char *opt);
/*
 * The `rollup_dims` option: "a, b" or "a, b: m1, m2" (top-level scalar leaves; the measures are the integer and numeric top-level leaves when none are named): the
 * rollup of the rows of every unit, a cell per distinct combination of the dims (list -1 of spec). "" / "off": none. Same returns as sift_cpagg_rollup_apply.
 */
extern const char *sift_cpagg_rows_apply(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec);
extern const char *sift_cpagg_rows_syntax(const char *opt);
/*
 * The `calendar_columns` option: "a, b" or "all" (date, timestamp and timestamptz columns of the top level): the calendar dims of sift_cpagg_spec_calendar for
 * these columns only. Refused for a schema with JSON path leaves. "" / "off": none. Same returns as sift_cpagg_rollup_apply.
 */
extern const char *sift_cpagg_calendar_apply(const char *opt, const SiftColSchema *s, const char *const *names, SiftCPaggSpec *spec);
extern const char *sift_cpagg_calendar_syntax(const char *opt);

/* One leaf of a unit as the writer sees it, in the key domain; key is the segment's code for a dictionary leaf. n: the rows of the unit, the elements of the unit's list for a leaf of one */
typedef struct SiftCPaggCol
{
	const int64 *key;
	const uint64 *nulls;
	const uint64 *ovf;
	uint32		n;
	bool		ready;					/* key is usable: false for a STR leaf stored inline and for a leaf that was not decoded */
} SiftCPaggCol;

/*
 * The spec that builds, over other rows of a unit, the dims and rollups the unit's cube blob and element blob hold (same leaves, measures and calendar grains, no
 * byte budget, no minimum and maximum): the exceptions of a unit (docs/COL-PREAGG.md 11). false when a blob does not decode.
 */
extern bool sift_cpagg_mirror(const SiftColSchema *s, const SiftCPaggSpec *spec, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, SiftCPaggSpec *out);

/*
 * The exceptions of a unit (docs/COL-PREAGG.md 11): the cube and element rollups of the rows where its one NULL-heavy INT leaf is not NULL, with the dims of the unit's
 * own blobs; appended (leaf first) to xcube and xecube. leaf: -1 chooses a leaf, otherwise checks that stored leaf. eoff[c]: the element offsets of a leaf of a list, NULL for the others. false: the unit has none.
 */
extern bool sift_cpagg_excepts(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, const uint32 *const *eoff, uint32 nrows, int leaf,
							   const uint8 *cube, uint32 clen, const uint8 *ecube, uint32 elen, StringInfo xcube, StringInfo xecube);

/* The exact sums of one INT leaf's values */
extern void sift_cpagg_leaf(const SiftCPaggCol *c, SiftCLeafAgg *a);
/*
 * The cube of a unit of nrows rows, appended to out as one blob (nothing appended when no dim fits). unit_bytes is the encoded size of the unit,
 * the base of the default byte budget. Deterministic: the same columns give the same bytes.
 *
 * The rollups of the spec's lists are appended to eout as one blob, in a unit's element blob (below): the rows of a rollup are the elements of the
 * list, cols[leaf].n of them. eout NULL: no rollup is built (a segment of column format minor 3 has none). The byte budget is shared: the cube
 * first, then the rollups in list order take what is left; a rollup that does not fit drops its dim of the most distinct values until it does.
 *
 * Element blob: u8 nlists, nlists x (list leaf, bytes of its rollup) as varints, then the rollups (format in col/preagg.c), in ascending list order.
 */
extern void sift_cpagg_cube(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, Size unit_bytes,
							StringInfo out, StringInfo eout);

/*
 * Superunits (column format minor 12, docs/COL-PREAGG.md section 10): the units of a segment in groups of `fan` consecutive ones, each group with the
 * merged exact sums of every INT leaf and the merged cube of the dims all its units hold. A float measure is left out of the merged cube (its sum
 * depends on the order of the additions, which the units fix).
 */
#define SIFT_SUPER_MIN_FAN		2
#define SIFT_SUPER_MAX_FAN		256
#define SIFT_SUPER_DEFAULT_FAN	16			/* the default of the reloption superunit: +1.0% of the index, a whole-table aggregate 4 to 7 times faster, and the groups stay small enough for a range to cut few of them */
#define SIFT_SUPER_LEAF_OK		0x2			/* in SiftCSuperLeaf.flags, above SIFT_CAGG_SUMSQ_OVF: the leaf is summarised for this group */

/* One INT or text leaf of one group of units, 64 bytes as stored (a text leaf has no sums, and its min and max are bounds of the codes) */
typedef struct SiftCSuperLeaf
{
	int64		min;					/* key domain, of the units that hold a value; 0 when none does */
	int64		max;
	uint32		nnull;
	uint32		nvals;
	uint64		sum_lo;
	uint64		sum_hi;
	uint64		sumsq_lo;				/* 0 with SIFT_CAGG_SUMSQ_OVF */
	uint64		sumsq_hi;
	uint32		flags;					/* SIFT_CAGG_SUMSQ_OVF | SIFT_SUPER_LEAF_OK; 0: the leaf has no summary in this group */
	uint32		zflags;					/* SIFT_COLZ_CONST: one value in all the group */
} SiftCSuperLeaf;

StaticAssertDecl(sizeof(SiftCSuperLeaf) == 64, "a superunit leaf record is 64 bytes");

/*
 * The summary of the leaf over n units (their SiftCAgg); false when a unit flags an overflow value or inline text, has no sums while sums is set (an INT leaf has
 * them, a text leaf only counts), or the values exceed 2^32
 */
extern bool sift_cpagg_super_leaf(const SiftCAgg *u, uint32 n, bool sums, SiftCSuperLeaf *out);
/*
 * The cube of n units (their blobs, length 0: none) merged by group key, appended to out in the format of a unit's cube; nothing is appended when no dim
 * is in every unit with the same measures. false when a blob does not decode. Allocates in the current memory context, which the caller resets.
 */
extern bool sift_cpagg_super_cube(const SiftColSchema *s, const uint8 *const *blob, const uint32 *len, uint32 n, StringInfo out);

/* A decoded cube. Group arrays are in ascending key order, the NULL group (when has_null) last with no key */
typedef struct SiftCHistDim
{
	int			leaf;
	uint32		ngroups;
	bool		has_null;
	bool		minmax;					/* the cube holds min and max for the INT measures */
	int			nmeas;
	int16		meas[SIFT_PAGG_MAX_MEAS];
	int64	   *key;					/* [ngroups] (the NULL group's is 0) */
	uint32	   *count;					/* rows of each group */
	uint32	   *nn[SIFT_PAGG_MAX_MEAS];	/* non-NULL values of the measure in each group */
	SiftCI128  *sum[SIFT_PAGG_MAX_MEAS];	/* INT measure: exact sum; NULL for a float measure */
	double	   *sumf[SIFT_PAGG_MAX_MEAS];	/* float measure: sum in row order; NULL for an INT one */
	int64	   *min[SIFT_PAGG_MAX_MEAS];	/* INT measure of a cube with minmax: the smallest and the largest value of each group (key domain; 0 where nn is 0), else NULL */
	int64	   *max[SIFT_PAGG_MAX_MEAS];
} SiftCHistDim;

typedef struct SiftCHist
{
	int			ndims;
	SiftCHistDim dim[SIFT_PAGG_MAX_DIMS];
} SiftCHist;

/*
 * A decoded rollup: ntuples distinct tuples of the dims' values in ascending order (the first dim first, the NULL of a dim last), with the number of
 * elements of each and per measure the non-NULL count, the sum (INT measure: exact; float: in the order of the elements), the smallest and the largest value
 * (key domain for an INT measure) of the tuples that have one.
 */
typedef struct SiftCRollup
{
	int			list;
	uint32		ntuples;
	int			ndims;
	int16		dim[SIFT_PAGG_MAX_RDIMS];
	int64	   *key[SIFT_PAGG_MAX_RDIMS];		/* [ntuples] (a NULL's is 0) */
	uint8	   *isnull[SIFT_PAGG_MAX_RDIMS];	/* [ntuples], NULL when no tuple has a NULL there */
	uint32	   *count;
	int			nmeas;
	int16		meas[SIFT_PAGG_MAX_MEAS];
	uint32	   *nn[SIFT_PAGG_MAX_MEAS];
	SiftCI128  *sum[SIFT_PAGG_MAX_MEAS];		/* INT measure, else NULL */
	double	   *sumf[SIFT_PAGG_MAX_MEAS];		/* float measure, else NULL */
	int64	   *min[SIFT_PAGG_MAX_MEAS];		/* INT measure: (key domain; 0 where nn is 0) */
	int64	   *max[SIFT_PAGG_MAX_MEAS];
	double	   *minf[SIFT_PAGG_MAX_MEAS];		/* float measure */
	double	   *maxf[SIFT_PAGG_MAX_MEAS];
} SiftCRollup;

/*
 * The columns of a rollup a reader uses; the others are skipped without being stored (their arrays are NULL in the result). A dim or measure not listed is skipped,
 * a measure listed without sums or minmax has no sums or no minima and maxima. The counts of the tuples and the non-NULL counts of a listed measure are always read.
 */
typedef struct SiftCRollupNeed
{
	int			ndims;
	int16		dim[SIFT_PAGG_MAX_RDIMS + SIFT_PAGG_MAX_MEAS];
	int			nmeas;
	struct
	{
		int16		leaf;
		bool		sums;
		bool		minmax;
	}			meas[SIFT_PAGG_MAX_MEAS];
} SiftCRollupNeed;

/* false when the bytes are not a rollup of this schema; the arrays are palloc'd in mcxt; need NULL: every column */
extern bool sift_cpagg_rollup_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCRollup *r);
/* the tuples a rollup blob says it holds, read from its header (0: not a rollup) */
extern uint32 sift_cpagg_rollup_tuples(const uint8 *blob, uint32 len);
extern bool sift_cpagg_rollup_decode_need(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, const SiftCRollupNeed *need, SiftCRollup *r);
/* The rollup added up by one of its dims: the histogram of that leaf over the elements (false: the rollup lacks the dim) */
extern bool sift_cpagg_rollup_dim(const SiftColSchema *s, const SiftCRollup *r, int leaf, MemoryContext mcxt, SiftCHistDim *out);

/* false when the bytes are not a cube of this schema; the arrays are palloc'd in mcxt */
extern bool sift_cpagg_decode(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, SiftCHist *h);
/* Decodes only the dim of this leaf: 1 found (out set), 0 the unit has none (the cap, the byte budget, an overflow), -1 the bytes are not a cube of this schema */
extern int	sift_cpagg_decode_dim(const SiftColSchema *s, const uint8 *blob, uint32 len, MemoryContext mcxt, int leaf, SiftCHistDim *out);
/* The rollup of the list group (-1: of the rows) in an element blob (NULL: none, or the bytes are not an element blob of this schema: *len is then UINT32_MAX) */
extern const uint8 *sift_cpagg_elem_cube(const SiftColSchema *s, const uint8 *eblob, uint32 elen, int group, uint32 *len);
/* The dim with this leaf, NULL if the unit has none (the cap, the budget, an overflow) */
/* The measure slot of leaf m in the dim, -1 if it is not stored */
extern int	sift_cpagg_meas_slot(const SiftCHistDim *d, int m);

/*
 * C4 for the sidecar: recomputes the cube and the rollups of the stored dims and measures from the unit's columns and compares the bytes;
 * false on any difference, or when the cube holds min and max (minmax) a calendar dim or a rollup of the rows and the segment's column format (format) may not. A blob of length 0 is none.
 */
extern bool sift_cpagg_check(const SiftColSchema *s, const SiftCPaggCol *cols, uint32 nrows, const uint8 *blob, uint32 len,
							 const uint8 *eblob, uint32 elen, int format);
/*
 * What the stored sidecar of the units of a segment holds, against what the spec of the index writes now (sift_col_refresh, the planner's share of the
 * units that have a section). Read from the headers of the blobs only. dim[i], mm[i] and roll[i] count the units that hold the dim of spec dim i (with
 * min and max), and the rollup of spec rollup i; odd counts the units that hold something the spec no longer asks: a dim or a list it does not
 * name, other measures (exactly its own with cube(...)), other rollup dims (exactly its own with rollup(...)), min and max it does not ask, or
 * bytes that do not parse. minmax: whether the spec's min and max are written at all (the setting and the kinds of the segment allow minor 7).
 */
typedef struct SiftCPaggCensus
{
	uint32		units;
	uint32		odd;
	uint32		dim[SIFT_PAGG_MAX_DIMS];
	uint32		mm[SIFT_PAGG_MAX_DIMS];
	uint32		roll[SIFT_PAGG_MAX_ROLLUPS];
} SiftCPaggCensus;

/* Adds one unit (blobs of length 0: none) */
extern void sift_cpagg_census(const SiftColSchema *s, const SiftCPaggSpec *spec, bool minmax, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen,
							  SiftCPaggCensus *c);
/* The leaves a stored cube and element blob read (lists, dims and measures), marked true in need[ncols]; false when they do not decode */
extern bool sift_cpagg_check_excepts(const SiftColSchema *s, const SiftCPaggCol *cols, const uint32 *const *eoff, uint32 nrows, const uint8 *blob, uint32 len,
									 const uint8 *eblob, uint32 elen, int format, const uint8 *xblob, uint32 xlen, const uint8 *xeblob, uint32 xelen);
extern bool sift_cpagg_leaves(const SiftColSchema *s, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen, bool *need);
/* The cells of a calendar grain of the n rows of a date or timestamp leaf (the dead rows of a unit, col/delta.c); false for an infinity or a day no cell can start on */
extern bool sift_cpagg_cal_rows(const SiftColSchema *s, int leaf, int grain, const SiftCPaggCol *c, int64 *cells);

#endif							/* SIFT_COL_PREAGG_H */
