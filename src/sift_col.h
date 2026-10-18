/*
 * sift_col.h - the column engine and the index access method sift_col: the interface the executor, the planner and the shredder build against.
 * Design, format and invariants: the design notes. This header is normative for the C interface.
 *
 *   1. leaf columns and values       (what an index is made of; the shredder fills rows with them)
 *   2. metadata                      (planner: which leaves/expressions an index covers, statistics)
 *   3. the column source             (executor: units, filters, selection vectors, reads, MVCC)
 *   4. the shred interface           (consumed from sift_shred.h, owned by col-json)
 *   5. SQL entry points and options
 */
#ifndef SIFT_COL_H
#define SIFT_COL_H

#include "sift_host.h"

#include "sift.h"
#include "sift_cpu.h"
#include "sift_col_kern.h"
#include "sift_filter.h"

#define SIFT_COL_FORMAT_MINOR	1			/* SiftSegEntry.format of a col segment; 3.0 reads only its own (no release wrote 0) */
#define SIFT_COL_META_FLAG		0x00010000	/* SiftMetaPageData.flags: this is a sift_col index */
#define SIFT_COL_MAX_COLS		128
#define SIFT_COL_MAX_VALUE		0x3FFFFFFF	/* bytes of a string value: PostgreSQL's 1 GB */
#define SIFT_COL_DICT_MAX_ENTRY	255			/* a longer string is never a dictionary entry of a segment: its unit stores the strings inline */
#define SIFT_COL_DICT_MAX_BYTES	(64 << 20)	/* bytes of one segment dictionary: a unit that would exceed it stores its strings inline */
#define SIFT_COL_UNIT_MAX_ROWS	(SIFT_SB_PAGES * MaxHeapTuplesPerPage)
#define SIFT_COL_VEC_ROWS		SIFT_COL_UNIT_MAX_ROWS

/* ---------------------------------------------------------------------------
 * 1. Leaf columns and values
 * ------------------------------------------------------------------------- */

typedef enum SiftColKind
{
	SIFT_COLK_SCALAR = 1,
	SIFT_COLK_GROUP				/* array of objects: its value in a row is the element count; its children are SCALAR leaves */
} SiftColKind;

/* Storage class. Every chunk holds integers in the key domain: INT as is, F4/F8 as the order-preserving key of the IEEE bits, STR a dictionary code */
typedef enum SiftColStore
{
	SIFT_COLS_INT = 1,			/* int2, int4, int8, bool, date, timestamp, timestamptz (and a GROUP's counts) */
	SIFT_COLS_F4,
	SIFT_COLS_F8,
	SIFT_COLS_STR,				/* text, varchar, char, name; per-segment dictionary */
	/*
	 * Opaque bytes of any length (up to 1 GB) with an exact round trip: numeric, uuid, long text, jsonb, arrays. The engine never
	 * interprets them; the owner of the segment (the table AM) defines the image. Not filterable except IS NULL, no dictionary, zones
	 * carry only nvals/nnull. The index AM does not offer it (CREATE INDEX refuses those types).
	 */
	SIFT_COLS_VAR
} SiftColStore;

/* The table AM keeps its next virtual block in the metapage field the BM25 side uses for the total document length (always 0 for col segments) */
#define SIFT_COL_META_NEXT_VBLOCK(meta)		((meta)->sum_dl)

#define SIFT_COL_MAX_SCALE		18	/* numeric precision (and so scale) the INT class holds exactly */

#define SIFT_COLD_LOSSY		0x0001	/* shredded path whose values may not fit the declared type: predicates on it must not be pushed down */

/*
 * Logical type of a leaf, part of the stored schema and of its hash. It is a tag of the format, not of any host: a host maps its own types
 * to it at its boundary (the PostgreSQL side is sift_col_pg.h) and keeps what it cannot name here as OTHER, resolving the exact type from
 * its own catalog. The tag never changes how a chunk is stored (that is the storage class).
 */
typedef enum SiftColType
{
	SIFT_COLT_OTHER = 0,
	SIFT_COLT_BOOL,
	SIFT_COLT_INT2,
	SIFT_COLT_INT4,
	SIFT_COLT_INT8,
	SIFT_COLT_FLOAT4,
	SIFT_COLT_FLOAT8,
	SIFT_COLT_NUMERIC,			/* exact decimal; a scaled integer (SiftColDef.scale) in the INT class */
	SIFT_COLT_TEXT,				/* text, varchar, char(n), name */
	SIFT_COLT_BYTEA,
	SIFT_COLT_DATE,
	SIFT_COLT_TIMESTAMP,
	SIFT_COLT_TIMESTAMPTZ,
	SIFT_COLT_UUID,
	SIFT_COLT_JSONB,
	SIFT_COLT_END
} SiftColType;

