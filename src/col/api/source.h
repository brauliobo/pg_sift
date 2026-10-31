/*
 * col/api/source.h - 3. The column source: units, filters, selection vectors, reads, MVCC (executor), and the SQL entry points
 * A piece of col/col.h: include col/col.h, not this file.
 */
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
	SIFT_COLOP_PREFIX,			/* STR only: vals[0] is a prefix of the value (LIKE 'abc%' in the C collation) */
	SIFT_COLOP_CONTAINS,		/* STR only: vals[0] (not empty) is a substring of the value, bytewise (LIKE '%abc%') */
	SIFT_COLOP_NOTCONTAINS,		/* STR only: a non-NULL value that does not hold vals[0] (NOT LIKE '%abc%') */
	SIFT_COLOP_SUFFIX,			/* STR only: vals[0] is a suffix of the value, bytewise (LIKE '%abc') */
	SIFT_COLOP_ICONTAINS,		/* STR only: lower(value) holds vals[0], ASCII in lower case (ILIKE '%abc%'); with nvals 3, vals[1] and vals[2] are lower() of U+0130 and of U+212A, the two characters whose lower case is ASCII */
	SIFT_COLOP_SHAPE			/* the head of a range or a network: vals[0].v.s is the SiftColWTest of a containment operator (col/shape.c), made by the access method scan */
} SiftColOp;

/* The STR tests that read every string of a dictionary, in code order, and not a run of its sorted order */
#define SIFT_COLOP_SCANS(op) ((op) == SIFT_COLOP_CONTAINS || (op) == SIFT_COLOP_NOTCONTAINS || (op) == SIFT_COLOP_ICONTAINS || (op) == SIFT_COLOP_SUFFIX)

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

/* A bound of a range: the two of one leaf are tested together (a range found in a sorted run, scan_filter.c) */
static inline bool
sift_col_pred_is_bound(const SiftColPred *p)
{
	return p->op == SIFT_COLOP_LT || p->op == SIFT_COLOP_LE || p->op == SIFT_COLOP_GT || p->op == SIFT_COLOP_GE;
}

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
	/* the codes in byte order of their strings: stored (minor 14) or sorted already, else NULL; `rows`: the rows a scan is about to compare with the strings, which
	 * the dictionary adds to those of the scans before it and sorts itself once they reach a few times its entries, as a dictionary tested often builds its
	 * hash (cdict_find); octx is its argument */
	const uint32 *(*order) (const void *octx, uint64 rows);
	const void *octx;
	/* A dictionary of many strings that the segment stores in blocks and the backend has not loaded (sift_col_read_lane): off and bytes are NULL, get gives the string of a code (into buf of
	 * SIFT_COL_DICT_MAX_ENTRY bytes, valid until the next call) by decompressing its block, and sift_col_dict_full(gctx, &d) sets the arrays of the whole dictionary in d (its id stays), loaded now */
	const char *(*get) (const void *gctx, uint32 code, uint32 *len, char *buf);
	const void *gctx;
} SiftColDict;
extern void sift_col_dict_full(const void *gctx, SiftColDict *self);

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

#define SIFT_COLO_NOWBUF		0x0001	/* skip the write buffers (verification, maintenance reading immutable data) */
#define SIFT_COLO_NOCHECKS		0x0002	/* skip the privilege / RLS checks (internal callers that did them) */
#define SIFT_COLO_NOACL		0x0008	/* skip the table privilege check only (the RLS check stays): the executor of the query checked the privileges of the role, or of the view owner, on the columns it reads */
#define SIFT_COLO_POINT_TIDS	0x0020	/* ... and the heap addresses of a directory that lists them are returned as they are, without reading a unit (sift_col_point_tids): the caller wants no values of the rows, only the addresses, and has the heap check the condition again */
#define SIFT_COLO_POINT_ROWS	0x0040	/* ... and a unit comes narrowed to the rows the directory lists (sift_col_point_narrow): the caller tests every predicate again on them, as it does on the rows of a unit */
#define SIFT_COLO_POINT_LAZY	0x0080	/* ... and with SIFT_COLO_POINT_TIDS, the addresses of a lookup are handed over as the caller asks for them (sift_col_point_next), not all at the begin: a scan that stops after a few rows decodes a few */
#define SIFT_COLO_POINTS		0x0010	/* the scan may answer an equality or a short IN list from the point directory of a segment, whose rows are the fingerprint's: the caller has the heap check the condition again (sift_col_filter_points) */
#define SIFT_COLO_SNAP_HELD		0x0004	/* the caller keeps the snapshot registered while the source is open: one registration for the thousands of sources of a partitioned table's scan (a resource owner holding that many registrations of one snapshot takes quadratic time to add and forget them) */

