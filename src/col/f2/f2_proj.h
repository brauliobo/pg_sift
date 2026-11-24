/*
 * col/f2/f2_proj.h - secondary projections of a sift_col index or a sift_table of format major 2 (core/f2/proj.h is the page format): the spec of a projection, the cube of a SIFT_PROJ_AGG
 * part, the rule that says which base parts a projection may stand for, and the steps that keep the projections in line with the parts of the relation.
 *
 * Nothing here knows the host: a column is its name, its SiftCellKind and a few facts, a part is its SiftPartDesc, and the answer to "may this base part be read from the projection" is a
 * function of the descriptor of the part, its link and what the host says of the visibility of the part (f2_proj_fresh.c). The host binds the two to a relation in f2_proj_build.c.
 */
#ifndef SIFT_F2_PROJ_H
#define SIFT_F2_PROJ_H

#include "core/host.h"

#include "col/f2/rowbuf.h"
#include "core/f2/manifest.h"
#include "core/f2/proj.h"

#define SIFT_PROJ_MAX_COLS		32
#define SIFT_PROJ_SPEC_MAX		1024

/* ---- the columns of the relation a spec names ---- */

typedef enum SiftProjTime
{
	SPT_NONE = 0,
	SPT_TIMESTAMP,				/* timestamp without time zone, microseconds since 2000-01-01: date_trunc is exact over it */
	SPT_DATE,					/* date, days since 2000-01-01: date_trunc to a day or coarser is exact, with no time zone in it */
	SPT_TIMESTAMPTZ				/* depends on the session time zone, so date_trunc is refused */
} SiftProjTime;

typedef struct SiftProjColumn
{
	const char *name;
	SiftCellKind kind;
	uint8		time;			/* SiftProjTime */
	uint8		scale;			/* a numeric: the digits after the point its cell holds as an integer */
	bool		numeric;
	bool		boolean;
	bool		usable;			/* false: a column of an array of objects or a key of a jsonb document, which a projection does not keep */
} SiftProjColumn;

typedef struct SiftProjColumns
{
	int			n;
	const SiftProjColumn *col;
} SiftProjColumns;

/* ---- the spec ---- */

typedef enum SiftProjFn
{
	SPF_COLUMN = 0,
	SPF_TRUNC					/* date_trunc(unit, column) of a timestamp */
} SiftProjFn;

typedef enum SiftProjUnit
{
	SPU_NONE = 0,
	SPU_MINUTE,
	SPU_HOUR,
	SPU_DAY,
	SPU_WEEK,
	SPU_MONTH,
	SPU_QUARTER,
	SPU_YEAR
} SiftProjUnit;

typedef enum SiftProjMeasureFn
{
	SPM_COUNT_STAR = 0,
	SPM_COUNT,
	SPM_SUM,
	SPM_AVG,
	SPM_MIN,
	SPM_MAX,
	SPM_COUNT_DISTINCT			/* no cell of its own: the distinct column is a group key of the part and its rows are the multiplicity */
} SiftProjMeasureFn;

typedef enum SiftProjOp
{
	SPO_LT = 0,
	SPO_LE,
	SPO_EQ,
	SPO_GE,
	SPO_GT,
	SPO_NE,
	SPO_IN,						/* any of the values; none: none */
	SPO_IS_NULL,				/* the cell is NULL and holds no exception tag */
	SPO_IS_NOT_NULL				/* the cell holds a value, and no exception tag */
} SiftProjOp;

#define SIFT_PROJ_PRED_VALS		4
#define SIFT_PROJ_PRED_BYTES	24
#define SIFT_PROJ_WHERE_MAX		8
#define SIFT_PROJ_FILTER_MAX	4

/* A predicate of a spec (WHERE of a projection, FILTER of a measure) with its constants held inline, so that a spec is a plain value */
typedef struct SiftProjSpecPred
{
	int16		leaf;
	uint8		op;				/* SiftProjOp */
	bool		bytes;			/* compared as bytes (text, uuid), else as integers */
	uint8		nvals;
	uint8		slen[SIFT_PROJ_PRED_VALS];
	int64		ival[SIFT_PROJ_PRED_VALS];
	char		sval[SIFT_PROJ_PRED_VALS][SIFT_PROJ_PRED_BYTES];
} SiftProjSpecPred;

