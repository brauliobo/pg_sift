/*
 * core/store_ops.h - format-agnostic storage seam for the unified SiftAgg engine (Spec 86).
 *
 * A multi-format registry with 6 mandatory core pointers and 10 independent optional capability sub-vtables (NULL = unsupported, skipped in one
 * branch). The engine (agg/) reaches a format only through these tables and the plain structs of core/scan_types.h: no col/f2, col/v1, table/f2 or table/v1 header is included here or in agg/.
 * Format state that crosses the seam is opaque (a struct named here and defined by the format, or a void pointer).
 */
#ifndef SIFT_CORE_STORE_OPS_H
#define SIFT_CORE_STORE_OPS_H

#include "postgres.h"

#include "access/attnum.h"
#include "access/genam.h"
#include "access/skey.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "utils/snapshot.h"

#include "agg/vec.h"
#include "agg/vk.h"
#include "col/col.h"
#include "core/scan_types.h"
#include "core/sift.h"

#define SIFT_STORE_OPS_MAX	8

struct SiftCardStatsView;
struct SiftClause;
struct SiftColSchema;
struct SiftGpuPlan;
struct SiftHybridBatch;
struct SiftGpuQuery;
struct SiftPartDict;
struct SiftScopeRouter;
struct SiftStoreQuals;
struct SiftTidBridge;
struct SiftV6Batch;
struct SiftV6FoldCol;
struct SiftV6FuseCohort;
struct SiftV6Group;
struct SiftV6Input;
struct SiftV6Rows;
struct SiftV6Scan;
struct SiftV6ScanArm;
struct SiftV6ScanStats;
struct SiftV6Source;
struct SiftV6StarRun;
struct SiftV6TopNSource;
struct SiftV6TopNSpec;
struct SiftV6Spec;
struct SiftV6Vexpr;

/* Per-attribute descriptor returned by SiftStoreOps.schema */
typedef struct SiftStoreAtt
{
	AttrNumber	attno;			/* 1-based attribute number */
	Oid			typid;
	int32		typmod;
	SiftVecType vec_type;		/* native SVEC_* or SVEC_DATUM */
	int16		scale;
	uint8		prec;
	bool		is_jsonb;
	bool		is_wide;
} SiftStoreAtt;

/*
 * Relation schema summary returned by SiftStoreOps.schema. The leaf map is the one the planner of the aggregate reads the columns by, for an index (its attributes are its columns) and for a
 * table (its attributes are its columns) alike: attribute att (1-based) is held by att_nleaves[att] leaves from leaf att_first[att] of the leaves schema (att_first[att] < 0: none); att_opfamily[att]
 * is the operator family of the column. The arrays and the SiftStoreAtt descriptors live as long as the relation is open (or the cache of the format that owns them).
 */
typedef struct SiftStoreSchema
{
	int			natts;			/* 1-based attribute count */
	bool		is_table;
	bool		has_jsonb;
	SiftStoreAtt *atts;			/* palloc'd array [0..natts-1], optional */
	const struct SiftColSchema *leaves;
	const int  *att_first;
	const int  *att_nleaves;
	const Oid  *att_opfamily;
	const SiftCellKind *leaf_kinds;	/* per leaf of leaves */
	const uint8 *leaf_hints;
} SiftStoreSchema;

/* Relation statistics summary returned by SiftStoreOps.stats */
typedef struct SiftStoreStats
{
	double		live_rows;
	uint64		stored_bytes;	/* total on-disk bytes for b_io cost model */
	double		allvis_frac;	/* [0.0, 1.0] fraction needing no heap vet */
	const struct SiftCardStatsView *card_view;	/* optional, NULL if unavailable */
	int			(*card_leaf) (Oid relid, int col);
	/* optional: the share [0, 1] of the units that keep a point directory of index column col, which an equality or a short list on it reads instead of the whole unit (NULL, 0: none does) */
	double		(*point_cover) (Oid relid, int col);
} SiftStoreStats;

/* ---- 10 Independent Optional Capability Sub-Vtables (NULL = unsupported) ---- */

/*
 * 1. Key order: the sort order the parts keep, and the scan that keeps the best rows of it. lead_att: the attribute of the first key column of the index (0: none, or not a column);
 * scan_rows: the rows of an ORDER BY .. LIMIT of one table go to rows instead of the groups, and to totals as well when the statement carries the aggregates of the whole set; star: the dimensions
 * a join of the fact table reads (index NULL: heap is a sift_table, which quals say what it decides).
 */