/* EXPLAIN and cost feedback of a source */
typedef struct SiftColCounters
{
	int64		segments;		/* immutable segments in the directory snapshot */
	int64		wbuf_rows;	/* rows read from write buffers */
	int64		units;			/* candidate units visited */
	int64		units_pruned;	/* skipped by their zone maps or small-set summaries */
	int64		units_set;		/* of those, the ones only a small-set summary could skip */
	int64		rows_scanned;	/* rows of the units visited */
	int64		point_units;	/* units narrowed to the rows a point directory lists (SIFT_COLO_POINT_ROWS) */
	int64		point_rows;		/* and the rows they were narrowed to */
	int64		run_windows;	/* units whose range on a leaf with a sorted run was answered by the run (sift_col_filter) */
	int64		rows_vetted;	/* rows checked against the heap (page not all-visible) */
	int64		heap_pages;
	int64		bytes_decoded;
	int64		tests_made;		/* predicates turned into a test of the stored codes (a LUT over a segment's dictionary for a text or an image leaf) */
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
	bool		in_wbuf;		/* rows of a write buffer (chunks encoded in memory) */
	uint32		nsu;			/* a superunit (sift_col_super): the units it stands for, whose merged pre-aggregates are all it has; 0 for a unit */
	int			except;			/* sift_col_unit_except: the leaf whose NULL rows the histograms and rollups of the unit answer for; -1: the unit's own rows */
	void	   *priv;
} SiftColUnit;

/* Whether the next sift_col_read and sift_col_read_idx of the unit may give a STR leaf a dictionary that stays in the segment (get, no off and bytes): for a reader that asks for the strings of a few codes */
extern void sift_col_unit_lazy_dict(const SiftColUnit *unit, bool on);

/*
 * Superunits (docs/COL-PREAGG.md section 10). With sift_col_super(src, true), a source whose next unit starts a group of units of a segment that
 * holds a summary of the group hands over the group as one unit, when every unit of the group passed the zone tests and is exact (clean,
 * every page all-visible to the snapshot). It has pre-aggregates only (sift_col_unit_agg, sift_col_unit_hist): a consumer that cannot answer
 * from them calls sift_col_unit_split, and the next calls return the units of the group one by one.
 */
extern void sift_col_super(SiftColSource *src, bool on);
/* With a key leaf and the measures to read of it, the next unit may be a whole segment that has the key table of the leaf, with those measures (cu->nsu is its units): its histogram of the leaf, sift_col_unit_hist, is all it has; call after sift_col_super */
extern void sift_col_super_whole(SiftColSource *src, int leaf, const int *meas, int nmeas);
/* A count of the rows of a text leaf that pass one test (EQ, LT, LE, GT, GE, PREFIX against s, bytewise) and nothing else: a segment whose dictionary of the leaf holds row counts (option count_columns), and whose units are all exact (clean, every page all-visible), is handed over as one unit of cu->nsu units whose rows sift_col_unit_counted gives; call after sift_col_super */
extern void sift_col_count_range(SiftColSource *src, int leaf, SiftColOp op, const char *s, uint32 len);
/* ... or of an integer leaf with a global order (order_array_columns): a segment of exact units counts the rows of the range of the scan's comparisons, all on that leaf, from it */
extern void sift_col_count_cmp(SiftColSource *src, int leaf);
/* A count of the rows that own one value of a list (the lone equality on an element leaf), answered by the point directory of a segment whose units are all exact; call after sift_col_super */
extern void sift_col_count_elem(SiftColSource *src);
extern bool sift_col_unit_counted(const SiftColUnit *unit, uint64 *rows);
extern void sift_col_unit_split(SiftColSource *src, const SiftColUnit *unit);
/* The superblocks a parallel range should span for a group of units to fit in it: several groups of the largest fan, whatever the number of segments that interleave */
extern uint32 sift_col_super_span(SiftColSource *src);