typedef struct SiftProjKey
{
	int16		leaf;			/* the column of the relation */
	uint8		fn;				/* SiftProjFn */
	uint8		unit;			/* SiftProjUnit */
	bool		desc;			/* SORT only */
	bool		nulls_first;
} SiftProjKey;

typedef struct SiftProjMeasure
{
	uint8		fn;				/* SiftProjMeasureFn */
	int16		leaf;			/* -1 for count(*) */
	int			nfilter;		/* FILTER (WHERE ..): the predicates, all of which must hold */
	SiftProjSpecPred filter[SIFT_PROJ_FILTER_MAX];
} SiftProjMeasure;

typedef struct SiftProjSpec
{
	uint8		kind;			/* SiftProjKind */
	int			nkeys;			/* the sort columns, or the group columns */
	SiftProjKey key[SIFT_PROJ_MAX_COLS];
	bool		all_columns;	/* SORT without INCLUDE: every usable column */
	int			ninclude;		/* SORT: the columns kept besides the sort columns when not all_columns */
	int16		include[SIFT_PROJ_MAX_COLS];
	int			nmeas;			/* AGG */
	SiftProjMeasure meas[SIFT_PROJ_MAX_COLS];
	int			nwhere;			/* WHERE: the rows the projection keeps, the predicates all of which must hold */
	SiftProjSpecPred where[SIFT_PROJ_WHERE_MAX];
	int16		distinct_leaf;	/* AGG: the column count(DISTINCT) is of, kept as the last group key; -1: none */
} SiftProjSpec;

/* false with the reason in err when the text is not a spec of a projection of these columns */
extern bool sift_proj_spec_parse(const char *text, const SiftProjColumns *cols, SiftProjSpec *out, char *err, size_t errlen);

/* The one text a spec has (keywords in capitals, names quoted when they need it), and its CRC32C: the spec of two projections is the same iff their texts are */
extern void sift_proj_spec_canonical(const SiftProjSpec *s, const SiftProjColumns *cols, char out[SIFT_PROJ_SPEC_MAX]);
extern uint32 sift_proj_spec_hash(const char *canonical);

/* The two predicates are the same test (the same column, operator and constants) */
extern bool sift_proj_spec_pred_equal(const SiftProjSpecPred *a, const SiftProjSpecPred *b);

/* The flags of the directory's ref (SIFT_PROJREF_FLAG_*) the spec calls for */
extern uint32 sift_proj_spec_flags(const SiftProjSpec *s);

/* A dead row of a part can be subtracted from the cells of an AGG projection: it holds no minimum and no maximum */
extern bool sift_proj_spec_invertible(const SiftProjSpec *s);

/* date_trunc of a timestamp (microseconds since 2000-01-01) to the unit; the units a coarser unit can be made of by truncating again */
extern int64 sift_proj_trunc(int64 ts, SiftProjUnit unit);

/* The same of a date (days since 2000-01-01) to day or coarser; sift_proj_trunc_cell takes the cell of the time kind */
extern int64 sift_proj_trunc_date(int64 days, SiftProjUnit unit);
extern int64 sift_proj_trunc_cell(SiftProjTime time, int64 v, SiftProjUnit unit);
extern bool sift_proj_unit_refines(SiftProjUnit fine, SiftProjUnit coarse);

/* ---- the columns of the parts of a projection ---- */

typedef enum SiftProjRole
{
	SPR_KEY = 0,				/* an AGG group column, or a SORT column */
	SPR_INCLUDE,				/* a SORT column kept besides the sort columns */
	SPR_ROWS,					/* the rows of the group: hidden count(*), always the first column after the keys */
	SPR_COUNT,					/* measure state */
	SPR_SUM_HI,
	SPR_SUM_LO,
	SPR_SUM_NN,					/* the values the sum is of: its NULL when none */
	SPR_MIN,
	SPR_MAX
} SiftProjRole;

