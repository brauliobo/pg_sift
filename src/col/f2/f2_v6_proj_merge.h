/*
 * col/f2/f2_v6_proj_merge.h - rows in the order of a SIFT_PROJ_SORT projection, from several sources at once: the sorted rows of each projection part, and the rows of whatever the projection does
 * not stand for (a part written since, a part with a deletion vector, the slab), which arrive in no order and are kept in a bounded heap. The merge hands out the rows by the sort columns of
 * the projection (descending and NULLS FIRST as the spec says, a float as PostgreSQL orders it, a string by its bytes: the caller has made sure that is the order of the statement) and stops at
 * the limit. Nothing here reads a page.
 */
#ifndef SIFT_COL_F2_V6_PROJ_MERGE_H
#define SIFT_COL_F2_V6_PROJ_MERGE_H

#include "core/host.h"

#include "col/f2/f2_proj.h"

/* The next row of a source in its order: its address and its cells (one for each column of the relation), valid until the next call; false after the last */
typedef bool (*SiftProjNextFn) (void *arg, uint64 *tid, const SiftCell **row);

typedef struct SiftProjStream
{
	SiftProjNextFn next;
	void	   *arg;
} SiftProjStream;

/* The order of two rows of the sort columns of spec: the first row is before (< 0), with (0) or after (> 0) the second */
extern int	sift_proj_order_cmp(const SiftProjSpec *spec, const SiftProjColumns *cols, const SiftCell *a, const SiftCell *b);

typedef struct SiftProjMerge SiftProjMerge;

/* limit < 0: no limit */
extern SiftProjMerge *sift_proj_merge_new(const SiftProjSpec *spec, const SiftProjColumns *cols, int64 limit);
extern void sift_proj_merge_add(SiftProjMerge *m, SiftProjStream s);
extern bool sift_proj_merge_next(SiftProjMerge *m, uint64 *tid, const SiftCell **row);
extern void sift_proj_merge_free(SiftProjMerge *m);

/* The rows of unordered sources kept in the order of spec: the first limit of them (all when limit < 0), each copied with the bytes of its strings */
typedef struct SiftProjTopK SiftProjTopK;

extern SiftProjTopK *sift_proj_topk_new(const SiftProjSpec *spec, const SiftProjColumns *cols, int ncols, int64 limit);
extern void sift_proj_topk_add(SiftProjTopK *t, uint64 tid, const SiftCell *row);
extern int	sift_proj_topk_count(const SiftProjTopK *t);

/* The rows kept, in order, as a source (the first call sorts them; nothing may be added after it) */
extern SiftProjStream sift_proj_topk_stream(SiftProjTopK *t);
extern void sift_proj_topk_free(SiftProjTopK *t);

#endif							/* SIFT_COL_F2_V6_PROJ_MERGE_H */