/*
 * Opens the source: copies the directory (registered snapshot held until close), starts the visibility state (sift_vis) for snapshot, takes
 * PredicateLockRelation under SERIALIZABLE, checks SELECT on the table and refuses row-level security (ERRCODE_FEATURE_NOT_SUPPORTED) unless
 * SIFT_COLO_NOCHECKS. mcxt owns everything the source allocates (close releases buffers and pins, never memory). The caller holds
 * at least AccessShareLock on the heap and the index.
 */
extern SiftColSource *sift_col_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags);
extern void sift_col_close(SiftColSource *src);
/* Releases the buffers the source keeps pinned between units (a partitioned table's scan leaves a partition for good); reading on takes them again */
extern void sift_col_unpin(SiftColSource *src);

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


extern int	sift_col_ncols(const SiftColSource *src);
extern const SiftColDef *sift_col_def(const SiftColSource *src, int col);

/* Superblocks of the heap now: the range [0, heap_sbs) for a morsel counter (a later insert may add rows past it: they are not visible to the snapshot) */
extern uint32 sift_col_heap_sbs(const SiftColSource *src);
/* Is the number of each stored unit the heap superblock of its rows? Not once a segment cuts its units by key or TID range */
extern bool sift_col_sb_heap(const SiftColSource *src);

/*
 * Starts (or restarts) the iteration over the units of superblocks [sb_lo, sb_hi) (sb_hi exclusive, PG_UINT32_MAX = to the end). Units whose
 * zone maps exclude the predicates (all ANDed) are skipped and counted. preds are copied; may be NULL.
 */
extern void sift_col_begin(SiftColSource *src, const SiftColPred *preds, int npreds, uint32 sb_lo, uint32 sb_hi);

/*
 * A zone test of the scan beyond the conjunction of predicates: a unit is kept when every pred may select a row of it and every kid keeps
 * it (any: some kid keeps it, each tested on its own: an OR). It tests the zones only; the caller filters the rows.
 */
typedef struct SiftColPrune
{
	const SiftColPred *preds;
	int			npreds;
	bool		any;
	int			nkids;
	const struct SiftColPrune *kids;
} SiftColPrune;

/* sift_col_begin, the units also kept by every one of the nzones tests */
extern void sift_col_begin_zones(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 sb_lo, uint32 sb_hi);
extern bool sift_col_kept_map(SiftColSource *src, const SiftColPred *preds, int npreds, const SiftColPrune *zones, int nzones, uint32 nspace, uint64 *kept, uint64 *rows, bool count);

/*
 * Adds in-memory units to the source before sift_col_begin: each batch (one SB, rows in key order, all live) is read like a unit of a write
 * buffer (in_wbuf), together with the stored units of the same SB. The rows' visibility is the installed SiftColVisibility's. The batches
 * must outlive the source (the table AM adds the rows of its delta pages this way).
 */
struct SiftColBatch;
extern void sift_col_add_batches(SiftColSource *src, struct SiftColBatch *const *batches, int nbatches);

/*
 * A batch (one SB, rows in key order, all live) as the unit the source is at, in place of the one it was at. Invalidates the vectors of the
 * previous unit like sift_col_next; the batch must stay until the next unit. Its rows are checked against the snapshot like those of a stored unit.
 */
extern void sift_col_unit_batch(SiftColSource *src, struct SiftColBatch *b, SiftColUnit *unit);
struct SiftColSchema;
extern const struct SiftColSchema *sift_col_schema(const SiftColSource *src);

/* The next candidate unit: ascending SB, immutable segments in directory order then the write buffer; false at the end. Invalidates the vectors of the previous unit. */
extern bool sift_col_next(SiftColSource *src, SiftColUnit *unit);

/* ---- selections ---- */

extern void sift_col_sel_alloc(SiftColSource *src, uint32 n, SiftColSel *sel);	/* all clear, from the source's arena, reused after sift_col_next */
extern void sift_col_sel_all(const SiftColUnit *unit, SiftColSel *sel);		/* every live row of the unit (allocates sel) */
extern uint32 sift_col_sel_count(SiftColSel *sel);							/* recounts */
extern uint32 sift_col_sel_idx(const SiftColSel *sel, uint32 *idx);		/* positions of the set bits (room for sel->count + SIFT_COL_IDXPAD) */