typedef struct SiftProjLayoutCol
{
	uint8		role;			/* SiftProjRole */
	SiftCellKind kind;
	int16		leaf;			/* the column of the relation the column derives from */
	int16		measure;		/* the measure of the spec, -1 for none */
} SiftProjLayoutCol;

/* The columns of a part of the projection in order; returns their number (at most 3 * SIFT_PROJ_MAX_COLS + SIFT_PROJ_MAX_COLS) */
#define SIFT_PROJ_LAYOUT_MAX	(4 * SIFT_PROJ_MAX_COLS + 2)
extern int	sift_proj_layout(const SiftProjSpec *s, const SiftProjColumns *cols, SiftProjLayoutCol *out);

/* ---- the cube of a SIFT_PROJ_AGG part ---- */

typedef struct SiftProjMState
{
	int64		cnt;			/* count(x), or the values a sum, avg, min or max is of */
	int128		sum;
	bool		has;			/* min and max: a value was seen */
	int64		mi;
	double		mf;
} SiftProjMState;

typedef struct SiftProjGroup
{
	SiftCell   *key;
	int64		rows;
	SiftProjMState *st;
} SiftProjGroup;

typedef struct SiftProjCube SiftProjCube;

/* The predicates of a spec (a WHERE, a FILTER) hold on the row of the relation: a NULL cell passes none but IS NULL */
extern bool sift_proj_spec_preds_pass(const SiftProjSpecPred *p, int n, const SiftCell *row);
extern bool sift_proj_spec_preds_decidable(const SiftProjSpecPred *p, int n, const SiftCell *row);

extern SiftProjCube *sift_proj_cube_new(const SiftProjSpec *s, const SiftProjColumns *cols);
extern void sift_proj_cube_free(SiftProjCube *c);

/* A row of the relation (one cell per column) added (sign 1) or taken away (sign -1: the group goes when its rows are 0); false, nothing changed, when a row cannot be taken away */
extern bool sift_proj_cube_add(SiftProjCube *c, const SiftCell *row, int sign);

/* The groups the cube holds now, those whose rows were all taken away included */
extern int	sift_proj_cube_size(const SiftProjCube *c);

/* The group of the key (one cell for each group column of the cube), made with no rows when it is not there: a count(DISTINCT) is added to the groups it is of */
extern SiftProjGroup *sift_proj_cube_touch(SiftProjCube *c, const SiftCell *key);

/* The cube of a projection part read back into a cube of a query that groups by fewer columns or coarser ones (the rollup): one stored row, in the layout of the part */
typedef struct SiftProjRollup
{
	int			nkeys;			/* of the query */
	int			src_key[SIFT_PROJ_MAX_COLS];	/* the group column of the projection each key comes from */
	uint8		unit[SIFT_PROJ_MAX_COLS];	/* SPU_NONE, or the unit the stored value is truncated to again */
	int			nmeas;
	int			src_meas[SIFT_PROJ_MAX_COLS];	/* the measure of the projection each measure comes from */
	uint8		filter_mode[SIFT_PROJ_MAX_COLS];	/* SiftProjFilterMode of each measure */
	uint8		pred_use[SIFT_PROJ_MAX_COLS];	/* SiftProjPredUse of each predicate of the query, on a stored row */
	uint8		pred_op[SIFT_PROJ_MAX_COLS];	/* SPPU_MAPPED: the operator and bound the bucket value is tested with */
	int64		pred_val[SIFT_PROJ_MAX_COLS];
} SiftProjRollup;

/* A predicate of the query on a stored row: as it is, as its bound on the edge of a bucket says (a column kept as date_trunc), or not at all (the projection's WHERE says it for every row it keeps) */
typedef enum SiftProjPredUse
{
	SPPU_AS_IS = 0,
	SPPU_MAPPED,
	SPPU_SKIP
} SiftProjPredUse;

/* How a FILTER of a query measure is met: not at all (none), by the source measure that holds the same filter (baked), or by testing the group columns of every stored row (on_key) */
typedef enum SiftProjFilterMode
{
	SPFM_NONE = 0,
	SPFM_BAKED,
	SPFM_ON_KEY
} SiftProjFilterMode;

