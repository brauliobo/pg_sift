/*
 * col/f2/agg/scan.h - the entries of the aggregate scan of a Format 2.0 relation (f2_v6_scan.c) that the zone, order, par and hybrid tables of f2_store_ops*.c give the engine (core/store/ops.h).
 */
#ifndef SIFT_COL_F2_V6_SCAN_H
#define SIFT_COL_F2_V6_SCAN_H

#include "postgres.h"

#include "agg/engine.h"

extern SiftV6Fold *sift_v6_fold_begin(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, Snapshot snap, SiftV6ScanStats *stats, SiftV6Recheck recheck, void *recheck_arg);
extern const SiftV6FoldCol *sift_v6_fold_cols(const SiftV6Fold *f, int *n);
extern void sift_v6_fold_batch(SiftV6Fold *f, const uint64 *tid, const uint64 *live, const uint64 *maybe, const void *views, int n);
extern void sift_v6_fold_batch_scored(SiftV6Fold *f, const uint64 *tid, const uint64 *live, const uint64 *maybe, const void *views, const float4 *score, int n);
extern void sift_v6_fold_end(SiftV6Fold *f);
extern SiftV6Scan *sift_v6_scan_begin(const SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, const struct SiftStoreQuals *quals, Snapshot snap,
									  SiftV6ScanStats *stats, struct SiftV6StarRun *star, SiftV6BatchFn emit, void *emit_arg);
extern void sift_v6_scan_run_ext(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, const uint64 *skip, int nskip,
								 const struct SiftScopeRouter *router, SiftV6ScanStats *stats, struct SiftV6StarRun *star, SiftV6ChunkFn next_chunk, void *chunk_arg);
extern void sift_v6_scan_plan(SiftV6Scan *s, struct SiftF2ParPlan *out);
extern void sift_v6_scan_morsel_begin(SiftV6Scan *s, const uint64 *staged_dead, uint32 nstaged);
extern void sift_v6_scan_morsel(SiftV6Scan *s, struct SiftV6Group *into, const struct SiftParPartRef *ref, uint32 first, uint32 n);
extern void sift_v6_scan_end(SiftV6Scan *s);
extern void sift_v6_scan_rows(const SiftV6Spec *spec, SiftV6Rows *rows, SiftV6Group *totals, struct SiftV6StarRun *star, Relation heap, Relation index, ScanKey keys, int nkeys, const struct SiftStoreQuals *quals, Snapshot snap,
							  SiftV6ScanStats *stats, SiftV6ChunkFn next_chunk, void *chunk_arg);
extern void sift_v6_scan_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap,
							 const uint64 *skip, int nskip, const struct SiftScopeRouter *router, SiftV6ScanStats *stats, SiftV6ChunkFn next_chunk, void *chunk_arg);
extern void sift_v6_scan_table(const SiftV6Spec *spec, SiftV6Group *g, Relation rel, const struct SiftStoreQuals *quals, Snapshot snap, const uint64 *skip,
							   int nskip, SiftV6ScanStats *stats, struct SiftV6StarRun *star, SiftV6ChunkFn next_chunk, void *chunk_arg);
extern void sift_v6_scan_run_arms(struct SiftV6FuseCohort *cohort, const SiftV6ScanArm *arms, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap);

#endif							/* SIFT_COL_F2_V6_SCAN_H */