/* ---- filters: each narrows sel and returns whether any row is left ---- */

/* Top level predicates (leaf ids of scalar leaves); a predicate on a child leaf is an error (use the group forms) */
extern bool sift_col_filter(const SiftColUnit *unit, const SiftColPred *preds, int npreds, SiftColSel *sel);
/* The heap addresses the point directories of heap addresses answered at the last begin (SIFT_COLO_POINT_TIDS), ascending: their units are not read; the caller has the heap check the conditions */
extern const SiftKey *sift_col_point_tids(const SiftColSource *src, uint32 *n);
/* An equality or IN predicate (at most SIFT_CWBUF_LOOKUP_MAX values, sorted for a binary search above 8) on a plain integer-like, text or uuid leaf finds the rows of the write buffers in their records: sift_col_wbuf_lookup, for a source opened with SIFT_COLO_NOWBUF; their heap addresses are for the heap to check */
#define SIFT_CWBUF_LOOKUP_MAX	65536	/* the keys of an IN list the point directories answer (SIFT_CPOINT_MAX_KEYS) */
extern bool sift_col_wbuf_lookup_ok(const SiftColDef *def, const SiftColPred *p);
extern ItemPointerData *sift_col_wbuf_lookup(SiftColSource *src, const SiftColPred *p, MemoryContext mcxt, uint32 *n);
/*
 * With SIFT_COLO_POINT_LAZY: whether the begin left lists of addresses to read (sift_col_point_next); the next at most max addresses of the units that are not
 * read, ascending in each segment; 0 once all are handed over, after which sift_col_next returns the units that have a dead row, as the rest of the scan.
 */
extern bool sift_col_point_lazy(const SiftColSource *src);
extern uint32 sift_col_point_next(SiftColSource *src, SiftKey *out, uint32 max);
/*
 * The global order of leaf col (col/gorder.h) as sources of sorted entries, for an ORDER BY .. LIMIT that has the heap say which rows are live: the number of
 * sources (one for each segment, one for the rows of the write buffers), or -1 when it cannot answer. _info: entries and values of source i; _head: the first w entries of
 * the chunk that holds entry p, from the entry *at, as keys and heap addresses, each SIFT_CGORD_CAP slots; the count returned.
 */
extern int sift_col_gorder_open(SiftColSource *src, int col, int rows, int npin, const int *pinleaf, const int64 *pinval, int *grouped, uint32 collid, const struct SiftCGOrdWant *want);
extern bool sift_col_gorder_cuts(SiftColSource *src, int i, int col, struct SiftCGOrdCuts *cuts);
extern const char *sift_col_gorder_str(SiftColSource *src, int i, int col, int64 key, uint32 *len);
extern uint32 sift_col_gorder_strrank(SiftColSource *src, int i, int col, const char *s, uint32 len, bool after);
extern bool sift_col_gorder_info(SiftColSource *src, int i, int col, uint32 *entries, uint32 *values);
extern bool sift_col_gorder_exact(SiftColSource *src, int i, SiftKey tid);
extern void sift_col_gorder_head(SiftColSource *src, int i, int col, uint32 p, uint32 w, struct SiftCGOrdBuf *buf);
extern void sift_col_gorder_range(SiftColSource *src, int i, int col, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne);
extern uint32 sift_col_gorder_seek(SiftColSource *src, int i, int col, uint32 vs, uint32 ve, int64 key, bool after, struct SiftCGOrdBuf *keep);
/* the predicates (bit i: preds[i] of the begin) the rows of the unit's point directory answer exactly (SIFT_COLO_POINT_ROWS) */
extern uint64 sift_col_point_exact(const SiftColUnit *unit);
/*
 * sift_col_filter for a source opened with SIFT_COLO_POINTS: a unit may come with the rows that a point directory lists for one predicate of the begin
 * (the rows whose fingerprint matches). Then sel is narrowed to them, that predicate is left out of the filter, and *used is true: the caller has the heap
 * check the condition again. A unit without such rows (a write buffer, a segment without a directory) is filtered by every predicate, *used false.
 */