typedef struct SiftColDef
{
	int16		id;				/* position in the leaf list, 0..ncols-1 */
	int16		parent;			/* id of the GROUP leaf this is a child of, -1 for a top level leaf */
	uint8		kind;			/* SiftColKind */
	uint8		store;			/* SiftColStore */
	uint16		flags;			/* SIFT_COLD_* */
	int16		attno;			/* 1-based attribute of the index the leaf comes from */
	uint8		type;			/* SiftColType: the logical type of the values the leaf yields to the host */
	const char *path;			/* "" for a plain attribute, else the shredder's path text (e.g. items[].category) */
	uint8		scale;			/* numeric(p <= 18, s): digits after the decimal point; the INT value is the number times 10^scale. 0 otherwise */
} SiftColDef;

/* A value of a leaf in a row, or a constant of a predicate. INT: v.i. F4/F8: v.f (float4 promoted exactly). STR: v.s with len bytes (not terminated). */
typedef struct SiftColValue
{
	union
	{
		int64		i;
		double		f;
		const char *s;
	}			v;
	uint32		len;
	bool		isnull;
	bool		overflow;		/* the value exists but the leaf cannot hold it (the shredder's OVERFLOW): stored as NULL and flagged; isnull is also set */
} SiftColValue;

/*
 * One row to index, as ambuild, aminsert and the shredder produce it. val[id] is the value of a top level scalar leaf; for a GROUP leaf
 * val[id].v.i is the element count (isnull: the array is absent). elem[id] of a child leaf points to val[parent].v.i values, NULL for
 * the other leaves. All memory belongs to the caller (a per-row context).
 */
typedef struct SiftColRow
{
	ItemPointerData tid;
	int			ncols;
	SiftColValue *val;
	SiftColValue **elem;
} SiftColRow;

/*
 * Order-preserving key of a float and back. The sign of zero is kept: -0 is the key just below +0 (-1 and 0), so every non-NaN value
 * comes back with the bits it went in with; PostgreSQL compares -0 and +0 equal, which the tests built by sift_ctest_make account
 * for (a constant of zero spans both keys). Every NaN is one NaN above +Inf.
 */
static inline int64
sift_col_f8_key(double f)
{
	uint64		b;

	if (f != f)
		b = UINT64CONST(0x7FF8000000000000);
	else
		memcpy(&b, &f, sizeof(b));
	return (int64) (b ^ ((uint64) ((int64) b >> 63) >> 1)) ;
}

/*
 * Order-preserving key of the first 8 bytes of a string: big endian, zero padded, sign bit flipped. s < t (bytewise) implies
 * prefix(s) <= prefix(t); strings of equal keys differ only after byte 8 or by trailing zero bytes. Zones and filters of inline strings use it.
 */
static inline int64
sift_col_str_prefix(const char *s, uint32 len)
{
	uint64		b = 0;

	for (uint32 i = 0; i < 8; i++)
		b = b << 8 | (i < len ? (uint8) s[i] : 0);
	return (int64) (b ^ UINT64CONST(0x8000000000000000));
}

static inline double
sift_col_f8_from_key(int64 key)
{
	uint64		b = (uint64) key ^ ((uint64) (key >> 63) >> 1);
	double		f;

	memcpy(&f, &b, sizeof(f));
	return f;
}

/* float4 keys are the float8 keys of the promoted value (exact), so one domain serves both */
static inline int64
sift_col_f4_key(float4 f)
{
	return sift_col_f8_key((double) f);
}

/* ---------------------------------------------------------------------------
 * 2. Metadata (planner)
 * ------------------------------------------------------------------------- */

typedef struct SiftColIndexStats
{
	uint64		nrows;			/* rows of the immutable segments + mutable records (dead ones included) */
	uint32		nsegments;		/* immutable */
	uint32		nmutable;		/* mutable segments */
	uint64		bytes;			/* of the immutable segments */
	uint32		nunits;			/* (segment, sb) units */
	double		allvisible_frac;	/* share of the heap's pages the visibility map says all-visible (0..1) */
	BlockNumber heap_pages;
} SiftColIndexStats;

