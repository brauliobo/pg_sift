/*
 * agg/v6/v6.h - SiftAggV6, the aggregate scan of an index of format major 2: the rows the scan of col/f2/f2_scan.c accepts (its pruning, its filters on the compressed
 * chunks, its deletion vectors and pending deletes) are vetted against the heap in batches, decoded to the vectors of agg/vec.h and folded into the groups of agg/group/ht.h
 * by the kernels of agg/kern6.h; one tuple leaves for each group. The planner side adds the path for a single table whose sift_col index of format major 2 holds every
 * column the query reads.
 */
#ifndef SIFT_AGG_V6_H
#define SIFT_AGG_V6_H

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "access/skey.h"
#include "executor/executor.h"
#include "utils/rel.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

#include "agg/vec.h"
#include "core/tidbridge.h"
#include "col/f2/f2.h"

typedef enum SiftV6Kind
{
	SV6_COUNT_STAR,
	SV6_COUNT,
	SV6_SUM,
	SV6_AVG,
	SV6_MIN,
	SV6_MAX,
	SV6_COUNT_DISTINCT,
	SV6_GROUPING,				/* GROUPING(keys): arg.attno is the index of its SiftV6Func */
	SV6_ARG_MAX,				/* sift.arg_max(val, cmp): arg is val, arg2 is cmp */
	SV6_ARG_MIN,
	SV6_STDDEV_POP,				/* of a float column: the state of PostgreSQL's float8_accum (group_ext.h) */
	SV6_STDDEV_SAMP,
	SV6_VAR_POP,
	SV6_VAR_SAMP,
	SV6_BOOL_OR,
	SV6_BOOL_AND
} SiftV6Kind;

#define SIFT_V6_MAX_KEYS	32
#define SIFT_V6_MAX_DCOLS	8		/* the columns of a COUNT(DISTINCT (a, b, ..)) */
#define SIFT_V6_MAX_FILTERS	64		/* the distinct FILTER (WHERE ..) clauses of a statement: one bit of a row's mask each (v6_filter_pivot.h) */
#define SIFT_V6_MAX_FCOLS	128		/* the columns they read */

static inline bool
sift_v6_kind_reads_column(SiftV6Kind kind)
{
	return kind != SV6_COUNT_STAR && kind != SV6_GROUPING;
}

/* The physical class of a column the scan reads: how it decodes (SiftVecType) and what the group makes of it */
typedef enum SiftV6Class
{
	SV6C_NONE = 0,
	SV6C_I32,					/* int2, int4, date */
	SV6C_I64,					/* int8, timestamp, timestamptz */
	SV6C_F64,					/* float4, float8 */
	SV6C_BOOL,
	SV6C_TEXT					/* text, varchar */
} SiftV6Class;

static inline SiftV6Class
sift_v6_class(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return SV6C_I32;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case NUMERICOID:		/* a numeric of the INT class: its scaled integer; an argument of an aggregate only */
			return SV6C_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return SV6C_F64;
		case BOOLOID:
			return SV6C_BOOL;
		case TEXTOID:
		case VARCHAROID:
			return SV6C_TEXT;
		default:
			return SV6C_NONE;
	}
}

/* Where the values of a column of the scan come from */
typedef enum SiftV6Src
{
	SV6SRC_ATTR = 0,			/* a leaf of the index: the first one of attno, or the one jleaf names */
	SV6SRC_DIM,					/* column slot of the dimension dim of a star join */
	SV6SRC_ELEM					/* the element of the unnested array of the index */
} SiftV6Src;

/* SiftV6Col.attno of sift.score(ctid), the score of the row's hit: no column of the index, a float4 the rows of a fold come with (sift_v6_fold_batch_scored) */
#define SIFT_V6_SCORE_ATTNO		(-1)

typedef struct SiftV6Col
{
	int			attno;			/* of the index, 1-based; of the table, when the spec is_table */
	Oid			typid;
	int			scale;			/* numeric: the digits after the point of the scaled integer the leaf holds; 0 for any other type */
	int			jleaf;			/* 1 + the leaf of a key of the jsonb attribute whose `->>` text (typid text) or float8 cast the column is; 0: the first leaf of attno */
	SiftV6Src	src;
	int			dim;			/* SV6SRC_DIM: the dimension */
	int			slot;			/* SV6SRC_DIM: the payload column of the dimension */
} SiftV6Col;