extern void sift_col_point_narrow(const SiftColUnit *unit, SiftColSel *sel);
/* the predicates (bit i: preds[i] of the begin) the rows of the unit's point directory answer exactly (SIFT_COLO_POINT_ROWS) */
extern uint64 sift_col_point_exact(const SiftColUnit *unit);
extern bool sift_col_filter_points(const SiftColUnit *unit, const SiftColPred *preds, int npreds, SiftColSel *sel, bool *used);

/*
 * Wide leaves. A predicate on the head of a wide integer (two INT lanes) has constants whose two keys are v.i and .lo;
 * sift_col_filter evaluates it as the comparison of the pair, from ordinary tests of the lanes. A constant beyond the 128-bit range is clamped to
 * the largest or smallest 128-bit integer, which no value of a 38-digit column reaches. The head of a numeric image is an ordinary STR leaf whose
 * constants are images; IS NULL and IS NOT NULL on any head are the plain ones. sift_col_filter_lane is the plain test of one lane, constants in
 * v.i. sift_col_read_lanes reads the def->lanes lanes (vecs has room for SIFT_COL_MAX_LANES vectors) of the selected rows.
 */
extern bool sift_col_filter_lane(const SiftColUnit *unit, const SiftColPred *p, SiftColSel *sel);
extern void sift_col_read_lanes(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vecs);

/*
 * The conjunct of a query on a wide leaf as what the engine tests:
 * the operands are Datums of the leaf's type. op is 0..5 for = <> < <= > >= (the order of SiftCmpOp), SIFT_COLW_IN, SIFT_COLW_NOTIN, SIFT_COLW_ISNULL,
 * SIFT_COLW_NOTNULL. The verdict of the constants is NONE (no row passes), ALL (every row), NONNULL (every non-NULL row), NONNULL_NONAN (the same but the NaNs of
 * a numeric), NAN (the NaNs only) or PRED: the engine predicate `pred`, whose result for the NaNs (rows stored as overflow, which the engine
 * keeps as candidates of every predicate) is replaced by nan_passes. sift_colw_apply narrows sel by it and says whether any row is left.
 */
#define SIFT_COLW_IN		6
#define SIFT_COLW_NOTIN		7
#define SIFT_COLW_ISNULL	8
#define SIFT_COLW_NOTNULL	9

typedef enum SiftColWVerdict
{
	SIFT_COLW_NONE,
	SIFT_COLW_ALL,				/* every row, NULL ones too (x <> ALL of an empty list) */
	SIFT_COLW_NONNULL,
	SIFT_COLW_NONNULL_NONAN,
	SIFT_COLW_NAN,
	SIFT_COLW_PRED,
	SIFT_COLW_SHAPE				/* a containment operator on a range or a network: `shape` (col/shape.c) */
} SiftColWVerdict;

/*
 * The operators on the head of a range or a network (col/shape.c) as ops of sift_colw_test, with the head on the left and the operand (a range, an
 * element of its subtype, a network) on the right; the same numbers as SWT_* of agg/colsrc.h.
 */
#define SIFT_COLW_SHAPE_FIRST	16
#define SIFT_COLW_R_CONTAINS	16		/* @> a range */
#define SIFT_COLW_R_CONTAINED	17		/* <@ a range */
#define SIFT_COLW_R_OVERLAPS	18
#define SIFT_COLW_R_BEFORE		19		/* << */
#define SIFT_COLW_R_AFTER		20		/* >> */
#define SIFT_COLW_R_OVERLEFT	21		/* &< */
#define SIFT_COLW_R_OVERRIGHT	22		/* &> */
#define SIFT_COLW_R_ADJACENT	23		/* -|- */
#define SIFT_COLW_R_ELEM		24		/* @> an element */
#define SIFT_COLW_N_SUB			25		/* << a network */
#define SIFT_COLW_N_SUBEQ		26		/* <<= */
#define SIFT_COLW_N_SUP			27		/* >> */
#define SIFT_COLW_N_SUPEQ		28		/* >>= */
#define SIFT_COLW_N_OVERLAP		29		/* && */
#define SIFT_COLW_R_EQ			30		/* = a range (only the access method scan asks for it) */
#define SIFT_COLW_N_EQ			31		/* = a network: the node and the host bits of an IPv4 value, the rows of the node of an IPv6 one are decided by PostgreSQL's operator */
#define SIFT_COL_SHAPE_PREDS	4		/* the lane tests of one alternative of a shape test */