typedef struct SiftColIndexMeta
{
	Oid			indexoid;
	Oid			heapoid;
	bool		valid;			/* indisvalid && indisready && indislive */
	bool		partial;		/* indpred != NIL: the plan must prove the query implies it (the index only holds those rows) */
	int			natts;			/* index attributes */
	int			ncols;			/* leaves */
	SiftColDef *cols;			/* [ncols] */
	SiftColIndexStats stats;
} SiftColIndexMeta;

/* ---------------------------------------------------------------------------
 * 3. The column source
 * ------------------------------------------------------------------------- */

/* Bit vector of n bits (a selection of rows, or of elements); nwords = SIFT_COL_NWORDS(n); bits past n are zero; count is the popcount */
typedef struct SiftColSel
{
	uint32		n;
	uint32		nwords;
	uint32		count;
	uint64	   *bits;
} SiftColSel;

typedef enum SiftColOp
{
	SIFT_COLOP_EQ = 1,			/* vals[0] */
	SIFT_COLOP_NE,				/* vals[0]: rows with a value different from it (NULL rows are not selected, as in SQL) */
	SIFT_COLOP_LT,
	SIFT_COLOP_LE,
	SIFT_COLOP_GT,
	SIFT_COLOP_GE,
	SIFT_COLOP_IN,				/* nvals constants (no NULL among them) */
	SIFT_COLOP_NOTIN,			/* nvals constants: rows whose value is none of them (NULL rows are not selected) */
	SIFT_COLOP_ISNULL,
	SIFT_COLOP_NOTNULL,
	SIFT_COLOP_PREFIX			/* STR only: vals[0] is a prefix of the value (LIKE 'abc%' in the C collation) */
} SiftColOp;

/*
 * A predicate on one leaf; the constants are SiftColValues of the leaf's store (INT: v.i, F4/F8: v.f, STR: v.s/len). Several
 * predicates in an array are ANDed. For an F4 leaf a double constant is compared as float8 against the promoted value, as PostgreSQL does.
 * Range operators on STR compare bytes (the planner only offers them for the C collation).
 */
typedef struct SiftColPred
{
	int			col;			/* leaf id */
	SiftColOp	op;
	int			nvals;
	const SiftColValue *vals;
} SiftColPred;

/* Same-element predicates: a row is selected when some element of the group satisfies every predicate (preds are on children of group); negate: when none does */
typedef struct SiftColGroupPred
{
	int			group;			/* GROUP leaf id */
	int			npreds;
	const SiftColPred *preds;
	bool		negate;
} SiftColGroupPred;

/* A STR leaf's dictionary for the unit's segment: code -> bytes. Valid for the life of the unit's segment in the source. */
typedef struct SiftColDict
{
	uint32		n;
	const uint32 *off;			/* n + 1 byte offsets */
	const char *bytes;
	uint64		id;				/* identifies (segment, leaf): equal ids mean the same codes */
} SiftColDict;

#define SIFT_COL_DICT_LEN(d, c)		((d)->off[(c) + 1] - (d)->off[(c)])
#define SIFT_COL_DICT_STR(d, c)		((d)->bytes + (d)->off[c])

/* Decoded values of a leaf for selected rows (or elements), in selection order */
typedef struct SiftColVec
{
	SiftColStore store;
	uint32		n;
	const int64 *i;				/* INT */
	const double *f;			/* F4, F8 (the float4 promoted) */
	const uint32 *codes;		/* STR: indexes into dict */
	const SiftColDict *dict;	/* STR */
	const uint64 *voff;			/* VAR: n + 1 byte offsets into vbytes */
	const char *vbytes;			/* VAR */
	const uint64 *nulls;		/* NULL when the leaf has no NULL among these values; bit set = NULL */
	const uint64 *ovf;			/* NULL when none of these values overflowed; bit set = overflow (also set in nulls): the heap is the truth for them */
	/*
	 * Leaf read through a row selection that is a child of a group: the values are the elements of the selected rows, row k owning
	 * elements offsets[k] .. offsets[k + 1] - 1 (nrows + 1 entries). NULL for other leaves. A GROUP leaf read this way yields the
	 * counts as INT values (its nulls are the absent arrays).
	 */
	const uint32 *offsets;
	uint32		nrows;
} SiftColVec;