struct SiftV6Ext;

typedef struct SiftV6Agg
{
	SiftV6Kind	kind;
	SiftV6Col	arg;			/* attno 0 for count(*) */
	SiftV6Col	arg2;			/* sift.arg_max, sift.arg_min: the column the order is of; attno 0 for the others */
	int			nxcols;			/* COUNT(DISTINCT (a, b, ..)): the columns after arg, which is the first */
	SiftV6Col	xcol[SIFT_V6_MAX_DCOLS - 1];
	int			fclause;		/* FILTER (WHERE ..): 1 + the number of its clause in SiftV6Spec.filter, 0 for none */
	Oid			restype;
} SiftV6Agg;

/* A numeric NaN is not NULL: these aggregates fold it. It counts, and makes a sum or an average NaN, a maximum NaN and a minimum NaN when no other row has a value, and is one more distinct value */
static inline bool
sift_v6_agg_takes_nan(const SiftV6Agg *a)
{
	return a->kind == SV6_COUNT || a->kind == SV6_SUM || a->kind == SV6_AVG || a->kind == SV6_MIN || a->kind == SV6_MAX || (a->kind == SV6_COUNT_DISTINCT && a->nxcols == 0);
}

/* The keys (indexes into SiftV6Spec.key) the arguments of a GROUPING() name */
typedef struct SiftV6Func
{
	int			nargs;
	int			key[SIFT_V6_MAX_KEYS];
} SiftV6Func;

/* A key that is date_bin(stride, column, origin) of a timestamp or timestamptz column: the key's column is the source, and the scan bins it (v6_datebin_win.h) */
typedef struct SiftV6Bin
{
	int			key;			/* the index into SiftV6Spec.key */
	int64		stride_us;
	int64		origin;
} SiftV6Bin;

typedef struct SiftV6Spec
{
	Oid			relid;			/* what the scan reads: the sift_col index, or the sift_table when is_table */
	bool		is_table;
	int			nparts;			/* a partitioned table: the index of each live partition, read into the one set of groups (indexoid is the first); 0 for one index */
	Oid		   *part_index;
	int			nkeys;
	SiftV6Col  *key;
	int			naggs;
	SiftV6Agg  *agg;
	int			nsets;			/* GROUPING SETS, ROLLUP, CUBE: the sets as masks over the keys; 0 for a plain GROUP BY */
	uint32	   *set_mask;
	int			nfuncs;
	SiftV6Func *func;
	List	   *indexqual;		/* the quals of the scan as an index scan has them: clauses on INDEX_VAR attributes */
	int			nfilters;
	List	   *filter;			/* of each distinct FILTER clause, the list of its quals in the form of indexqual */
	int			nfcols;
	SiftV6Col	fcol[SIFT_V6_MAX_FCOLS];	/* the columns the clauses read, after the columns of the aggregates in the vectors of a batch */
	int			nbins;
	SiftV6Bin	bin[SIFT_V6_MAX_KEYS];
	List	   *jsonqual;		/* the quals on the keys of jsonb documents the clauses of an operator class cannot say (agg/v6/jsonb_qual.c) */
	struct SiftV6Ext *ext;		/* the dimensions of a star join or the array an unnest takes apart (v6_ext.h); NULL for the scan of the columns of one table */
	Oid			text_indexoid;	/* a sift index whose text query (a Const, text_query) keeps the rows: its hits go to the scan as bridges (v6_text.c); InvalidOid without one */
	Const	   *text_query;
	int			par_strategy;	/* SiftV6ParStrategy of the plan (0: serial) */
	int			par_workers;
} SiftV6Spec;

/*
 * The vectors of a batch: argvec[j] is the argument of aggregate j, argvec[naggs + j] its second argument (sift.arg_max, sift.arg_min), and then the columns after the first of each
 * COUNT(DISTINCT (a, b, ..)), the ones of aggregate j from sift_v6_agg_xvec.
 */
static inline int
sift_v6_agg_xvec(const SiftV6Spec *s, int j)
{
	int			at = 2 * s->naggs;

	for (int i = 0; i < j; i++)
		at += s->agg[i].nxcols;
	return at;
}

/* and then the columns of the FILTER clauses, fcol[i] at sift_v6_spec_fvec + i */
static inline int
sift_v6_spec_fvec(const SiftV6Spec *s)
{
	return sift_v6_agg_xvec(s, s->naggs);
}