typedef struct SiftOrderOps
{
	bool		(*sorted_by) (Oid relid, bool is_table, const int *atts, int n);
	int			(*lead_att) (Oid relid);
	void		(*scan_rows) (const struct SiftV6Spec *spec, struct SiftV6Rows *rows, struct SiftV6Group *totals, struct SiftV6StarRun *star, Relation heap, Relation index, ScanKey keys, int nkeys, const struct SiftStoreQuals *quals,
							  Snapshot snap, struct SiftV6ScanStats *stats, bool (*next_chunk) (void *arg, const struct SiftTidBridge **bridge), void *chunk_arg);
	bool		rows_extras;	/* scan_rows also folds the aggregates of the whole set into totals, reads the dimensions of a join (star) and takes the chunks of a text match */
	/* the source of the Top-N tournament over the parts of an index (NULL, or false: the best rows are those of a sort of the rows the source gives) */
	bool		(*topn_open) (EState *estate, const struct SiftV6TopNSpec *spec, const struct SiftV6Input *in, const struct SiftV6Vexpr *quals, struct SiftV6TopNSource *dst);
} SiftOrderOps;

/* 2. Pre-aggregates: whether the parts of the relation keep cubes (preagg is not off) */
typedef struct SiftPreaggOps
{
	bool		(*keeps_cubes) (Oid relid, bool is_table);
} SiftPreaggOps;

/*
 * 3. Materialized projections: the groups of an aggregate statement from the cells of a pre-aggregating projection, and the planner path of a sorted projection for a row query. serve:
 * false means no projection answers it and nothing was changed; index NULL means heap is a sift_table. The groups are in out, allocated in cx, and next gives them one at a time.
 */
#define SIFT_PROJ_SERVE_NAME_LEN	64

typedef struct SiftProjResult
{
	bool		used;
	char		name[SIFT_PROJ_SERVE_NAME_LEN];
	int			nparts;			/* projection parts read */
	int			nsubtracted;	/* of them, the base parts whose dead rows were taken away */
	int			nbase;			/* base parts the projection does not stand for: read from the relation with the slabs */
	int64		nrest_rows;		/* the rows those reads found */
	int			natts;
	int			nrows;
	int			next;
	Datum	   *values;			/* nrows * natts: the keys then the aggregates of each group */
	bool	   *isnull;
} SiftProjResult;

typedef struct SiftProjOps
{
	bool		(*serve) (const struct SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, MemoryContext cx,
						  SiftProjResult *out);
	bool		(*next) (SiftProjResult *r, Datum *values, bool *isnull);
	bool		(*sort_offered) (const RelOptInfo *rel);
} SiftProjOps;

/*
 * 4. Scope summaries: the paths that answer a join of the facts with the pairs of a scope of the index from the cubes of its summary, the scope of one key a join of a small side pins the
 * scan to, and the router that keeps a scan to the granules of that key (build_router: NULL when the summary cannot route).
 */
