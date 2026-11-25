/*
 * col/f2/f2_store_ops_int.h - what the files of f2_store_ops*.c share: the sub-vtables of the SiftStoreOps of format major 2 (core/store_ops.h), one for each file, and the entries of the
 * device plan (col/f2/f2_gpu_plan.c) that the table of the GPU wraps. The tables serve a sift_col index and a sift_table alike: both are a SiftF2Store.
 */
#ifndef SIFT_COL_F2_STORE_OPS_INT_H
#define SIFT_COL_F2_STORE_OPS_INT_H

#include "postgres.h"

#include "agg/gpu_v6.h"
#include "core/store_ops.h"

extern const SiftStoreOps sift_f2_store_ops;
extern const SiftOrderOps sift_f2_order_ops;
extern const SiftPreaggOps sift_f2_preagg_ops;
extern const SiftProjOps sift_f2_proj_ops;
extern const SiftScopeOps sift_f2_scope_ops;
extern const SiftHybridOps sift_f2_hybrid_ops;
extern const SiftGpuOps sift_f2_gpu_ops;
extern const SiftZoneOps sift_f2_zone_ops;
extern const SiftDictOps sift_f2_dict_ops;
extern const SiftStoreParOps sift_f2_par_ops;
extern const SiftJsonbOps sift_f2_jsonb_ops;

/* The core of the table: the parts of a relation are written sorted by the columns of its key_order, the leaf of an attribute that a scan decodes as it is stored */
extern bool sift_f2_ops_owns_rel(Oid relid, bool is_table);
extern struct SiftV6Source *sift_f2_ops_open(EState *estate, const struct SiftV6Input *in, const struct SiftV6Vexpr *quals, bool sorted);
extern int	sift_f2_ops_card_leaf(Oid relid, int col);
extern bool sift_f2_ops_sorted_by(Oid relid, bool is_table, const int *atts, int n);
extern bool sift_f2_ops_leaf_holds(Oid relid, bool is_table, int att, int cls);

/* The device plan (f2_gpu_plan.c) */
extern SiftGpuPlan *sift_gpu_f2_plan(const SiftGpuQuery *q, MemoryContext mcxt);
extern void sift_gpu_f2_keys(const SiftGpuPlan *p, SiftVkKey *out);
extern void sift_gpu_f2_tab_key(const SiftGpuPlan *p, uint32 g0, uint32 g1, SiftVkTabKey *out);
extern bool sift_gpu_f2_load(SiftGpuPlan *p, uint32 g0, uint32 gmax, uint64 max_bytes, const uint8 *resident, uint32 *g1, const char **why);
extern void sift_gpu_f2_unload(SiftGpuPlan *p);
extern void sift_gpu_f2_part(const SiftGpuPlan *p, uint32 i, uint32 *g0, uint32 *g1);
extern const struct SiftPartDict *sift_gpu_f2_dict(const SiftGpuPlan *p, uint32 i, int slot);
extern void sift_gpu_f2_locate(const SiftGpuPlan *p, uint32 i, uint32 *part_id, uint64 *part_gen, uint32 *granule);
extern bool sift_gpu_f2_covers(const SiftGpuPlan *p, uint64 part_gen);
extern void sift_gpu_f2_free(SiftGpuPlan *p);

#endif							/* SIFT_COL_F2_STORE_OPS_INT_H */