static inline int
sift_v6_spec_nvec(const SiftV6Spec *s)
{
	return sift_v6_spec_fvec(s) + s->nfcols;
}

/* An aggregate over sift.score(ctid): the rows of the fold have to come with their scores */
static inline bool
sift_v6_spec_reads_score(const SiftV6Spec *s)
{
	for (int j = 0; j < s->naggs; j++)
		if (sift_v6_kind_reads_column(s->agg[j].kind) && s->agg[j].arg.attno == SIFT_V6_SCORE_ATTNO)
			return true;
	return false;
}

/* What the planner side tells of the relation a jsonb qual or read is bound against */
typedef struct SiftV6Rel
{
	Index		relid;
	const IndexOptInfo *index;
	const SiftColRel *colrel;
	const SiftF2Rel *f2;
} SiftV6Rel;

/*
 * agg/v6/jsonb_qual.c: `doc ? 'k'`, jsonb_exists(doc, 'k') and jsonb_typeof(doc -> 'k') = 'type' (or IN a list of types) over a document of the index whose key is a leaf, as the element of
 * SiftV6Spec.jsonqual the scan keys are made from; and `doc ->> 'k'` and `(doc ->> 'k')::float8` as a column of the scan. NULL / false when the leaf does not hold what the query asks.
 */
extern List *sift_v6_json_qual(const SiftV6Rel *rel, const Expr *clause);
extern bool sift_v6_json_read(const SiftV6Rel *rel, const Expr *e, SiftV6Col *out);
/* keys and nkeys with the scan keys of spec->jsonqual appended */
extern void sift_v6_json_scankeys(const SiftV6Spec *spec, Relation index, ScanKey *keys, int *nkeys);
/* The value of a row whose leaf could not hold it, from the payload of its exception (a jsonb datum): the `->>` text, or its float8 cast; false: NULL */
extern bool sift_v6_json_exception(SiftV6Class cls, const char *payload, uint32 len, MemoryContext cx, const char **text, uint32 *tlen, double *f);

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_plan_methods;

extern void sift_agg_v6_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
extern Plan *sift_agg_v6_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);
/*
 * The aggregate of the statement over the columns of index ix (open as irel) of rel, without its WHERE quals, for a scan that brings its own rows (agg/hybrid_plan.c): the keys and aggregates in
 * *spec, the entries of the rows the aggregate leaves in *scan_tlist (the keys, then the aggregates), the grouping expressions in *group_exprs; false when the index does not hold what the statement reads.
 * extra is the GroupPathExtraData of the upper path. sift_v6_plan_shape says whether the statement is of the shape the aggregate scan serves (one table, no windows or DISTINCT ON).
 */
extern bool sift_v6_compile_aggregate(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, IndexOptInfo *ix, Relation irel, void *extra, SiftV6Spec *spec, List **scan_tlist, List **group_exprs);
extern bool sift_v6_plan_shape(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);
/* The index holds attribute att (1-based), of the class, in one plain leaf the scan decodes as it is stored */
extern bool sift_v6_leaf_holds(const SiftColRel *r, int att, SiftV6Class cls);
/* The same for column att of a sift_table (1-based) */
struct SiftF2Store;
extern bool sift_v6_table_leaf_holds(const struct SiftF2Store *store, int att, SiftV6Class cls);
extern bool sift_v6_agg_kind(const Aggref *a, SiftV6Kind *kind);
/* The aggregate of the facts of a join as a scan of the index (the tuple of its scan tlist): false when a key, an aggregate or a qual of the facts is not one the scan decides (scope_v6.c adds the members of a scope key) */
extern bool sift_v6_compile_fact(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *out, IndexOptInfo *ix, GroupPathExtraData *extra, SiftV6Spec *spec, List **scan_tlist);
/* stddev, variance, bool_or and bool_and by name and argument type (group_ext.c) */
extern bool sift_v6_ext_kind(const char *name, Oid argtype, SiftV6Kind *kind);
extern List *sift_v6_spec_pack(const SiftV6Spec *s);
extern SiftV6Spec *sift_v6_spec_unpack(const List *l);

/* The scope aggregate (agg/v6/scope_v6.c): a join of the facts with the pairs of a scope of the index is answered by the cubes of its summary (col/f2/f2_scope.h) */
extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_scope_plan_methods;