/* Zone of a leaf in a unit (the exact min/max/sum/count over all its rows, dead ones included) */
typedef struct SiftColZone
{
	int64		min;			/* key domain; meaningless when nnull == nvals */
	int64		max;
	int64		sum_i;			/* INT; valid unless noSum */
	double		sum_f;			/* F4, F8, in row order */
	uint32		nvals;			/* rows (elements for a child) */
	uint32		nnull;
	bool		nosum;			/* the integer sum overflowed int64 */
	bool		has_nan;
	bool		is_const;		/* every non-NULL value equal (min == max) */
	bool		has_ovf;		/* some value overflowed: min/max/sum/nnull do not describe it; no pruning or metadata answer from this leaf and unit */
} SiftColZone;

#define SIFT_COLO_NOMUTABLE		0x0001	/* skip the mutable segments (verification, maintenance reading immutable data) */
#define SIFT_COLO_NOCHECKS		0x0002	/* skip the privilege / RLS checks (internal callers that did them) */

/* EXPLAIN and cost feedback of a source */
typedef struct SiftColCounters
{
	int64		segments;		/* immutable segments in the directory snapshot */
	int64		mutable_rows;	/* rows read from mutable segments */
	int64		units;			/* candidate units visited */
	int64		units_pruned;	/* skipped by their zone maps */
	int64		rows_scanned;	/* rows of the units visited */
	int64		rows_vetted;	/* rows checked against the heap (page not all-visible) */
	int64		heap_pages;
	int64		bytes_decoded;
	SiftSimdLevel simd;			/* the level the kernels ran at (sift_cpu.active) */
} SiftColCounters;

typedef struct SiftColSource SiftColSource;

/*
 * One unit = the rows of one segment inside one superblock. The fields before priv are read-only for the caller.
 */
typedef struct SiftColUnit
{
	SiftColSource *src;
	uint32		sb;
	uint32		nrows;
	bool		in_mutable;		/* rows of a mutable segment (chunks encoded in memory) */
	bool		clean;			/* no dead rows (the SB is not dirty) */
	void	   *priv;
} SiftColUnit;

/*
 * Opens the source: copies the directory (registered snapshot held until close), starts the visibility state (sift_vis) for snapshot, takes
 * PredicateLockRelation under SERIALIZABLE, checks SELECT on the table and refuses row-level security (ERRCODE_FEATURE_NOT_SUPPORTED) unless
 * SIFT_COLO_NOCHECKS. mcxt owns everything the source allocates (close releases buffers and pins, never memory). The caller holds
 * at least AccessShareLock on the heap and the index.
 */
extern SiftColSource *sift_col_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags);
extern void sift_col_close(SiftColSource *src);

/*
 * The one hook between the engine and the table it indexes: which rows of a unit are visible. The index glue installs the
 * heap/visibility-map implementation (sift_vis) at open; the table AM (col-tam) installs its own. sb_mask: the pages of the superblock whose
 * rows are all visible to the snapshot (256 bits, zero = check everything). filter_page: keeps in offmask (bit off-1 per heap offset) only the
 * offsets of block blk visible to the snapshot and returns how many are left.
 */
typedef struct SiftColVisibility
{
	void		(*sb_mask) (void *arg, uint32 sb, uint64 *pagemask);
	int			(*filter_page) (void *arg, BlockNumber blk, uint64 *offmask);
	void		(*counters) (void *arg, int64 *rows_vetted_heap_pages);	/* heap pages read since the last call, may be NULL */
	void	   *arg;
} SiftColVisibility;

extern void sift_col_set_visibility(SiftColSource *src, const SiftColVisibility *vis);

extern int	sift_col_ncols(const SiftColSource *src);
extern const SiftColDef *sift_col_def(const SiftColSource *src, int col);

/* Superblocks of the heap now: the range [0, heap_sbs) for a morsel counter (a later insert may add rows past it: they are not visible to the snapshot) */
extern uint32 sift_col_heap_sbs(const SiftColSource *src);

/*
 * Starts (or restarts) the iteration over the units of superblocks [sb_lo, sb_hi) (sb_hi exclusive, PG_UINT32_MAX = to the end). Units whose
 * zone maps exclude the predicates (all ANDed) are skipped and counted. preds are copied; may be NULL.
 */
extern void sift_col_begin(SiftColSource *src, const SiftColPred *preds, int npreds, uint32 sb_lo, uint32 sb_hi);