typedef struct SiftColShape SiftColShape;

typedef struct SiftColWTest
{
	SiftColWVerdict verdict;
	SiftColPred pred;
	bool		nan_passes;
	SiftColShape *shape;
} SiftColWTest;

extern void sift_colw_test(const SiftColDef *def, Oid typid, int op, int nvals, const Datum *vals, const bool *isnull, MemoryContext mcxt, SiftColWTest *t);
extern bool sift_colw_apply(const SiftColUnit *unit, const SiftColWTest *t, SiftColSel *sel);
/* The alternatives whose union a unit is pruned by for a containment test (each is the predicates, all true, of one way the test holds; 0: the test prunes nothing); the zone of a lane that holds an overflow keeps its unit */
extern int sift_colw_alts(const SiftColWTest *t, const SiftColPred *alt[2], int npred[2]);
/* The rows of sel that the lane col holds as overflows into out (sel->nwords words), the rows a test decides with PostgreSQL's operator */
extern void sift_colw_ovf_rows(const SiftColUnit *unit, int col, const SiftColSel *sel, uint64 *out);
extern SiftColSel *sift_colw_sel_copy(const SiftColUnit *unit, const SiftColSel *base);

extern int	sift_col_shape_of(Oid typid);
extern void sift_col_shape_values(const SiftColDef *def, Datum d, SiftColValue *v);
extern void sift_col_shape_test(const SiftColDef *def, Oid typid, int op, Datum operand, MemoryContext mcxt, SiftColWTest *t);
extern bool sift_col_shape_apply(const SiftColUnit *unit, const SiftColWTest *t, SiftColSel *sel);

/*
 * The JSON type of the value of the top level leaf col, as jsonb_typeof gives it for the path: a stored value has
 * the leaf's own type deftag (an astext leaf, mixed, holds the text of a string, a number or a boolean: STRING unless its chunk says
 * otherwise), an overflow value the tag the chunk kept for it, a NULL slot SIFT_COLTAG_JNULL for a JSON null and SIFT_COLTAG_NONE
 * (jsonb_typeof is NULL) for a missing key, and for a value that is no value at all.
 *
 * Narrows sel (rows of the unit) to those whose type is in tagmask (bit t for SIFT_COLTAG_t). *known is false when the chunk cannot say
 * the type of a row of sel: an overflow value without a tag, a value of a unit written before column format minor 4 (or by a writer that
 * did not know its type), or in such a unit a NULL slot / astext value where tagmask tells what that unit does not store: sel is then not
 * a proper answer. Returns whether any row is left.
 */
extern bool sift_col_filter_types(const SiftColUnit *unit, int col, uint32 tagmask, uint8 deftag, bool mixed, SiftColSel *sel, bool *known);

/* out[0..nrows) = the type of every row of the unit as above (0 where it is not known); false when some row's is not */
extern bool sift_col_row_types(const SiftColUnit *unit, int col, uint8 deftag, bool mixed, uint8 *out);

/*
 * The JSON nulls among the slots of the leaf col (a NULL slot whose chunk kept the kind SIFT_COLTAG_JNULL, which a missing key does not have):
 * bit k of bits is set for the k-th slot in the numbering of sift_col_read_idx (idx, m: the compacted rows of a match set, NULL: the unit as
 * stored). Returns the number of slots (SIFT_COL_NWORDS of them are written); *known is false when the unit was written without the kinds
 * and has a NULL slot, so that it cannot tell a JSON null from a missing key.
 */
extern uint32 sift_col_jnull_bits(const SiftColUnit *unit, int col, const uint32 *idx, uint32 m, uint64 *bits, bool *known);

/* The rows of the unit whose value of the top level leaf col overflowed, in bits (SIFT_COL_NWORDS(nrows) words); false, bits clear, when none did */
extern bool sift_col_ovf_bits(const SiftColUnit *unit, int col, uint64 *bits);

/* The overflow bits of the chunk of leaf col, *n slots (the rows of a top level leaf, the elements of the unit for a child leaf); NULL when the leaf holds no overflow value. Valid until the next unit */
extern const uint64 *sift_col_ovf_slots(const SiftColUnit *unit, int col, uint32 *n);