extern void sift_agg_v6_scope_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
/* ORDER BY ring leaf DESC LIMIT n of the facts of a scope key: the newest rows of its ring (ORDERED stage) */
extern void sift_agg_v6_scope_topk_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);
extern Plan *sift_agg_v6_scope_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

/* The groups: keys and aggregates folded from batches, then every group once */
typedef struct SiftV6Group SiftV6Group;

/* desc is the tuple the groups leave as: its attributes are the keys then the aggregates */
extern SiftV6Group *sift_v6_group_new(const SiftV6Spec *spec, MemoryContext cx, TupleDesc desc);
/* keyvec[i] is key i and argvec[j] the argument of aggregate j (NULL for count(*)); the rows are those of bs */
extern void sift_v6_group_update(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec);
/* Rows that no key or filter tells apart, for a statement of count(*) alone (sift_v6_spec_count_only) */
extern bool sift_v6_spec_count_only(const SiftV6Spec *spec);
extern void sift_v6_group_add_rows(SiftV6Group *g, uint64 n);
extern void sift_v6_group_finish(SiftV6Group *g);
/* The next group as the Datums of the keys then the aggregates; false after the last */
extern bool sift_v6_group_next(SiftV6Group *g, Datum *values, bool *isnull);
extern uint64 sift_v6_group_bytes(const SiftV6Group *g);
extern void sift_v6_group_spill_stats(const SiftV6Group *g, uint64 *puts, uint64 *bytes);
extern void sift_v6_group_free(SiftV6Group *g);
/* A row of a COUNT(DISTINCT) of a participant of a parallel run: the bytes of its key (the group and the value, or the value of a set of no keys) */
typedef void (*SiftV6DistPut) (void *arg, uint32 set_idx, uint32 slot, const void *bytes, uint32 len);
extern void sift_v6_group_scan_sink(SiftV6Group *g, SiftV6DistPut put, void *arg);
extern void sift_v6_group_recycle(SiftV6Group *g);
extern Datum sift_v6_i128_numeric(SiftI128 v);
/* v * 10^-scale as the numeric PostgreSQL makes of the digits: the scale is its display scale */
extern Datum sift_v6_scaled_numeric(SiftI128 v, int scale);
extern const char *sift_v6_group_mode(const SiftV6Group *g);

/* The scan: every batch of rows of the index of the spec goes to the group (or to the function of the caller) */
typedef struct SiftV6ScanStats
{
	uint64		batches;
	uint64		rows;
	uint64		heap_vet_rows;
	uint64		heap_rows_dead;
	uint64		chunks;			/* passes of a scan that takes the hits of a text query as bridges */
	uint64		bridge_tids;
} SiftV6ScanStats;

/* The next chunk of the hits of the spec's text query as a finalized bridge that holds no lossy page, false at the end of the hits */
typedef bool (*SiftV6ChunkFn) (void *arg, const SiftTidBridge **bridge);

/* v6_text.c: the text query of a statement of the aggregate (the planner reads it from the quals, the executor feeds it to the scan as chunks) */
extern IndexOptInfo *sift_v6_text_match(RelOptInfo *rel, RestrictInfo *ri, Const **query);
extern bool sift_v6_text_usable(IndexOptInfo *text, Const *query);
/* touch: the fraction of the rows of the store that the scan of the chunks reads (the granules the hits are in) */
extern double sift_v6_text_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *text, RestrictInfo *ri, Const *query, double *touch);

typedef struct SiftV6Text SiftV6Text;
extern SiftV6Text *sift_v6_text_open(const SiftV6Spec *spec, MemoryContext parent);
extern bool sift_v6_text_chunk(void *text, const SiftTidBridge **bridge);
extern void sift_v6_text_close(SiftV6Text *t);

/*
 * The fold of the rows of a scan into the groups, which sift_v6_scan_run drives with the rows of a scan of the index and a caller with the rows of its own (agg/hybrid_agg_exec.c): sift_v6_fold_batch
 * takes a batch of rows (the ColView of each leaf, indexed by leaf; sift_v6_fold_cols are the ones to decode), the live ones decided by the stored values and the maybe ones they could not decide;
 * the recheck says whether the heap keeps a maybe row (visible, and passing the quals the scan holds). A fold lives in the memory context it began in.
 */