extern void sift_proj_cube_absorb(SiftProjCube *c, const SiftProjSpec *src, const SiftProjLayoutCol *layout, int nlayout, const SiftProjRollup *map, const SiftCell *stored);

/* The groups with rows (those whose rows were all taken away are gone); the first call after a change settles the cube */
extern int	sift_proj_cube_ngroups(SiftProjCube *c);
extern const SiftProjGroup *sift_proj_cube_group(SiftProjCube *c, int i);

/* The groups as the rows of a part, by their group columns ascending, nulls last: *out is rows * *ncols cells in the order of sift_proj_layout, owned by the cube; returns the rows */
extern int	sift_proj_cube_rows(SiftProjCube *c, const SiftCell **out, int *ncols);

/* ---- which base parts a projection stands for ---- */

typedef enum SiftProjServe
{
	SPS_BASE = 0,				/* read the base part */
	SPS_EXACT,					/* read the projection part: it is what the base part holds */
	SPS_OVERLAY					/* read the projection part and meet what has happened to the base part since: SORT drops the dead addresses, AGG takes the dead rows away once */
} SiftProjServe;

typedef enum SiftProjWhy
{
	SPW_SERVED = 0,
	SPW_NOT_READY,				/* the projection is being built, or its definition no longer fits the relation */
	SPW_NO_LINK,				/* the part was written after the projection was last refreshed (a flush, a compaction): the next maintenance links it */
	SPW_NOT_SETTLED,			/* the inserters are not all committed and older than every snapshot, or the host sees rows of the part that it cannot account for */
	SPW_DV_CHANGED,				/* a deletion vector the projection part was not built from, and the overlay cannot meet it */
	SPW_COL_DELTA,				/* the part has column patches on a column the projection reads (sift_table) */
	SPW_PEND_DEL,				/* a delete the overlay cannot meet: a pending one, or a changed heap block, against an AGG part that holds a minimum or maximum */
	SPW_WHERE_EXC				/* a row of the part held an exception tag on a column of the WHERE or a FILTER of the projection */
} SiftProjWhy;

/* What the host says of the visibility of a base part for the snapshot (col/f2/part_vis.h: sift_vis_part_class for a sift_table, the heap's visibility map over the part's addresses for a sift_col) */
typedef enum SiftProjHostVis
{
	SPV_UNSETTLED = 0,			/* the inserters are not settled, or too many heap blocks of the part are not all-visible for an AGG overlay */
	SPV_CLEAN,					/* every row visible, none dead */
	SPV_SETTLED_DV,				/* every row visible, the rows of the deletion vector dead outright */
	SPV_LIVE_DV,				/* the inserters are settled; the deletion vector is not (or a delta tail may grow it), and/or some heap blocks are not all-visible */
	SPV_SORT_INS_SETTLED		/* a sift_col SORT part: the inserters are settled; the heap decides every output row */
} SiftProjHostVis;

#define SIFT_PROJ_MAX_DIRTY_VM_BLKS		64			/* heap blocks of a part that may be not all-visible for an AGG overlay */
#define SIFT_PROJ_OVERLAY_DEAD_DENOM	16			/* the dead rows of a part an overlay meets: at most 1/16 of its rows */
#define SIFT_PROJ_SORT_DV_OVERLAY_MAX	4096		/* the dead addresses a SORT overlay of one part holds */

/* The facts of one base part the decision is made of; the host has them without reading a page of the part */
typedef struct SiftProjServeIn
{
	const SiftManifestProjRef *ref;
	const SiftPartDesc *base;
	const SiftProjLink *link;	/* the projection's link for the part, or NULL */
	SiftProjHostVis vis;
	bool		part_has_pend;	/* a pending delete of the partition the snapshot sees may name a row of this part: its address range meets the part's */
	uint64		col_delta_bloom;	/* the attributes the column patches of the partition touch, as a bloom (table/f2/f2_col_delta.h: sift_cd_att_bit) */
	uint64		proj_att_bloom;	/* the attributes the projection reads (keys, measures, WHERE, FILTER, INCLUDE), in the same bloom */
} SiftProjServeIn;