/*
 * Adds in-memory units to the source before sift_col_begin: each batch (one SB, rows in key order, all live) is read like a unit of a mutable
 * segment (in_mutable), together with the stored units of the same SB. The rows' visibility is the installed SiftColVisibility's. The batches
 * must outlive the source (the table AM adds the rows of its delta pages this way).
 */
struct SiftColBatch;
extern void sift_col_add_batches(SiftColSource *src, struct SiftColBatch *const *batches, int nbatches);

/* The next candidate unit: ascending SB, immutable segments in directory order then the mutable ones; false at the end. Invalidates the vectors of the previous unit. */
extern bool sift_col_next(SiftColSource *src, SiftColUnit *unit);

/* ---- selections ---- */

extern void sift_col_sel_alloc(SiftColSource *src, uint32 n, SiftColSel *sel);	/* all clear, from the source's arena, reused after sift_col_next */
extern void sift_col_sel_all(const SiftColUnit *unit, SiftColSel *sel);		/* every live row of the unit (allocates sel) */
extern uint32 sift_col_sel_count(SiftColSel *sel);							/* recounts */
extern uint32 sift_col_sel_idx(const SiftColSel *sel, uint32 *idx);		/* positions of the set bits (room for sel->count + SIFT_COL_IDXPAD) */

/* ---- filters: each narrows sel and returns whether any row is left ---- */

/* Top level predicates (leaf ids of scalar leaves); a predicate on a child leaf is an error (use the group forms) */
extern bool sift_col_filter(const SiftColUnit *unit, const SiftColPred *preds, int npreds, SiftColSel *sel);

/* Array-element predicates with same-element semantics: keeps the rows with a satisfying element (or none, negate) */
extern bool sift_col_filter_group(const SiftColUnit *unit, const SiftColGroupPred *gp, SiftColSel *sel);

/* The elements of the unit's group that satisfy every predicate, for the rows of rowsel (NULL: all rows): elemsel has n = elements of the unit */
extern uint32 sift_col_filter_elems(const SiftColUnit *unit, const SiftColGroupPred *gp, const SiftColSel *rowsel, SiftColSel *elemsel);

/* AND with the TID masks of a TIDBitmap (sift_filter.c): a BM25 match, a btree or BRIN bitmap */
extern bool sift_col_filter_tids(const SiftColUnit *unit, const SiftFilter *filter, SiftColSel *sel);

/*
 * MVCC: keeps the selected rows that are visible to the snapshot: rows of all-visible pages pass, the others are checked
 * against the heap. Call it after the value filters. Counts rows_vetted / heap_pages.
 */
extern bool sift_col_vet(const SiftColUnit *unit, SiftColSel *sel);

/* ---- reading ---- */

/*
 * Decodes leaf col for the rows of sel (NULL: all rows of the unit; for a child leaf: the elements of those rows, with vec->offsets), or, for
 * a child leaf, with elemsel set (the elements it selects; offsets then count the selected elements per row of rowsel, or are NULL if rowsel
 * is NULL). The vectors live in the source's arena until the next read of the same leaf or sift_col_next.
 */
extern void sift_col_read(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vec);
extern void sift_col_read_elems(const SiftColUnit *unit, int col, const SiftColSel *elemsel, SiftColVec *vec);

/* Per element of the unit's group: the ordinal of its row (nelems entries) and the element count of every row (nrows entries, NULL array = 0) */
extern uint32 sift_col_group_elems(const SiftColUnit *unit, int group);
extern void sift_col_group_parents(const SiftColUnit *unit, int group, const uint32 **parent, const uint32 **counts);

/* The TIDs of the rows of sel (room for sel->count), in row order */
extern void sift_col_tids(const SiftColUnit *unit, const SiftColSel *sel, ItemPointerData *tids);

/*
 * The zone of a leaf in the unit (no chunk read). sift_col_unit_exact: the zone describes exactly the rows a query without filters would
 * see, i.e. the unit is clean and every row is on an all-visible page: then count(col), sum, min and max need no chunk.
 */
extern void sift_col_zone(const SiftColUnit *unit, int col, SiftColZone *zone);
extern bool sift_col_unit_exact(const SiftColUnit *unit);

extern const SiftColCounters *sift_col_counters(const SiftColSource *src);
/* The index-bound declarations (relcache, planner expressions, shredder adapter, options, SQL entry points): sift_col_pg.h, PostgreSQL only */

#endif							/* SIFT_COL_H */