/* Array-element predicates with same-element semantics: keeps the rows with a satisfying element (or none, negate) */
extern bool sift_col_filter_group(const SiftColUnit *unit, const SiftColGroupPred *gp, SiftColSel *sel);

/* The elements of the unit's group that satisfy every predicate, for the rows of rowsel (NULL: all rows): elemsel has n = elements of the unit */

/* AND with the TID masks of a TIDBitmap (core/filter.c): a BM25 match, a btree or BRIN bitmap */
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
/*
 * A top-level STR leaf as strings (VAR: voff, vbytes; a NULL slot is empty): for few rows of a segment whose dictionary is stored plain and
 * not at hand, each string is read in place and the dictionary is not loaded.
 */
extern void sift_col_read_text(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vec);
/*
 * A top-level STR leaf of a stored unit as the codes of every row and no dictionary (vec->dict NULL), when its segment stores the dictionary in a form read in
 * place and the strings read so far are few against its size: the caller takes the few strings it needs with sift_col_code_str. False (vec untouched): read
 * the leaf with sift_col_read.
 */
extern bool sift_col_read_codes(const SiftColUnit *unit, int col, SiftColVec *vec);
/* The string of a code of a leaf read with sift_col_read_codes, into buf (SIFT_COL_DICT_MAX_ENTRY bytes); once the strings read in place are many, the dictionary is loaded (and cached) instead. Valid until the next call. */
extern const char *sift_col_code_str(const SiftColUnit *unit, int col, uint32 code, uint32 *len, char *buf);
/* The same for the rows idx[0..m) (ascending ordinals) a caller keeps: it saves the positions of the selection, which every leaf read would compute again */
extern void sift_col_read_idx(const SiftColUnit *unit, int col, const uint32 *idx, uint32 m, SiftColVec *vec);

/*
 * The whole leaf straight into the consumer's lane, with no intermediate int64 vector: I32 the int4/date/bool keys (and a group's element
 * counts per row), I64 the int8/timestamp/numeric keys, F64 the doubles of a float4 or float8 leaf, CODE the dictionary codes (info->dict is
 * their dictionary). The values are those of sift_col_read with NULL slots 0. dst has room for sift_col_lane_n() values of the lane's size
 * and SIFT_COL_PAD bytes more (decoders store whole vectors; what they write past the values is undefined); isnull, when given, receives n
 * 0/1 flags (and the same room), info->nulls says whether any is set (NULL isnull: not asked, the flags are not written). A leaf whose
 * chunk is absent reads as all NULL. info->ovf: some value overflowed the index, the lane holds filler there.
 */
typedef enum SiftColLane
{
	SIFT_COLL_I32,
	SIFT_COLL_I64,
	SIFT_COLL_F64,
	SIFT_COLL_CODE
} SiftColLane;

typedef struct SiftColLaneInfo
{
	uint32		n;
	bool		nulls;
	bool		ovf;
	SiftColDict	dict;			/* CODE */
} SiftColLaneInfo;

/* Values of leaf col in the unit: its rows, or for a child leaf the elements of the whole unit */
extern uint32 sift_col_lane_n(const SiftColUnit *unit, int col);
extern void sift_col_read_lane(const SiftColUnit *unit, int col, SiftColLane lane, void *dst, uint8 *isnull, SiftColLaneInfo *info);

/* Per element of the unit's group: the ordinal of its row (nelems entries) and the element count of every row (nrows entries, NULL array = 0) */
/* The element offsets of the rows of the unit's group (nrows + 1 entries), the ones the scan reads the elements by */
extern const uint32 *sift_col_group_offsets(const SiftColUnit *unit, int col);
extern void sift_col_group_parents(const SiftColUnit *unit, int group, const uint32 **parent, const uint32 **counts);

/* The TIDs of the rows of sel (room for sel->count), in row order */
extern void sift_col_tids(const SiftColUnit *unit, const SiftColSel *sel, ItemPointerData *tids);
/*
 * Where the rows of a unit that is not key-cut sit, without their keys: a copy of the unit's page directory (some 20 bytes a page), kept after the
 * unit is left, in which a TID is bisected to the ordinal of its row. NULL for a key-cut unit (its rows have to be decoded).
 */