typedef struct SiftProjPartServe
{
	uint8		serve;			/* SiftProjServe */
	uint8		why;			/* SiftProjWhy */
	uint64		proj_gen;		/* the projection part, when served */
} SiftProjPartServe;

/* The decision for one base part (col/f2/f2_proj_overlay.c) */
extern SiftProjPartServe sift_proj_serve_part(const SiftProjServeIn *in);

/* Whether the deletion vector of the part has taken in rows since the link: it was replaced, or it is still open to a delta tail that does not change its generation */
extern bool sift_proj_dv_unchanged(const SiftPartDesc *base, const SiftProjLink *link);

/* The columns a spec reads: used[c] for each of the cols->n columns */
extern void sift_proj_spec_uses(const SiftProjSpec *s, const SiftProjColumns *cols, bool *used);

/* The link of a projection part written from the base part as the descriptor shows it: the state of its deletion vector the cells reflect */
extern SiftProjLink sift_proj_link_make(const SiftManifestProjRef *ref, const SiftProjSpec *spec, const SiftPartDesc *base, uint64 proj_gen, bool where_exc);

/* The link of a base part among the links of a projection (ascending by src_gen), or NULL */
extern const SiftProjLink *sift_proj_link_of(const SiftProjParts *p, uint64 src_gen);

/* Whether a base part may be projected now: sealed for its inserters, no column patches, and its deletion vector, when it has one, settled */
extern bool sift_proj_part_projectable(const SiftPartDesc *base);

/* ---- keeping the projection in line (f2_proj_build.c) ---- */

typedef struct SiftProjRefreshPlan
{
	int			nbuild;			/* base parts to project: no link, or a link of another deletion vector that the projection cannot take rows away for, and projectable */
	uint64	   *build_gens;
	int			ndrop;			/* projection parts the new ones replace; the parts of base parts that are gone leave with the commit that dropped those (core/f2/manifest/projdir.c) */
	uint64	   *drop_proj_gens;
	uint64	   *drop_src_gens;
} SiftProjRefreshPlan;

/* What a refresh of the base parts given has to write: the base parts of one partition, with the projection's parts and links */
extern SiftProjRefreshPlan sift_proj_refresh_plan(const SiftManifestProjRef *ref, const SiftPartDesc *base, int nbase, const SiftProjParts *p);
extern void sift_proj_refresh_plan_free(SiftProjRefreshPlan *plan);

/* ---- the host (f2_proj_build.c, f2_proj_sql.c) ---- */

struct SiftF2Store;

/* The columns of the store as a projection spec names them: by the names of the attributes of the relation (palloc'd) */
extern SiftProjColumns sift_proj_columns_of_store(Relation rel, const struct SiftF2Store *store);

/* The shape of the parts of a projection: the columns, their kinds, the sort columns, as the part writer takes them */
typedef struct SiftProjPartSpec
{
	SiftPartSpec spec;
	int			nlay;
	SiftProjLayoutCol lay[SIFT_PROJ_LAYOUT_MAX];
	SiftCellKind kinds[SIFT_PROJ_LAYOUT_MAX];
	uint8		hints[SIFT_PROJ_LAYOUT_MAX];
	uint8		scales[SIFT_PROJ_LAYOUT_MAX];
	uint16		sort[SIFT_KEY_ORDER_MAX];
} SiftProjPartSpec;

extern void sift_proj_part_spec(const struct SiftF2Store *store, const SiftProjSpec *s, const SiftProjColumns *cols, SiftProjPartSpec *out);

/* A definition of sift.projection_defs of the relation, parsed against its columns; spec_ok: it parses; hash: of its canonical text, which a projection of the directory must carry to be built or read */
typedef struct SiftProjDef
{
	char		name[SIFT_PROJ_NAME_LEN];
	SiftProjSpec spec;
	bool		spec_ok;
	uint32		hash;
} SiftProjDef;

extern SiftProjDef *sift_proj_defs_load(Relation rel, const SiftProjColumns *cols, int *n);

