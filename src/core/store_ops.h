/*
 * core/store_ops.h - format-agnostic storage seam for the unified SiftAgg engine (Spec 86).
 *
 * Evolves the 6-pointer SiftV6Format into a multi-format registry with 6 mandatory core pointers
 * and 10 independent optional capability sub-vtables (NULL = unsupported, skipped in one branch).
 * Zero format-specific headers (col/f2, col/v1, table/f2, table/v1) are ever included here.
 */
#ifndef SIFT_CORE_STORE_OPS_H
#define SIFT_CORE_STORE_OPS_H

#include "postgres.h"

#include "access/attnum.h"
#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"

#include "agg/vec.h"
#include "core/sift.h"

#define SIFT_STORE_OPS_MAX	8

struct SiftCardStatsView;
struct SiftV6Source;
struct SiftV6Input;
struct SiftV6Vexpr;
struct SiftV6Spec;
struct SiftV6Col;
struct SiftV6Batch;

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

/* Relation schema summary returned by SiftStoreOps.schema */
typedef struct SiftStoreSchema
{
	int			natts;			/* 1-based attribute count */
	bool		is_table;
	bool		has_jsonb;
	SiftStoreAtt *atts;			/* palloc'd array [0..natts-1], optional */
} SiftStoreSchema;

/* Relation statistics summary returned by SiftStoreOps.stats */
typedef struct SiftStoreStats
{
	double		live_rows;
	uint64		stored_bytes;	/* total on-disk bytes for b_io cost model */
	double		allvis_frac;	/* [0.0, 1.0] fraction needing no heap vet */
	const struct SiftCardStatsView *card_view;	/* optional, NULL if unavailable */
	int			(*card_leaf) (Oid relid, int col);
} SiftStoreStats;

/* ---- 10 Independent Optional Capability Sub-Vtables (NULL = unsupported) ---- */

/* 1. Key-order / sorted streaming (Group, Window, ASOF, Top-N) */
typedef struct SiftOrderOps
{
	bool		(*sorted_by) (Oid relid, bool is_table, const int *atts, int n);
	bool		(*can_seek) (Oid relid, bool is_table, int att);
	bool		(*key_bounds) (Oid relid, bool is_table, int att, int64 *lo, int64 *hi);
} SiftOrderOps;

/* 2. Pre-aggregates / cubes / rollups / calendar / HLL / presence */
typedef struct SiftPreaggOps
{
	bool		(*can_answer) (Oid relid, bool is_table, const struct SiftV6Spec *spec);
	void	   *(*open_preagg) (EState *estate, Oid relid, bool is_table, const struct SiftV6Spec *spec);
	bool		(*next_preagg) (void *state, Datum *values, bool *isnull);
	void		(*close_preagg) (void *state);
} SiftPreaggOps;

/* 3. Materialized projections & projection-ordered scans */
typedef struct SiftProjOps
{
	bool		(*find_proj) (Oid relid, bool is_table, const int *atts, int natts, int *proj_id);
	bool		(*proj_sorted_by) (Oid relid, bool is_table, int proj_id, const int *atts, int n);
	struct SiftV6Source *(*open_proj) (EState *estate, int proj_id, const struct SiftV6Input *in,
									   const struct SiftV6Vexpr *quals, bool sorted);
	bool		(*proj_stats) (Oid relid, bool is_table, int proj_id, SiftStoreStats *out);
} SiftProjOps;

/* 4. Scope index routing (scope_v6 routing, scope units, delta overlay) */
typedef struct SiftScopeOps
{
	bool		(*has_scope) (Oid relid, bool is_table);
	bool		(*route_scope) (Oid relid, bool is_table, Oid scope_relid, const int *key_atts, int nkeys);
	struct SiftV6Source *(*open_scope) (EState *estate, const struct SiftV6Input *in,
										const struct SiftV6Vexpr *quals, void *scope_ctx);
	void		(*close_scope) (void *scope_ctx);
} SiftScopeOps;

/* 5. BM25 hybrid hit-set -> rowid/bridge filter & score feed */
typedef struct SiftHybridOps
{
	bool		(*can_hybrid) (Oid relid, bool is_table);
	struct SiftV6Source *(*open_hybrid) (EState *estate, const struct SiftV6Input *in,
										 const struct SiftV6Vexpr *quals, const void *match_set,
										 bool with_scores);
	bool		(*batch_scores) (struct SiftV6Source *src, const float4 **scores, uint16 *n);
} SiftHybridOps;

/* 6. Vulkan / GPU compressed-chunk feed & device capability */
typedef struct SiftGpuOps
{
	bool		(*can_feed) (Oid relid, bool is_table, const struct SiftV6Spec *spec);
	void	   *(*open_chunks) (EState *estate, Oid relid, bool is_table, const struct SiftV6Spec *spec);
	bool		(*next_chunk) (void *state, void *chunk_out);
	void		(*close_chunks) (void *state);
} SiftGpuOps;

/* 7. Granule/part zone-map pruning & pushed filter evaluation */
typedef struct SiftZoneOps
{
	bool		(*can_prune) (Oid relid, bool is_table, int att);
	bool		(*push_filters) (struct SiftV6Source *src, const void *filters, int nfilters);
	uint64		(*pruned_units) (const struct SiftV6Source *src);
} SiftZoneOps;

/* 8. Zero-decode SVEC_DICT32 batch aggregation */
typedef struct SiftDictOps
{
	bool		(*has_dict32) (Oid relid, bool is_table, int att);
	uint32		(*dict_card) (const struct SiftV6Batch *batch, int col_slot);
	bool		(*dict_str) (const struct SiftV6Batch *batch, int col_slot, uint32 code,
							 const char **ptr, uint32 *len);
} SiftDictOps;

/* 9. Parallel worker snapshot export/import & part slicing */
typedef struct SiftParOps
{
	Size		(*view_size) (Oid relid, bool is_table, Snapshot snap);
	void		(*view_export) (Oid relid, bool is_table, Snapshot snap, void *buf, Size len);
	uint32		(*part_count) (const void *view, Size len);
	struct SiftV6Source *(*open_worker) (EState *estate, const struct SiftV6Input *in,
										 const struct SiftV6Vexpr *quals, const void *view,
										 Size view_len, uint32 part_lo, uint32 part_hi);
} SiftParOps;

/* 10. Shredded JSONB paths, typeof, unnest arrays, exception overflow */
typedef struct SiftJsonbOps
{
	bool		(*has_path) (Oid relid, bool is_table, int att, const char *path);
	int			(*path_leaf) (Oid relid, bool is_table, int att, const char *path);
	bool		(*read_exception) (int cls, const char *payload, uint32 len, MemoryContext cx,
								   const char **text, uint32 *tlen, double *f);
	struct SiftV6Source *(*open_unnest) (EState *estate, const struct SiftV6Input *in,
										 const struct SiftV6Vexpr *quals, void *unnest_ctx);
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
extern bool sift_store_ops_any(void);
extern int	sift_store_ops_count(void);
extern const SiftStoreOps *sift_store_ops_at(int i);

#endif							/* SIFT_CORE_STORE_OPS_H */