typedef struct SiftColUnitIndex SiftColUnitIndex;
extern SiftColUnitIndex *sift_col_unit_index(const SiftColUnit *unit);
/* The ordinal of the row at key in the unit of superblock sb, PG_UINT32_MAX when the unit holds no row there (liveness is not looked at) */
extern uint32 sift_col_unit_index_find(const SiftColUnitIndex *ix, uint32 sb, SiftKey key);
/* The same for the rows ords (ascending ordinals of the unit), n of them: without decoding the keys of the other rows */
extern void sift_col_row_tids(const SiftColUnit *unit, const uint32 *ords, uint32 n, ItemPointerData *tids);

/*
 * The sorted run of leaf col in the unit: false when the unit has none (a unit of a write buffer, a leaf without the option, a unit written
 * before it). Positions [0, *nvals) of the run are the ordinals of the rows with a value, ascending by (key, ordinal), the next *nnull those of the NULL
 * rows ascending, the rest those of the rows whose value overflowed the leaf, ascending. sift_col_run_read gives the entries [pos, pos + cnt), each a row of the unit.
 * *id names the unit among those of the scan (the generation of its segment and its place in it), so that a scan can come back to it.
 */
/* One entry of the head of a sorted run (col/runs.h): the key of a row and its heap address (block << 16 | offset) */
typedef struct SiftCHeadEnt
{
	int64		key;
	uint64		addr;
} SiftCHeadEnt;
/* The head of the run found by sift_col_run_info: its H (0: the run has none), then nlo entries from the low end and nhi from the high end into out (2 H entries) */
extern bool sift_col_sb_heads(SiftColSource *src, uint32 sb, int col, SiftCHeadEnt *out, uint32 *nlo, uint32 *nhi);
extern int	sift_col_run_heads(const SiftColUnit *unit, int col, SiftCHeadEnt *out, uint32 *nlo, uint32 *nhi);
extern bool sift_col_run_info(const SiftColUnit *unit, int col, uint32 *nvals, uint32 *nnull, uint64 *id);
/* The order of the keys of leaf col in this unit in row order (sift_cc_order): 1 never decrease, -1 never increase, 2 unknown but a run of rows is cheap to decode, 0 neither. The NULL rows hold a filler that keeps the order */
extern int	sift_col_key_order(const SiftColUnit *unit, int col);
/* ... and whether that run is the rows in their own order (no entries stored) */
extern bool sift_col_run_identity(const SiftColUnit *unit, int col);
extern void sift_col_run_read(const SiftColUnit *unit, int col, uint32 pos, uint32 cnt, uint32 *ords);

/*
 * The zone of a leaf in the unit (no chunk read). sift_col_unit_exact: the zone describes exactly the rows a query without filters would
 * see, i.e. the unit is clean and every row is on an all-visible page: then count(col), sum, min and max need no chunk.
 */
extern void sift_col_zone(const SiftColUnit *unit, int col, SiftColZone *zone);
/* The zone of leaf col over every unit of the superblocks [sb_lo, sb_hi) as stored (a write buffer unit in the range makes it unbounded), from the segment headers: no unit is opened. Returns the units counted */
extern uint32 sift_col_zone_range(SiftColSource *src, int col, uint32 sb_lo, uint32 sb_hi, SiftColZone *zone);
extern uint32 sift_col_zone_range_segs(SiftColSource *src, int col, uint32 sb_lo, uint32 sb_hi, SiftColZone *zone);
typedef struct SiftColWalkStamp
{
	Oid			index;
	uint32		file;
	uint32		version;
} SiftColWalkStamp;
extern bool sift_col_walk_stamp(SiftColSource *src, SiftColWalkStamp *st, uint32 *mem, int cap, int *nmem);
extern bool sift_col_unit_exact(const SiftColUnit *unit);
/* Is every page of the unit all-visible: sift_col_vet has nothing to check */
extern bool sift_col_unit_allvisible(const SiftColUnit *unit);

extern const SiftColCounters *sift_col_counters(const SiftColSource *src);

/* An unsigned value as a varint at the end of out */
static inline void
sift_col_put_varint(StringInfo out, uint64 v)
{
	enlargeStringInfo(out, 10);
	out->len = (char *) sift_put_varint((uint8 *) out->data + out->len, v) - out->data;
	out->data[out->len] = '\0';
}

/* The index-bound declarations (relcache, planner expressions, shredder adapter, options, SQL entry points): col/pg.h, PostgreSQL only */