typedef struct SiftV6Scan SiftV6Fold;
typedef bool (*SiftV6Recheck) (void *arg, uint64 key);

/* The columns a fold reads: their column of the index (1-based), their leaf in the store and the class of their values */
typedef struct SiftV6FoldCol
{
	int			attno;
	int			leaf;
	SiftV6Class cls;
	int			scale;			/* a numeric: the digits after the point of the scaled integer the leaf holds */
} SiftV6FoldCol;

extern SiftV6Fold *sift_v6_fold_begin(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, Snapshot snap, SiftV6ScanStats *stats, SiftV6Recheck recheck, void *recheck_arg);
extern const SiftV6FoldCol *sift_v6_fold_cols(const SiftV6Fold *f, int *n);
extern void sift_v6_fold_batch(SiftV6Fold *f, const uint64 *tid, const uint64 *live, const uint64 *maybe, const void *views, int n);
/* The same for rows that come with the score of their hit (score[r] for row r): the batch of a fold whose spec reads the score (sift_v6_spec_reads_score) */
extern void sift_v6_fold_batch_scored(SiftV6Fold *f, const uint64 *tid, const uint64 *live, const uint64 *maybe, const void *views, const float4 *score, int n);
extern void sift_v6_fold_end(SiftV6Fold *f);

/* The rows of a batch as the vectors of the keys and of the arguments of the aggregates (keyvec[i] is key i, argvec[j] the argument of aggregate j); what a scan hands each batch to */
typedef void (*SiftV6BatchFn) (void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec);
typedef struct SiftV6Scan SiftV6Scan;
struct SiftParPartRef;
struct SiftF2ParPlan;

extern SiftV6Scan *sift_v6_scan_begin(const SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, SiftV6ScanStats *stats, SiftV6BatchFn emit,
									  void *emit_arg);
/* The parts of the generations skip (ascending) are not read: another scan took them */
struct SiftV6StarRun;
/* The same for a statement of ext: star is the dimensions of the pass the scan is of (v6_star_run.h), NULL for an unnest or none */
extern void sift_v6_scan_run_ext(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, const uint64 *skip, int nskip,
								 SiftV6ScanStats *stats, struct SiftV6StarRun *star, SiftV6ChunkFn next_chunk, void *chunk_arg);
extern void sift_v6_scan_all(SiftV6Scan *s, const uint64 *skip, int nskip);
extern void sift_v6_scan_plan(SiftV6Scan *s, struct SiftF2ParPlan *out);
extern void sift_v6_scan_morsel_begin(SiftV6Scan *s, const uint64 *staged_dead, uint32 nstaged);
extern void sift_v6_scan_morsel(SiftV6Scan *s, const struct SiftParPartRef *ref, uint32 first, uint32 n);
extern void sift_v6_scan_end(SiftV6Scan *s);
extern void sift_v6_scan_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap,
							 const uint64 *skip, int nskip, SiftV6ScanStats *stats, SiftV6ChunkFn next_chunk, void *chunk_arg);

/* The same for a spec that is_table: the rows of the sift_table the quals accept, which its own versions show to the snapshot */
struct SiftStoreQuals;
extern void sift_v6_scan_table(const SiftV6Spec *spec, SiftV6Group *g, Relation rel, const struct SiftStoreQuals *quals, Snapshot snap, const uint64 *skip,
							   int nskip, SiftV6ScanStats *stats);

/*
 * The arms of a cohort of SiftV6Fuse (v6_fuse.h) that scan the same index with the same keys, in one pass: the rows are looked at in the heap once and every arm folds them into its groups. arms[0]
 * is the leader, whose keys, heap and snapshot the pass uses.
 */
struct SiftV6FuseCohort;

struct SiftClause;

/* An arm of a pass: its spec (with the columns of its gate when the arms have different keys), its groups, and the gate: the rows of the pass it takes (NULL: all) */
typedef struct SiftV6ScanArm
{
	const SiftV6Spec *spec;
	SiftV6Group *group;
	SiftV6ScanStats *stats;
	const struct SiftClause *gate;
} SiftV6ScanArm;

extern void sift_v6_scan_run_arms(struct SiftV6FuseCohort *cohort, const SiftV6ScanArm *arms, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap);

#endif							/* SIFT_AGG_V6_H */