typedef struct SiftScopeOps
{
	void		(*upper_paths) (PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
	void		(*topk_paths) (PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);
	bool		(*one_key) (PlannerInfo *root, RelOptInfo *fact, RelOptInfo *pairs, IndexOptInfo *ix, AttrNumber fact_att, AttrNumber pairs_att, int *ord, Expr **key);
	const struct SiftScopeRouter *(*build_router) (Relation index, int ord, Snapshot snap, const int64 *keys, int nkeys, MemoryContext cx);
	void		(*router_counts) (const struct SiftScopeRouter *r, int64 *kept, int64 *total);
} SiftScopeOps;

/*
 * 5. BM25 hybrid: the scan of the store a text scan joins (SiftHybridScan names what it reads; the sink takes the rows as a SiftHybridBatch, agg/hybrid.h), the survey of the granules
 * its quals leave, whether the predicates a sift_table compiled (zone.compile_quals) leave a scan to do, and the fold of the rows of a scan into the groups of an aggregate (SiftV6Fold,
 * agg/v6/v6.h): begin, the columns it reads, the batch with the scores of its hits, end. A format that reads through a SiftV6Source instead (Format 1) offers can_hybrid,
 * open_hybrid with the match set and batch_scores, and leaves the entries above NULL.
 */
typedef struct SiftHybridScan
{
	Relation	heap;
	Relation	index;			/* the index of a sift_col; NULL for a sift_table */
	Snapshot	snap;
	ScanKey		keys;			/* the keys the store takes */
	int			nkeys;
	const SiftColPred *ext_preds;
	int			n_ext_preds;
	const int  *leaves;			/* the columns the sink reads (NULL: none) */
	int			nleaves;
	const struct SiftTidBridge *bridge;
	uint64	   *granules_seen;
	uint64	   *granules_skipped;
	uint64	   *parts_skipped;
	bool		heap_decide;
	const struct SiftStoreQuals *quals;	/* a sift_table: its predicates */
} SiftHybridScan;

typedef struct SiftHybridOps
{
	void		(*scan) (const SiftHybridScan *s, void (*sink) (void *arg, const struct SiftHybridBatch *batch), void *arg);
	uint64		(*survey) (const SiftHybridScan *s);
	bool		(*table_has_preds) (const struct SiftStoreQuals *q);
	struct SiftV6Scan *(*fold_begin) (const struct SiftV6Spec *spec, struct SiftV6Group *g, Relation heap, Relation index, Snapshot snap, struct SiftV6ScanStats *stats,
									  bool (*recheck) (void *arg, uint64 key), void *recheck_arg);
	const struct SiftV6FoldCol *(*fold_cols) (const struct SiftV6Scan *f, int *n);
	void		(*fold_batch_scored) (struct SiftV6Scan *f, const uint64 *tid, const uint64 *live, const uint64 *maybe, const void *views, const float4 *score, int n);
	void		(*fold_end) (struct SiftV6Scan *f);
	bool		(*can_hybrid) (Oid relid, bool is_table);
	struct SiftV6Source *(*open_hybrid) (EState *estate, const struct SiftV6Input *in,
										 const struct SiftV6Vexpr *quals, const void *match_set,
										 bool with_scores);
	bool		(*batch_scores) (struct SiftV6Source *src, const float4 **scores, uint16 *n);
} SiftHybridOps;

/*
 * 6. Vulkan: the parts of the relation the device reads (plan, then load and unload their chunks), what the CPU scan leaves out because the device read it, and where a granule of the plan
 * is in its part. The plan and its query are the engine's (agg/gpu_v6.h); the format fills them.
 */
typedef struct SiftGpuOps
{
	struct SiftGpuPlan *(*plan) (const struct SiftGpuQuery *q, MemoryContext mcxt);
	void		(*free) (struct SiftGpuPlan *p);
	bool		(*load) (struct SiftGpuPlan *p, uint32 g0, uint32 gmax, uint64 max_bytes, const uint8 *resident, uint32 *g1, const char **why);
	void		(*unload) (struct SiftGpuPlan *p);
	void		(*keys) (const struct SiftGpuPlan *p, SiftVkKey *out);
	void		(*tab_key) (const struct SiftGpuPlan *p, uint32 g0, uint32 g1, SiftVkTabKey *out);
	void		(*part) (const struct SiftGpuPlan *p, uint32 i, uint32 *g0, uint32 *g1);
	const struct SiftPartDict *(*dict) (const struct SiftGpuPlan *p, uint32 i, int slot);
	void		(*locate) (const struct SiftGpuPlan *p, uint32 i, uint32 *part_id, uint64 *part_gen, uint32 *granule);
	bool		(*covers) (const struct SiftGpuPlan *p, uint64 part_gen);
} SiftGpuOps;

/*
 * 7. Zone-pruned scan: the aggregate scan of the parts of a relation (the granules its zones, dictionaries and skipping indexes leave, the rows its visibility shows), and the predicates of
 * the statement's quals a sift_table decides on its leaves. compile_quals makes them from the index quals (a struct the format owns, in cx); bind_quals gives them the values of the
 * runtime keys; quals_keys says whether any row can qualify and the keys of the scan as an index scan's.
 */
typedef struct SiftZoneOps
{
	struct SiftStoreQuals *(*compile_quals) (Relation heap, List *indexqual, MemoryContext cx);
	void		(*bind_quals) (Relation heap, struct SiftStoreQuals *q, ExprContext *econtext);
	bool		(*quals_keys) (const struct SiftStoreQuals *q, ScanKey *keys, int *nkeys);
	void		(*scan_table) (const struct SiftV6Spec *spec, struct SiftV6Group *g, Relation rel, const struct SiftStoreQuals *quals, Snapshot snap, const uint64 *skip,
							   int nskip, struct SiftV6ScanStats *stats);
	void		(*scan_run) (const struct SiftV6Spec *spec, struct SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, const uint64 *skip,
							 int nskip, const struct SiftScopeRouter *router, struct SiftV6ScanStats *stats, struct SiftV6StarRun *star,
							 bool (*next_chunk) (void *arg, const struct SiftTidBridge **bridge), void *chunk_arg);
	void		(*scan_arms) (struct SiftV6FuseCohort *cohort, const struct SiftV6ScanArm *arms, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap);
} SiftZoneOps;

/* 8. Dictionary-coded strings: the attribute is kept as the codes of a dictionary a batch aggregates without decoding (a scan then gives SVEC_DICT32) */
typedef struct SiftDictOps
{
	bool		(*has_dict32) (Oid relid, bool is_table, int att);
} SiftDictOps;

/*
 * 9. Parallel scan: the leader lists the units of the scan once (plan, in memory a DSM chunk holds), the participants read runs of the granules of a unit. begin makes a scan that is
 * not run apart; morsel_begin and morsel are a participant's.
 */
typedef struct SiftParOps
{
	struct SiftV6Scan *(*begin) (const struct SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, struct SiftV6ScanStats *stats,
								 void (*emit) (void *arg, const struct SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec), void *emit_arg);
	void		(*plan) (struct SiftV6Scan *s, SiftF2ParPlan *out);
	void		(*morsel_begin) (struct SiftV6Scan *s, const uint64 *staged_dead, uint32 nstaged);
	void		(*morsel) (struct SiftV6Scan *s, struct SiftV6Group *into, const SiftParPartRef *ref, uint32 first, uint32 n);
	void		(*end) (struct SiftV6Scan *s);
} SiftParOps;

/* 10. Shredded JSONB: the leaf that keeps whether a top-level key is present on a row of a document, and the scan of the elements of an array column (UNNEST) */
typedef struct SiftJsonbOps
{
	int			(*key_leaf) (Oid indexoid, int att, const char *key, int len);
	int			(*table_key_leaf) (Oid relid, int att, const char *key, int len);	/* the same for a sift_table: the leaf of the declared path of the key (NULL: a table has none) */
	bool		exact_values;	/* the scan of the store gives what a document gives the heap: the value of a key that its leaf cannot hold, read from the exception of its row, and the array an index expression computes */
	bool		(*path_declared) (Oid relid, bool is_table, int att, const List *path);	/* the paths option of the index declares the path (a list of String keys) of the document attribute att */
	void		(*unnest_run) (const struct SiftV6Spec *spec, struct SiftV6Group *g, Relation heap, Relation index, Snapshot snap, struct SiftV6ScanStats *stats, const struct SiftV6StarRun *star);
} SiftJsonbOps;

/* Unified multi-format storage ops descriptor */
typedef struct SiftStoreOps
{
	/* --- 6 mandatory core fields (every format implements these) --- */
	const char *name;			/* "f2", "v1", "test" */
	bool		(*owns_rel) (Oid relid, bool is_table);
	bool		(*schema) (Oid relid, bool is_table, SiftStoreSchema *out);
	bool		(*stats) (Oid relid, bool is_table, SiftStoreStats *out);
	struct SiftV6Source *(*open) (EState *estate, const struct SiftV6Input *in,
								  const struct SiftV6Vexpr *quals, bool sorted);
	/* SiftV6Source already carries next(), skip(), fetch(), close() */
	bool		(*leaf_holds) (Oid relid, bool is_table, int att, int cls);

	/* --- 10 independent optional capability sub-vtables (NULL = unsupported) --- */
	const SiftOrderOps *order;
	const SiftPreaggOps *preagg;
	const SiftProjOps *proj;
	const SiftScopeOps *scope;
	const SiftHybridOps *hybrid;
	const SiftGpuOps *gpu;
	const SiftZoneOps *zone;
	const SiftDictOps *dict;
	const SiftParOps *par;
	const SiftJsonbOps *jsonb;
} SiftStoreOps;

extern void sift_store_ops_register(const SiftStoreOps *ops);
extern void sift_store_ops_unregister(const SiftStoreOps *ops);
extern const SiftStoreOps *sift_store_ops_for_oid(Oid relid, bool is_table);
/* The format scans a store for the hybrid aggregate (BM25 hits joined with the store's rows) and folds its rows into groups: both entries of the 7-pointer shape are there */
static inline bool
sift_store_ops_serves_hybrid(const SiftStoreOps *ops)
{
	return ops != NULL && ops->hybrid != NULL && ops->hybrid->scan != NULL && ops->hybrid->fold_begin != NULL;
}

extern bool sift_store_ops_any(void);
extern int	sift_store_ops_count(void);
extern const SiftStoreOps *sift_store_ops_at(int i);

#endif							/* SIFT_CORE_STORE_OPS_H */