/* The rows of a part, granule by granule, as fn takes them: the live rows of its deletion vector, or with dead only the dead ones (the rows an AGG part still counts after the base part was given a deletion vector) */
typedef void (*SiftProjRowFn) (void *arg, const SiftRowBuf *rows, int r);
extern void sift_proj_scan_part(Relation rel, const SiftPartDesc *d, uint32 part_id, const SiftPartSpec *spec, bool dead, SiftProjRowFn fn, void *arg);

/* The rows of one granule of a part that the mask (a bit for each row, in the order of the granule) names, dead or live: what an overlay takes away */
struct SiftPart;
extern void sift_proj_scan_granule(Relation rel, const struct SiftPart *part, uint32 g, const SiftPartSpec *spec, const uint64 *mask, SiftProjRowFn fn, void *arg);

/* The same rows one at a time (f2_proj_scan.c): the row in hand is valid until the next call */
typedef struct SiftProjCursor SiftProjCursor;

extern SiftProjCursor *sift_proj_cursor_open(Relation rel, const SiftPartDesc *d, uint32 part_id, const SiftPartSpec *spec, bool dead_only);
extern bool sift_proj_cursor_next(SiftProjCursor *c, const SiftRowBuf **rows, int *r);
extern void sift_proj_cursor_close(SiftProjCursor *c);

/*
 * Before the first row: the cursor reads only the granules the key marks (SEC_KEY_MARKS) of the part leave to the plan, found by binary search over the marks. false: the part has no marks and is read whole.
 * The key spec and the plan come from col/f2/f2_v6_proj_seek.h.
 */
struct SiftKeySpec;
struct SiftKeyPlan;
extern bool sift_proj_cursor_seek(SiftProjCursor *c, const struct SiftKeySpec *ks, const struct SiftKeyPlan *plan);

/*
 * Before the first row: of the rows of a granule only the hull of those whose first sort column (an integer) is in [lo, hi], or holds an exception tag, are made as cells; the others are not rows the
 * cursor gives. The rows are tested on their cells as before, so the answer never depends on the window.
 */
extern void sift_proj_cursor_window(SiftProjCursor *c, int64 lo, int64 hi);

/* Before the first row: the columns of the spec the caller reads (one flag each, which must outlive the cursor); the cells of the others are NULL */
extern void sift_proj_cursor_columns(SiftProjCursor *c, const bool *want);

/* Before the first row: the cursor decodes a granule a window of rows at a time, the rows of a window at their own values, so that a merge that stops early reads a few rows of each part */
extern void sift_proj_cursor_progressive(SiftProjCursor *c);

/* Granules the cursor left out and granules it has read */
extern void sift_proj_cursor_stats(const SiftProjCursor *c, uint32 *skipped, uint32 *read);

/* Builds the projection parts of every base part that has no link and may be projected, in one commit for each partition, and makes the projections READY; returns the parts built */
extern int	sift_proj_refresh(Relation rel, const struct SiftF2Store *store);

/*
 * The part of generation proj_gen of a projection is copied as it is (no row decoded) into the blocks the caller prefers (sift_part_prefer_extents) and the directory points at the copy; false
 * when the part is gone, or a commit that dropped it or its base part came first: the directory is not changed then and the copy is released. sift_proj_move_dir rewrites the pages of the
 * projection's directory and of the refs, into the same preferred blocks (a page of a directory is taken from them first).
 */
extern bool sift_proj_move_part(Relation rel, uint16 proj_id, uint64 proj_gen);
extern bool sift_proj_move_dir(Relation rel, uint16 proj_id);

/* The projection of the spec created in the relation (the ref BUILDING, then refreshed to READY), and dropped */
extern void sift_proj_create(Relation rel, const struct SiftF2Store *store, const char *name, const SiftProjSpec *spec, const SiftProjColumns *cols, const char *canonical);
extern bool sift_proj_drop(Relation rel, const char *name);

/* True when the relation has projections (the feature bit of block 0) */
extern bool sift_proj_relation_has(Relation rel);

#endif							/* SIFT_F2_PROJ_H */
